#pragma once

namespace switchu::daemon::update {

// Puts a staged launcher update in place, if the menu left one ready.
//
// Called once at boot, before the menu is launched: that is the only moment at
// which none of the files being replaced is open. Does nothing when there is no
// staged update, and gives up after a few failed attempts rather than delaying
// every boot. Returns whether the console should restart so the new daemon runs.
// Before any uninstall/update/menu work at boot: repair a pass or rollback
// interrupted by a power cut. False when the previous install could not be
// restored completely; the caller must not apply over it or start a menu.
bool recoverInterruptedUpdate();

bool applyStagedUpdate();

// The files an update replaced are kept in a rollback backup until the new menu
// has proven healthy (see boot_guard.hpp). These three manage that backup.
//
// True while a backup of a completed update is held.
bool backupPending();
// The new menu proved healthy: drop the backup.
void acceptUpdate();
// The new menu keeps failing: put the previous files back. True when every
// file was restored. The caller reboots afterwards.
bool rollbackUpdate();

} // namespace switchu::daemon::update
