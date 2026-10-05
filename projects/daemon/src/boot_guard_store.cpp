#include "boot_guard_store.hpp"

#include <switchu/file_log.hpp>
#include <switchu/sd_commit.hpp>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/stat.h>

namespace switchu::daemon::guard_store {
namespace {

bool readFile(const char* path, std::string& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    char buf[256];
    out.clear();
    std::size_t n;
    // The file is tens of bytes; refuse anything large as corrupt.
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > 1024) {
            std::fclose(f);
            return false;
        }
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool exists(const char* path) {
    struct stat st {};
    return stat(path, &st) == 0;
}

} // namespace

boot_guard::State load() {
    boot_guard::State state;
    std::string text;
    // The temporary copy is complete when the primary is absent: a power cut
    // between removing the old file and renaming the new one leaves only it.
    for (const char* path : {kPath, kTemporary}) {
        if (readFile(path, text) && boot_guard::parse(text, state))
            return state;
        state = {};
    }
    return {};
}

bool save(const boot_guard::State& state) {
    std::error_code ec;
    std::filesystem::create_directory("sdmc:/config", ec);
    ec.clear();
    std::filesystem::create_directory("sdmc:/config/OmniLaunch", ec);

    const std::string text = boot_guard::serialize(state);
    std::FILE* f = std::fopen(kTemporary, "wb");
    if (!f)
        return false;
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool flushed = std::fflush(f) == 0;
    std::fclose(f);
    if (!wrote || !flushed) {
        std::remove(kTemporary);
        return false;
    }
    std::remove(kPath);   // fsdev rename does not replace an existing file
    if (std::rename(kTemporary, kPath) != 0)
        return false;     // the complete temporary copy is still on the card
    return switchu::commitSdCard("boot guard state");
}

bool disableOverride() {
    static constexpr const char* kActive =
        "sdmc:/atmosphere/contents/0100000000001000/exefs.nsp";
    static constexpr const char* kDisabled =
        "sdmc:/atmosphere/contents/0100000000001000/exefs.nsp.disabled";
    if (!exists(kActive)) {
        switchu::FileLog::log("[guard] override not present; nothing to disable");
        return false;
    }
    if (exists(kDisabled)) {
        switchu::FileLog::log("[guard] a disabled override already exists; refusing to replace it");
        return false;
    }
    if (std::rename(kActive, kDisabled) != 0) {
        switchu::FileLog::log("[guard] override rename FAIL errno=%d", errno);
        return false;
    }
    return switchu::commitSdCard("boot guard stock fallback");
}

} // namespace switchu::daemon::guard_store
