#pragma once
#include <switchu/boot_guard.hpp>

namespace switchu::daemon::guard_store {

// Plain-text state next to config.json. Its own file so nothing the theme,
// settings or updater code rewrites can reset the counters.
inline constexpr const char* kPath = "sdmc:/config/OmniLaunch/boot_guard.state";
inline constexpr const char* kTemporary = "sdmc:/config/OmniLaunch/boot_guard.state.tmp";

// Missing, unreadable or malformed state is the default (Normal, zero): the
// in-memory counters still enforce the ceiling for the running boot.
boot_guard::State load();

// Write-then-rename, committed to the card. False when it could not be made
// durable; callers must not rely on the next boot seeing the new state.
bool save(const boot_guard::State& state);

// Renames the qlaunch override exefs.nsp to exefs.nsp.disabled, the same
// reversible switch the Manager uses. Nothing is deleted. False when the
// override is absent, a disabled copy already exists, or the rename fails.
bool disableOverride();

} // namespace switchu::daemon::guard_store
