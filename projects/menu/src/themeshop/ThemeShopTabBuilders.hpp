#pragma once

#include "ThemeShopScreen.hpp"

namespace themeshop::tabs {

class InstalledTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

class CommunityTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

class AnimatedTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

class MusicTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

class SteamGridDbTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

class OptionsTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

class UpdateTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

class UninstallTab {
public:
    static ThemeShopScreen::Tab build(ThemeShopScreen& screen);
};

} // namespace themeshop::tabs
