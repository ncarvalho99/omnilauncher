#include "TabBuilders.hpp"
#include <nxui/core/I18n.hpp>
#include <switch.h>
#include <algorithm>

namespace {
// libnx documents SetSysTvSettings::flags as "bitmask with TvFlag" without
// declaring the enum. Bit 3 is PreventsScreenBurnIn, per switchbrew's Settings
// services page; bits 0-2 are Allows4k, Allows3d and AllowsCec.
//
// This toggle used to write bit 0 of SetSysBacklightSettings::auto_brightness_flags,
// which is the stored auto-brightness flag -- the setting the toggle directly
// above it already owns. Turning burn-in reduction on quietly changed
// auto-brightness instead, and burn-in reduction itself was never touched.
constexpr u32 kTvFlagPreventsScreenBurnIn = 1u << 3;
} // namespace

SettingsScreen::Tab settings::tabs::DisplayTab::build(SettingsScreen& screen) {
    using Tab = SettingsScreen::Tab;
    using SettingItem = SettingsScreen::SettingItem;
    using ItemType = SettingsScreen::ItemType;
    auto& i18n = nxui::I18n::instance();
    Tab t;
    t.name = i18n.tr("settings.tabs.display", "Display");

    {
        SettingItem it; it.label = i18n.tr("settings.display.brightness", "Brightness"); it.type = ItemType::Slider;
        float val = 0.5f;
        lblGetCurrentBrightnessSetting(&val);
        it.floatVal = val;
        it.anim01 = std::clamp(val, 0.f, 1.f);
        it.onChange = [](SettingItem& self) {
            lblSetCurrentBrightnessSetting(self.floatVal);
            // lbl holds the new value for the running session only. Without
            // this the slider moved, the panel dimmed, and the console came
            // back from a reboot at the old brightness.
            lblSaveCurrentSetting();
        };
        t.items.push_back(std::move(it));
    }

    // Glass sharpness, background speed and background blur used to sit here.
    // They are appearance, not display hardware, and nobody looked for them
    // next to brightness and burn-in; they live in the Themes screen now,
    // beside the other things that change how the menu looks. The callbacks on
    // SettingsScreen stay -- the config still drives them at startup.

    {
        SettingItem it; it.label = i18n.tr("settings.display.auto_brightness", "Auto-Brightness"); it.type = ItemType::Toggle;
        it.description = i18n.tr("settings.display.auto_brightness_desc", "Adjust brightness automatically based on ambient light.");
        bool val = false;
        lblIsAutoBrightnessControlEnabled(&val);
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            if (self.boolVal)
                lblEnableAutoBrightnessControl();
            else
                lblDisableAutoBrightnessControl();
            lblSaveCurrentSetting();
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.display.burn_in", "Screen Burn-In Reduction"); it.type = ItemType::Toggle;
        it.description = i18n.tr("settings.display.burn_in_desc", "Reduce screen burn-in during long usage.");
        bool val = false;
        SetSysTvSettings tv{};
        if (R_SUCCEEDED(setsysGetTvSettings(&tv)))
            val = (tv.flags & kTvFlagPreventsScreenBurnIn) != 0;
        it.boolVal = val;
        it.anim01 = val ? 1.f : 0.f;
        it.onChange = [](SettingItem& self) {
            SetSysTvSettings tv{};
            if (R_SUCCEEDED(setsysGetTvSettings(&tv))) {
                if (self.boolVal)
                    tv.flags |= kTvFlagPreventsScreenBurnIn;
                else
                    tv.flags &= ~kTvFlagPreventsScreenBurnIn;
                setsysSetTvSettings(&tv);
            }
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.audio.tv_resolution", "TV Resolution"); it.type = ItemType::Selector;
        it.description = i18n.tr("settings.audio.tv_resolution_desc", "Adjust HDMI resolution. Auto uses recommended value.");
        // In the order the system stores them: 0 Auto, 1 1080p, 2 720p, 3 480p.
        // The list used to read Auto, 720p, 1080p against those same indices,
        // so choosing 720p set the console to 1080p and the other way round,
        // and 480p could not be chosen at all.
        it.options = {
            i18n.tr("common.auto", "Auto"),
            i18n.tr("settings.audio.res_1080p", "1080p"),
            i18n.tr("settings.audio.res_720p", "720p"),
            i18n.tr("settings.audio.res_480p", "480p")
        };
        SetSysTvSettings tv{};
        if (R_SUCCEEDED(setsysGetTvSettings(&tv)))
            it.intVal = std::clamp((int)tv.tv_resolution, 0, 3);
        it.onChange = [](SettingItem& self) {
            SetSysTvSettings tv{};
            if (R_SUCCEEDED(setsysGetTvSettings(&tv))) {
                tv.tv_resolution = (s32)self.intVal;
                setsysSetTvSettings(&tv);
            }
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it; it.label = i18n.tr("settings.audio.hdmi_rgb_range", "HDMI RGB Range"); it.type = ItemType::Selector;
        it.description = i18n.tr("settings.audio.hdmi_rgb_range_desc", "Full for PC monitors, Limited for most TVs.");
        it.options = {
            i18n.tr("common.auto", "Auto"),
            i18n.tr("settings.audio.full", "Full"),
            i18n.tr("settings.audio.limited", "Limited")
        };
        SetSysTvSettings tv{};
        if (R_SUCCEEDED(setsysGetTvSettings(&tv)))
            it.intVal = std::clamp((int)tv.rgb_range, 0, 2);
        it.onChange = [](SettingItem& self) {
            SetSysTvSettings tv{};
            if (R_SUCCEEDED(setsysGetTvSettings(&tv))) {
                tv.rgb_range = (s32)self.intVal;
                setsysSetTvSettings(&tv);
            }
        };
        t.items.push_back(std::move(it));
    }

    // {
    //     SettingItem it; it.label = i18n.tr("settings.display.tv_screen_size", "TV Screen Size"); it.type = ItemType::Slider;
    //     it.description = i18n.tr("settings.display.tv_screen_size_desc", "Adjust overscan so the full image is visible.");
    //     SetSysTvSettings tv{};
    //     if (R_SUCCEEDED(setsysGetTvSettings(&tv)))
    //         it.floatVal = tv.underscan / 100.f;
    //     else
    //         it.floatVal = 1.f;
    //     it.anim01 = std::clamp(it.floatVal, 0.f, 1.f);
    //     it.onChange = [](SettingItem& self) {
    //         SetSysTvSettings tv{};
    //         if (R_SUCCEEDED(setsysGetTvSettings(&tv))) {
    //             tv.underscan = (u32)(self.floatVal * 100.f);
    //             setsysSetTvSettings(&tv);
    //         }
    //     };
    //     t.items.push_back(std::move(it));
    // }

    {
        SettingItem it; it.label = i18n.tr("settings.display.album_storage", "Primary Album Storage"); it.type = ItemType::Selector;
        it.description = i18n.tr("settings.display.album_storage_desc", "Where screenshots and videos are saved.");
        it.options = {
            i18n.tr("settings.display.album_sd", "SD Card"),
            i18n.tr("settings.display.album_nand", "NAND")
        };
        SetSysPrimaryAlbumStorage storage = SetSysPrimaryAlbumStorage_SdCard;
        setsysGetPrimaryAlbumStorage(&storage);
        it.intVal = (storage == SetSysPrimaryAlbumStorage_SdCard) ? 0 : 1;
        it.onChange = [](SettingItem& self) {
            SetSysPrimaryAlbumStorage s = (self.intVal == 0)
                ? SetSysPrimaryAlbumStorage_SdCard
                : SetSysPrimaryAlbumStorage_Nand;
            setsysSetPrimaryAlbumStorage(s);
        };
        t.items.push_back(std::move(it));
    }

#ifdef SWITCHU_DEBUG_UI
    // UI Wireframe draws an outline around every Box widget. It is a layout
    // debugging aid and it shipped visible to end users by mistake, so it is
    // now built only in debug builds, next to the other SWITCHU_DEBUG_UI
    // tooling. The item is kept intact rather than deleted: to bring it back
    // for everyone, remove this guard. The rest of the plumbing
    // (SettingsScreen::m_wireframeEnabled / m_wireframeCb,
    // WiiUMenuApp::m_showWireframe, Renderer::setBoxWireframeEnabled) is
    // untouched and stays compiled in; without this item nothing can enable
    // it in a release build, because the flag is never persisted to config
    // and defaults to false. The `settings.display.ui_wireframe*` translations
    // were removed from romfs/i18n so the shipped assets no longer advertise a
    // setting nobody can reach; a debug build falls back to the English
    // literals below.
    {
        SettingItem it; it.label = i18n.tr("settings.display.ui_wireframe", "UI Wireframe"); it.type = ItemType::Toggle;
        it.description = i18n.tr("settings.display.ui_wireframe_desc", "Show outlines of all Box widgets for layout debugging.");
        it.boolVal = screen.m_wireframeEnabled;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_wireframeEnabled = self.boolVal;
            if (screen.m_wireframeCb) screen.m_wireframeCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }
#else
    (void)screen;
#endif

    return t;
}
