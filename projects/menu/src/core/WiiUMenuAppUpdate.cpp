// In-launcher updates: ask GitHub what the newest release is, show what it
// changed, and install it when the player accepts.
//
// The payload is downloaded and verified in full before a single file is
// replaced, because the archive being unpacked contains the menu that is doing
// the unpacking. A half-written download must never reach the card.

#include "WiiUMenuApp.hpp"

#include "themeshop/ThemeHttp.hpp"
#include "themeshop/ZipReader.hpp"
#include <switchu/sd_commit.hpp>
#include <switchu/install_txn.hpp>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include "core/DebugLog.hpp"

#include <nxui/core/I18n.hpp>

#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include <mutex>
#include <string>

#ifndef SWITCHU_VERSION
#define SWITCHU_VERSION "unknown"
#endif

namespace {

// SHA-256 of a file, read in chunks. Empty when the file cannot be read (and on
// hosts without libnx, where updates are not applied anyway).
std::string fileSha256Hex(const char* path) {
#ifdef __SWITCH__
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return {};
    Sha256Context ctx;
    sha256ContextCreate(&ctx);
    static unsigned char buf[64 * 1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        sha256ContextUpdate(&ctx, buf, n);
    const bool readOk = std::ferror(f) == 0;
    std::fclose(f);
    if (!readOk)
        return {};
    std::uint8_t digest[SHA256_HASH_SIZE];
    sha256ContextGetHash(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0f]);
    }
    return out;
#else
    (void)path;
    return {};
#endif
}

constexpr const char* kUpdateDir = "sdmc:/config/OmniLaunch/update";
constexpr const char* kUpdateArchive = "sdmc:/config/OmniLaunch/update/update.zip";
constexpr const char* kReadyMarker = "sdmc:/config/OmniLaunch/update/ready";
// Written once the daemon inside the archive is already on the card, which is
// what lets the boot that applies the rest skip its own restart.
constexpr const char* kDaemonMarker = "sdmc:/config/OmniLaunch/update/daemon";
// The archive lays out atmosphere/ and switch/ exactly as they sit on the card,
// so the card root is the extraction target.
constexpr const char* kCardRoot = "sdmc:/";

// The staging directory does not exist on a fresh card, and curl will not
// create it. Only this one level is needed: its parent is the config directory
// the menu already owns.
bool ensureUpdateDirectory() {
    struct stat info {};
    if (stat(kUpdateDir, &info) == 0)
        return true;
    return mkdir(kUpdateDir, 0777) == 0;
}

// The marker the daemon looks for at boot. While it is there the update is
// downloaded, verified and staged, and the only step left is the restart that
// applies it. The daemon removes it once the files are in place, so its absence
// after a reboot is what says the update went through.
//
// Checked from the card rather than remembered in memory on purpose: the menu
// restarts every time a game closes, and a flag that did not survive that was
// how the same update kept being offered over and over.
bool updateRestartPending() {
    struct stat info {};
    return stat(kReadyMarker, &info) == 0;
}

int todayNumber() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return (int)std::chrono::duration_cast<std::chrono::hours>(now).count() / 24;
}

// Notes for the version that is installed, shipped with the build. The tab can
// answer "what changed" before any network call, and a console that never
// reaches GitHub still gets an answer. tools/export_release_notes.py derives
// this from CHANGELOG.md during release preparation.
std::string loadBundledReleaseNotes(const std::string& languageTag) {
    std::ifstream file(std::string(SD_ASSETS) + "/notes/release-notes.txt");
    if (!file)
        return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    constexpr const char* kMarker = "--pt-BR--";
    const std::size_t split = text.find(kMarker);
    if (split == std::string::npos)
        return text;
    if (languageTag.rfind("pt", 0) == 0)
        return text.substr(split + std::char_traits<char>::length(kMarker));
    return text.substr(0, split);
}

} // namespace

struct WiiUMenuApp::UpdateDownload {
    std::mutex mutex;
    update::UpdateClient::Release release;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    bool extracting = false;
    bool done = false;
    bool ok = false;
    std::string error;
};

void WiiUMenuApp::startUpdateCheck(bool forced) {
#ifdef SWITCHU_MENU
    // Nothing a check could find is actionable while a package waits to be
    // applied, and asking again is how the console ended up downloading the
    // same release repeatedly.
    if (updateRestartPending()) {
        m_pendingUpdate = {};
        m_updateStatus = nxui::I18n::instance().tr("themeshop.update_restart_pending",
                                                   "Downloaded. Restart to apply it.");
        publishUpdateState();
        DebugLog::log("[update] restart pending; check skipped");
        return;
    }
    if (!forced) {
        const int today = todayNumber();
        // Once a day. The menu restarts every time a game is closed, and asking
        // GitHub on each of those would burn the unauthenticated rate limit for
        // no new information.
        if (m_config.lastUpdateCheckDay == today)
            return;
        m_config.lastUpdateCheckDay = today;
        m_config.save();
    }
    m_updateOffered = false;
    m_updateCheckForced = forced;
    m_updateStatus = nxui::I18n::instance().tr("settings.about.check_updates_running",
                                               "Checking for updates...");
    publishUpdateState();
    m_updateClient.check(m_threadPool, SWITCHU_VERSION);
    DebugLog::log("[update] check started (current %s)", SWITCHU_VERSION);
#else
    (void)forced;
#endif
}

void WiiUMenuApp::publishUpdateState() {
#ifdef SWITCHU_MENU
    if (!m_themeShop)
        return;
    const std::string language = nxui::I18n::instance().activeLanguageTag();
    const bool havePublished = !m_latestRelease.notes.empty();
    const bool restartPending = updateRestartPending();
    m_themeShop->setUpdateState(
        std::string("SwitchU ") + SWITCHU_VERSION,
        // With the restart pending there is nothing left to install, so the row
        // that offers it must not appear at all.
        restartPending ? std::string() : m_pendingUpdate.version,
        m_updateStatus,
        m_updateDownload != nullptr,
        havePublished ? m_latestRelease.version : std::string(SWITCHU_VERSION),
        havePublished ? update::UpdateClient::condenseNotes(m_latestRelease.notes, language)
                      : loadBundledReleaseNotes(language),
        restartPending);
#endif
}

void WiiUMenuApp::syncUpdateCheck() {
#ifdef SWITCHU_MENU
    const auto snapshot = m_updateClient.snapshot();
    if (snapshot.revision == m_updateSeenRevision)
        return;
    if (snapshot.phase == update::UpdateClient::Phase::Checking)
        return;
    m_updateSeenRevision = snapshot.revision;

    auto& i18n = nxui::I18n::instance();
    // A check the player asked for has to answer, even when the answer is that
    // nothing changed. The daily one stays silent unless there is news.
    if (snapshot.phase == update::UpdateClient::Phase::Failed) {
        DebugLog::log("[update] check failed: %s", snapshot.error.c_str());
        m_updateStatus = i18n.tr("dialog.update_check_failed",
                                 "Could not reach the update server.");
        publishUpdateState();
        if (m_updateCheckForced && m_settings)
            m_settings->requestToast(m_updateStatus, 3.0f);
        return;
    }
    if (snapshot.phase != update::UpdateClient::Phase::Available) {
        DebugLog::log("[update] already on the newest release");
        m_latestRelease = snapshot.release;
        m_pendingUpdate = {};
        m_updateStatus = i18n.tr("dialog.update_up_to_date", "SwitchU is up to date.");
        publishUpdateState();
        if (m_updateCheckForced && m_settings)
            m_settings->requestToast(m_updateStatus, 2.6f);
        return;
    }
    DebugLog::log("[update] %s available (%llu bytes)", snapshot.release.version.c_str(),
                  (unsigned long long)snapshot.release.sizeBytes);
    m_latestRelease = snapshot.release;
    m_pendingUpdate = snapshot.release;
    m_updateStatus = i18n.tr("dialog.update_available_short", "Update available")
                   + " (" + snapshot.release.version + ")";
    publishUpdateState();
    offerUpdate(snapshot.release, true);
#endif
}

void WiiUMenuApp::showReleaseNotes() {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    auto& i18n = nxui::I18n::instance();
    const std::string language = i18n.activeLanguageTag();
    const bool havePublished = !m_latestRelease.notes.empty();
    const std::string notes = havePublished
        ? update::UpdateClient::condenseNotes(m_latestRelease.notes, language)
        : loadBundledReleaseNotes(language);
    if (notes.empty())
        return;
    m_audio.playSfx(Sfx::ModalShow);
    raiseOverlay(m_dialog);
    m_dialog->show(
        std::string("SwitchU ") + (havePublished ? m_latestRelease.version : SWITCHU_VERSION),
        notes,
        {{i18n.tr("button.ok", "OK"), [this]() {}, true}},
        0, {});
    focusManager().setFocus(m_dialog.get());
#endif
}

void WiiUMenuApp::offerUpdate(const update::UpdateClient::Release& release, bool automatic) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    // The courtesy of not interrupting belongs to the offer nobody asked for.
    // Applied to the button as well it blocked the button, because that button
    // lives inside the very screen the guard was testing for: pressing Install
    // in the SwitchU tab did nothing at all.
    if (updateRestartPending()) {
        DebugLog::log("[update] restart pending; offer suppressed");
        return;
    }
    if (automatic) {
        if (m_updateOffered)
            return;
        if (m_dialog->isActive() || (m_settings && m_settings->isActive())
            || (m_themeShop && m_themeShop->isActive()) || (m_gameDetails && m_gameDetails->isActive())
            || (m_gameGallery && m_gameGallery->isActive()) || (m_gameMods && m_gameMods->isActive())
            || (m_gameCheats && m_gameCheats->isActive())
            || (m_userSelect && m_userSelect->isActive()) || m_editMode)
            return;
    }
    m_updateOffered = true;

    auto& i18n = nxui::I18n::instance();
    const std::string notes = update::UpdateClient::condenseNotes(
        release.notes, i18n.activeLanguageTag());
    std::string body = i18n.tr("dialog.update_body", "A new version of SwitchU is available.");
    body += "\n\n" + i18n.tr("dialog.update_installed", "Installed") + ": " + SWITCHU_VERSION;
    body += "\n" + i18n.tr("dialog.update_new", "New") + ": " + release.version;
    if (!notes.empty())
        body += "\n\n" + notes;

    m_audio.playSfx(Sfx::ModalShow);
    raiseOverlay(m_dialog);
    m_dialog->show(
        i18n.tr("dialog.update_title", "Update available"),
        body,
        {
            {i18n.tr("dialog.update_later", "Later"), [this]() {}, true},
            {i18n.tr("dialog.update_install", "Update"), [this, release]() {
                 startUpdateDownload(release);
             }, false},
        },
        0, {});
    focusManager().setFocus(m_dialog.get());
#else
    (void)release;
#endif
}

void WiiUMenuApp::startUpdateDownload(const update::UpdateClient::Release& release) {
#ifdef SWITCHU_MENU
    if (m_updateDownloadFuture.valid()
        && m_updateDownloadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    auto shared = std::make_shared<UpdateDownload>();
    shared->release = release;
    shared->total = release.sizeBytes;
    m_updateDownload = shared;

    auto& i18n = nxui::I18n::instance();
    if (m_progressDialog) {
        m_progressDialog->setTheme(&m_theme);
        raiseOverlay(m_progressDialog);
        m_progressDialog->show(i18n.tr("dialog.update_title", "Update available"),
                               i18n.tr("dialog.update_downloading", "Downloading update..."),
                               -1.f);
        focusManager().setFocus(m_progressDialog.get());
    }

    m_updateDownloadFuture = m_threadPool.submit([shared]() {
        try {
            if (!ensureUpdateDirectory())
                throw std::runtime_error("could not create the update folder");
            std::remove(kUpdateArchive);
            const std::uint64_t written = themeshop::http::getToFile(
                shared->release.downloadUrl, kUpdateArchive,
                [shared](std::uint64_t received, std::uint64_t total) {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    shared->received = received;
                    if (total) shared->total = total;
                });

            // Only now, with the whole archive on the card and its length
            // agreeing with what the release published, is anything replaced.
            if (shared->release.sizeBytes && written != shared->release.sizeBytes)
                throw std::runtime_error("download size did not match the release");

            // Verify the whole archive before creating the ready marker or
            // replacing the daemon. Missing or malformed release digests are
            // refused when the release is parsed. A digest from the same HTTPS
            // API response protects against a corrupt/swapped download, not a
            // compromised publisher or release metadata.
            if (shared->release.sha256.empty())
                throw std::runtime_error("release has no published SHA-256 checksum");
            const std::string actual = fileSha256Hex(kUpdateArchive);
            if (actual != shared->release.sha256) {
                DebugLog::log("[update] SHA-256 mismatch");
                throw std::runtime_error("download did not match the published checksum");
            }
            DebugLog::log("[update] SHA-256 verified");

            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->extracting = true;
            }
            // Not unpacked here. SDL_ttf holds the font open for as long as
            // this menu runs, and the first version of this code destroyed that
            // font trying to replace it. The daemon applies the update at the
            // next boot, before the menu exists and before anything is open.
            //
            // The archive is still checked now, while there is a screen to
            // report on: a payload that reaches outside what SwitchU installs
            // is refused before it is ever staged, not at boot with nobody
            // watching.
            themeshop::ZipExtractPolicy policy;
            policy.allowExecutablePayload = true;
            policy.requiredRoots = {"atmosphere/", "switch/"};
            const auto inspection = themeshop::inspectZipFile(kUpdateArchive, policy);
            if (!inspection.success)
                throw std::runtime_error(inspection.error.empty()
                                             ? "the update package is not valid"
                                             : inspection.error);

            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->extracting = true;
            }
            // Written once the archive is whole and inspected: the daemon
            // treats this marker as the only sign that an update is ready.
            std::remove(kDaemonMarker);
            if (std::FILE* marker = std::fopen(kReadyMarker, "wb")) {
                std::fputs("ready", marker);
                std::fclose(marker);
            } else {
                throw std::runtime_error("could not mark the update as ready");
            }

            // And now the one file in the payload that can be replaced from
            // here: the daemon. Nothing holds it open -- Atmosphère read it at
            // boot and closed it -- and the extractor swaps every file in by
            // renaming a .part over it, so an interrupted write leaves the
            // running daemon untouched.
            //
            // This is what makes one restart enough. The daemon is what applies
            // an update, and it is inside the update: whichever copy of it the
            // console loaded at boot is the copy that unpacks its replacement,
            // so it can never be the new one. Putting it in place before the
            // restart turns that around -- the console comes up on the new
            // daemon, which then unpacks the menu half and goes straight into
            // it. If any of this fails the marker is simply not written, and
            // the daemon falls back to applying everything and restarting once
            // more by itself.
            switchu::install_txn::Txn daemonTxn(switchu::install_txn::kUpdateBackupRoot, kCardRoot);
            themeshop::ZipExtractPolicy daemonOnly = policy;
            daemonOnly.onlyRoots = {"atmosphere/"};
            themeshop::ZipExtractResult daemonApply;
            // An earlier pass or rollback that was cut short is the daemon's to
            // resolve at boot; this thread never undoes or builds on it. A held
            // backup of a finished update is simply extended by a new pass.
            const bool blocked =
                switchu::install_txn::interrupted(switchu::install_txn::kUpdateBackupRoot);
            const bool journalOk = !blocked && daemonTxn.begin();
            if (!journalOk) {
                // No journal means no way to undo: replace nothing from here. The boot
                // that applies the rest does it, under its own journal.
                daemonApply.success = false;
                daemonApply.error = blocked ? "an earlier update pass is waiting for recovery"
                                            : "could not open the rollback journal";
            } else {
                daemonOnly.txn = &daemonTxn;
                daemonApply = themeshop::extractZipFile(kUpdateArchive, kCardRoot, {}, daemonOnly);
                if (daemonApply.success && !daemonTxn.completePass())
                    daemonApply.success = false;
            }
            if (journalOk && !daemonApply.success) {
                std::string failed;
                const bool restored = switchu::install_txn::rollback(
                    switchu::install_txn::kUpdateBackupRoot, kCardRoot, &failed);
                DebugLog::log("[update] daemon half failed (%s); rolled back (%s)",
                              daemonApply.error.c_str(), restored ? "ok" : failed.c_str());
            }
            if (daemonApply.success) {
                switchu::commitSdCard("update daemon in place");
                if (std::FILE* marker = std::fopen(kDaemonMarker, "wb")) {
                    std::fputs(shared->release.version.c_str(), marker);
                    std::fclose(marker);
                    switchu::commitSdCard("update daemon marked");
                }
            } else {
                DebugLog::log("[update] daemon half not applied (%s); the boot that "
                              "applies the rest will restart once more",
                              daemonApply.error.c_str());
            }

            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->ok = true;
            shared->done = true;
        } catch (const std::exception& error) {
            std::remove(kUpdateArchive);
            std::remove(kReadyMarker);
            std::remove(kDaemonMarker);
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->error = error.what();
            shared->done = true;
        } catch (...) {
            std::remove(kUpdateArchive);
            std::remove(kReadyMarker);
            std::remove(kDaemonMarker);
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->error = "unknown update failure";
            shared->done = true;
        }
    });
#else
    (void)release;
#endif
}

void WiiUMenuApp::syncUpdateDownload() {
#ifdef SWITCHU_MENU
    auto shared = m_updateDownload;
    if (!shared)
        return;

    auto& i18n = nxui::I18n::instance();
    bool done = false, ok = false;
    std::string error;
    std::uint64_t received = 0, total = 0;
    bool extracting = false;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        done = shared->done;
        ok = shared->ok;
        error = shared->error;
        received = shared->received;
        total = shared->total;
        extracting = shared->extracting;
    }

    if (!done) {
        if (m_progressDialog && m_progressDialog->isActive()) {
            if (extracting) {
                m_progressDialog->updateState(
                    i18n.tr("dialog.update_applying", "Applying update..."), -1.f);
            } else if (total > 0) {
                m_progressDialog->updateState(
                    i18n.tr("dialog.update_downloading", "Downloading update..."),
                    (float)received / (float)total);
            }
        }
        return;
    }

    m_updateDownload.reset();
    if (ok)
        m_pendingUpdate = {};   // installed: there is nothing left to offer
    m_updateStatus = nxui::I18n::instance().tr(
        ok ? "dialog.update_done" : "dialog.update_failed",
        ok ? "Update downloaded. Restart the console to apply it. The first boot may take around half a minute."
           : "The update could not be installed.");
    // Releases the tab's busy state and shows the outcome there as well, not
    // only in the dialog the player is about to dismiss.
    publishUpdateState();
    if (m_progressDialog)
        m_progressDialog->hide();

    // The card holds the new files now; committing before the reboot keeps a
    // pulled power cable from leaving half of them behind.
    if (ok)
        quiesceWritersForPowerAction();

    m_audio.playSfx(Sfx::ModalShow);
    raiseOverlay(m_dialog);
    m_dialog->show(
        i18n.tr("dialog.update_title", "Update available"),
        ok ? i18n.tr("dialog.update_done",
                     "Update downloaded. Restart the console to apply it. The first boot may take around half a minute.")
           : i18n.tr("dialog.update_failed", "The update could not be installed."),
        {{i18n.tr("button.ok", "OK"), [this]() {}, true}},
        0, {});
    focusManager().setFocus(m_dialog.get());
    DebugLog::log("[update] %s%s", ok ? "staged for the next boot" : "failed: ",
                  ok ? "" : error.c_str());
#endif
}
