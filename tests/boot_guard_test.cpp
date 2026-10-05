#include "switchu/boot_guard.hpp"

#include <cassert>
#include <string>

using namespace switchu::boot_guard;

int main() {
    // Normal -> Safe after kFastExitsBeforeSafe consecutive failures.
    {
        State s;
        for (std::uint32_t i = 1; i < kFastExitsBeforeSafe; ++i) {
            assert(onStartupFailure(s) == Verdict::Relaunch);
            assert(s.stage == Stage::Normal && s.fastExits == i);
        }
        assert(onStartupFailure(s) == Verdict::Relaunch);
        assert(s.stage == Stage::Safe && s.fastExits == 0 && s.safeFastExits == 0);
    }
    // Safe -> Stock after kSafeExitsBeforeStock failures, and only then.
    {
        State s;
        s.stage = Stage::Safe;
        for (std::uint32_t i = 1; i < kSafeExitsBeforeStock; ++i)
            assert(onStartupFailure(s) == Verdict::Relaunch);
        assert(s.stage == Stage::Safe);
        assert(onStartupFailure(s) == Verdict::FallBackToStock);
        assert(s.stage == Stage::Stock);
    }
    // Healthy clears counters, reports a change only when there was one, and
    // keeps Safe sticky.
    {
        State s;
        assert(!onHealthy(s));                    // healthy boot: no write
        s.fastExits = 2;
        assert(onHealthy(s) && s.fastExits == 0);
        s.stage = Stage::Safe;
        s.safeFastExits = 1;
        assert(onHealthy(s) && s.safeFastExits == 0 && s.stage == Stage::Safe);
    }
    // A healthy run between failures restarts the count (consecutive only).
    {
        State s;
        onStartupFailure(s);
        onStartupFailure(s);
        onHealthy(s);
        onStartupFailure(s);
        onStartupFailure(s);
        assert(s.stage == Stage::Normal && s.fastExits == 2);
    }
    // Stale in-flight marker at daemon boot = a start-up failure nobody saw.
    {
        State s;
        s.inFlight = true;
        Verdict v{};
        State r = onDaemonBoot(s, &v);
        assert(v == Verdict::Relaunch && !r.inFlight && r.fastExits == 1);
        State clean;
        clean.fastExits = 2;
        r = onDaemonBoot(clean, &v);
        assert(r == clean && v == Verdict::Relaunch);          // no marker: untouched
        // Marker in Safe at the failure limit falls back to stock.
        State sf;
        sf.stage = Stage::Safe;
        sf.safeFastExits = kSafeExitsBeforeStock - 1;
        sf.inFlight = true;
        r = onDaemonBoot(sf, &v);
        assert(v == Verdict::FallBackToStock && r.stage == Stage::Stock);
        // A pending Stock is left for the caller; it is never re-counted.
        State pending;
        pending.stage = Stage::Stock;
        r = onDaemonBoot(pending, &v);
        assert(r == pending && v == Verdict::Relaunch);
        // After the override was disabled, a later re-enable starts in safe mode.
        r = afterStockFallback();
        assert(r.stage == Stage::Safe && r.fastExits == 0 && r.safeFastExits == 0 && !r.inFlight);
    }
    // Launch / clean exit marker bookkeeping.
    {
        State s;
        assert(onLaunch(s) && s.inFlight);
        assert(!onLaunch(s));                                  // already marked: no write
        assert(onCleanExit(s) && !s.inFlight);
        assert(!onCleanExit(s));
        assert(onLaunch(s) && onHealthy(s) && !s.inFlight);
    }
    // Run tracker: failure only when no healthy proof and no MenuClosing.
    {
        Run r;
        r.begin();
        assert(r.endIsStartupFailure());                       // died before anything
        r.onFirstFrame(1000);
        assert(!r.healthyDue(1000 + kHealthyAfterFirstFrameMs - 1));
        assert(r.healthyDue(1000 + kHealthyAfterFirstFrameMs));
        r.markHealthy();
        assert(!r.endIsStartupFailure() && !r.healthyDue(99999));
        Run c;
        c.begin();
        c.onClosing();
        assert(!c.endIsStartupFailure());                      // deliberate exit
        Run idle;
        assert(!idle.endIsStartupFailure());                   // nothing running
        Run noFrame;
        noFrame.begin();
        assert(!noFrame.healthyDue(1u << 30));                 // no first frame, never healthy
    }    // Leaving safe mode resets everything.
    {
        State s;
        s.stage = Stage::Safe;
        s.safeFastExits = 1;
        s.inFlight = true;
        assert(onLeaveSafeMode() == State{});
    }
    // Round trip, including every stage.
    for (Stage st : {Stage::Normal, Stage::Safe, Stage::Stock}) {
        State a;
        a.stage = st;
        a.fastExits = 2;
        a.safeFastExits = 1;
        State b;
        assert(parse(serialize(a), b) && a == b);
    }
    // Malformed input fails closed and leaves the output untouched.
    {
        State out;
        out.fastExits = 7;
        assert(!parse("", out));
        assert(!parse("wrong header\nstage=safe\n", out));
        assert(!parse(std::string(kHeader) + "\n", out));              // no stage
        assert(!parse(std::string(kHeader) + "\nstage=bogus\n", out));
        assert(!parse(std::string(kHeader) + "\nstage=safe\nfast=x\n", out));
        assert(!parse(std::string(kHeader) + "\nstage=safe\nfast=99999\n", out));
        assert(!parse(std::string(kHeader) + "\nstage=safe\nnoeq\n", out));
        assert(out.fastExits == 7);
        // Truncated mid-line is not accepted as a different valid state.
        assert(!parse(std::string(kHeader) + "\nstage=sa", out));
        // Cut exactly at a line boundary (no end marker) is rejected, not read as an older state.
        {
            const std::string full = serialize(State{});
            const std::string cut = full.substr(0, full.find("\nsafe_fast=") + 1);
            assert(!parse(cut, out));
            assert(!parse(full.substr(0, full.size() - 6), out));
        }
        // CRLF and unknown keys are tolerated.
        assert(parse(std::string(kHeader) + "\r\nstage=safe\r\nfuture=1\r\nfast=1\r\nsafe_fast=0\r\ninflight=0\r\nend=1\r\n", out));
        assert(out.stage == Stage::Safe && out.fastExits == 1);
    }
    return 0;
}
