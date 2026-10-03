#include "update_apply.hpp"

#include "themeshop/ZipReader.hpp"
#include <switchu/file_log.hpp>
#include <switchu/sd_commit.hpp>

#include <cstdio>
#include <string>
#include <sys/stat.h>

// Applying an update belongs here, not in the menu.
//
// The menu cannot replace the files it is running from: SDL_ttf holds the font
// open for as long as the menu lives, and the first attempt to update in place
// destroyed that font and left the installation half replaced. The daemon runs
// before the menu is launched, so at this moment nothing in switch/SwitchU is
// open by anybody. The menu only downloads and verifies; deciding that the
// payload is good and putting it in place are separate jobs on purpose.
namespace switchu::daemon::update {
namespace {

constexpr const char* kStagedArchive = "sdmc:/config/OmniLaunch/update/update.zip";
// Written last by the menu, once the archive is complete and verified. Its
// presence is the whole signal: a partial download leaves no marker and is
// simply ignored here.
constexpr const char* kReadyMarker   = "sdmc:/config/OmniLaunch/update/ready";
constexpr const char* kAttemptFile   = "sdmc:/config/OmniLaunch/update/attempts";
// Left by the menu when it managed to put the archive's daemon on the card
// before the restart. Its presence means the daemon reading it is already the
// one out of the archive, so this boot does not have to be redone.
constexpr const char* kDaemonMarker  = "sdmc:/config/OmniLaunch/update/daemon";
constexpr const char* kCardRoot      = "sdmc:/";
// An update that cannot be applied must not delay every boot forever. After
// this many tries the staging is cleared and the console starts normally.
constexpr int kMaxAttempts = 3;

bool exists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

int readAttempts() {
    std::FILE* f = std::fopen(kAttemptFile, "rb");
    if (!f)
        return 0;
    int value = 0;
    if (std::fscanf(f, "%d", &value) != 1)
        value = 0;
    std::fclose(f);
    return value;
}

void writeAttempts(int value) {
    std::FILE* f = std::fopen(kAttemptFile, "wb");
    if (!f)
        return;
    std::fprintf(f, "%d", value);
    std::fclose(f);
}

void clearStaging() {
    std::remove(kReadyMarker);
    std::remove(kAttemptFile);
    std::remove(kStagedArchive);
    std::remove(kDaemonMarker);
}

} // namespace

bool applyStagedUpdate() {
    if (!exists(kReadyMarker))
        return false;
    if (!exists(kStagedArchive)) {
        switchu::FileLog::log("[update] marker without an archive; clearing");
        clearStaging();
        return false;
    }

    const int attempts = readAttempts() + 1;
    if (attempts > kMaxAttempts) {
        switchu::FileLog::log("[update] giving up after %d attempts; booting as installed",
                              kMaxAttempts);
        clearStaging();
        return false;
    }
    // Recorded before the work starts, so a failure that never returns still
    // counts against the limit and cannot loop the console forever.
    writeAttempts(attempts);

    switchu::FileLog::log("[update] applying staged update (attempt %d)", attempts);

    themeshop::ZipExtractPolicy policy;
    policy.allowExecutablePayload = true;
    policy.requiredRoots = {"atmosphere/", "switch/"};

    const auto result = themeshop::extractZipFile(kStagedArchive, kCardRoot, {}, policy);

    // Commit whichever way it went, before anything else runs.
    //
    // This is the largest write the console ever makes on its own: 527 files
    // and about 43 MB, each one created as .part and renamed over its target,
    // which is three directory operations apiece. That much FAT churn left
    // outstanding is exactly the state the power sequence was taught to avoid,
    // and it was left outstanding here -- extraction happens in the daemon, at
    // boot, and nothing committed it. The console then ran for ten minutes and
    // rebooted, and hekate came up unable to find nyx.
    //
    // The failure path commits too: the extractor writes beside its targets and
    // swaps at the end, so a run that gave up partway still moved real files.
    //
    // A power cut during the extraction itself is still a risk, but it is a
    // twenty-second window at boot rather than a whole session.
    switchu::commitSdCard("update applied");

    if (!result.success) {
        switchu::FileLog::log("[update] apply failed: %s", result.error.c_str());
        return false;   // staging stays; the next boot retries until the limit
    }

    switchu::FileLog::log("[update] applied %d files, %llu bytes",
                          result.filesWritten, (unsigned long long)result.bytesWritten);

    // Read before the staging is cleared, because clearing takes it with it.
    const bool daemonAlreadyRunning = exists(kDaemonMarker);
    if (daemonAlreadyRunning) {
        switchu::FileLog::log("[update] daemon was already in place before this boot");
    } else {
        switchu::FileLog::log(
            "[update] daemon replaced on the card; this one is the version it replaces");
    }

    clearStaging();
    // Dropping the archive frees another 43 MB of clusters, which is its own
    // batch of metadata. It costs nothing to make it durable here.
    switchu::commitSdCard("update staging cleared");
    return !daemonAlreadyRunning;
}

} // namespace switchu::daemon::update
