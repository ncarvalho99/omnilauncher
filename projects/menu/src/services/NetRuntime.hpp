#pragma once

namespace switchu::services::net {

// Shared, reference-counted nifm + BSD socket runtime.
//
// libnx reference-counts nifmInitialize but NOT the socket layer:
// socketInitialize answers "already initialized" to a second caller and
// socketExit() removes the "soc:" device unconditionally. Two independent users
// (Theme Shop HTTP and the NTP clock sync) each initialising and exiting it on
// their own therefore let one tear the device down while the other still holds
// an open descriptor; the next close() on that descriptor dereferences a null
// devoptab entry and the menu dies with a data abort. Every user goes through
// here instead, and the socket layer is only exited when the last one leaves.
//
// acquire() returns false if the runtime could not be brought up (nothing is
// held then, so do not call release()). Each successful acquire() must be
// paired with exactly one release(); Guard does this.
bool acquire();
void release();

class Guard {
public:
    Guard() : m_ok(acquire()) {}
    ~Guard() { if (m_ok) release(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    explicit operator bool() const { return m_ok; }

private:
    bool m_ok;
};

} // namespace switchu::services::net
