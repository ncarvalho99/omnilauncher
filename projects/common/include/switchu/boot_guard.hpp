#pragma once
// Menu start-up crash-loop guard: pure state machine and on-disk format.
//
// No libnx or filesystem calls here, so the transitions are unit-tested on the
// host (tests/boot_guard_test.cpp). The daemon owns persistence and the
// reboot/override side effects; the menu only reads the stage.
//
// Evidence the thresholds come from (daemon.log of a real crash loop): the menu
// reached MenuReady ~2.1 s after launch and the first frame ~2.4 s after the
// launch origin, then died ~3 s later with no MenuClosing, twelve times in
// about a minute. A clean exit (game launch, HOME, sleep, power) always sends
// MenuClosing from onDestroy first; a crash never does.
//
//   * A start-up failure is a menu exit WITHOUT MenuClosing before the menu has
//     proven itself healthy. Healthy = first frame submitted and still alive
//     kHealthyAfterFirstFrameMs later (3x the observed crash delay).
//   * kFastExitsBeforeSafe consecutive failures start the menu in safe mode.
//     3 is the threshold the daemon already used, and one or two failures can
//     be a transient (card hiccup, memory pressure after a game).
//   * kSafeExitsBeforeStock further failures in safe mode stop relaunching and
//     give the console back to stock qlaunch. 2 is enough: safe mode removes
//     every user-supplied asset, so a third failure is not about the theme.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace switchu::boot_guard {

inline constexpr std::uint32_t kFastExitsBeforeSafe = 3;
inline constexpr std::uint32_t kSafeExitsBeforeStock = 2;
inline constexpr std::uint32_t kHealthyAfterFirstFrameMs = 10000;

enum class Stage : std::uint32_t {
    Normal = 0,
    Safe = 1,
    Stock = 2,   // persisted only: the daemon is about to hand over to qlaunch
};

struct State {
    Stage stage = Stage::Normal;
    std::uint32_t fastExits = 0;       // consecutive start-up failures, Normal
    std::uint32_t safeFastExits = 0;   // consecutive start-up failures, Safe
    // A menu was started and has not yet proven healthy or exited cleanly. Written
    // at launch, so a crash that resets the whole console (the daemon then never
    // sees the exit) is still counted at the next daemon start.
    bool inFlight = false;

    bool operator==(const State& o) const {
        return stage == o.stage && fastExits == o.fastExits
            && safeFastExits == o.safeFastExits && inFlight == o.inFlight;
    }
};

enum class Verdict {
    Relaunch,        // launch the menu again (stage may have changed)
    FallBackToStock, // stop relaunching; disable the override and reboot
};

// The menu exited without MenuClosing before it was healthy.
inline Verdict onStartupFailure(State& s) {
    s.inFlight = false;
    if (s.stage == Stage::Normal) {
        if (++s.fastExits >= kFastExitsBeforeSafe) {
            s.stage = Stage::Safe;
            s.fastExits = 0;
            s.safeFastExits = 0;
        }
        return Verdict::Relaunch;
    }
    // Safe. (Stock never reaches here: the caller acts on it before any launch.)
    if (++s.safeFastExits >= kSafeExitsBeforeStock) {
        s.stage = Stage::Stock;
        return Verdict::FallBackToStock;
    }
    return Verdict::Relaunch;
}

// Daemon start, before the first menu launch. A marker left over from the
// previous boot means a menu was started and neither proved healthy nor closed
// cleanly: the console went down with it (hard reset, power loss, a crash that
// took everything). That is a start-up failure the daemon could not observe, so
// count it now. `verdict` receives the outcome of that failure.
//
// A persisted Stage::Stock is NOT interpreted here. The daemon is itself the
// qlaunch override, so it only runs while that override is enabled; Stock at
// boot therefore means "fall back was requested and the override still has to be
// disabled", which the caller does and then calls afterStockFallback().
inline State onDaemonBoot(State s, Verdict* verdict = nullptr) {
    if (verdict) *verdict = Verdict::Relaunch;
    if (s.stage != Stage::Stock && s.inFlight) {
        const Verdict v = onStartupFailure(s);
        if (verdict) *verdict = v;
    }
    return s;
}

// The override was disabled. If the player later re-enables it the daemon must
// come up in safe mode with fresh counters so Settings stays reachable.
inline State afterStockFallback() {
    State s;
    s.stage = Stage::Safe;
    return s;
}
// A menu process was started. True when the state changed and must be saved.
inline bool onLaunch(State& s) {
    if (s.inFlight) return false;
    s.inFlight = true;
    return true;
}

// The menu exited after sending MenuClosing: a deliberate exit (game, HOME, power),
// not a failure. The counters are left alone; only the marker clears.
inline bool onCleanExit(State& s) {
    if (!s.inFlight) return false;
    s.inFlight = false;
    return true;
}

// A run proved healthy. Counters clear; Safe stays sticky until the player
// leaves it on purpose, because the theme/asset that caused the loop is still
// installed and the next normal boot would crash again.
// Returns true when something changed (so the caller persists only then and a
// healthy boot costs zero card writes).
inline bool onHealthy(State& s) {
    if (s.fastExits == 0 && s.safeFastExits == 0 && !s.inFlight)
        return false;
    s.fastExits = 0;
    s.safeFastExits = 0;
    s.inFlight = false;
    return true;
}

inline State onLeaveSafeMode() { return State{}; }

inline constexpr const char* kHeader = "OmniLaunch boot guard v1";

inline const char* stageName(Stage s) {
    switch (s) {
    case Stage::Normal: return "normal";
    case Stage::Safe:   return "safe";
    case Stage::Stock:  return "stock";
    }
    return "normal";
}

inline std::string serialize(const State& s) {
    std::string out = kHeader;
    out += "\nstage=";
    out += stageName(s.stage);
    out += "\nfast=" + std::to_string(s.fastExits);
    out += "\nsafe_fast=" + std::to_string(s.safeFastExits);
    out += s.inFlight ? "\ninflight=1" : "\ninflight=0";
    out += "\nend=1";
    out += "\n";
    return out;
}

// Fails closed on a malformed file by returning false; the caller then uses a
// default State (the in-memory counters keep the ceiling working regardless).
inline bool parse(const std::string& text, State& out) {
    State s;
    std::size_t pos = 0;
    bool first = true;
    bool sawStage = false;
    bool sawFast = false, sawSafeFast = false, sawInflight = false, sawEnd = false;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            if (line != kHeader) return false;
            first = false;
            continue;
        }
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "stage") {
            if (val == "normal") s.stage = Stage::Normal;
            else if (val == "safe") s.stage = Stage::Safe;
            else if (val == "stock") s.stage = Stage::Stock;
            else return false;
            sawStage = true;
        } else if (key == "fast" || key == "safe_fast") {
            if (val.empty() || val.size() > 4) return false;
            std::uint32_t n = 0;
            for (char c : val) {
                if (c < '0' || c > '9') return false;
                n = n * 10 + static_cast<std::uint32_t>(c - '0');
            }
            (key == "fast" ? s.fastExits : s.safeFastExits) = n;
            (key == "fast" ? sawFast : sawSafeFast) = true;
        } else if (key == "inflight") {
            if (val != "0" && val != "1") return false;
            s.inFlight = val == "1";
            sawInflight = true;
        } else if (key == "end") {
            if (val != "1") return false;
            sawEnd = true;
        }
        // Unknown keys are ignored so a newer file does not disable the guard.
    }
    // Every field and the end marker must be present: a file cut short anywhere,
    // including exactly at a line boundary, must not parse as a valid older state.
    if (first || !sawStage || !sawFast || !sawSafeFast || !sawInflight || !sawEnd) return false;
    out = s;
    return true;
}


// What the daemon knows about the menu process it currently has running.
// Times are milliseconds on any monotonic clock.
struct Run {
    bool active = false;
    bool firstFrameSeen = false;
    bool closingSeen = false;
    bool healthy = false;
    std::uint64_t firstFrameMs = 0;

    void begin() { *this = Run{}; active = true; }
    void onFirstFrame(std::uint64_t nowMs) {
        if (active && !firstFrameSeen) { firstFrameSeen = true; firstFrameMs = nowMs; }
    }
    void onClosing() { if (active) closingSeen = true; }
    bool healthyDue(std::uint64_t nowMs) const {
        return active && !healthy && firstFrameSeen
            && nowMs - firstFrameMs >= kHealthyAfterFirstFrameMs;
    }
    void markHealthy() { healthy = true; }
    // The process ended. A start-up failure is an end with neither a proven
    // healthy run nor a deliberate MenuClosing.
    bool endIsStartupFailure() const { return active && !healthy && !closingSeen; }
    void end() { active = false; }
};
} // namespace switchu::boot_guard
