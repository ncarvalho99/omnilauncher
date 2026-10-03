#include "YouTubeClient.hpp"

#include "ThemeHttp.hpp"
#include "core/DebugLog.hpp"

#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace {

std::string percentEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string normalizedComparison(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string formatBytes(std::uint64_t bytes) {
    if (bytes == 0)
        return "0 B";
    constexpr std::uint64_t kKiB = 1024;
    constexpr std::uint64_t kMiB = 1024 * 1024;
    constexpr std::uint64_t kGiB = 1024 * 1024 * 1024;
    char buffer[32];
    if (bytes >= kGiB) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / kGiB);
    } else if (bytes >= kMiB) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / kMiB);
    } else if (bytes >= kKiB) {
        std::snprintf(buffer, sizeof(buffer), "%.0f KB", static_cast<double>(bytes) / kKiB);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

} // namespace

YouTubeClient::YouTubeClient() {
    loadInstalledTracks();
}

YouTubeClient::~YouTubeClient() {
    cancelSearch();
}

std::string YouTubeClient::musicDirectory() {
    return "sdmc:/config/OmniLaunch/music";
}

std::string YouTubeClient::sanitizeFilename(const std::string& name) {
    std::string clean;
    clean.reserve(name.size());
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if ((uc >= 'a' && uc <= 'z') ||
            (uc >= 'A' && uc <= 'Z') ||
            (uc >= '0' && uc <= '9') ||
            uc == ' ' || uc == '-' || uc == '_' || uc == '.' ||
            uc == '(' || uc == ')' || uc == '[' || uc == ']' ||
            uc == '\'' || uc == ',') {
            clean.push_back(c);
        } else if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                   c == '"' || c == '<' || c == '>' || c == '|' || c == '\n' || c == '\r' || c == '\t') {
            clean.push_back('_');
        }
    }
    // Collapse multiple consecutive spaces and underscores
    std::string result;
    result.reserve(clean.size());
    for (char c : clean) {
        if (c == ' ' && !result.empty() && result.back() == ' ')
            continue;
        if (c == '_' && !result.empty() && result.back() == '_')
            continue;
        result.push_back(c);
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.' || result.back() == '_'))
        result.pop_back();
    while (!result.empty() && (result.front() == ' ' || result.front() == '.' || result.front() == '_'))
        result.erase(result.begin());
    if (result.empty())
        result = "track";
    if (result.size() > 80)
        result.resize(80);
    return result;
}

std::vector<YouTubeClient::TrackItem> YouTubeClient::tracksSnapshot() const {
    std::lock_guard<std::mutex> lk(m_tracksMutex);
    return m_tracks;
}

size_t YouTubeClient::trackCount() const {
    std::lock_guard<std::mutex> lk(m_tracksMutex);
    return m_tracks.size();
}

bool YouTubeClient::getTrack(size_t index, TrackItem& outTrack) const {
    std::lock_guard<std::mutex> lk(m_tracksMutex);
    if (index < m_tracks.size()) {
        outTrack = m_tracks[index];
        return true;
    }
    return false;
}

const YouTubeClient::TrackItem* YouTubeClient::trackAt(size_t index) const {
    std::lock_guard<std::mutex> lk(m_tracksMutex);
    if (index < m_tracks.size()) {
        return &m_tracks[index];
    }
    return nullptr;
}

void YouTubeClient::cancelSearch() {
    m_cancelRequested.store(true, std::memory_order_release);
}

void YouTubeClient::search(const std::string& query, SearchCallback cb) {
    if (m_isSearching.load(std::memory_order_acquire)) {
        if (cb) cb(false, "Already searching");
        return;
    }

    m_lastQuery = query;
    if (query.empty()) {
        loadInstalledTracks();
        if (cb) cb(true, "");
        return;
    }

    m_isSearching.store(true, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);

    auto task = [this, query, cb]() {
        DebugLog::log("[youtube] Starting search for: '%s'", query.c_str());
        std::vector<TrackItem> items;
        std::string err;

        // 1. Try backend service first (only if configured)
        if (!m_backendUrl.empty()) {
            try {
                items = searchBackend(query);
                DebugLog::log("[youtube] Backend returned %zu results", items.size());
            } catch (const std::exception& ex) {
                DebugLog::log("[youtube] Backend search failed: %s, falling back to InnerTube", ex.what());
            } catch (...) {
                DebugLog::log("[youtube] Backend search failed with unknown exception");
            }
        }

        // 2. Fall back to direct YouTube InnerTube search if backend returned nothing
        if (items.empty() && !m_cancelRequested.load(std::memory_order_acquire)) {
            try {
                items = searchInnerTube(query);
                DebugLog::log("[youtube] InnerTube returned %zu results", items.size());
            } catch (const std::exception& ex) {
                err = ex.what();
                DebugLog::log("[youtube] InnerTube search failed: %s", ex.what());
            } catch (...) {
                err = "Unknown search error";
            }
        }

        if (m_cancelRequested.load(std::memory_order_acquire)) {
            m_isSearching.store(false, std::memory_order_release);
            if (cb) cb(false, "Search cancelled");
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_tracksMutex);
            m_tracks = std::move(items);
        }

        refreshDownloadedStatus();

        m_isSearching.store(false, std::memory_order_release);
        if (cb) {
            cb(err.empty(), err);
        }
    };

    if (m_pool) {
        m_pool->submit(std::move(task));
    } else {
        std::thread(std::move(task)).detach();
    }
}

std::vector<YouTubeClient::TrackItem> YouTubeClient::searchBackend(const std::string& query) {
    std::string url = m_backendUrl + "/api/search?q=" + percentEncode(query) + "&limit=24";
    std::string resp = themeshop::http::getText(url);
    if (resp.empty())
        return {};

    auto root = nlohmann::json::parse(resp);
    if (!root.contains("items") || !root["items"].is_array())
        return {};

    std::vector<TrackItem> items;
    for (const auto& it : root["items"]) {
        TrackItem track;
        track.id = it.value("id", "");
        track.title = it.value("title", "");
        track.author = it.value("author", "");
        track.duration = it.value("duration", "0:00");
        track.thumbnailUrl = it.value("thumbnail", "");
        if (!track.id.empty() && !track.title.empty()) {
            items.push_back(std::move(track));
        }
    }
    return items;
}

std::vector<YouTubeClient::TrackItem> YouTubeClient::searchInnerTube(const std::string& query) {
    std::string url = "https://www.youtube.com/youtubei/v1/search";
    nlohmann::json payload;
    payload["context"]["client"]["clientName"] = "WEB";
    payload["context"]["client"]["clientVersion"] = "2.20240101.00.00";
    payload["context"]["client"]["hl"] = "en";
    payload["context"]["client"]["gl"] = "US";
    payload["query"] = query;

    std::string resp = themeshop::http::postJson(url, payload.dump());
    if (resp.empty())
        return {};

    auto root = nlohmann::json::parse(resp);
    std::vector<TrackItem> items;

    try {
        const auto& contents = root["contents"]["twoColumnSearchResultsRenderer"]["primaryContents"]["sectionListRenderer"]["contents"];
        for (const auto& section : contents) {
            if (!section.contains("itemSectionRenderer") || !section["itemSectionRenderer"].contains("contents"))
                continue;
            const auto& itemSection = section["itemSectionRenderer"]["contents"];
            for (const auto& item : itemSection) {
                if (!item.contains("videoRenderer"))
                    continue;
                const auto& vr = item["videoRenderer"];
                TrackItem track;
                track.id = vr.value("videoId", "");
                if (track.id.empty())
                    continue;

                if (vr.contains("title")) {
                    if (vr["title"].contains("runs") && vr["title"]["runs"].is_array() && !vr["title"]["runs"].empty()) {
                        track.title = vr["title"]["runs"][0].value("text", "");
                    } else {
                        track.title = vr["title"].value("simpleText", "");
                    }
                }

                if (vr.contains("ownerText") && vr["ownerText"].contains("runs") && !vr["ownerText"]["runs"].empty()) {
                    track.author = vr["ownerText"]["runs"][0].value("text", "");
                } else if (vr.contains("shortBylineText") && vr["shortBylineText"].contains("runs") && !vr["shortBylineText"]["runs"].empty()) {
                    track.author = vr["shortBylineText"]["runs"][0].value("text", "");
                }

                if (vr.contains("lengthText")) {
                    track.duration = vr["lengthText"].value("simpleText", "0:00");
                } else {
                    track.duration = "0:00";
                }

                if (vr.contains("thumbnail") && vr["thumbnail"].contains("thumbnails") && vr["thumbnail"]["thumbnails"].is_array() && !vr["thumbnail"]["thumbnails"].empty()) {
                    track.thumbnailUrl = vr["thumbnail"]["thumbnails"].back().value("url", "");
                }
                if (track.thumbnailUrl.empty()) {
                    track.thumbnailUrl = "https://i.ytimg.com/vi/" + track.id + "/hqdefault.jpg";
                }

                if (!track.id.empty() && !track.title.empty()) {
                    items.push_back(std::move(track));
                    if (items.size() >= 24)
                        break;
                }
            }
            if (items.size() >= 24)
                break;
        }
    } catch (const std::exception& ex) {
        DebugLog::log("[youtube] InnerTube parse warning: %s", ex.what());
    }

    return items;
}

std::string YouTubeClient::resolveStreamUrl(const std::string& videoId, const std::string& /*title*/, StatusProgressCallback onProgress) {
    // 1. Try custom backend service first (if configured)
    if (!m_backendUrl.empty()) {
        if (onProgress) onProgress("Conectando ao servidor...", 0.05f);
        try {
            DebugLog::log("[youtube] Trying custom backend %s for %s...", m_backendUrl.c_str(), videoId.c_str());
            std::string url = m_backendUrl + "/api/stream?id=" + videoId;
            std::string resp = themeshop::http::getText(url);
            if (!resp.empty()) {
                auto root = nlohmann::json::parse(resp);
                if (root.value("status", "") == "ok" && root.contains("stream_url")) {
                    std::string streamUrl = root.value("stream_url", "");
                    if (!streamUrl.empty())
                        return streamUrl;
                }
            }
        } catch (const std::exception& ex) {
            DebugLog::log("[youtube] custom backend /api/stream failed: %s", ex.what());
        }

        // Try /api/download endpoint on custom backend
        try {
            std::string dlCheckUrl = m_backendUrl + "/api/download?id=" + videoId;
            std::string checkResp = themeshop::http::getText(m_backendUrl + "/api/health");
            if (!checkResp.empty()) {
                return dlCheckUrl;
            }
        } catch (...) {}
    }

    // 2. Direct cloud MP3 converter engine (loader.to)
    try {
        if (onProgress) onProgress("Iniciando conversão MP3...", 0.10f);
        DebugLog::log("[youtube] Trying cloud MP3 converter for %s...", videoId.c_str());
        std::string initUrl = "https://loader.to/ajax/download.php?button=1&start=1&end=1&format=mp3&url=https://www.youtube.com/watch?v=" + videoId;
        std::string initResp = themeshop::http::getText(initUrl);
        if (!initResp.empty()) {
            auto initJson = nlohmann::json::parse(initResp);
            std::string progressUrl = initJson.value("progress_url", "");
            if (!progressUrl.empty()) {
                for (int attempt = 0; attempt < 15; ++attempt) {
                    if (m_cancelRequested.load(std::memory_order_acquire))
                        break;
                    float convProgress = 0.10f + 0.15f * (static_cast<float>(attempt + 1) / 15.0f);
                    std::string statusMsg = "Convertendo áudio... (" + std::to_string(attempt + 1) + "s)";
                    if (onProgress) onProgress(statusMsg, convProgress);

                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    std::string progResp = themeshop::http::getText(progressUrl);
                    if (progResp.empty())
                        continue;
                    auto progJson = nlohmann::json::parse(progResp);
                    int success = progJson.value("success", 0);
                    std::string dlUrl = progJson.value("download_url", "");
                    if (success == 1 && !dlUrl.empty()) {
                        DebugLog::log("[youtube] Cloud MP3 converter ready: %s", dlUrl.c_str());
                        if (onProgress) onProgress("Download pronto! Iniciando transferência...", 0.25f);
                        return dlUrl;
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        DebugLog::log("[youtube] Cloud MP3 converter failed: %s", ex.what());
    }

    // 3. Fallback to public Invidious instances
    static const std::vector<std::string> s_invidiousInstances = {
        "https://invidious.f5.si",
        "https://inv.nadeko.net",
        "https://invidious.nerdvpn.de"
    };
    for (const auto& instance : s_invidiousInstances) {
        if (m_cancelRequested.load(std::memory_order_acquire))
            break;
        try {
            if (onProgress) onProgress("Buscando stream direto alternativo...", 0.20f);
            DebugLog::log("[youtube] Trying Invidious instance %s for %s...", instance.c_str(), videoId.c_str());
            std::string vidUrl = instance + "/api/v1/videos/" + videoId;
            std::string vidResp = themeshop::http::getText(vidUrl);
            if (!vidResp.empty()) {
                auto vidJson = nlohmann::json::parse(vidResp);
                if (vidJson.contains("adaptiveFormats") && vidJson["adaptiveFormats"].is_array()) {
                    for (const auto& fmt : vidJson["adaptiveFormats"]) {
                        std::string type = fmt.value("type", "");
                        std::string u = fmt.value("url", "");
                        if (!u.empty() && type.find("audio/") != std::string::npos) {
                            DebugLog::log("[youtube] Found audio stream on %s: %s", instance.c_str(), u.c_str());
                            return u;
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            DebugLog::log("[youtube] Invidious %s failed: %s", instance.c_str(), ex.what());
        }
    }

    throw std::runtime_error("Could not resolve audio stream URL for video");
}

void YouTubeClient::loadInstalledTracks() {
    std::string musicDir = musicDirectory();
    std::vector<TrackItem> items;
    std::error_code ec;
    if (std::filesystem::is_directory(musicDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(musicDir, ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!entry.is_regular_file(ec))
                continue;

            std::error_code szEc;
            auto sz = entry.file_size(szEc);
            if (!szEc && sz < 1024)
                continue;

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (ext == ".mp3" || ext == ".ogg" || ext == ".wav") {
                TrackItem track;
                track.id = "local:" + entry.path().filename().string();
                track.title = entry.path().stem().string();
                track.author = "SD: /music";
                track.duration = formatBytes(sz);
                track.isDownloaded = true;
                track.localFilePath = entry.path().string();
                track.statusText = "✓ Instalado";

                std::string thumbPath = entry.path().string().substr(0, entry.path().string().find_last_of('.')) + ".jpg";
                std::error_code thumbEc;
                if (std::filesystem::exists(thumbPath, thumbEc)) {
                    track.thumbnailUrl = "file://" + thumbPath;
                }

                items.push_back(std::move(track));
            }
        }
    }

    std::sort(items.begin(), items.end(), [](const TrackItem& a, const TrackItem& b) {
        return a.title < b.title;
    });

    std::lock_guard<std::mutex> lk(m_tracksMutex);
    m_tracks = std::move(items);
}

bool YouTubeClient::deleteTrack(size_t trackIndex) {
    std::string pathToDelete;
    {
        std::lock_guard<std::mutex> lk(m_tracksMutex);
        if (trackIndex >= m_tracks.size())
            return false;
        pathToDelete = m_tracks[trackIndex].localFilePath;
        if (pathToDelete.empty()) {
            std::string cleanName = sanitizeFilename(m_tracks[trackIndex].title);
            pathToDelete = musicDirectory() + "/" + cleanName + ".mp3";
        }
    }

    if (!pathToDelete.empty()) {
        std::error_code ec;
        std::filesystem::remove(pathToDelete, ec);
        std::string thumbPath = pathToDelete.substr(0, pathToDelete.find_last_of('.')) + ".jpg";
        std::filesystem::remove(thumbPath, ec);
        DebugLog::log("[youtube] Deleted track file: %s (ec=%d)", pathToDelete.c_str(), ec.value());
    }

    if (m_lastQuery.empty()) {
        loadInstalledTracks();
    } else {
        refreshDownloadedStatus();
    }
    return true;
}

void YouTubeClient::refreshDownloadedStatus() {
    struct ExistingFile {
        std::string normalized;
        std::string fullPath;
    };
    std::vector<ExistingFile> existingFiles;
    std::error_code ec;
    std::string musicDir = musicDirectory();
    if (std::filesystem::is_directory(musicDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(musicDir, ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!entry.is_regular_file(ec)) continue;
            std::string filename = entry.path().filename().string();
            if (filename.empty() || filename.front() == '.') continue;

            std::error_code szEc;
            auto sz = entry.file_size(szEc);
            if (!szEc && sz < 1024) continue;

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".mp3" || ext == ".ogg" || ext == ".wav") {
                existingFiles.push_back({
                    normalizedComparison(entry.path().stem().string()),
                    entry.path().string()
                });
            }
        }
    }

    std::lock_guard<std::mutex> lk(m_tracksMutex);
    for (auto& track : m_tracks) {
        std::string cleanNorm = normalizedComparison(sanitizeFilename(track.title));
        bool found = false;
        std::string matchedPath;
        for (const auto& existing : existingFiles) {
            if (existing.normalized == cleanNorm ||
                existing.normalized.find(cleanNorm) != std::string::npos ||
                cleanNorm.find(existing.normalized) != std::string::npos) {
                found = true;
                matchedPath = existing.fullPath;
                break;
            }
        }
        track.isDownloaded = found;
        if (found) {
            track.localFilePath = matchedPath;
            track.statusText = "✓ Instalado";
        }
    }
}

bool YouTubeClient::downloadTrack(size_t trackIndex, StatusProgressCallback onProgress, CompleteCallback onComplete) {
    if (m_isDownloading.exchange(true)) {
        if (onComplete) onComplete(false, "", "Another download is already in progress");
        return false;
    }

    TrackItem track;
    {
        std::lock_guard<std::mutex> lk(m_tracksMutex);
        if (trackIndex >= m_tracks.size()) {
            m_isDownloading.store(false);
            if (onComplete) onComplete(false, "", "Invalid track index");
            return false;
        }
        m_tracks[trackIndex].isDownloading = true;
        m_tracks[trackIndex].downloadProgress = 0.01f;
        track = m_tracks[trackIndex];
    }

    auto task = [this, trackIndex, track, onProgress, onComplete]() {
        std::string cleanName = sanitizeFilename(track.title);
        std::string musicDir = musicDirectory();
        std::error_code ec;
        std::filesystem::create_directories(musicDir, ec);

        std::string destPath = musicDir + "/" + cleanName + ".mp3";
        std::string tempPath = musicDir + "/." + track.id + "_download.tmp";

        // Remove any previous temporary artifact
        std::filesystem::remove(tempPath, ec);
        ec.clear();

        DebugLog::log("[youtube] Starting download for '%s' to '%s'", track.title.c_str(), destPath.c_str());

        bool success = false;
        std::string errorMsg;

        try {
            std::string streamUrl = resolveStreamUrl(track.id, track.title, onProgress);
            DebugLog::log("[youtube] Resolved stream URL: %s", streamUrl.c_str());

            std::uint64_t expectedBytes = 5ULL * 1024ULL * 1024ULL;
            int durSec = 0;
            size_t colon = track.duration.find(':');
            if (colon != std::string::npos) {
                size_t secondColon = track.duration.find(':', colon + 1);
                if (secondColon != std::string::npos) {
                    durSec = std::atoi(track.duration.substr(0, colon).c_str()) * 3600
                           + std::atoi(track.duration.substr(colon + 1, secondColon - colon - 1).c_str()) * 60
                           + std::atoi(track.duration.substr(secondColon + 1).c_str());
                } else {
                    durSec = std::atoi(track.duration.substr(0, colon).c_str()) * 60
                           + std::atoi(track.duration.substr(colon + 1).c_str());
                }
            }
            if (durSec > 0) {
                expectedBytes = static_cast<std::uint64_t>(durSec) * 24000ULL;
            }

            auto progressCb = [this, trackIndex, onProgress, expectedBytes](std::uint64_t dl, std::uint64_t total) {
                float p = 0.25f;
                std::string statusMsg;
                if (total > 0) {
                    float ratio = static_cast<float>(dl) / static_cast<float>(total);
                    p = 0.25f + 0.74f * std::clamp(ratio, 0.0f, 1.0f);
                    statusMsg = "Baixando: " + formatBytes(dl) + " / " + formatBytes(total);
                } else {
                    float ratio = std::clamp(static_cast<float>(dl) / static_cast<float>(expectedBytes), 0.0f, 0.98f);
                    p = 0.25f + 0.74f * ratio;
                    statusMsg = "Baixando: " + formatBytes(dl) + " / ~" + formatBytes(expectedBytes);
                }
                {
                    std::lock_guard<std::mutex> lk(m_tracksMutex);
                    if (trackIndex < m_tracks.size()) {
                        m_tracks[trackIndex].downloadProgress = p;
                    }
                }
                if (onProgress) {
                    onProgress(statusMsg, p);
                }
            };

            std::uint64_t written = themeshop::http::getToFile(streamUrl, tempPath, progressCb);
            DebugLog::log("[youtube] Download finished: %llu bytes written to %s",
                          static_cast<unsigned long long>(written), tempPath.c_str());
            if (written > 1024) {
                std::filesystem::remove(destPath, ec);
                ec.clear();
                std::filesystem::rename(tempPath, destPath, ec);
                if (ec) {
                    std::filesystem::copy_file(tempPath, destPath, std::filesystem::copy_options::overwrite_existing, ec);
                    std::filesystem::remove(tempPath, ec);
                }

                // Download video thumbnail to disk alongside the audio track
                if (!track.thumbnailUrl.empty() && track.thumbnailUrl.rfind("file://", 0) != 0) {
                    std::string thumbDestPath = musicDir + "/" + cleanName + ".jpg";
                    try {
                        std::vector<std::uint8_t> thumbBytes;
                        {
                            std::lock_guard<std::mutex> lk(m_thumbMutex);
                            auto it = m_thumbnailCache.find(track.id);
                            if (it != m_thumbnailCache.end() && it->second) {
                                std::lock_guard<std::mutex> innerLk(it->second->mutex);
                                thumbBytes = it->second->bytes;
                            }
                        }
                        if (thumbBytes.empty()) {
                            thumbBytes = themeshop::http::getBytes(track.thumbnailUrl);
                        }
                        if (!thumbBytes.empty()) {
                            std::ofstream out(thumbDestPath, std::ios::binary);
                            if (out) {
                                out.write(reinterpret_cast<const char*>(thumbBytes.data()), thumbBytes.size());
                                DebugLog::log("[youtube] Saved thumbnail: %s (%zu bytes)", thumbDestPath.c_str(), thumbBytes.size());
                            }
                        }
                    } catch (const std::exception& thEx) {
                        DebugLog::log("[youtube] Failed to save thumbnail: %s", thEx.what());
                    }
                }

                success = true;
                if (onProgress) onProgress("Download concluído! (100%)", 1.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(450));
            } else {
                std::filesystem::remove(tempPath, ec);
                errorMsg = "Arquivo baixado corrompido ou incompleto (tamanho < 1KB)";
            }
        } catch (const std::exception& ex) {
            errorMsg = ex.what();
            DebugLog::log("[youtube] Download failed: %s", ex.what());
            std::filesystem::remove(tempPath, ec);
        } catch (...) {
            errorMsg = "Erro desconhecido durante o download";
            std::filesystem::remove(tempPath, ec);
        }

        {
            std::lock_guard<std::mutex> lk(m_tracksMutex);
            if (trackIndex < m_tracks.size()) {
                m_tracks[trackIndex].isDownloading = false;
                if (success) {
                    m_tracks[trackIndex].isDownloaded = true;
                    m_tracks[trackIndex].downloadProgress = 1.0f;
                    m_tracks[trackIndex].localFilePath = destPath;
                } else {
                    m_tracks[trackIndex].downloadProgress = 0.0f;
                }
            }
        }

        m_isDownloading.store(false);
        if (onComplete) {
            onComplete(success, success ? destPath : "", errorMsg);
        }
    };

    if (m_pool) {
        m_pool->submit(std::move(task));
    } else {
        std::thread(std::move(task)).detach();
    }

    return true;
}

YouTubeClient::PreviewPhase YouTubeClient::thumbnailPhase(const std::string& videoId) const {
    if (videoId.empty())
        return PreviewPhase::Failed;
    std::lock_guard<std::mutex> lk(m_thumbMutex);
    auto it = m_thumbnailCache.find(videoId);
    if (it == m_thumbnailCache.end() || !it->second)
        return PreviewPhase::Idle;
    std::lock_guard<std::mutex> innerLk(it->second->mutex);
    return it->second->phase;
}

const nxui::Texture* YouTubeClient::thumbnailTexture(const std::string& videoId) const {
    if (videoId.empty())
        return nullptr;
    std::lock_guard<std::mutex> lk(m_thumbMutex);
    auto it = m_thumbnailCache.find(videoId);
    if (it == m_thumbnailCache.end() || !it->second)
        return nullptr;
    std::lock_guard<std::mutex> innerLk(it->second->mutex);
    if (it->second->phase != PreviewPhase::Ready || !it->second->texture.valid())
        return nullptr;
    return &it->second->texture;
}

void YouTubeClient::primeThumbnail(const std::string& videoId, const std::string& thumbnailUrl) {
    if (videoId.empty() || thumbnailUrl.empty())
        return;

    std::shared_ptr<PreviewState> state;
    {
        std::lock_guard<std::mutex> lk(m_thumbMutex);
        auto& entry = m_thumbnailCache[videoId];
        if (!entry) {
            entry = std::make_shared<PreviewState>();
        }
        state = entry;
        state->lastAccessed = ++m_accessCounter;
    }

    {
        std::lock_guard<std::mutex> innerLk(state->mutex);
        if (state->phase != PreviewPhase::Idle)
            return;
        state->phase = PreviewPhase::Loading;
    }

    auto fetchTask = [state, videoId, thumbnailUrl]() {
        try {
            std::vector<std::uint8_t> bytes;
            if (thumbnailUrl.rfind("file://", 0) == 0) {
                std::string filePath = thumbnailUrl.substr(7);
                std::ifstream f(filePath, std::ios::binary | std::ios::ate);
                if (f) {
                    auto size = f.tellg();
                    f.seekg(0, std::ios::beg);
                    bytes.resize(size);
                    f.read(reinterpret_cast<char*>(bytes.data()), size);
                }
            } else {
                bytes = themeshop::http::getBytes(thumbnailUrl);
            }

            std::lock_guard<std::mutex> innerLk(state->mutex);
            if (!bytes.empty()) {
                state->bytes = std::move(bytes);
                state->phase = PreviewPhase::Downloaded;
            } else {
                state->phase = PreviewPhase::Failed;
            }
        } catch (...) {
            std::lock_guard<std::mutex> innerLk(state->mutex);
            state->phase = PreviewPhase::Failed;
        }
    };

    if (m_pool) {
        m_pool->submit(std::move(fetchTask));
    } else {
        std::thread(std::move(fetchTask)).detach();
    }
}

void YouTubeClient::updateThumbnailTextures() {
    if (!m_gpu || !m_renderer)
        return;

    std::lock_guard<std::mutex> lk(m_thumbMutex);
    for (auto& pair : m_thumbnailCache) {
        auto& state = pair.second;
        if (!state) continue;

        std::lock_guard<std::mutex> innerLk(state->mutex);
        if (state->phase == PreviewPhase::Downloaded && !state->bytes.empty()) {
            nxui::Texture tex;
            bool ok = tex.loadFromMemory(*m_gpu, *m_renderer, state->bytes.data(), state->bytes.size(), 320);
            state->bytes.clear();
            state->bytes.shrink_to_fit();
            if (ok) {
                state->texture = std::move(tex);
                state->phase = PreviewPhase::Ready;
            } else {
                state->phase = PreviewPhase::Failed;
            }
        }
    }
}

void YouTubeClient::trimThumbnailCache(size_t maxItems) {
    std::lock_guard<std::mutex> lk(m_thumbMutex);
    if (m_thumbnailCache.size() <= maxItems)
        return;

    std::vector<std::pair<std::string, std::uint64_t>> entries;
    entries.reserve(m_thumbnailCache.size());
    for (const auto& pair : m_thumbnailCache) {
        if (pair.second) {
            entries.emplace_back(pair.first, pair.second->lastAccessed);
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.second < b.second; // oldest first
    });

    size_t toRemove = m_thumbnailCache.size() - maxItems;
    for (size_t i = 0; i < toRemove && i < entries.size(); ++i) {
        m_thumbnailCache.erase(entries[i].first);
    }
}
