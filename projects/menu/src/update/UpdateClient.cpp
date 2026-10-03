#include "UpdateClient.hpp"

#include "themeshop/ThemeHttp.hpp"

#include <nlohmann/json.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace update {
namespace {

struct Parsed {
    bool ok = false;
    int major = 0, minor = 0, patch = 0, fork = 0;
};

// Accepts the shape this project publishes: "1.1.0+fork.N", with the fork part
// optional so an upstream tag still compares sensibly.
Parsed parseVersion(const std::string& text) {
    Parsed out;
    const std::size_t plus = text.find('+');
    const std::string core = plus == std::string::npos ? text : text.substr(0, plus);
    std::istringstream stream(core);
    char dot = 0;
    if (!(stream >> out.major)) return out;
    if (!(stream >> dot) || dot != '.') return out;
    if (!(stream >> out.minor)) return out;
    if (!(stream >> dot) || dot != '.') return out;
    if (!(stream >> out.patch)) return out;

    if (plus != std::string::npos) {
        const std::string suffix = text.substr(plus + 1);
        const std::size_t dotAt = suffix.rfind('.');
        if (dotAt != std::string::npos) {
            try {
                out.fork = std::stoi(suffix.substr(dotAt + 1));
            } catch (...) {
                return out;   // unparsable fork revision: refuse the whole tag
            }
        }
    }
    out.ok = true;
    return out;
}

std::string stripTagPrefix(std::string tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    return tag;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Removes the Markdown a dialog cannot render: emphasis, links, headings and
// list markers. The text itself is left alone.
std::string plainText(std::string line) {
    line.erase(std::remove(line.begin(), line.end(), '*'), line.end());
    line.erase(std::remove(line.begin(), line.end(), '`'), line.end());
    line.erase(std::remove(line.begin(), line.end(), '#'), line.end());
    // [label](url) keeps only the label.
    for (std::size_t open = line.find('['); open != std::string::npos; open = line.find('[', open)) {
        const std::size_t close = line.find(']', open);
        if (close == std::string::npos) break;
        std::string label = line.substr(open + 1, close - open - 1);
        std::size_t end = close + 1;
        if (end < line.size() && line[end] == '(') {
            const std::size_t paren = line.find(')', end);
            if (paren != std::string::npos) end = paren + 1;
        }
        line = line.substr(0, open) + label + line.substr(end);
        open += label.size();
    }
    return trim(line);
}

} // namespace

bool UpdateClient::isNewer(const std::string& candidate, const std::string& current) {
    const Parsed a = parseVersion(stripTagPrefix(candidate));
    const Parsed b = parseVersion(current);
    if (!a.ok || !b.ok) return false;
    // OmniLauncher 1.0.0+ succeeding SwitchU 2.6.5:
    // Any valid 1.x version is accepted as newer when running SwitchU 2.6.5
    if (a.major == 1 && b.major == 2 && b.minor >= 6) {
        return true;
    }
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    if (a.patch != b.patch) return a.patch > b.patch;
    return a.fork > b.fork;
}

std::string UpdateClient::condenseNotes(const std::string& body, const std::string& languageTag) {
    // The body carries an English half and a Portuguese half separated by a
    // rule. Show the reader's own language when it is there.
    const bool portuguese = languageTag.rfind("pt", 0) == 0;
    std::string section = body;
    const std::size_t split = body.find("## Português");
    if (split != std::string::npos)
        section = portuguese ? body.substr(split) : body.substr(0, split);

    std::istringstream stream(section);
    std::string line;
    std::vector<std::string> kept;
    // The dialog scrolls, so the whole section is kept: headings give the notes
    // their shape and bullets carry the content. The cap only exists so a
    // runaway release body cannot allocate without bound.
    constexpr std::size_t kMaxChars = 6000;
    std::size_t total = 0;

    // A bullet in the changelog wraps over several lines and only the first one
    // starts with "- ". Dropping every line that is not a heading or a bullet
    // therefore threw away all but the opening clause of each item -- the notes
    // arrived on the console cut off mid-sentence, which is how this was
    // reported. The continuations have to be joined back on.
    //
    // tools/export_release_notes.py already does this for the notes shipped
    // inside the build. This is the same text arriving by the other route, and
    // it needs the same treatment.
    std::string pending;
    auto flushPending = [&]() {
        if (pending.empty())
            return;
        if (total + pending.size() <= kMaxChars) {
            total += pending.size();
            kept.push_back("- " + pending);
        }
        pending.clear();
    };

    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed == "---") {
            flushPending();
            continue;
        }
        const bool heading = trimmed.rfind("### ", 0) == 0 || trimmed.rfind("## ", 0) == 0;
        const bool bullet = trimmed.rfind("- ", 0) == 0;
        if (heading) {
            flushPending();
            const std::string text = plainText(trimmed);
            if (text.empty()) continue;
            if (total + text.size() > kMaxChars) break;
            total += text.size();
            kept.push_back(std::string(1, '\n') + text);
            continue;
        }
        if (bullet) {
            flushPending();
            pending = plainText(trimmed.substr(2));
            continue;
        }
        // A continuation of the bullet above. Outside one it is prose between
        // sections, which the dialog has no room for.
        if (!pending.empty())
            pending += " " + plainText(trimmed);
    }
    flushPending();

    std::string notes;
    for (const auto& entry : kept) {
        if (!notes.empty()) notes += "\n";
        notes += entry;
    }
    return notes;
}

void UpdateClient::check(nxui::ThreadPool& pool, std::string currentVersion) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t revision = ++m_revision;
    m_snapshot = {};
    m_snapshot.phase = Phase::Checking;
    m_snapshot.revision = revision;
    m_future = pool.submit([this, currentVersion = std::move(currentVersion), revision]() {
        Snapshot next;
        try {
            next = fetch(currentVersion, revision);
        } catch (const std::exception& error) {
            next.phase = Phase::Failed;
            next.error = error.what();
            next.revision = revision;
        } catch (...) {
            next.phase = Phase::Failed;
            next.error = "unknown update error";
            next.revision = revision;
        }
        std::lock_guard<std::mutex> resultLock(m_mutex);
        if (revision == m_revision)
            m_snapshot = std::move(next);
    });
}

UpdateClient::Snapshot UpdateClient::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

std::string UpdateClient::feedUrl() {
    // A file that does not exist on a normal install. When it is there, the
    // release feed is read from the address inside it instead of from GitHub,
    // which is what makes the whole update path testable end to end -- against
    // a payload of your own, as often as you like -- without publishing a
    // release to everybody first.
    std::ifstream override("sdmc:/config/OmniLaunch/update-source.txt");
    if (override) {
        std::string url;
        std::getline(override, url);
        while (!url.empty() && std::isspace((unsigned char)url.back()))
            url.pop_back();
        if (url.rfind("https://", 0) == 0)
            return url;
    }
    return kReleasesUrl;
}

UpdateClient::Snapshot UpdateClient::fetch(const std::string& currentVersion, std::uint64_t revision) {
    Snapshot result;
    result.revision = revision;

    nlohmann::json feed;
    bool usingOmniLauncher = false;
    try {
        feed = nlohmann::json::parse(
            themeshop::http::getText(kOmniLaunchReleasesUrl, {"Accept: application/vnd.github+json"}));
        if (feed.contains("tag_name") && feed["tag_name"].is_string()) {
            usingOmniLauncher = true;
        }
    } catch (...) {}

    if (!usingOmniLauncher) {
        try {
            feed = nlohmann::json::parse(
                themeshop::http::getText(kOmniLauncherReleasesUrl, {"Accept: application/vnd.github+json"}));
            if (feed.contains("tag_name") && feed["tag_name"].is_string()) {
                usingOmniLauncher = true;
            }
        } catch (...) {
            // Fall back to SwitchU feed if omnilauncher repository has no releases yet
        }
    }

    if (!usingOmniLauncher) {
        try {
            feed = nlohmann::json::parse(
                themeshop::http::getText(feedUrl(), {"Accept: application/vnd.github+json"}));
        } catch (const std::exception& e) {
            result.phase = Phase::Failed;
            result.error = e.what();
            return result;
        }
    }

    const auto tag = feed.find("tag_name");
    if (tag == feed.end() || !tag->is_string()) {
        result.phase = Phase::Failed;
        result.error = "release feed had no tag";
        return result;
    }

    Release release;
    release.version = stripTagPrefix(tag->get<std::string>());
    const auto name = feed.find("name");
    if (name != feed.end() && name->is_string()) release.title = name->get<std::string>();
    const auto body = feed.find("body");
    if (body != feed.end() && body->is_string()) release.notes = body->get<std::string>();

    // The sysmodule payload is the only asset worth offering; ignore anything
    // else a release might carry.
    const auto assets = feed.find("assets");
    if (assets != feed.end() && assets->is_array()) {
        for (const auto& asset : *assets) {
            if (!asset.is_object()) continue;
            const auto assetName = asset.find("name");
            const auto url = asset.find("browser_download_url");
            if (assetName == asset.end() || !assetName->is_string()) continue;
            if (url == asset.end() || !url->is_string()) continue;
            const std::string fileName = assetName->get<std::string>();
            if (fileName.size() < 4 || fileName.compare(fileName.size() - 4, 4, ".zip") != 0) continue;
            const std::string href = url->get<std::string>();
            if (href.rfind("https://", 0) != 0) continue;   // never a plain-HTTP payload
            release.downloadUrl = href;
            const auto size = asset.find("size");
            if (size != asset.end() && size->is_number_unsigned())
                release.sizeBytes = size->get<std::uint64_t>();
            break;
        }
    }

    // The release is carried back either way. Up to date is not the same as
    // having nothing to say: the tab shows what the installed version brought.
    const bool newer = isNewer(release.version, currentVersion) && !release.downloadUrl.empty();
    result.phase = newer ? Phase::Available : Phase::UpToDate;
    result.release = std::move(release);
    return result;
}

} // namespace update
