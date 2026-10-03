#include "ThemeHttp.hpp"

#include "core/DebugLog.hpp"
#include "ClientKey.hpp"

#include <curl/curl.h>
#include <switch.h>

#include <cstdio>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace {

std::mutex g_themeHttpMutex;
bool g_nifmInitialized = false;
bool g_socketInitialized = false;
bool g_curlInitialized = false;
std::atomic<bool> g_cancelPendingRequests{false};

constexpr int kRequestAttemptCount = 2;

std::string resultToString(Result rc) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned int)rc);
    return buf;
}

bool isOfficialHost(const std::string& url) {
    if (url.size() < 8) return false;
    const char* expectedScheme = "https://";
    for (size_t i = 0; i < 8; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c != expectedScheme[i]) return false;
    }

    const size_t hostStart = 8;
    const size_t hostEnd = url.find_first_of("/:?#", hostStart);
    std::string host = (hostEnd == std::string::npos)
        ? url.substr(hostStart)
        : url.substr(hostStart, hostEnd - hostStart);

    for (char& c : host) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }

    return (host == "gallery.nclabs.dev" ||
            host == "switchu-api.nclabs.dev" ||
            host == "omnilaunch-api.nclabs.dev" ||
            host == "themes.nclabs.dev" ||
            host == "ytdl.nclabs.dev");
}

bool runtimeInitializedLocked() {
    return g_nifmInitialized && g_socketInitialized && g_curlInitialized;
}

void shutdownRuntimeLocked() {
    if (g_curlInitialized) {
        curl_global_cleanup();
        g_curlInitialized = false;
    }
    if (g_socketInitialized) {
        socketExit();
        g_socketInitialized = false;
    }
    if (g_nifmInitialized) {
        nifmExit();
        g_nifmInitialized = false;
    }
}

bool initializeRuntimeLocked() {
    if (runtimeInitializedLocked())
        return true;

    if (!g_nifmInitialized) {
        Result rc = nifmInitialize(NifmServiceType_User);
        if (R_FAILED(rc)) {
            DebugLog::log("[themeshop] nifmInitialize failed: %s", resultToString(rc).c_str());
            shutdownRuntimeLocked();
            return false;
        }
        g_nifmInitialized = true;
    }

    if (!g_socketInitialized) {
        Result rc = socketInitializeDefault();
        if (R_FAILED(rc)) {
            DebugLog::log("[themeshop] socketInitializeDefault failed: %s", resultToString(rc).c_str());
            shutdownRuntimeLocked();
            return false;
        }
        g_socketInitialized = true;
    }

    if (!g_curlInitialized) {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            DebugLog::log("[themeshop] curl_global_init failed: %s", curl_easy_strerror(rc));
            shutdownRuntimeLocked();
            return false;
        }
        g_curlInitialized = true;
    }

    DebugLog::log("[themeshop] http runtime ready");
    return true;
}

void ensureInternetConnectionReady(const std::string& url) {
    NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
    u32 strength = 0;
    Result rc = nifmGetInternetConnectionStatus(nullptr, &strength, &status);
    if (R_FAILED(rc)) {
        throw std::runtime_error("nifmGetInternetConnectionStatus failed: " + resultToString(rc));
    }
    if (status != NifmInternetConnectionStatus_Connected) {
        throw std::runtime_error("Internet connection is not ready for " + url);
    }
}

size_t appendResponse(char* data, size_t size, size_t count, void* userData) {
    const size_t bytes = size * count;
    auto* response = static_cast<std::string*>(userData);
    response->append(data, bytes);
    return bytes;
}

struct BytesProgressContext {
    const themeshop::http::ProgressCallback* onProgress = nullptr;
};

int reportBytesProgress(void* userData, curl_off_t total, curl_off_t downloaded,
                        curl_off_t, curl_off_t) {
    if (g_cancelPendingRequests.load(std::memory_order_acquire))
        return 1;
    auto* context = static_cast<BytesProgressContext*>(userData);
    if (context && context->onProgress && *context->onProgress) {
        try {
            (*context->onProgress)(downloaded > 0 ? static_cast<std::uint64_t>(downloaded) : 0,
                                   total > 0 ? static_cast<std::uint64_t>(total) : 0);
        } catch (...) {
            return 1;
        }
    }
    return 0;
}

int cancelIfAppletHandoff(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_cancelPendingRequests.load(std::memory_order_acquire) ? 1 : 0;
}

struct FileWriteContext {
    std::FILE* out = nullptr;
    std::uint64_t written = 0;
    bool writeFailed = false;
    const std::function<void(std::uint64_t, std::uint64_t)>* onProgress = nullptr;
};

size_t writeDownload(char* data, size_t size, size_t count, void* userData) {
    auto* ctx = static_cast<FileWriteContext*>(userData);
    const size_t bytes = size * count;
    if (g_cancelPendingRequests.load(std::memory_order_acquire))
        return 0;
    if (std::fwrite(data, 1, bytes, ctx->out) != bytes) {
        ctx->writeFailed = true;
        return 0;
    }
    ctx->written += bytes;
    if (ctx->onProgress && *ctx->onProgress) {
        try {
            (*ctx->onProgress)(ctx->written, 0);
        } catch (...) {
            ctx->writeFailed = true;
            return 0;
        }
    }
    return bytes;
}

std::vector<std::uint8_t> performRequestBytes(const std::string& url,
                                              const std::list<std::string>& headers,
                                              const themeshop::http::ProgressCallback& onProgress,
                                              const std::string& postBody = "") {
    CURL* request = curl_easy_init();
    if (!request)
        throw std::runtime_error("Could not create HTTP request");

    const bool official = isOfficialHost(url);
    std::string response;
    curl_easy_setopt(request, CURLOPT_URL, url.c_str());
    curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, official ? 0L : 1L);
    curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(request, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(request, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    const std::string agent = std::string("SwitchU/") + SWITCHU_VERSION;
    curl_easy_setopt(request, CURLOPT_USERAGENT, agent.c_str());
    if (!postBody.empty()) {
        curl_easy_setopt(request, CURLOPT_POST, 1L);
        curl_easy_setopt(request, CURLOPT_POSTFIELDS, postBody.c_str());
        curl_easy_setopt(request, CURLOPT_POSTFIELDSIZE, (long)postBody.size());
    }
    curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(request, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
    BytesProgressContext progressContext{&onProgress};
    curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, reportBytesProgress);
    curl_easy_setopt(request, CURLOPT_XFERINFODATA, &progressContext);

    struct curl_slist* requestHeaders = nullptr;
    for (const auto& header : headers)
        requestHeaders = curl_slist_append(requestHeaders, header.c_str());
    if (official && switchu::auth::kClientKey && switchu::auth::kClientKey[0] != '\0') {
        const std::string authHeader = std::string("X-SwitchU-Key: ") + switchu::auth::kClientKey;
        requestHeaders = curl_slist_append(requestHeaders, authHeader.c_str());
        const std::string authHeader2 = std::string("X-OmniLaunch-Key: ") + switchu::auth::kClientKey;
        requestHeaders = curl_slist_append(requestHeaders, authHeader2.c_str());
        DebugLog::log("[http] Official keys attached for %s", url.c_str());
    } else if (official) {
        DebugLog::log("[http] WARNING: official host %s requested without client key!", url.c_str());
    }
    if (requestHeaders)
        curl_easy_setopt(request, CURLOPT_HTTPHEADER, requestHeaders);

    const CURLcode result = curl_easy_perform(request);
    if (requestHeaders)
        curl_slist_free_all(requestHeaders);
    if (result != CURLE_OK) {
        curl_easy_cleanup(request);
        if (result == CURLE_ABORTED_BY_CALLBACK &&
            g_cancelPendingRequests.load(std::memory_order_acquire)) {
            throw std::runtime_error("HTTP request cancelled for system applet handoff");
        }
        throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(result));
    }

    long statusCode = 0;
    curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(request);
    if (statusCode < 200 || statusCode >= 300) {
        throw std::runtime_error("HTTP error " + std::to_string(statusCode));
    }

    return std::vector<std::uint8_t>(response.begin(), response.end());
}

std::vector<std::uint8_t> performBytes(const std::string& url,
                                       const std::list<std::string>& headers,
                                       const themeshop::http::ProgressCallback& onProgress = {},
                                       const std::string& postBody = "") {
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);

    std::string lastError = "Theme Shop HTTP request failed";
    for (int attempt = 1; attempt <= kRequestAttemptCount; ++attempt) {
        try {
            if (g_cancelPendingRequests.load(std::memory_order_acquire))
                throw std::runtime_error("HTTP request cancelled for system applet handoff");
            if (!initializeRuntimeLocked()) {
                throw std::runtime_error("Theme Shop HTTP runtime is unavailable");
            }

            ensureInternetConnectionReady(url);
            auto bytes = performRequestBytes(url, headers, onProgress, postBody);
            if (attempt > 1) {
                DebugLog::log("[themeshop] request recovered on retry %d: %s", attempt, url.c_str());
            }
            return bytes;
        } catch (const std::exception& ex) {
            lastError = ex.what();
            DebugLog::log("[themeshop] request failed (%d/%d): %s -> %s",
                          attempt,
                          kRequestAttemptCount,
                          url.c_str(),
                          ex.what());
        } catch (...) {
            lastError = "Unknown HTTP error";
            DebugLog::log("[themeshop] request failed (%d/%d): %s -> unknown error",
                          attempt,
                          kRequestAttemptCount,
                          url.c_str());
        }

        if (g_cancelPendingRequests.load(std::memory_order_acquire))
            break;

        if (attempt < kRequestAttemptCount) {
            shutdownRuntimeLocked();
        }
    }

    throw std::runtime_error(lastError);
}

} // namespace

namespace themeshop::http {

bool initialize() {
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);
    return initializeRuntimeLocked();
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);
    shutdownRuntimeLocked();
}

bool isInitialized() {
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);
    return runtimeInitializedLocked();
}

void cancelPendingRequests() {
    g_cancelPendingRequests.store(true, std::memory_order_release);
}

std::vector<std::uint8_t> getBytes(const std::string& url,
                                   const std::list<std::string>& headers,
                                   const ProgressCallback& onProgress) {
    return performBytes(url, headers, onProgress);
}

std::uint64_t getToFile(const std::string& url,
                        const std::string& destinationPath,
                        const std::function<void(std::uint64_t, std::uint64_t)>& onProgress) {
    std::lock_guard<std::mutex> lk(g_themeHttpMutex);

    std::string lastError = "Theme package download failed";
    for (int attempt = 1; attempt <= kRequestAttemptCount; ++attempt) {
        if (g_cancelPendingRequests.load(std::memory_order_acquire))
            throw std::runtime_error("HTTP request cancelled for applet handoff");
        if (!initializeRuntimeLocked())
            throw std::runtime_error("Theme Shop HTTP runtime is unavailable");
        ensureInternetConnectionReady(url);

        std::FILE* out = std::fopen(destinationPath.c_str(), "wb");
        if (!out)
            throw std::runtime_error("Could not open " + destinationPath + " for writing");

        try {
            CURL* request = curl_easy_init();
            if (!request)
                throw std::runtime_error("Could not create HTTP request");

            const bool official = isOfficialHost(url);
            FileWriteContext writeCtx{out, 0, false, &onProgress};
            const std::string agent = std::string("SwitchU/") + SWITCHU_VERSION;
            curl_easy_setopt(request, CURLOPT_URL, url.c_str());
            curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, official ? 0L : 1L);
            curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, 4L);
            // Large packages have no total timeout, but the transfer callback
            // can now abort even while no body data is arriving.
            curl_easy_setopt(request, CURLOPT_TIMEOUT, 0L);
            curl_easy_setopt(request, CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(request, CURLOPT_LOW_SPEED_TIME, 30L);
            curl_easy_setopt(request, CURLOPT_BUFFERSIZE, 128 * 1024L);
            curl_easy_setopt(request, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
            curl_easy_setopt(request, CURLOPT_USERAGENT, agent.c_str());
            curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, writeDownload);
            curl_easy_setopt(request, CURLOPT_WRITEDATA, &writeCtx);
            curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, cancelIfAppletHandoff);

            struct curl_slist* requestHeaders = nullptr;
            if (official && switchu::auth::kClientKey && switchu::auth::kClientKey[0] != '\0') {
                const std::string authHeader = std::string("X-SwitchU-Key: ") + switchu::auth::kClientKey;
                requestHeaders = curl_slist_append(requestHeaders, authHeader.c_str());
                curl_easy_setopt(request, CURLOPT_HTTPHEADER, requestHeaders);
                DebugLog::log("[http] Official key attached for download %s", url.c_str());
            } else if (official) {
                DebugLog::log("[http] WARNING: official download %s requested without client key!", url.c_str());
            }

            const CURLcode result = curl_easy_perform(request);
            if (requestHeaders)
                curl_slist_free_all(requestHeaders);
            long statusCode = 0;
            curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &statusCode);
            curl_easy_cleanup(request);

            if (result != CURLE_OK) {
                if (g_cancelPendingRequests.load(std::memory_order_acquire))
                    throw std::runtime_error("HTTP request cancelled for applet handoff");
                throw std::runtime_error(std::string("HTTP request failed: ")
                                         + curl_easy_strerror(result));
            }
            if (statusCode < 200 || statusCode >= 300)
                throw std::runtime_error("HTTP error " + std::to_string(statusCode));

            if (writeCtx.writeFailed)
                throw std::runtime_error("Could not write to " + destinationPath);
            std::fclose(out);
            return writeCtx.written;
        } catch (const std::exception& ex) {
            std::fclose(out);
            std::remove(destinationPath.c_str());    // nada pela metade fica no cartao
            lastError = ex.what();
            DebugLog::log("[themeshop] package download failed (%d/%d): %s -> %s",
                          attempt, kRequestAttemptCount, url.c_str(), lastError.c_str());
            if (g_cancelPendingRequests.load(std::memory_order_acquire))
                break;
            if (attempt < kRequestAttemptCount)
                shutdownRuntimeLocked();
        }
    }

    throw std::runtime_error(lastError);
}

std::string getText(const std::string& url,
                    const std::list<std::string>& headers) {
    auto bytes = performBytes(url, headers);
    return std::string(bytes.begin(), bytes.end());
}

std::string postJson(const std::string& url,
                     const std::string& jsonBody,
                     const std::list<std::string>& headers) {
    std::list<std::string> allHeaders = headers;
    allHeaders.push_back("Content-Type: application/json");
    auto bytes = performBytes(url, allHeaders, {}, jsonBody);
    return std::string(bytes.begin(), bytes.end());
}

} // namespace themeshop::http
