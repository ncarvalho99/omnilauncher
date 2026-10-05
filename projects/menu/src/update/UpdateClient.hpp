#pragma once

#include <cstdint>
#include <future>
#include <mutex>
#include <string>

namespace nxui { class ThreadPool; }

namespace update {

// Reads the fork's own GitHub releases and says whether a newer build exists.
//
// Only the published release feed is consulted, and only over HTTPS. Nothing is
// downloaded by this class: it answers what the newest version is, what it
// changed, and where its payload lives. Fetching and applying that payload is a
// separate, explicit step the player has to accept.
class UpdateClient {
public:
    static constexpr const char* kReleasesUrl =
        "https://api.github.com/repos/ncarvalho99/SwitchU/releases/latest";
    static constexpr const char* kOmniLauncherReleasesUrl =
        "https://api.github.com/repos/ncarvalho99/omnilauncher/releases/latest";
    static constexpr const char* kOmniLaunchReleasesUrl =
        "https://api.github.com/repos/ncarvalho99/omnilaunch/releases/latest";

    struct Release {
        std::string version;      // "1.1.0+fork.8"
        std::string title;        // release name as published
        std::string notes;        // condensed for a dialog, already plain text
        std::string downloadUrl;  // the sysmodule zip asset
        std::uint64_t sizeBytes = 0;
        // Lower-case hex SHA-256 published with the release ZIP asset.
        // A missing or malformed digest prevents offering this update.
        std::string sha256;
    };

    enum class Phase { Idle, Checking, UpToDate, Available, Failed };

    struct Snapshot {
        Phase phase = Phase::Idle;
        Release release;
        std::string error;
        std::uint64_t revision = 0;
    };

    void check(nxui::ThreadPool& pool, std::string currentVersion);
    Snapshot snapshot() const;

    // "1.1.0+fork.8" is newer than "1.1.0+fork.7", and 1.2.0 beats any fork of
    // 1.1.0. A version that cannot be parsed is never treated as newer, so a
    // malformed tag cannot talk the console into downloading anything.
    static bool isNewer(const std::string& candidate, const std::string& current);

    // Release bodies are long, bilingual Markdown. The dialog has no scrolling,
    // so this keeps the leading bullet points of the reader's half and drops the
    // markup rather than showing seven thousand characters of it.
    static std::string condenseNotes(const std::string& body, const std::string& languageTag);

    // Normally the fork's release feed; overridden by a file on the card so the
    // update path can be tested without publishing anything.
    static std::string feedUrl();

private:
    Snapshot fetch(const std::string& currentVersion, std::uint64_t revision);

    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
    std::future<void> m_future;
    std::uint64_t m_revision = 0;
};

} // namespace update
