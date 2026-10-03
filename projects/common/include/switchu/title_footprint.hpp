#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "fs_remove.hpp"

namespace switchu::titles {

// Everywhere on the SD card a title id can live, outside the registered content
// that ns owns.
//
// nsDeleteApplicationCompletely() removes only what ns installed: the NCAs under
// Nintendo/Contents for an NSP or XCI. A port or a mod copied to the card by
// hand is invisible to it. Deleting a GTA V port installed as an NSP through DBI
// reported success, took its icon off the grid, and left 49 GB of romfs sitting
// in atmosphere/contents -- the free space on the card never moved. The same
// applies to any LayeredFS content: translations, texture packs, romfs
// replacements, and the cheats folder inside them.
//
// Both Atmosphere layouts are covered, plus the SX OS one, because ports are
// still published against all three and a card that has been through more than
// one CFW keeps whichever it was given.
inline bool isProtectedTitleId(std::uint64_t titleId) {
    // SwitchU's own daemon stands in for qlaunch. Removing its content would
    // take the running menu with it.
    constexpr std::uint64_t kSwitchUDaemon = 0x0100000000001000ULL;
    return titleId == 0 || titleId == kSwitchUDaemon;
}

// Existing directories only. Each candidate is one stat, so this is cheap enough
// to run while a confirmation dialog is being built; nothing here walks a tree.
inline std::vector<std::string> sdFootprint(std::uint64_t titleId) {
    std::vector<std::string> found;
    if (isProtectedTitleId(titleId))
        return found;

    char upper[17]{};
    char lower[17]{};
    std::snprintf(upper, sizeof(upper), "%016llX",
                  static_cast<unsigned long long>(titleId));
    std::snprintf(lower, sizeof(lower), "%016llx",
                  static_cast<unsigned long long>(titleId));

    std::vector<std::string> candidates;
    // FAT is case insensitive, so one spelling normally answers for both. Both
    // are tried anyway: the pair costs two stat calls, and removing one of them
    // makes the other report "already gone", which is the outcome asked for.
    for (const char* id : {upper, lower}) {
        candidates.emplace_back(std::string("sdmc:/atmosphere/contents/") + id);
        candidates.emplace_back(std::string("sdmc:/atmosphere/titles/") + id);
        candidates.emplace_back(std::string("sdmc:/sxos/titles/") + id);
    }
    // The menu's own per-title caches. Artwork is cosmetic and is rebuilt by a
    // later scan, so leaving it behind only orphans it.
    candidates.emplace_back(std::string("sdmc:/config/OmniLaunch/steamgriddb/") + upper); candidates.emplace_back(std::string("sdmc:/config/SwitchU/steamgriddb/") + upper);
    candidates.emplace_back(std::string("sdmc:/config/OmniLaunch/game_art/") + upper); candidates.emplace_back(std::string("sdmc:/config/SwitchU/game_art/") + upper);

    for (const std::string& path : candidates) {
        struct stat st {};
        if (stat(path.c_str(), &st) != 0)
            continue;
        if (std::find(found.begin(), found.end(), path) == found.end())
            found.push_back(path);
    }
    return found;
}

// One walk of the tree before removing it, so the progress bar has a total.
// Counting is far cheaper than deleting -- it reads directory entries and never
// writes -- but a 49 GB port still has enough files to be worth showing.
inline std::uint64_t countEntries(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return 1;
    std::uint64_t total = 1; // the directory itself
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return total;
    while (const dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            continue;
        std::string child = path;
        if (!child.empty() && child.back() != '/')
            child.push_back('/');
        child += name;
        total += countEntries(child);
    }
    closedir(dir);
    return total;
}

inline std::uint64_t countSdFootprint(std::uint64_t titleId) {
    std::uint64_t total = 0;
    for (const std::string& path : sdFootprint(titleId))
        total += countEntries(path);
    return total;
}

struct RemovalReport {
    int removed = 0;
    std::string firstFailure;
    bool ok() const { return firstFailure.empty(); }
};

inline RemovalReport removeSdFootprint(
    std::uint64_t titleId, std::atomic<std::uint64_t>* progress = nullptr) {
    RemovalReport report;
    for (const std::string& path : sdFootprint(titleId)) {
        std::string failed;
        if (removeRecursive(path, &failed, progress))
            ++report.removed;
        else if (report.firstFailure.empty())
            report.firstFailure = failed.empty() ? path : failed;
    }
    return report;
}

} // namespace switchu::titles
