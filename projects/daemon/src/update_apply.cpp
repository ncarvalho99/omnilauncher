#include "update_apply.hpp"

#include "themeshop/ZipReader.hpp"
#include <switchu/file_log.hpp>
#include <switchu/sd_commit.hpp>
#include <switchu/install_txn.hpp>

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

// The journal must reach the card before the file moves it describes. Pure std code
// cannot commit the card, so this installs the hook.
void installSyncHook() {
    switchu::install_txn::g_syncHook = +[]() { return switchu::commitSdCard("update journal"); };
}

// Puts the previous files back and forgets that the daemon half was ever placed.
bool restoreBackup(const char* why) {
    std::string failed;
    const bool restored = switchu::install_txn::rollback(
        switchu::install_txn::kUpdateBackupRoot, kCardRoot, &failed);
    // The marker says the daemon on the card is the new one. After a rollback it
    // is the old one again, so the marker would lie.
    std::remove(kDaemonMarker);
    if (restored)
        switchu::FileLog::log("[update] rollback (%s): previous files restored", why);
    else
        switchu::FileLog::log("[update] rollback (%s) INCOMPLETE at %s; backup kept for the next boot",
                              why, failed.c_str());
    switchu::commitSdCard("update rollback");
    return restored;
}

} // namespace

bool backupPending() {
    installSyncHook();
    return switchu::install_txn::pending(switchu::install_txn::kUpdateBackupRoot);
}

void acceptUpdate() {
    installSyncHook();
    if (!backupPending())
        return;
    const bool ok = switchu::install_txn::commit(switchu::install_txn::kUpdateBackupRoot);
    switchu::FileLog::log("[update] new version healthy; rollback backup %s", ok ? "dropped" : "could not be removed");
    switchu::commitSdCard("update accepted");
}

bool rollbackUpdate() {
    installSyncHook();
    return restoreBackup("new menu keeps failing");
}

bool recoverInterruptedUpdate() {
    // A pass that was cut short (power loss, crash) left a journal that does not
    // end in a completed pass: undo it before anything else, whether or not an
    // update is still staged. Without this the console would run a mix of old
    // and new files.
    installSyncHook();
    switchu::install_txn::resumeCleanup(switchu::install_txn::kUpdateBackupRoot);
    if (switchu::install_txn::interrupted(switchu::install_txn::kUpdateBackupRoot)) {
        switchu::FileLog::log("[update] found an interrupted update pass; rolling back to the previous version");
        if (!restoreBackup("interrupted pass")) {
            // Fail closed: re-applying over a half-restored tree would back up
            // files that are already part-new. The backup is kept and the rollback
            // is retried at the next boot.
            switchu::FileLog::log("[update] recovery incomplete; not applying anything this boot");
            return false;
        }
    }
    return true;
}

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

    // Every file this replaces is first set aside in the rollback backup. The
    // journal may already hold the menu's daemon-half pass; rollback returns to
    // the state before the first pass.
    switchu::install_txn::Txn txn(switchu::install_txn::kUpdateBackupRoot, kCardRoot);
    const bool journalOk = txn.begin();
    if (!journalOk) {
        switchu::FileLog::log("[update] could not open the rollback journal; not touching the installed files");
        switchu::commitSdCard("update refused");
        return false;   // staging stays; retried until the limit
    }
    policy.txn = &txn;

    auto result = themeshop::extractZipFile(kStagedArchive, kCardRoot, {}, policy);
    if (result.success && !txn.completePass()) {
        result.success = false;
        result.error = "could not record the completed pass";
    }
    if (!result.success) {
        switchu::FileLog::log("[update] apply failed: %s", result.error.c_str());
        if (!restoreBackup("apply failed"))
            return false;   // backup and journal kept; resumed at the next boot
    }

    // Commit whichever way it went, before anything else runs.
    switchu::commitSdCard("update applied");

    if (!result.success)
        return false;   // originals restored above; staging stays, the next boot retries until the limit

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
