#include "ThemeShopTabBuilders.hpp"

#include <nxui/core/I18n.hpp>
#include <algorithm>
#include <cmath>

ThemeShopScreen::Tab themeshop::tabs::OptionsTab::build(ThemeShopScreen& screen) {
    using Tab = ThemeShopScreen::Tab;
    using SettingItem = ThemeShopScreen::SettingItem;
    using ItemType = ThemeShopScreen::ItemType;
    auto& i18n = nxui::I18n::instance();

    Tab t;
    t.name = i18n.tr("themeshop.tabs.options", "Options");

    {
        SettingItem it;
        it.label = i18n.tr("settings.music.menu_music", "Menu Music");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_musicEnabled;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_musicEnabled = self.boolVal;
            if (screen.m_musicEnabledCb) screen.m_musicEnabledCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.music.music_volume", "Music Volume");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_musicVolume, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_musicVolume = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_musicVolumeCb) screen.m_musicVolumeCb(screen.m_musicVolume);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.music.sfx_volume", "SFX Volume");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_sfxVolume, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_sfxVolume = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_sfxVolumeCb) screen.m_sfxVolumeCb(screen.m_sfxVolume);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.glass_sharpness", "Glass sharpness");
        it.description = i18n.tr("settings.display.glass_sharpness_desc",
                                 "How clearly the screen behind menus shows through them.");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_glassSharpness, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_glassSharpness = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_glassSharpnessCb) screen.m_glassSharpnessCb(screen.m_glassSharpness);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.background_speed", "Background animation speed");
        it.description = i18n.tr("settings.display.background_speed_desc",
                                 "How fast the shapes drift. Centre is the theme's own pace.");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_backgroundSpeed, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_backgroundSpeed = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_backgroundSpeedCb) screen.m_backgroundSpeedCb(screen.m_backgroundSpeed);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.background_blur", "Background blur");
        it.description = i18n.tr("settings.display.background_blur_desc",
                                 "Soften the wallpaper and the shapes drifting over it.");
        it.type = ItemType::Slider;
        it.floatVal = std::clamp(screen.m_backgroundBlur, 0.f, 1.f);
        it.anim01 = it.floatVal;
        it.onChange = [&screen](SettingItem& self) {
            self.anim01 = self.floatVal;
            screen.m_backgroundBlur = std::clamp(self.floatVal, 0.f, 1.f);
            if (screen.m_backgroundBlurCb) screen.m_backgroundBlurCb(screen.m_backgroundBlur);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.launcher_view", "Launcher View");
        it.description = i18n.tr("settings.display.launcher_view_desc",
                                 "Choose between standard Grid, Dynamic Line carousel, 3D Flow, or 3D Shelf.");
        it.type = ItemType::Selector;
        it.options = {
            i18n.tr("settings.layout.grid", "Grid"),
            i18n.tr("settings.layout.dynamic_line", "Dynamic Line"),
            i18n.tr("settings.layout.flow", "Flow (3D)"),
            i18n.tr("settings.layout.shelf", "Shelf (3D)"),
        };
        it.intVal = screen.m_layoutMode == AppLayoutMode::Shelf ? 3
                  : screen.m_layoutMode == AppLayoutMode::Flow  ? 2
                  : screen.m_layoutMode == AppLayoutMode::DynamicLine ? 1 : 0;
        it.onChange = [&screen](SettingItem& self) {
            AppLayoutMode mode = self.intVal == 3 ? AppLayoutMode::Shelf
                               : self.intVal == 2 ? AppLayoutMode::Flow
                               : self.intVal == 1 ? AppLayoutMode::DynamicLine
                                                  : AppLayoutMode::Grid;
            screen.m_layoutMode = mode;
            if (screen.m_layoutModeCb) screen.m_layoutModeCb(mode);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.grid_columns", "Home Grid Columns");
        it.type = ItemType::Slider;
        it.sliderSteps = 5;
        int cols = std::clamp(screen.m_gridColumns, 3, 8);
        it.floatVal = (float)(cols - 3) / 5.f;
        it.anim01 = it.floatVal;
        it.infoText = std::to_string(cols);
        it.onChange = [&screen](SettingItem& self) {
            int cols = std::clamp(3 + (int)std::round(std::clamp(self.floatVal, 0.f, 1.f) * 5.f), 3, 8);
            self.floatVal = (float)(cols - 3) / 5.f;
            self.anim01 = self.floatVal;
            self.infoText = std::to_string(cols);
            screen.m_gridColumns = cols;
            if (screen.m_gridColumnsCb) screen.m_gridColumnsCb(cols);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.grid_rows", "Home Grid Rows");
        it.type = ItemType::Slider;
        it.sliderSteps = 3;
        int rows = std::clamp(screen.m_gridRows, 2, 5);
        it.floatVal = (float)(rows - 2) / 3.f;
        it.anim01 = it.floatVal;
        it.infoText = std::to_string(rows);
        it.onChange = [&screen](SettingItem& self) {
            int rows = std::clamp(2 + (int)std::round(std::clamp(self.floatVal, 0.f, 1.f) * 3.f), 2, 5);
            self.floatVal = (float)(rows - 2) / 3.f;
            self.anim01 = self.floatVal;
            self.infoText = std::to_string(rows);
            screen.m_gridRows = rows;
            if (screen.m_gridRowsCb) screen.m_gridRowsCb(rows);
        };
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.display.dynamic_pages", "Dynamic Pages");
        it.description = i18n.tr("settings.display.dynamic_pages_desc",
                                 "Adjust home pages to fit installed games and homebrew instead of keeping 8 fixed pages.");
        it.type = ItemType::Toggle;
        it.boolVal = screen.m_dynamicPages;
        it.anim01 = it.boolVal ? 1.f : 0.f;
        it.onChange = [&screen](SettingItem& self) {
            screen.m_dynamicPages = self.boolVal;
            if (screen.m_dynamicPagesCb) screen.m_dynamicPagesCb(self.boolVal);
        };
        t.items.push_back(std::move(it));
    }

    // Asked for by a player who had just made a shortcut and could only get the
    // grid to show it by rebooting the console.
    {
        SettingItem it;
        it.label = i18n.tr("settings.display.reload_grid", "Reload games and shortcuts");
        it.description = i18n.tr("settings.display.reload_grid_desc",
                                 "Reads the installed titles again. Use it after creating or "
                                 "deleting a shortcut.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_reloadCatalogCb) screen.m_reloadCatalogCb();
        };
        t.items.push_back(std::move(it));
    }

    // The expensive half of what "reload" used to do, on its own item. Reading
    // a title's control data costs about a second, so a hundred-title console
    // spends minutes on this -- worth it when a name or an icon is actually
    // wrong, and pure waste when a shortcut was merely added.
    {
        SettingItem it;
        it.label = i18n.tr("settings.display.rebuild_names",
                           "Rebuild names and icons");
        it.description = i18n.tr("settings.display.rebuild_names_desc",
                                 "Discards every cached name and icon and reads them from the "
                                 "titles again. Takes a few minutes on a full console.");
        it.type = ItemType::Action;
        it.onChange = [&screen](SettingItem&) {
            if (screen.m_rebuildControlCacheCb) screen.m_rebuildControlCacheCb();
        };
        t.items.push_back(std::move(it));
    }

    return t;
}
