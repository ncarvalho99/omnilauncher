#include "self_uninstall.hpp"

#include <switchu/file_log.hpp>
#include <switchu/fs_remove.hpp>
#include <switchu/sd_commit.hpp>
#include <switchu/self_uninstall.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <switch.h>

namespace switchu::daemon::self_uninstall {
namespace {

bool inspectRegularFile(const char* path, bool& exists) {
    struct stat info {};
    // lstat deliberately rejects a symlink even when its target is regular;
    // self-removal is permitted to touch only the exact physical request file.
    if (lstat(path, &info) != 0) {
        exists = false;
        return errno == ENOENT;
    }
    exists = true;
    return S_ISREG(info.st_mode);
}

bool requestIsValid() {
    using namespace switchu::self_uninstall;

    std::FILE* request = std::fopen(kRequest, "rb");
    if (!request)
        return false;
    char contents[sizeof(kRequestContents)]{};
    const size_t expectedSize = sizeof(kRequestContents) - 1;
    const size_t read = std::fread(contents, 1, expectedSize, request);
    const bool atEnd = std::fgetc(request) == EOF && std::ferror(request) == 0;
    std::fclose(request);
    return read == expectedSize && atEnd
        && std::memcmp(contents, kRequestContents, expectedSize) == 0;
}

} // namespace

StagedRequestResult applyStagedRequest() {
    using namespace switchu::self_uninstall;

    bool requestExists = false;
    const bool requestRegular = inspectRegularFile(kRequest, requestExists);
    if (!requestExists && requestRegular)
        return StagedRequestResult::None;
    if (!requestRegular) {
        switchu::FileLog::log("[uninstall] invalid request path retained");
        return StagedRequestResult::Pending;
    }
    if (!requestIsValid()) {
        switchu::FileLog::log("[uninstall] malformed request retained");
        return StagedRequestResult::Pending;
    }

    switchu::FileLog::log("[uninstall] applying full purge request");

    std::string failedPath;

    // 1. Purge Atmosphere qlaunch override directory.
    if (!switchu::removeRecursive(kOverrideDirectory, &failedPath)) {
        switchu::FileLog::log("[uninstall] override directory purge FAIL: %s", failedPath.c_str());
        return StagedRequestResult::Pending;
    }

    // 2. Purge SwitchU menu directory and any standalone NRO.
    (void)switchu::removeRecursive(kMenuDirectory);
    (void)switchu::removeRecursive(kMenuNro);

    // 3. Purge SwitchU Manager directory and NRO.
    (void)switchu::removeRecursive(kManagerDirectory);
    (void)switchu::removeRecursive(kManagerNro);

    // Purge legacy SwitchU directories so uninstallation leaves nothing behind.
    (void)switchu::removeRecursive(kLegacyMenuDirectory);
    (void)switchu::removeRecursive(kLegacyMenuNro);
    (void)switchu::removeRecursive(kLegacyManagerDirectory);
    (void)switchu::removeRecursive(kLegacyManagerNro);
    (void)switchu::removeRecursive(kLegacyConfigDirectory);

    // 4. Close FileLog before deleting the config directory where logs live.
    switchu::FileLog::close();

    // 5. Purge SwitchU config directory (configs, themes, artwork, cache, logs, update, request marker).
    if (!switchu::removeRecursive(kConfigDirectory, &failedPath)) {
        svcOutputDebugString("[SwitchU-daemon] config directory purge incomplete", 48);
    }

    // 6. Commit SD card filesystem metadata.
    if (!switchu::commitSdCard("self-uninstall full purge")) {
        svcOutputDebugString("[SwitchU-daemon] purge commit FAIL", 33);
        return StagedRequestResult::Pending;
    }

    svcOutputDebugString("[SwitchU-daemon] full purge applied; stock qlaunch starts now", 61);
    return StagedRequestResult::Applied;
}

} // namespace switchu::daemon::self_uninstall
