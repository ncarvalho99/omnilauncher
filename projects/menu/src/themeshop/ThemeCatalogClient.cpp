#include "ThemeCatalogClient.hpp"

#include "ThemeHttp.hpp"

#include "core/DebugLog.hpp"

#include <nxui/core/I18n.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <curlpp/Easy.hpp>
#include <curlpp/Exception.hpp>
#include <curlpp/Infos.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <nlohmann/json.hpp>
#include <switch.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>

namespace {

void readStringOpt(const nlohmann::json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return;
    out = it->get<std::string>();
}

void appendUniqueString(std::vector<std::string>& out, std::string value) {
    if (value.empty())
        return;
    if (std::find(out.begin(), out.end(), value) != out.end())
        return;
    out.push_back(std::move(value));
}

void readStringArrayOpt(const nlohmann::json& j, const char* key, std::vector<std::string>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& item : *it) {
        if (item.is_string())
            appendUniqueString(out, item.get<std::string>());
    }
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool safeThemeId(const std::string& value) {
    if (value.size() < 2 || value.size() > 64)
        return false;
    for (unsigned char ch : value) {
        if (!std::islower(ch) && !std::isdigit(ch) && ch != '_' && ch != '-')
            return false;
    }
    return true;
}

std::string trimSlashes(std::string value) {
    while (!value.empty() && value.front() == '/')
        value.erase(value.begin());
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string parentDirectory(const std::string& path) {
    std::string clean = trimSlashes(path);
    std::size_t slash = clean.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return clean.substr(0, slash);
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

std::string deriveManifestPath(const std::string& path) {
    if (path.empty())
        return {};
    return path + "/theme.json";
}

std::string resolvePreviewPath(const ThemeCatalogClient::Entry& entry, std::string previewPath) {
    previewPath = trimSlashes(std::move(previewPath));
    if (previewPath.empty())
        return {};
    if (startsWith(previewPath, "https://") || startsWith(previewPath, "http://"))
        return previewPath;

    if (!entry.path.empty())
        return joinPath(trimSlashes(entry.path), previewPath);

    return joinPath(parentDirectory(entry.manifest), previewPath);
}

void appendResolvedPreviewPath(const ThemeCatalogClient::Entry& entry,
                               std::vector<std::string>& out,
                               const std::string& previewPath) {
    appendUniqueString(out, resolvePreviewPath(entry, previewPath));
}

void readResolvedStringArrayOpt(const nlohmann::json& j,
                                const char* key,
                                const ThemeCatalogClient::Entry& entry,
                                std::vector<std::string>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& item : *it) {
        if (item.is_string())
            appendResolvedPreviewPath(entry, out, item.get<std::string>());
    }
}

void ensureCoverFirst(ThemeCatalogClient::Entry& entry) {
    if (entry.cover.empty()) {
        if (!entry.screenshots.empty())
            entry.cover = entry.screenshots.front();
        return;
    }

    auto it = std::find(entry.screenshots.begin(), entry.screenshots.end(), entry.cover);
    if (it == entry.screenshots.end()) {
        entry.screenshots.insert(entry.screenshots.begin(), entry.cover);
        return;
    }

    if (it != entry.screenshots.begin()) {
        std::string cover = *it;
        entry.screenshots.erase(it);
        entry.screenshots.insert(entry.screenshots.begin(), std::move(cover));
    }
}

struct PreviewData {
    std::string cover;
    std::vector<std::string> screenshots;
};

std::string absoluteFromCatalog(const std::string& catalogUrl, const std::string& relative) {
    if (relative.empty()) return relative;
    if (relative.rfind("http://", 0) == 0 || relative.rfind("https://", 0) == 0)
        return relative;
    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos) return relative;
    return catalogUrl.substr(0, slash + 1) + trimSlashes(relative);
}

PreviewData derivePreviewDataFromManifest(const std::string& catalogUrl,
                                          const ThemeCatalogClient::Entry& entry) {
    if (entry.manifest.empty())
        return {};

    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos)
        return {};

    const std::string manifestUrl = catalogUrl.substr(0, slash + 1) + trimSlashes(entry.manifest);
    const std::string manifestText = themeshop::http::getText(manifestUrl);

    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(manifestText);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid theme manifest JSON: ") + ex.what());
    }

    PreviewData data;
    std::string cover;
    readStringOpt(manifest, "cover", cover);
    if (!cover.empty())
        data.cover = resolvePreviewPath(entry, cover);

    appendUniqueString(data.screenshots, data.cover);

    std::string screenshot;
    readStringOpt(manifest, "screenshot", screenshot);
    appendResolvedPreviewPath(entry, data.screenshots, screenshot);
    readResolvedStringArrayOpt(manifest, "screenshots", entry, data.screenshots);

    auto previewIt = manifest.find("preview");
    if (previewIt == manifest.end() || !previewIt->is_object()) {
        if (data.cover.empty() && !data.screenshots.empty())
            data.cover = data.screenshots.front();
        return data;
    }

    cover.clear();
    readStringOpt(*previewIt, "cover", cover);
    if (!cover.empty() && data.cover.empty())
        data.cover = resolvePreviewPath(entry, cover);
    appendResolvedPreviewPath(entry, data.screenshots, cover);

    auto screenshotsIt = previewIt->find("screenshots");
    if (screenshotsIt != previewIt->end() && screenshotsIt->is_array()) {
        for (const auto& screenshot : *screenshotsIt) {
            if (screenshot.is_string())
                appendResolvedPreviewPath(entry, data.screenshots, screenshot.get<std::string>());
        }
    }

    auto screenshotIt = previewIt->find("screenshot");
    if (screenshotIt != previewIt->end() && screenshotIt->is_string())
        appendResolvedPreviewPath(entry, data.screenshots, screenshotIt->get<std::string>());

    if (data.cover.empty() && !data.screenshots.empty())
        data.cover = data.screenshots.front();

    return data;
}

} // namespace

ThemeCatalogClient::ThemeCatalogClient(std::string catalogUrl) {
    m_catalogUrls.push_back(std::move(catalogUrl));
    if (m_catalogUrls.front() == kDefaultCatalogUrl) {
        m_catalogUrls.emplace_back(kFallbackCatalogUrl);
    }
    m_catalogUrls.emplace_back(kUpstreamCatalogUrl);
}

void ThemeCatalogClient::setCatalogUrl(std::string catalogUrl) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_catalogUrls.clear();
    m_catalogUrls.push_back(std::move(catalogUrl));
}

void ThemeCatalogClient::setCatalogUrls(std::vector<std::string> urls) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_catalogUrls = std::move(urls);
}

void ThemeCatalogClient::refresh(nxui::ThreadPool& pool) {
    if (m_future.valid() && m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    if (m_future.valid())
        m_future.get();

    std::string url;
    std::vector<std::string> urls;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        urls = m_catalogUrls;
        url = urls.empty() ? std::string() : urls.front();
        // Traduzida na origem: esta mensagem e escrita no estado da
        // transferencia e desenhada tal e qual, sem passar por i18n depois.
        m_loading.begin(nxui::I18n::instance().tr("themeshop.community.loading",
                                                  "Loading theme catalog..."));
        ++m_revision;
    }

    DebugLog::log("[themeshop] catalog refresh start: %d source(s)", (int)urls.size());

    m_future = pool.submit([this, urls]() {
        Snapshot loaded;
        std::string lastError;
        int okSources = 0;
        // One unreachable catalogue must not empty the shop: whatever the other
        // returns is still worth showing, and only a total failure is an error.
        for (const auto& source : urls) {
            try {
                Snapshot part = loadCatalog(source);
                ++okSources;
                for (auto& entry : part.entries) {
                    const bool dup = std::any_of(loaded.entries.begin(), loaded.entries.end(),
                                                 [&](const Entry& e) { return e.id == entry.id; });
                    if (!dup)
                        loaded.entries.push_back(std::move(entry));
                }
                DebugLog::log("[themeshop] catalog %s: %d entries",
                              source.c_str(), (int)part.entries.size());
            } catch (const std::exception& ex) {
                lastError = ex.what();
                DebugLog::log("[themeshop] catalog %s failed: %s", source.c_str(), ex.what());
            } catch (...) {
                lastError = "Unknown network error";
                DebugLog::log("[themeshop] catalog %s failed: unknown error", source.c_str());
            }
        }

        try {
            if (okSources == 0) {
                // Mensagem para quem le, nao o texto da excecao. Ela ecoava o erro
                // cru -- em ingles e citando a URL do catalogo que falhou por
                // ultimo, que podia ser o da outra aba. O detalhe continua no log.
                throw std::runtime_error(nxui::I18n::instance().tr(
                    "themeshop.community.offline",
                    "No connection. Check the internet and try again."));
            }
            loaded.loading.succeed();
            DebugLog::log("[themeshop] catalog refresh done: %d entries from %d source(s)",
                          (int)loaded.entries.size(), okSources);
        } catch (const std::exception& ex) {
            loaded.loading.fail(ex.what());
            DebugLog::log("[themeshop] catalog refresh failed: %s", ex.what());
        } catch (...) {
            loaded.loading.fail("Unknown network error");
            DebugLog::log("[themeshop] catalog refresh failed: unknown error");
        }

        std::lock_guard<std::mutex> lk(m_mutex);
        if (loaded.loading.isReady())
            m_entries = std::move(loaded.entries);
        m_loading = loaded.loading;
        ++m_revision;
    });
}

bool ThemeCatalogClient::poll() {
    if (!m_future.valid())
        return false;
    if (m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;

    m_future.get();
    return true;
}

ThemeCatalogClient::Snapshot ThemeCatalogClient::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);

    Snapshot snapshot;
    snapshot.loading = m_loading;
    snapshot.entries = m_entries;
    snapshot.revision = m_revision;
    return snapshot;
}

ThemeCatalogClient::Snapshot ThemeCatalogClient::loadCatalog(const std::string& url) {
    Snapshot snapshot;

    const std::string body = themeshop::http::getText(url);

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Invalid catalog JSON: ") + ex.what());
    }

    auto themesIt = root.find("themes");
    if (themesIt == root.end() || !themesIt->is_array())
        throw std::runtime_error("Catalog is missing the themes array");

    snapshot.entries.reserve(themesIt->size());
    bool previewFallbackDisabled = false;
    for (const auto& item : *themesIt) {
        if (!item.is_object())
            continue;

        Entry entry;
        readStringOpt(item, "id", entry.id);
        readStringOpt(item, "name", entry.name);
        readStringOpt(item, "author", entry.author);
        readStringOpt(item, "version", entry.version);
        readStringOpt(item, "path", entry.path);
        readStringOpt(item, "manifest", entry.manifest);
        readStringOpt(item, "cover", entry.cover);
        readStringOpt(item, "package", entry.package);
        if (auto it = item.find("packageBytes"); it != item.end() && it->is_number_unsigned())
            entry.packageBytes = it->get<std::uint64_t>();
        if (auto it = item.find("installedBytes"); it != item.end() && it->is_number_unsigned())
            entry.installedBytes = it->get<std::uint64_t>();

        // For OmniLaunch, prefer hardware-accelerated MP4 video theme packages when available:
        // The video's size fields replace the ZIP's; when the catalogue omits
        // them the size is unknown (0 = not verified) rather than the ZIP's,
        // which would make a valid video fail the download size check. An
        // empty packageVideo never overwrites a usable package.
        if (auto it = item.find("packageVideo");
            it != item.end() && it->is_string() && !it->get<std::string>().empty()) {
            entry.package = it->get<std::string>();
            entry.packageBytes = 0;
            entry.installedBytes = 0;
            if (auto pb = item.find("packageVideoBytes"); pb != item.end() && pb->is_number_unsigned())
                entry.packageBytes = pb->get<std::uint64_t>();
            if (auto ib = item.find("installedVideoBytes"); ib != item.end() && ib->is_number_unsigned())
                entry.installedBytes = ib->get<std::uint64_t>();
        }
        readStringOpt(item, "thumbSheet", entry.thumbSheet);
        readStringOpt(item, "thumbSheetHd", entry.thumbSheetHd);
        {
            auto readInt = [&](const char* key, int& out) {
                auto it = item.find(key);
                if (it != item.end() && it->is_number_integer()) out = it->get<int>();
            };
            readInt("thumbCols", entry.thumbCols);
            readInt("thumbRows", entry.thumbRows);
            auto fpsIt = item.find("thumbFps");
            if (fpsIt != item.end() && fpsIt->is_number())
                entry.thumbFps = fpsIt->get<float>();
        }
        // Uma folha sem grade valida nao anima; melhor cair na capa parada do
        // que percorrer recortes inventados.
        if (entry.thumbCols < 1 || entry.thumbRows < 1)
            entry.thumbSheet.clear();
        readStringArrayOpt(item, "screenshots", entry.screenshots);
        entry.catalogUrl = url;

        std::string screenshot;
        readStringOpt(item, "screenshot", screenshot);
        appendUniqueString(entry.screenshots, screenshot);

        // The id eventually becomes a directory name on the SD card. Do not
        // retain an entry that could alter that destination.
        if (!safeThemeId(entry.id) || entry.name.empty())
            continue;
        if (entry.manifest.empty())
            entry.manifest = deriveManifestPath(entry.path);
        // Only when there is no cover at all.
        //
        // This used to fire when either the cover or the screenshots were
        // missing, and a catalogue that lists covers but not screenshots -- ours
        // does -- made it fetch all 56 manifests on every refresh. With the
        // network down that became 112 failing requests, each tearing down and
        // rebuilding the HTTP stack, on the thread drawing the menu. It looked
        // like the console had frozen applying a theme.
        //
        // Extra screenshots are a nicety. A missing cover is the only thing
        // worth a round trip, because without it the entry has nothing to show.
        if (entry.cover.empty() && !entry.manifest.empty() && !previewFallbackDisabled) {
            try {
                PreviewData preview = derivePreviewDataFromManifest(url, entry);
                if (entry.cover.empty())
                    entry.cover = preview.cover;
                for (auto& screenshotPath : preview.screenshots)
                    appendUniqueString(entry.screenshots, std::move(screenshotPath));
            } catch (const std::exception& ex) {
                // One failure stops the rest. If the network is down for this
                // entry it is down for the other fifty-five, and retrying each
                // of them is how a missing connection turned into a frozen menu.
                previewFallbackDisabled = true;
                DebugLog::log("[themeshop] manifest preview fallback failed for %s: %s "
                              "(skipping the rest)",
                              entry.id.c_str(),
                              ex.what());
            }
        }

        ensureCoverFirst(entry);

        // Preview paths are relative to their own index. Resolving them here
        // means every consumer downstream can stay ignorant of which catalogue
        // an entry came from — only the installer, which walks a whole theme
        // directory, still needs the origin.
        entry.cover = absoluteFromCatalog(url, entry.cover);
        for (auto& shot : entry.screenshots)
            shot = absoluteFromCatalog(url, shot);

        snapshot.entries.push_back(std::move(entry));
    }

    std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const Entry& lhs, const Entry& rhs) {
        if (lhs.name != rhs.name)
            return lhs.name < rhs.name;
        return lhs.id < rhs.id;
    });

    if (snapshot.entries.empty())
        snapshot.loading.succeed("Catalog is empty.");
    else
        snapshot.loading.succeed(std::to_string(snapshot.entries.size()) + " themes available.");

    return snapshot;
}
