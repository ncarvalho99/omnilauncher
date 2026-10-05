#include "NtpClient.hpp"
#include "NetRuntime.hpp"
#include "smi_commands.hpp"
#include <switchu/file_log.hpp>
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>

namespace switchu::services {

namespace {

struct NtpPacket {
    uint8_t  li_vn_mode;      // 0x23: LI=0, VN=4, Mode=3 (client)
    uint8_t  stratum;
    uint8_t  poll;
    uint8_t  precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_tm_s;
    uint32_t ref_tm_f;
    uint32_t orig_tm_s;
    uint32_t orig_tm_f;
    uint32_t rx_tm_s;
    uint32_t rx_tm_f;
    uint32_t tx_tm_s;
    uint32_t tx_tm_f;
} __attribute__((packed));

static_assert(sizeof(NtpPacket) == 48, "NtpPacket must be 48 bytes");

constexpr uint64_t kNtpEpochOffset = 2208988800ULL; // Seconds from 1900-01-01 to 1970-01-01

const std::vector<std::string> s_ntpServers = {
    "0.pool.ntp.org",
    "1.pool.ntp.org",
    "2.pool.ntp.org",
    "3.pool.ntp.org",
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
};

uint64_t queryServer(const std::string& host, int timeoutSec, bool* outDnsFailed = nullptr) {
    if (outDnsFailed) *outDnsFailed = false;

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = nullptr;
    int err = getaddrinfo(host.c_str(), "123", &hints, &res);
    if (err != 0 || !res) {
        switchu::FileLog::log("[ntp] DNS resolve failed for %s: %d (%s)",
                              host.c_str(), err, gai_strerror(err));
        if (outDnsFailed) *outDnsFailed = true;
        return 0;
    }

    uint64_t posixTime = 0;
    for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;

        struct timeval tv{};
        tv.tv_sec = timeoutSec > 0 ? timeoutSec : 2;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        NtpPacket packet{};
        packet.li_vn_mode = 0x23; // VN 4, mode 3 (Client)

        ssize_t sent = sendto(sock, &packet, sizeof(packet), 0, rp->ai_addr, rp->ai_addrlen);
        if (sent == sizeof(packet)) {
            socklen_t addrLen = rp->ai_addrlen;
            ssize_t recvd = recvfrom(sock, &packet, sizeof(packet), 0, rp->ai_addr, &addrLen);
            if (recvd >= static_cast<ssize_t>(sizeof(packet))) {
                uint32_t tx_sec = ntohl(packet.tx_tm_s);
                if (tx_sec > kNtpEpochOffset) {
                    posixTime = tx_sec - kNtpEpochOffset;
                    switchu::FileLog::log("[ntp] successfully synced from %s -> posix %llu",
                                          host.c_str(), (unsigned long long)posixTime);
                    close(sock);
                    break;
                }
            }
        }
        close(sock);
    }

    freeaddrinfo(res);
    return posixTime;
}

// Background worker thread state using native Horizon libnx Thread
alignas(0x1000) static u8 s_ntpStack[0x8000];
static Thread s_ntpThread;
static std::atomic<bool> s_ntpRunning{false};
static std::atomic<bool> s_cancelRequested{false};
static bool s_ntpThreadCreated = false;
static NtpClient::SyncCallback s_ntpCallback;
static std::mutex s_ntpSyncMutex;

static void ntpWorkerThreadFunc(void* arg) {
    (void)arg;
    uint64_t timestamp = NtpClient::queryNetworkTime(2);
    bool ok = false;
    if (timestamp > 0 && !s_cancelRequested.load()) {
        Result rc = switchu::menu::smi_cmd::setPosixTime(timestamp, true);
        ok = R_SUCCEEDED(rc);
        switchu::FileLog::log("[ntp] apply sync result: 0x%X (ok=%d)", rc, ok ? 1 : 0);
    }

    NtpClient::SyncCallback cb;
    {
        std::lock_guard<std::mutex> lock(s_ntpSyncMutex);
        cb = std::move(s_ntpCallback);
    }
    if (cb && !s_cancelRequested.load()) {
        cb(ok, timestamp);
    }
    s_ntpRunning.store(false);
}

} // namespace

const std::vector<std::string>& NtpClient::serverList() {
    return s_ntpServers;
}

uint64_t NtpClient::queryNetworkTime(int timeoutSeconds) {
    if (s_cancelRequested.load())
        return 0;

    // Hold a reference on the shared network runtime for the whole query. The
    // Theme Shop HTTP layer exits the socket device between request retries;
    // without this reference it could do so while a descriptor opened below was
    // still live, and the close() that follows crashed the menu (data abort in
    // _close_r on a removed "soc:" device).
    net::Guard netRuntime;
    if (!netRuntime) {
        switchu::FileLog::log("[ntp] network runtime unavailable");
        return 0;
    }

    for (const auto& server : s_ntpServers) {
        if (s_cancelRequested.load())
            return 0;

        bool dnsFailed = false;
        uint64_t t = queryServer(server, timeoutSeconds, &dnsFailed);
        if (t > 0) {
            return t;
        }

        // If DNS resolution failed on this server, attempting the remaining
        // server hostnames on the same broken connection will stall on repeated
        // resolver timeouts. Fail fast.
        if (dnsFailed || s_cancelRequested.load()) {
            break;
        }
    }
    return 0;
}

void NtpClient::syncAsync(SyncCallback callback) {
    std::lock_guard<std::mutex> lock(s_ntpSyncMutex);

    if (s_ntpRunning.load()) {
        switchu::FileLog::log("[ntp] sync already running, skipping request");
        return;
    }

    if (s_ntpThreadCreated) {
        threadWaitForExit(&s_ntpThread);
        threadClose(&s_ntpThread);
        s_ntpThreadCreated = false;
    }

    s_cancelRequested.store(false);
    s_ntpCallback = std::move(callback);
    s_ntpRunning.store(true);

    Result rc = threadCreate(&s_ntpThread, ntpWorkerThreadFunc, nullptr,
                             s_ntpStack, sizeof(s_ntpStack), 0x2C, 2);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[ntp] threadCreate failed: 0x%X", rc);
        s_ntpRunning.store(false);
        if (s_ntpCallback) {
            auto cb = std::move(s_ntpCallback);
            cb(false, 0);
        }
        return;
    }

    s_ntpThreadCreated = true;
    rc = threadStart(&s_ntpThread);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[ntp] threadStart failed: 0x%X", rc);
        s_ntpRunning.store(false);
        threadClose(&s_ntpThread);
        s_ntpThreadCreated = false;
        if (s_ntpCallback) {
            auto cb = std::move(s_ntpCallback);
            cb(false, 0);
        }
    }
}

void NtpClient::cleanup() {
    s_cancelRequested.store(true);
    std::lock_guard<std::mutex> lock(s_ntpSyncMutex);
    if (s_ntpThreadCreated) {
        s32 idx = 0;
        Result rc = svcWaitSynchronization(&idx, &s_ntpThread.handle, 1, 100'000'000ULL);
        if (R_SUCCEEDED(rc)) {
            threadWaitForExit(&s_ntpThread);
            threadClose(&s_ntpThread);
        } else {
            switchu::FileLog::log("[ntp] thread busy during cleanup; closing handle without blocking handoff");
            threadClose(&s_ntpThread);
        }
        s_ntpThreadCreated = false;
        s_ntpRunning.store(false);
    }
}

} // namespace switchu::services
