#include "ThemePackageInstaller.hpp"

#include "ThemeHttp.hpp"
#include "ZipReader.hpp"

#include "core/DebugLog.hpp"

#include <switchu/fs_remove.hpp>

#include <nxui/core/I18n.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Exception.hpp>
#include <curlpp/Infos.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <nlohmann/json.hpp>
#include <switch.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <stdexcept>
#include <system_error>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct GitHubRepoSource {
    std::string owner;
    std::string repo;
    std::string ref;
    std::string rawRoot;
};

struct RemoteFile {
    std::string relativePath;
    std::string downloadUrl;
};

std::string trimSlashes(std::string path) {
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

std::string parentDirectory(const std::string& path) {
    std::string clean = trimSlashes(path);
    std::size_t slash = clean.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return clean.substr(0, slash);
}

std::string basename(const std::string& path) {
    std::string clean = trimSlashes(path);
    std::size_t slash = clean.find_last_of('/');
    if (slash == std::string::npos)
        return clean;
    return clean.substr(slash + 1);
}

std::string dirnameForPreview(const std::string& path) {
    std::string name = basename(path);
    if (name.empty())
        return {};
    return "preview/" + name;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return (char)std::tolower(ch);
    });
    return value;
}

bool hasKnownAssetExtension(const std::string& path) {
    std::string name = lowerString(basename(path));
    std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;

    const std::string ext = name.substr(dot + 1);
    static constexpr const char* kAssetExtensions[] = {
        "png", "jpg", "jpeg", "webp", "bmp", "gif", "dds",
        "wav", "mp3", "ogg", "flac",
        "ttf", "otf",
        "json"
    };
    return std::any_of(std::begin(kAssetExtensions), std::end(kAssetExtensions), [&](const char* assetExt) {
        return ext == assetExt;
    });
}

bool hasParentTraversal(const std::string& path) {
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..")
            return true;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

bool isSafeThemeId(const std::string& value) {
    if (value.size() < 2 || value.size() > 64)
        return false;
    for (unsigned char ch : value) {
        if (!std::islower(ch) && !std::isdigit(ch) && ch != '_' && ch != '-')
            return false;
    }
    return true;
}

bool isSafeRelativePath(const std::string& value) {
    if (value.empty() || value.size() > 240 || value.front() == '/' || value.front() == '\\'
        || value.find(':') != std::string::npos || value.find('\\') != std::string::npos)
        return false;
    return !hasParentTraversal(value);
}

std::string urlEncodeComponent(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());

    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back((char)ch);
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4) & 0x0F]);
            encoded.push_back(kHex[ch & 0x0F]);
        }
    }

    return encoded;
}

std::string urlEncodePath(const std::string& path) {
    std::string encoded;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!encoded.empty())
            encoded.push_back('/');
        encoded += urlEncodeComponent(part);
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return encoded;
}

std::string rawGitHubFileUrl(const GitHubRepoSource& repo, const std::string& repoPath) {
    return repo.rawRoot + "/" + urlEncodePath(trimSlashes(repoPath));
}

void appendUniquePath(std::vector<std::string>& paths, std::string path) {
    path = trimSlashes(std::move(path));
    if (path.empty())
        return;
    if (startsWith(path, "https://") || startsWith(path, "http://"))
        return;
    if (std::find(paths.begin(), paths.end(), path) != paths.end())
        return;
    paths.push_back(std::move(path));
}

bool pathExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

bool ensureDirectoryRecursive(const std::string& path) {
    if (path.empty())
        return false;

    std::string clean = path;
    while (!clean.empty() && clean.back() == '/')
        clean.pop_back();
    if (clean.empty())
        return false;

    std::size_t scheme = clean.find(":/");
    std::string current;
    std::size_t start = 0;
    if (scheme != std::string::npos) {
        current = clean.substr(0, scheme + 2);
        start = scheme + 2;
    }

    while (start < clean.size()) {
        std::size_t slash = clean.find('/', start);
        std::string part = clean.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/')
                current.push_back('/');
            current += part;
            std::error_code ec;
            if (!std::filesystem::create_directory(current, ec) && ec)
                return false;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    return true;
}

// Quanto ainda cabe no cartao. Zero quando nao da para saber, e ai quem chama
// segue em frente em vez de recusar por falta de informacao.
std::uint64_t sdFreeBytes() {
    struct statvfs st{};
    if (statvfs("sdmc:/", &st) != 0)
        return 0;
    return (std::uint64_t)st.f_bavail * (std::uint64_t)st.f_frsize;
}

// The walk itself now lives in switchu/fs_remove.hpp. It was duplicated here
// while the theme deletion path had its own broken copy under the same name;
// keeping one implementation is how that stops happening again.
bool removeDirectoryRecursive(const std::string& path) {
    std::string failedPath;
    if (switchu::removeRecursive(path, &failedPath))
        return true;
    DebugLog::log("[themeshop] could not remove %s (stopped at %s, %d: %s)",
                  path.c_str(), failedPath.c_str(), errno, std::strerror(errno));
    return false;
}

// Do not erase a working theme until its replacement was fully unpacked.
bool renameDirectory(const std::string& from, const std::string& to,
                     std::string& error) {
    // std::filesystem::rename is not reliable on libnx's sdmc: filesystem.
    // The C runtime rename maps directly to the filesystem service instead.
    if (std::rename(from.c_str(), to.c_str()) == 0)
        return true;

    error = std::strerror(errno);
    DebugLog::log("[themeshop] rename failed: %s -> %s (%d: %s)",
                  from.c_str(), to.c_str(), errno, error.c_str());
    return false;
}

bool replaceDirectoryFromStaging(const std::string& destination,
                                 const std::string& staging,
                                 std::string& error) {
    const std::string previous = destination + ".previous";
    if (!removeDirectoryRecursive(previous) && pathExists(previous)) {
        error = "could not clear the previous installation";
        return false;
    }

    const bool hadPrevious = pathExists(destination);
    if (hadPrevious) {
        if (!renameDirectory(destination, previous, error))
            return false;
    }

    if (renameDirectory(staging, destination, error)) {
        removeDirectoryRecursive(previous);
        return true;
    }

    if (hadPrevious) {
        std::string restoreError;
        if (!renameDirectory(previous, destination, restoreError))
            DebugLog::log("[themeshop] could not restore previous theme: %s", restoreError.c_str());
    }
    return false;
}

std::string joinUrl(const std::string& baseUrl, const std::string& relativePath) {
    if (relativePath.empty())
        return baseUrl;
    if (startsWith(relativePath, "https://") || startsWith(relativePath, "http://"))
        return relativePath;

    std::size_t slash = baseUrl.find_last_of('/');
    if (slash == std::string::npos)
        return urlEncodePath(trimSlashes(relativePath));
    return baseUrl.substr(0, slash + 1) + urlEncodePath(trimSlashes(relativePath));
}

std::string joinPath(const std::string& base, const std::string& relative) {
    if (base.empty())
        return relative;
    if (relative.empty())
        return base;
    if (base.back() == '/')
        return base + relative;
    return base + "/" + relative;
}

bool parseRawGitHubUrl(const std::string& url, GitHubRepoSource& out) {
    static constexpr const char* kPrefix = "https://raw.githubusercontent.com/";
    if (!startsWith(url, kPrefix))
        return false;

    std::string rest = url.substr(sizeof(kPrefix) - 1);
    std::size_t slash1 = rest.find('/');
    if (slash1 == std::string::npos)
        return false;
    std::size_t slash2 = rest.find('/', slash1 + 1);
    if (slash2 == std::string::npos)
        return false;
    std::size_t slash3 = rest.find('/', slash2 + 1);
    if (slash3 == std::string::npos)
        return false;

    out.owner = rest.substr(0, slash1);
    out.repo = rest.substr(slash1 + 1, slash2 - slash1 - 1);
    out.ref = rest.substr(slash2 + 1, slash3 - slash2 - 1);
    out.rawRoot = std::string(kPrefix) + out.owner + "/" + out.repo + "/" + out.ref;
    return !(out.owner.empty() || out.repo.empty() || out.ref.empty());
}

std::vector<RemoteFile> listThemeFilesFromTree(const GitHubRepoSource& repo,
                                               const std::string& themePath) {
    std::vector<RemoteFile> files;

    const std::string apiUrl = "https://api.github.com/repos/" + repo.owner + "/" + repo.repo
        + "/git/trees/" + urlEncodeComponent(repo.ref) + "?recursive=1";
    const std::string body = themeshop::http::getText(apiUrl, {
        "Accept: application/vnd.github+json",
        "X-GitHub-Api-Version: 2022-11-28"
    });

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid GitHub tree JSON: ") + ex.what());
    }

    auto treeIt = root.find("tree");
    if (treeIt == root.end() || !treeIt->is_array())
        throw std::runtime_error("GitHub tree response is missing entries");

    const std::string cleanRoot = trimSlashes(themePath);
    const std::string prefix = cleanRoot.empty() ? std::string() : cleanRoot + "/";
    for (const auto& item : *treeIt) {
        if (!item.is_object())
            continue;

        auto typeIt = item.find("type");
        auto pathIt = item.find("path");
        if (typeIt == item.end() || pathIt == item.end() || !typeIt->is_string() || !pathIt->is_string())
            continue;
        if (typeIt->get<std::string>() != "blob")
            continue;

        std::string repoPath = trimSlashes(pathIt->get<std::string>());
        if (!prefix.empty() && !startsWith(repoPath, prefix))
            continue;

        std::string relativePath = prefix.empty() ? repoPath : repoPath.substr(prefix.size());
        if (basename(relativePath) == ".gitkeep")
            continue;

        files.push_back({relativePath, rawGitHubFileUrl(repo, repoPath)});
    }

    return files;
}

void listThemeFilesFromContentsRecursive(const GitHubRepoSource& repo,
                                         const std::string& rootPath,
                                         const std::string& currentPath,
                                         std::vector<RemoteFile>& files) {
    const std::string apiUrl = "https://api.github.com/repos/" + repo.owner + "/" + repo.repo
        + "/contents/" + urlEncodePath(currentPath) + "?ref=" + urlEncodeComponent(repo.ref);
    const std::string body = themeshop::http::getText(apiUrl, {
        "Accept: application/vnd.github+json",
        "X-GitHub-Api-Version: 2022-11-28"
    });

    nlohmann::json entries;
    try {
        entries = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid GitHub contents JSON: ") + ex.what());
    }
    if (!entries.is_array())
        throw std::runtime_error("GitHub contents response is not a directory listing");

    const std::string cleanRoot = trimSlashes(rootPath);
    const std::string prefix = cleanRoot.empty() ? std::string() : cleanRoot + "/";
    for (const auto& item : entries) {
        if (!item.is_object())
            continue;

        auto typeIt = item.find("type");
        auto pathIt = item.find("path");
        if (typeIt == item.end() || pathIt == item.end() || !typeIt->is_string() || !pathIt->is_string())
            continue;

        const std::string type = typeIt->get<std::string>();
        const std::string repoPath = trimSlashes(pathIt->get<std::string>());
        if (type == "dir") {
            listThemeFilesFromContentsRecursive(repo, rootPath, repoPath, files);
            continue;
        }
        if (type != "file")
            continue;
        if (!prefix.empty() && !startsWith(repoPath, prefix))
            continue;

        std::string relativePath = prefix.empty() ? repoPath : repoPath.substr(prefix.size());
        if (basename(relativePath) == ".gitkeep")
            continue;

        auto downloadIt = item.find("download_url");
        std::string downloadUrl = (downloadIt != item.end() && downloadIt->is_string())
            ? downloadIt->get<std::string>()
            : rawGitHubFileUrl(repo, repoPath);
        files.push_back({relativePath, downloadUrl});
    }
}

std::vector<RemoteFile> listThemeFilesFromContents(const GitHubRepoSource& repo,
                                                   const std::string& themePath) {
    std::vector<RemoteFile> files;

    listThemeFilesFromContentsRecursive(repo, trimSlashes(themePath), trimSlashes(themePath), files);
    return files;
}

void collectManifestAssets(const nlohmann::json& root, std::vector<std::string>& paths) {
    auto looksLikeRelativeAssetPath = [](const std::string& value, const std::string& key) {
        std::string path = trimSlashes(value);
        if (path.empty())
            return false;
        if (startsWith(path, "https://") || startsWith(path, "http://")
            || startsWith(path, "data:") || startsWith(path, "#"))
            return false;
        if (hasParentTraversal(path))
            return false;

        const bool hasAssetExtension = hasKnownAssetExtension(path);
        const bool hasDirectory = path.find('/') != std::string::npos;
        if (hasAssetExtension)
            return true;
        if (!hasDirectory)
            return false;

        static constexpr const char* kPathKeys[] = {
            "path", "file", "src", "image", "font", "regular", "normal", "ui",
            "small", "secondary", "caption", "cover", "screenshot", "background"
        };
        return std::any_of(std::begin(kPathKeys), std::end(kPathKeys), [&](const char* pathKey) {
            return key == pathKey;
        });
    };

    std::function<void(const nlohmann::json&, const std::string&)> collectRecursive =
        [&](const nlohmann::json& value, const std::string& key) {
            if (value.is_string()) {
                std::string path = value.get<std::string>();
                if (looksLikeRelativeAssetPath(path, key))
                    appendUniquePath(paths, std::move(path));
                return;
            }

            if (value.is_array()) {
                for (const auto& item : value)
                    collectRecursive(item, key);
                return;
            }

            if (!value.is_object())
                return;

            for (auto it = value.begin(); it != value.end(); ++it)
                collectRecursive(it.value(), it.key());
        };

    collectRecursive(root, {});

    auto addStringMember = [&](const nlohmann::json& object, const char* key) {
        auto it = object.find(key);
        if (it != object.end() && it->is_string())
            appendUniquePath(paths, it->get<std::string>());
    };

    addStringMember(root, "cover");
    addStringMember(root, "screenshot");
    auto screenshotsIt = root.find("screenshots");
    if (screenshotsIt != root.end() && screenshotsIt->is_array()) {
        for (const auto& screenshot : *screenshotsIt) {
            if (screenshot.is_string())
                appendUniquePath(paths, screenshot.get<std::string>());
        }
    }

    auto previewIt = root.find("preview");
    if (previewIt != root.end() && previewIt->is_object()) {
        addStringMember(*previewIt, "cover");
        addStringMember(*previewIt, "screenshot");
        auto previewScreenshotsIt = previewIt->find("screenshots");
        if (previewScreenshotsIt != previewIt->end() && previewScreenshotsIt->is_array()) {
            for (const auto& screenshot : *previewScreenshotsIt) {
                if (screenshot.is_string())
                    appendUniquePath(paths, screenshot.get<std::string>());
            }
        }
    }

    auto themeIt = root.find("theme");
    if (themeIt == root.end() || !themeIt->is_object())
        return;

    auto backgroundIt = themeIt->find("background");
    if (backgroundIt == themeIt->end() || !backgroundIt->is_object())
        return;

    auto imageIt = backgroundIt->find("image");
    if (imageIt == backgroundIt->end())
        return;
    if (imageIt->is_string()) {
        appendUniquePath(paths, imageIt->get<std::string>());
    } else if (imageIt->is_object()) {
        addStringMember(*imageIt, "path");
        addStringMember(*imageIt, "file");
        addStringMember(*imageIt, "src");
    }
}

std::vector<RemoteFile> listThemeFiles(const std::string& catalogUrl,
                                       const std::string& themePath,
                                       const std::string& manifestPath,
                                       const ThemeCatalogClient::Entry& entry) {
    std::vector<RemoteFile> files;

    GitHubRepoSource repo;
    if (parseRawGitHubUrl(catalogUrl, repo)) {
        try {
            files = listThemeFilesFromTree(repo, themePath);
        } catch (const std::exception& ex) {
            DebugLog::log("[themeshop] tree listing failed for %s: %s", themePath.c_str(), ex.what());
        }
        if (files.empty()) {
            try {
                files = listThemeFilesFromContents(repo, themePath);
            } catch (const std::exception& ex) {
                DebugLog::log("[themeshop] contents listing failed for %s: %s", themePath.c_str(), ex.what());
            }
        }
    }

    auto hasRelativePath = [&](const std::string& relativePath) {
        return std::any_of(files.begin(), files.end(), [&](const RemoteFile& file) {
            return file.relativePath == relativePath;
        });
    };
    auto addRelativeFile = [&](const std::string& relativePath, const std::string& downloadUrl) {
        std::string clean = trimSlashes(relativePath);
        if (clean.empty() || hasRelativePath(clean))
            return;
        files.push_back({clean, downloadUrl});
    };

    const bool usingFallbackListing = files.empty();
    std::string manifestText;
    std::string manifestRelativePath;
    std::string manifestUrl;
    if (!manifestPath.empty()) {
        manifestRelativePath = basename(manifestPath);
        if (manifestRelativePath.empty())
            manifestRelativePath = "theme.json";
        manifestUrl = joinUrl(catalogUrl, manifestPath);
        if (usingFallbackListing)
            addRelativeFile(manifestRelativePath, manifestUrl);
    }

    if (usingFallbackListing && !manifestUrl.empty()) {
        try {
            manifestText = themeshop::http::getText(manifestUrl);
            nlohmann::json manifest = nlohmann::json::parse(manifestText);
            std::vector<std::string> manifestAssets;
            collectManifestAssets(manifest, manifestAssets);
            for (const auto& assetPath : manifestAssets) {
                std::string remotePath = assetPath;
                if (!themePath.empty())
                    remotePath = joinPath(themePath, assetPath);
                addRelativeFile(assetPath, joinUrl(catalogUrl, remotePath));
            }
        } catch (const std::exception& ex) {
            DebugLog::log("[themeshop] manifest asset fallback failed for %s: %s",
                          manifestPath.c_str(),
                          ex.what());
        }
    }

    auto addPreviewFile = [&](const std::string& previewPath) {
        if (previewPath.empty())
            return;

        std::string clean = trimSlashes(previewPath);
        std::string relativePath;
        std::string downloadUrl;
        if (startsWith(clean, "https://") || startsWith(clean, "http://")) {
            relativePath = dirnameForPreview(clean);
            downloadUrl = clean;
        } else {
            std::string prefix = themePath.empty() ? std::string() : trimSlashes(themePath) + "/";
            std::string remotePath = clean;
            if (!themePath.empty() && !startsWith(clean, prefix))
                remotePath = joinPath(themePath, clean);
            relativePath = (!prefix.empty() && startsWith(clean, prefix))
                ? clean.substr(prefix.size())
                : clean;
            downloadUrl = joinUrl(catalogUrl, remotePath);
        }

        addRelativeFile(relativePath, downloadUrl);
    };

    addPreviewFile(entry.cover);
    for (const auto& screenshot : entry.screenshots)
        addPreviewFile(screenshot);

    std::sort(files.begin(), files.end(), [](const RemoteFile& lhs, const RemoteFile& rhs) {
        bool lhsManifest = lhs.relativePath == "theme.json";
        bool rhsManifest = rhs.relativePath == "theme.json";
        if (lhsManifest != rhsManifest)
            return lhsManifest;
        return lhs.relativePath < rhs.relativePath;
    });

    return files;
}

void writeFileBinary(const std::string& path, const std::string& data) {
    std::string parent = parentDirectory(path);
    if (!parent.empty() && !ensureDirectoryRecursive(parent))
        throw std::runtime_error("Failed to create directory: " + parent);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Failed to open file for writing: " + path);

    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!output)
        throw std::runtime_error("Failed to write file: " + path);
}

// A package can be a valid ZIP and still be an unusable animated theme when a
// proxy/CDN serves an old, preview-only archive. Validate the manifest against
// the staging directory before replacing the working installation; otherwise
// selecting a theme can silently remove the wallpaper that was active before.
bool hasAllDeclaredFrames(const std::string& root, std::string& detail) {
    const std::string manifestPath = joinPath(root, "theme.json");
    std::ifstream input(manifestPath);
    if (!input) {
        detail = "theme.json missing after extraction";
        return false;
    }

    try {
        nlohmann::json manifest;
        input >> manifest;

        // Video themes specify a video background rather than frame images
        const auto bg = manifest.value("theme", nlohmann::json::object())
                                .value("background", nlohmann::json::object());
        if (bg.contains("video") && bg["video"].is_string()) {
            const std::string videoRel = trimSlashes(bg["video"].get<std::string>());
            if (videoRel.empty() || !isSafeRelativePath(videoRel) || !pathExists(joinPath(root, videoRel))) {
                detail = "background video missing: " + videoRel;
                return false;
            }
            return true;
        }

        const auto image = bg.value("image", nlohmann::json::object());
        const auto frames = image.value("frames", nlohmann::json::array());
        if (!frames.is_array() || frames.empty())
            return true;

        std::size_t found = 0;
        for (const auto& entry : frames) {
            if (!entry.is_string()) {
                detail = "background frame list contains a non-string entry";
                return false;
            }
            const std::string relative = trimSlashes(entry.get<std::string>());
            if (relative.empty() || !isSafeRelativePath(relative)) {
                detail = "background frame list contains an unsafe path";
                return false;
            }
            if (pathExists(joinPath(root, relative)))
                ++found;
        }
        if (found != frames.size()) {
            detail = "background frames missing: " + std::to_string(found)
                   + "/" + std::to_string(frames.size());
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        detail = std::string("invalid extracted manifest: ") + error.what();
        return false;
    }
}

} // namespace

std::string ThemePackageInstaller::destinationRootFor(const std::string& themeId, Mode mode) {
    (void)mode;
    return "sdmc:/config/OmniLaunch/themes/" + themeId;
}

ThemePackageInstaller::Result ThemePackageInstaller::run(const std::string& catalogUrl,
                                                         const ThemeCatalogClient::Entry& entry,
                                                         Mode mode,
                                                         ProgressCallback onProgress) {
    if (!isSafeThemeId(entry.id))
        throw std::runtime_error("Catalog entry has an unsafe theme id");
    if ((!entry.path.empty() && !isSafeRelativePath(trimSlashes(entry.path)))
        || (!entry.manifest.empty() && !isSafeRelativePath(trimSlashes(entry.manifest)))
        || (!entry.package.empty() && !isSafeRelativePath(trimSlashes(entry.package))))
        throw std::runtime_error("Catalog entry has an unsafe package path");

    Result result;
    result.themeId = entry.id;
    result.installed = true;
    result.destinationPath = destinationRootFor(entry.id, mode);

    const std::string themePath = trimSlashes(!entry.path.empty() ? entry.path : parentDirectory(entry.manifest));
    const std::string manifestPath = trimSlashes(!entry.manifest.empty() ? entry.manifest : joinPath(themePath, "theme.json"));
    if (manifestPath.empty())
        throw std::runtime_error("Catalog entry is missing a manifest path");

    if (onProgress) {
        auto& i18n = nxui::I18n::instance();
        onProgress(mode == Mode::InstallAndApply
                       ? i18n.tr("themeshop.transfer.prepare_apply", "Preparing download + apply...")
                       : i18n.tr("themeshop.transfer.prepare_install", "Preparing download + install..."),
                   0.f);
    }

    // One request instead of one per file. An animated theme is around three
    // hundred frames, so the file-by-file path means three hundred connections:
    // slow, and every one of them a chance to fail halfway and leave a theme
    // that is missing frames without anything saying so.
    //
    // Falls through to the old path when the catalogue does not offer a
    // package, which is every catalogue published before this field existed.
    if (!entry.package.empty()) {
        const std::string stagingPath = result.destinationPath + ".installing";
        removeDirectoryRecursive(stagingPath);
        if (!ensureDirectoryRecursive(stagingPath))
            throw std::runtime_error("Failed to prepare installation staging area: " + stagingPath);

        const std::string packageUrl = joinUrl(catalogUrl, trimSlashes(entry.package));
        auto& i18n = nxui::I18n::instance();
        if (onProgress)
            onProgress(i18n.tr("themeshop.transfer.downloading_package", "Downloading theme"), 0.05f);

        // Straight to disk, never through memory. Held as bytes, a 40 MB
        // package peaked past 150 MB once curl's buffer and its copies were
        // counted, and the download died with "Failed writing body".
        const std::string archivePath = stagingPath + ".part";
        DebugLog::log("[themeshop] package download: %s", packageUrl.c_str());

        std::uint64_t downloaded = 0;
        const std::uint64_t expectedBytes = entry.packageBytes;
        try {
            downloaded = themeshop::http::getToFile(
                packageUrl, archivePath,
                [&](std::uint64_t received, std::uint64_t) {
                    if (onProgress && (received % (2u << 20)) < 16384) {
                        onProgress(i18n.tr("themeshop.transfer.downloading_package", "Downloading theme")
                                       + " (" + std::to_string(received / 1048576) + " MB)",
                                   expectedBytes > 0
                                       ? 0.05f + 0.5f * (float)std::min(received, expectedBytes)
                                                   / (float)expectedBytes
                                       // Legacy catalogues do not declare a size. Keep their
                                       // meter below extraction rather than jumping backward.
                                       : std::min(0.54f, 0.05f + 0.5f * (float)received / 52428800.f));
                    }
                });
        } catch (...) {
            std::remove(archivePath.c_str());
            throw;
        }
        if (downloaded == 0) {
            std::remove(archivePath.c_str());
            throw std::runtime_error("Theme package download was empty");
        }
        if (expectedBytes > 0 && downloaded != expectedBytes) {
            std::remove(archivePath.c_str());
            DebugLog::log("[themeshop] package size mismatch: expected=%llu received=%llu",
                          (unsigned long long)expectedBytes,
                          (unsigned long long)downloaded);
            throw std::runtime_error(i18n.tr("themeshop.transfer.unpack_failed",
                                             "Theme package could not be unpacked."));
        }

        // Antes de escrever coisa alguma. Um cartao cheio deixava a extracao ir
        // ate o fim do espaco e falhar no primeiro arquivo que nao coubesse --
        // que foi o theme.mp3, o ultimo da ordem -- e o relato saia como
        // "could not unpack media/music/theme.mp3", que manda quem investiga
        // procurar defeito no audio, no pacote e no formato antes do disco.
        //
        // O quanto o pacote ocupa depois de aberto nao esta escrito nele, entao
        // e estimado: os quadros comprimem cerca de 2.2x no zip, e 2.5 mais uma
        // margem cobre isso sem recusar instalacao que caberia.
        {
            const std::uint64_t livre = sdFreeBytes();
            const std::uint64_t preciso = downloaded * 5 / 2 + 32ull * 1024ull * 1024ull;
            if (livre > 0 && livre < preciso) {
                std::remove(archivePath.c_str());
                removeDirectoryRecursive(stagingPath);
                DebugLog::log("[themeshop] espaco insuficiente: %llu MB livres, "
                              "estimados %llu MB para abrir o pacote",
                              (unsigned long long)(livre / 1048576),
                              (unsigned long long)(preciso / 1048576));
                throw std::runtime_error(
                    i18n.tr("themeshop.transfer.no_space",
                            "Not enough space on the SD card for this theme")
                    + " (" + std::to_string(livre / 1048576) + " MB / "
                    + std::to_string(preciso / 1048576) + " MB)");
            }
        }

        const bool isDirectVideo = (archivePath.size() > 4 &&
            (entry.package.rfind(".mp4") == entry.package.size() - 4 ||
             entry.package.rfind(".mkv") == entry.package.size() - 4 ||
             entry.package.rfind(".webm") == entry.package.size() - 5));

        if (isDirectVideo) {
            if (onProgress)
                onProgress(i18n.tr("themeshop.transfer.finishing", "Finishing"), 0.85f);

            ensureDirectoryRecursive(stagingPath + "/media");
            std::rename(archivePath.c_str(), (stagingPath + "/media/video.mp4").c_str());

            nlohmann::json manifest;
            manifest["id"] = entry.id;
            manifest["name"] = entry.name.empty() ? entry.id : entry.name;
            manifest["author"] = entry.author.empty() ? "OmniLaunch" : entry.author;
            manifest["version"] = entry.version.empty() ? "1.0.0" : entry.version;
            manifest["theme"]["mode"] = "dark";
            manifest["theme"]["background"]["video"] = "media/video.mp4";
            manifest["theme"]["background"]["count"] = 1;
            manifest["theme"]["background"]["opacity"] = 0.0;
            manifest["theme"]["audio"]["preset"] = "wiiu";

            std::ofstream mOut(stagingPath + "/theme.json");
            mOut << manifest.dump(2);
            mOut.close();

            if (!entry.cover.empty()) {
                ensureDirectoryRecursive(stagingPath + "/media/screenshots");
                try {
                    themeshop::http::getToFile(joinUrl(catalogUrl, entry.cover),
                                               stagingPath + "/media/screenshots/00.jpg");
                } catch (...) {}
            }
        } else {
            if (onProgress)
                onProgress(i18n.tr("themeshop.transfer.extracting", "Extracting theme"), 0.55f);

            auto extracted = themeshop::extractZipFile(
                archivePath, stagingPath,
                [&](int done, int total) {
                    if (onProgress && total > 0 && (done % 32 == 0 || done == total)) {
                        onProgress(i18n.tr("themeshop.transfer.extracting", "Extracting theme"),
                                   0.55f + 0.4f * (float)done / (float)total);
                    }
                });

            std::remove(archivePath.c_str());   // o pacote nao fica no cartao

            if (!extracted.success) {
                // The temporary directory is disposable; keep the known-good theme.
                removeDirectoryRecursive(stagingPath);
                DebugLog::log("[themeshop] extracao falhou (%s), %llu MB livres no cartao",
                              extracted.error.c_str(),
                              (unsigned long long)(sdFreeBytes() / 1048576));
                throw std::runtime_error(i18n.tr("themeshop.transfer.unpack_failed",
                                                 "Theme package could not be unpacked."));
            }

            std::string packageValidationError;
            if (!hasAllDeclaredFrames(stagingPath, packageValidationError)) {
                removeDirectoryRecursive(stagingPath);
                DebugLog::log("[themeshop] package rejected before replacement: %s",
                              packageValidationError.c_str());
                throw std::runtime_error(i18n.tr("themeshop.transfer.unpack_failed",
                                                 "Theme package could not be unpacked."));
            }
        }

        std::string replaceError;
        if (!replaceDirectoryFromStaging(result.destinationPath, stagingPath, replaceError)) {
            removeDirectoryRecursive(stagingPath);
            DebugLog::log("[themeshop] installation replacement failed: %s", replaceError.c_str());
            throw std::runtime_error(i18n.tr("themeshop.transfer.replace_failed",
                                             "Theme package could not be installed."));
        }

        if (onProgress)
            onProgress(i18n.tr("themeshop.transfer.finishing", "Finishing"), 1.f);
        result.success = true;
        return result;
    }

    if (pathExists(result.destinationPath))
        removeDirectoryRecursive(result.destinationPath);
    if (!ensureDirectoryRecursive(result.destinationPath))
        throw std::runtime_error("Failed to prepare destination: " + result.destinationPath);

    std::vector<RemoteFile> files = listThemeFiles(catalogUrl, themePath, manifestPath, entry);
    if (files.empty())
        throw std::runtime_error("Theme package contains no downloadable files");

    DebugLog::log("[themeshop] package file list: id=%s count=%zu root=%s",
                  entry.id.c_str(),
                  files.size(),
                  themePath.c_str());

    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        float progress = files.size() > 1 ? (float)i / (float)files.size() : 0.f;
        if (onProgress) {
            auto& i18n = nxui::I18n::instance();
            onProgress(i18n.tr("themeshop.transfer.downloading_file", "Downloading")
                           + " " + file.relativePath + " (" + std::to_string(i + 1)
                           + "/" + std::to_string(files.size()) + ")",
                       progress);
        }

        const std::string body = themeshop::http::getText(file.downloadUrl);
        writeFileBinary(joinPath(result.destinationPath, file.relativePath), body);
    }

    if (onProgress) {
        auto& i18n = nxui::I18n::instance();
        onProgress(mode == Mode::InstallAndApply
                       ? i18n.tr("themeshop.transfer.finalize_apply", "Finalizing install before apply...")
                       : i18n.tr("themeshop.transfer.finalize_install", "Finalizing installation..."),
                   1.f);
    }

    result.success = true;
    auto& i18n = nxui::I18n::instance();
    result.message = mode == Mode::InstallAndApply
        ? i18n.tr("themeshop.transfer.installed_applying", "Theme installed. Applying it now...")
        : i18n.tr("themeshop.transfer.installed", "Theme installed.");
    return result;
}
