#pragma once
#include <nxui/Theme.hpp>
#include <string>
#include <vector>

enum class ThemePresetSource {
    BuiltIn,
    UserPreset,
    InstalledPackage,
};

struct ThemeColorSet {
    float cursorH = -1.f, cursorS = -1.f, cursorL = -1.f;
    float accentH = 0.f, accentS = 0.f, accentL = 0.f;
    float bgH = 0.f, bgS = 0.f, bgL = 0.f;
    float bgAccH = 0.f, bgAccS = 0.f, bgAccL = 0.f;
    float shapeH = 0.f, shapeS = 0.f, shapeL = 0.f;
};

enum class ThemeBackgroundLayout {
    Floating,
    Grid,
};

enum class ThemeBackgroundShapeSet {
    Mixed,
    Circle,
    Triangle,
    Square,
    Diamond,
    Hexagon,
};

enum class ThemeBackgroundSymmetry {
    None,
    MirrorX,
    MirrorY,
    Quad,
};

struct ThemeBackgroundConfig {
    std::string imagePath;
    std::string videoPath;
    // A wallpaper that moves. Frames are decoded once when the theme is
    // applied and then only swapped, because decoding per frame was measured
    // on hardware and is not affordable at any resolution.
    std::vector<std::string> imageFrames;
    float imageFps = 12.f;
    float imageOpacity = 0.f;
    bool imageCover = true;
    ThemeBackgroundLayout layout = ThemeBackgroundLayout::Floating;
    ThemeBackgroundShapeSet shapeSet = ThemeBackgroundShapeSet::Mixed;
    ThemeBackgroundSymmetry symmetry = ThemeBackgroundSymmetry::None;
    int shapeCount = 30;
    int gridColumns = 14;
    int gridRows = 8;
    float spacingX = 88.f;
    float spacingY = 88.f;
    float sizeMin = 14.f;
    float sizeMax = 54.f;
    float speedMin = 6.f;
    float speedMax = 28.f;
    float wobble = 16.f;
    float opacity = 1.f;
    float rotationSpeed = 0.5f;
    bool fixedOrientation = false;
    float orientationDegrees = 0.f;
    float cornerRoundness = 0.f;
};

struct ThemeFontConfig {
    std::string regularPath;
    std::string smallPath;
};

struct ThemeIconConfig {
    std::string basePath;
};

struct ThemePreset {
    std::string     id;
    std::string     name;
    std::string     author;
    std::string     version;
    nxui::ThemeMode mode   = nxui::ThemeMode::Dark;
    ThemeColorSet   colors;
    ThemeBackgroundConfig background;
    ThemeFontConfig fonts;
    ThemeIconConfig icons;
    bool            builtIn = true;
    ThemePresetSource source = ThemePresetSource::BuiltIn;
    std::string     soundPreset;

    // Faixas que o proprio tema traz, relativas a pasta dele. Vazio significa
    // usar o conjunto de sons do preset, que era a unica opcao antes: musica
    // era propriedade do preset de audio e nao do tema, entao trocar de tema
    // deixava a trilha anterior tocando.
    std::vector<std::string> music;
    std::string     installPath;

    nxui::Theme toTheme() const;

    static ThemeColorSet extractColors(const nxui::Theme& theme);

    static const std::vector<ThemePreset>& builtInPresets();

    static std::vector<ThemePreset> loadUserPresets();

    static std::vector<ThemePreset> loadInstalledPackages();

    static bool saveUserPresets(const std::vector<ThemePreset>& presets);

    // Removes the folders a theme install leaves behind when it is interrupted.
    // Returns how many went. Never touches an installed theme.
    static int sweepInstallLeftovers();
};
