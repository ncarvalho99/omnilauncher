#include "TabBuilders.hpp"
#include "core/DebugLog.hpp"
#include "smi_commands.hpp"
#include "services/NtpClient.hpp"
#include <nxui/core/I18n.hpp>
#include <switch.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

struct SystemPoolUsage {
    u64 total = 0;
    u64 used = 0;
};

bool querySystemPool(SystemPoolUsage& out) {
    // An svc outside the process's capability list is not an error return, it
    // is a crash. menu.json grants 0x6F; the homebrew build runs under
    // whatever its host title allows, so ask first.
    if (!envIsSyscallHinted(0x6F))
        return false;
    return R_SUCCEEDED(svcGetSystemInfo(&out.total, SystemInfoType_TotalPhysicalMemorySize,
                                        INVALID_HANDLE, PhysicalMemorySystemInfo_System))
        && R_SUCCEEDED(svcGetSystemInfo(&out.used, SystemInfoType_UsedPhysicalMemorySize,
                                        INVALID_HANDLE, PhysicalMemorySystemInfo_System))
        && out.total > 0 && out.used <= out.total;
}

// Below this the warning shows. The console that measured 10 MB free in 2.5.2
// was the one where launches were unstable.
constexpr u64 kLowSystemPoolMb = 16;

struct BootSysmodule {
    std::string titleId;
    std::string name;
};

// Games, their updates and add-ons live from here up. A modded library has a
// folder per game in atmosphere/contents and none of them can be a boot2
// sysmodule, so they are skipped on the name alone instead of costing two
// stat calls each on the card.
constexpr u64 kFirstApplicationId = 0x0100000000010000ULL;
constexpr u64 kLastApplicationId  = 0x01FFFFFFFFFFFFFFULL;

bool pathExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

// What Atmosphère starts at boot: a program in atmosphere/contents with
// flags/boot2.flag. The name comes from toolbox.json, the file most sysmodules
// ship for overlay managers; without one the title id is all there is.
std::vector<BootSysmodule> listBootSysmodules() {
    std::vector<BootSysmodule> out;
    DIR* dir = opendir("sdmc:/atmosphere/contents");
    if (!dir)
        return out;

    while (const dirent* entry = readdir(dir)) {
        std::string id = entry->d_name;
        if (id.size() != 16)
            continue;
        char* end = nullptr;
        const u64 tid = std::strtoull(id.c_str(), &end, 16);
        if (end != id.c_str() + id.size())
            continue;
        if (tid >= kFirstApplicationId && tid <= kLastApplicationId)
            continue;

        const std::string base = "sdmc:/atmosphere/contents/" + id;
        if (!pathExists(base + "/flags/boot2.flag"))
            continue;
        if (!pathExists(base + "/exefs.nsp") && !pathExists(base + "/exefs"))
            continue;

        BootSysmodule module;
        for (char& c : id)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        module.titleId = std::move(id);
        std::ifstream toolbox(base + "/toolbox.json");
        if (toolbox.is_open()) {
            const auto json = nlohmann::json::parse(toolbox, nullptr, false);
            if (json.is_object()) {
                const auto name = json.find("name");
                if (name != json.end() && name->is_string())
                    module.name = name->get<std::string>();
            }
        }
        out.push_back(std::move(module));
    }
    closedir(dir);

    // Named ones first, alphabetically; bare title ids after them.
    std::sort(out.begin(), out.end(), [](const BootSysmodule& a, const BootSysmodule& b) {
        if (a.name.empty() != b.name.empty())
            return !a.name.empty();
        return a.name.empty() ? a.titleId < b.titleId : a.name < b.name;
    });
    return out;
}

std::string uidToHex(const AccountUid& uid) {
    char buf[33] = {};
    std::snprintf(buf, sizeof(buf), "%016llX%016llX",
                  (unsigned long long)uid.uid[0],
                  (unsigned long long)uid.uid[1]);
    return std::string(buf);
}

struct ProfileOption {
    AccountUid uid{};
    std::string uidHex;
    std::string name;
};

std::vector<ProfileOption> listProfileOptions() {
    std::vector<ProfileOption> out;
    AccountUid uids[8] = {};
    s32 count = 0;
    if (R_FAILED(accountListAllUsers(uids, 8, &count)) || count <= 0)
        return out;

    out.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        AccountProfile profile{};
        if (R_FAILED(accountGetProfile(&profile, uids[i])))
            continue;

        AccountProfileBase base{};
        AccountUserData userData{};
        std::string name;
        if (R_SUCCEEDED(accountProfileGet(&profile, &userData, &base)))
            name = base.nickname;
        accountProfileClose(&profile);

        if (name.empty()) {
            char fallback[32] = {};
            std::snprintf(fallback, sizeof(fallback), "User %d", i + 1);
            name = fallback;
        }

        ProfileOption option;
        option.uid = uids[i];
        option.uidHex = uidToHex(uids[i]);
        option.name = std::move(name);
        out.push_back(std::move(option));
    }
    return out;
}

bool currentDateTimeValue(TabbedOverlayScreen::DateTimeEditorValue& value) {
    u64 timestamp = 0;
    TimeCalendarTime calendar{};
    TimeCalendarAdditionalInfo additional{};
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &timestamp)) ||
        R_FAILED(timeToCalendarTimeWithMyRule(timestamp, &calendar, &additional)))
        return false;
    value.year = calendar.year;
    value.month = calendar.month;
    value.day = calendar.day;
    value.hour = calendar.hour;
    value.minute = calendar.minute;
    return true;
}

bool setManualDateTime(
    SettingsScreen& screen,
    const TabbedOverlayScreen::DateTimeEditorValue& value) {
    switchu::smi::ManualDateTimeArgs args{};
    args.year = static_cast<uint32_t>(value.year);
    args.month = static_cast<uint32_t>(value.month);
    args.day = static_cast<uint32_t>(value.day);
    args.hour = static_cast<uint32_t>(value.hour);
    args.minute = static_cast<uint32_t>(value.minute);
    const Result rc = switchu::menu::smi_cmd::setManualDateTime(args);
    // Queued is not applied. The daemon answers with TimeSettingApplied once
    // it has set the clocks and read the time back, and that answer is what
    // says "updated" or not.
    if (R_SUCCEEDED(rc))
        return true;
    screen.requestToast(nxui::I18n::instance().tr(
        "settings.system.time_change_failed",
        "The date and time setting could not be changed."));
    return false;
}

} // namespace

SettingsScreen::Tab settings::tabs::SystemTab::build(SettingsScreen& screen) {
    using Tab = SettingsScreen::Tab;
    using SettingItem = SettingsScreen::SettingItem;
    using ItemType = SettingsScreen::ItemType;
    auto& i18n = nxui::I18n::instance();
    Tab t;
    t.name = i18n.tr("settings.tabs.system", "System");

    {
        // Keep SwitchU's own language first. The console language shown later
        // is read-only, and placing both among hardware details made users find
        // that value while missing the actual selector.
        SettingItem it;
        it.label = i18n.tr("settings.system.ui_language", "SwitchU Language");
        it.description = i18n.tr("settings.system.ui_language_desc",
                                 "Press A to choose the language used by SwitchU.");
        it.type = ItemType::Selector;

        std::vector<std::string> tags = nxui::I18n::supportedLanguageTags();
        it.options.reserve(tags.size());
        for (const auto& tag : tags) {
            if (tag == "auto") it.options.push_back(i18n.tr("common.auto", "Auto"));
            else it.options.push_back(i18n.tr(std::string("languages.") + tag, tag));
        }

        auto itTag = std::find(tags.begin(), tags.end(), screen.m_uiLanguageOverride);
        it.intVal = (itTag != tags.end()) ? (int)std::distance(tags.begin(), itTag) : 0;

        it.onChange = [&screen, tags = std::move(tags)](SettingItem& self) {
            int idx = std::clamp(self.intVal, 0, std::max(0, (int)tags.size() - 1));
            const std::string& selected = tags[idx];
            screen.m_uiLanguageOverride = selected;
            if (screen.m_uiLanguageCb) screen.m_uiLanguageCb(selected);
        };

        t.items.push_back(std::move(it));
    }

    {
        SetSysFirmwareVersion fw{};
        SettingItem it; it.label = i18n.tr("settings.system.firmware", "Firmware Version"); it.type = ItemType::Info;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            it.infoText = fw.display_version;
        else
            it.infoText = i18n.tr("common.na", "N/A");
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.system.custom_firmware", "Custom Firmware"); it.type = ItemType::Info;
        u64 cfg = 0;
        bool isAtmos = R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &cfg));
        if (isAtmos) {
            unsigned major = (cfg >> 56) & 0xFF;
            unsigned minor = (cfg >> 48) & 0xFF;
            unsigned micro = (cfg >> 40) & 0xFF;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Atmosphere %u.%u.%u", major, minor, micro);
            it.infoText = buf;
        } else {
            it.infoText = i18n.tr("settings.system.not_detected", "Not detected");
        }
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.system.emunand", "EmuNAND"); it.type = ItemType::Info;
        u64 cfg = 0;
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65007, &cfg)) && cfg != 0)
            it.infoText = i18n.tr("common.active", "Active");
        else
            it.infoText = i18n.tr("settings.system.inactive_sysnand", "Inactive / SysNAND");
        t.items.push_back(std::move(it));
    }

    {
        // Editable, like the stock menu has it. It was a read-only row, which
        // is a strange thing for a name the owner chooses.
        SetSysDeviceNickName nick{};
        SettingItem it; it.label = i18n.tr("settings.system.console_nickname", "Console Nickname"); it.type = ItemType::Action;
        it.description = i18n.tr("settings.system.console_nickname_desc",
                                 "The name other consoles and apps see.");
        if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)))
            it.infoText = nick.nickname;
        else
            it.infoText = i18n.tr("common.na", "N/A");
        it.onChange = [&screen](SettingItem& /* self */) {
            if (screen.m_consoleNicknameCb) screen.m_consoleNicknameCb();
        };
        t.items.push_back(std::move(it));
    }

    {
        SetSysSerialNumber sn{};
        SettingItem it; it.label = i18n.tr("settings.system.serial_number", "Serial Number"); it.type = ItemType::Info;
        if (R_SUCCEEDED(setsysGetSerialNumber(&sn)))
            it.infoText = sn.number;
        else
            it.infoText = i18n.tr("common.na", "N/A");
        t.items.push_back(std::move(it));
    }

    {
        TimeLocationName tz{};
        SettingItem it; it.label = i18n.tr("settings.system.timezone", "Timezone"); it.type = ItemType::Info;
        if (R_SUCCEEDED(timeGetDeviceLocationName(&tz)))
            it.infoText = tz.name;
        else
            it.infoText = i18n.tr("common.na", "N/A");
        t.items.push_back(std::move(it));
    }

    {
        bool automatic = true;
        const Result stateResult =
            setsysIsUserSystemClockAutomaticCorrectionEnabled(&automatic);
        SettingItem it;
        it.label = i18n.tr("settings.system.internet_time", "Synchronize Clock via Internet");
        it.description = i18n.tr(
            "settings.system.internet_time_desc",
            "Automatically correct the console clock using network time.");
        it.type = ItemType::Toggle;
        it.boolVal = R_SUCCEEDED(stateResult) ? automatic : false;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            const Result rc = switchu::menu::smi_cmd::setInternetTimeSync(self.boolVal);
            if (R_FAILED(rc)) {
                self.boolVal = !self.boolVal;
                screen.requestToast(nxui::I18n::instance().tr(
                    "settings.system.time_change_failed",
                    "The date and time setting could not be changed."));
                return;
            }
            if (self.boolVal) {
                screen.requestToast(nxui::I18n::instance().tr(
                    "settings.system.ntp_syncing",
                    "Synchronizing clock via Internet..."));
                // Only the failure is said here: ok means the network time
                // arrived and was handed to the daemon. Whether the clock
                // took it comes back as TimeSettingApplied.
                switchu::services::NtpClient::syncAsync([&screen](bool ok, uint64_t) {
                    if (!ok) {
                        screen.requestToast(nxui::I18n::instance().tr(
                            "settings.system.ntp_sync_failed",
                            "Could not synchronize clock. Check connection."));
                    }
                });
            }
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.system.ntp_sync_now", "Synchronize Clock Now");
        it.description = i18n.tr(
            "settings.system.ntp_sync_now_desc",
            "Synchronize with pool.ntp.org immediately.");
        it.type = ItemType::Action;
        it.buttonLabel = i18n.tr("settings.system.ntp_sync_button", "Sync");
        it.onChange = [&screen](SettingItem&) {
            screen.requestToast(nxui::I18n::instance().tr(
                "settings.system.ntp_syncing",
                "Synchronizing clock via Internet..."));
            switchu::services::NtpClient::syncAsync([&screen](bool ok, uint64_t) {
                if (!ok) {
                    screen.requestToast(nxui::I18n::instance().tr(
                        "settings.system.ntp_sync_failed",
                        "Could not synchronize clock. Check connection."));
                }
            });
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.system.manual_time", "Set Date and Time");
        it.description = i18n.tr(
            "settings.system.manual_time_desc",
            "Available when Internet clock synchronization is disabled.");
        it.type = ItemType::Action;
        it.buttonLabel = i18n.tr("settings.system.manual_time_button", "Change");
        it.onChange = [&screen](SettingItem&) {
            bool automatic = true;
            const Result stateRc =
                setsysIsUserSystemClockAutomaticCorrectionEnabled(&automatic);
            if (R_FAILED(stateRc) || automatic) {
                screen.requestToast(nxui::I18n::instance().tr(
                    "settings.system.disable_internet_time_first",
                    "Disable Internet clock synchronization first."));
                return;
            }
            TabbedOverlayScreen::DateTimeEditorValue initial;
            if (!currentDateTimeValue(initial)) {
                screen.requestToast(nxui::I18n::instance().tr(
                    "settings.system.time_change_failed",
                    "The date and time setting could not be changed."));
                return;
            }
            screen.requestDateTimeEditor(
                initial, [&screen](const auto& value) {
                    return setManualDateTime(screen, value);
                });
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.system.clock_12h", "12-Hour Clock");
        it.description = i18n.tr("settings.system.clock_12h_desc", "Show the home clock with AM and PM.");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_clockUse12Hour;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_clockUse12Hour = self.boolVal;
            if (screen.m_clockUse12HourCb)
                screen.m_clockUse12HourCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.system.usb30", "USB 3.0"); it.type = ItemType::Toggle;
        it.description = i18n.tr("settings.system.usb30_desc", "Enable USB 3.0 for faster transfer speeds.");
        bool val = false;
        setsysGetUsb30EnableFlag(&val);
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [&screen, &i18n](SettingItem& self) {
            setsysSetUsb30EnableFlag(self.boolVal);
            // The flag is read when the USB stack comes up, so the toggle does
            // nothing visible until the console restarts. Saying so beats a
            // switch that appears to have worked and did not.
            screen.requestToast(i18n.tr("settings.system.usb30_restart",
                                        "USB 3.0 changes take effect after a restart."),
                                3.0f);
        };
        t.items.push_back(std::move(it));
    }

    {
        // Both logs are held open while the console runs, so copying them over
        // MTP fails with "resource already in use". This closes them, which
        // leaves finished copies on the card that anything can read.
        SettingItem it; it.label = i18n.tr("settings.system.save_logs", "Save logs for copying");
        it.type = ItemType::Action;
        it.description = i18n.tr("settings.system.save_logs_desc",
                                 "Closes the current logs so they can be copied from config/OmniLaunch.");
        it.onChange = [&screen](SettingItem& /* self */) {
            if (screen.m_rotateLogsCb) screen.m_rotateLogsCb();
        };
        t.items.push_back(std::move(it));
    }

    {
        SetBatteryLot lot{};
        SettingItem it; it.label = i18n.tr("settings.system.battery_lot", "Battery Lot"); it.type = ItemType::Info;
        if (R_SUCCEEDED(setsysGetBatteryLot(&lot)))
            it.infoText = lot.lot;
        else
            it.infoText = i18n.tr("common.na", "N/A");
        t.items.push_back(std::move(it));
    }

    {
        // Opens the system's own account creation applet. Deleting an account
        // is deliberately not offered: it takes the save data with it, and the
        // system reserves that for Settings, behind its own confirmations.
        SettingItem it;
        it.label = i18n.tr("settings.system.add_user", "Add User");
        it.description = i18n.tr("settings.system.add_user_desc",
                                 "Create a new user account on this console.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_addUserCb) screen.m_addUserCb();
        };
        t.items.push_back(std::move(it));
    }

    {
        auto profiles = listProfileOptions();
        SettingItem it;
        it.label = i18n.tr("settings.system.default_profile", "Default Profile");
        it.description = i18n.tr("settings.system.default_profile_desc",
                                 "Launch games with this profile when possible.");
        it.type = ItemType::Selector;
        it.options.push_back(i18n.tr("settings.system.default_profile_ask", "Ask each time"));
        for (const auto& profile : profiles)
            it.options.push_back(profile.name);

        it.intVal = 0;
        if (!screen.m_defaultProfileUid.empty()) {
            for (int i = 0; i < (int)profiles.size(); ++i) {
                if (profiles[(size_t)i].uidHex == screen.m_defaultProfileUid) {
                    it.intVal = i + 1;
                    break;
                }
            }
        }

        it.onChange = [&screen, profiles = std::move(profiles)](SettingItem& self) {
            int idx = std::clamp(self.intVal, 0, (int)profiles.size());
            screen.m_defaultProfileUid = idx > 0 ? profiles[(size_t)(idx - 1)].uidHex : std::string();
            if (screen.m_defaultProfileCb)
                screen.m_defaultProfileCb(screen.m_defaultProfileUid);
        };

        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.system.console_language", "Console Language"); it.type = ItemType::Info;
        it.description = i18n.tr("settings.system.console_language_desc",
                                 "Read-only. Change this in Nintendo Switch System Settings.");
        u64 langCode = 0;
        if (R_SUCCEEDED(setGetSystemLanguage(&langCode))) {
            SetLanguage lang = SetLanguage_ENUS;
            if (R_SUCCEEDED(setMakeLanguage(langCode, &lang))) {
                std::string tag = nxui::I18n::detectSystemLanguageTag();
                it.infoText = i18n.tr(std::string("languages.") + tag, tag);
            }
        }
        if (it.infoText.empty()) it.infoText = i18n.tr("common.na", "N/A");
        t.items.push_back(std::move(it));
    }

    {
        // Read-only, and honestly so: this was a selector with no handler, so
        // the region appeared to change and nothing happened. The region a
        // console was sold as decides what the eShop and the system updater
        // will serve it, and writing it from here is not a setting this menu
        // should offer behind a d-pad press.
        SettingItem it; it.label = i18n.tr("settings.system.region", "Region"); it.type = ItemType::Info;
        it.description = i18n.tr("settings.system.region_desc",
                                 "Read-only. Change this in Nintendo Switch System Settings.");
        const char* names[] = {
            "settings.system.region_japan",     "settings.system.region_usa",
            "settings.system.region_europe",    "settings.system.region_australia",
            "settings.system.region_hong_kong", "settings.system.region_taiwan",
            "settings.system.region_south_korea",
        };
        const char* fallbacks[] = {
            "Japan", "USA", "Europe", "Australia", "Hong Kong", "Taiwan", "South Korea",
        };
        SetRegion reg = SetRegion_JPN;
        if (R_SUCCEEDED(setGetRegionCode(&reg))
            && (int)reg >= 0 && (int)reg < (int)(sizeof(names) / sizeof(names[0]))) {
            it.infoText = i18n.tr(names[(int)reg], fallbacks[(int)reg]);
        } else {
            it.infoText = i18n.tr("common.na", "N/A");
        }
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.system.auto_update", "Auto Update"); it.type = ItemType::Toggle;
        bool val = true;
        setsysGetAutoUpdateEnableFlag(&val);
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            setsysSetAutoUpdateEnableFlag(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.system.error_reporting", "Error Reporting"); it.type = ItemType::Toggle;
        bool val = false;
        setsysGetConsoleInformationUploadFlag(&val);
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            setsysSetConsoleInformationUploadFlag(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.system.memory_section", "Sysmodules and Memory");
        it.type = ItemType::Section;
        t.items.push_back(std::move(it));
    }

    {
        // The pool that runs out on a loaded console. Measured in 2.5.2 at 221
        // of 232 MB, with every sysmodule on the card drawing from it alongside
        // the services a game needs to start. Nothing showed it, so a game
        // failing to launch read as a menu bug rather than a card running too
        // much.
        SystemPoolUsage pool;
        const bool poolOk = querySystemPool(pool);
        const u64 totalMb = pool.total >> 20;
        const u64 usedMb = pool.used >> 20;
        const u64 freeMb = (pool.total - pool.used) >> 20;

        SettingItem bar;
        bar.label = i18n.tr("settings.system.system_pool", "System Memory Pool");
        bar.type = ItemType::Progress;
        bar.description = i18n.tr("settings.system.system_pool_desc",
                                  "Shared by every sysmodule on the card and by the services a game needs to start.");
        if (poolOk && freeMb < kLowSystemPoolMb) {
            bar.description += " " + i18n.tr("settings.system.system_pool_low",
                                             "Very little is left. If games fail to start, turn off the sysmodules you do not use.");
        }
        // Atmosphère moved 40 MB into this pool on older firmware and can move
        // 7 MB from 21.0.0 on, which is why the same card can be stable before
        // a system update and not after it.
        if (hosversionAtLeast(21, 0, 0)) {
            bar.description += " " + i18n.tr("settings.system.system_pool_fw21",
                                             "From firmware 21 on, Atmosphère can add only 7 MB to this pool.");
        }
        bar.floatVal = poolOk ? float((double)pool.used / (double)pool.total) : 0.f;
        bar.anim01 = bar.floatVal;
        bar.infoText = poolOk ? fmt::format("{} / {} MB", usedMb, totalMb)
                              : i18n.tr("common.na", "N/A");
        t.items.push_back(std::move(bar));

        SettingItem freeRow;
        freeRow.label = i18n.tr("settings.system.system_pool_free", "Free in the Pool");
        freeRow.type = ItemType::Info;
        freeRow.infoText = poolOk ? fmt::format("{} MB", freeMb) : i18n.tr("common.na", "N/A");
        t.items.push_back(std::move(freeRow));
    }

    {
        const auto modules = listBootSysmodules();

        SettingItem head;
        head.label = i18n.tr("settings.system.sysmodules", "Sysmodules Started at Boot");
        head.type = ItemType::Info;
        head.description = i18n.tr("settings.system.sysmodules_desc",
                                   "Each one runs from boot and takes its memory from the pool above.");
        head.infoText = modules.empty() ? i18n.tr("settings.system.sysmodules_none", "None")
                                        : std::to_string(modules.size());
        t.items.push_back(std::move(head));

        for (const auto& module : modules) {
            SettingItem row;
            row.type = ItemType::Info;
            row.label = module.name.empty() ? module.titleId : module.name;
            row.infoText = module.name.empty() ? std::string() : module.titleId;
            t.items.push_back(std::move(row));
        }
    }

    return t;
}
