#include "ThemeShopScreen.hpp"

#include "widgets/ActionButtonStyle.hpp"
#include "settings/SettingsGlassTuning.hpp"

#include "core/DebugLog.hpp"

#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <functional>
#include <system_error>
#include <vector>
#include <unordered_map>
#include <utility>

namespace {

constexpr int kGridCols = 2;
constexpr int kVisibleRows = 2;
constexpr float kContentInsetX = 24.f;
constexpr float kContentInsetY = 18.f;
constexpr float kHeaderHeight = 78.f;
constexpr float kGridGapX = 16.f;
constexpr float kGridGapY = 20.f;
constexpr float kFooterHintHeight = 74.f;
constexpr float kPreviewAspect = 16.f / 9.f;
constexpr float kSearchButtonWidth = 208.f;
constexpr float kRefreshButtonWidth = 144.f;
constexpr float kHeaderButtonGap = 12.f;
constexpr int kEntriesPerPage = kGridCols * kVisibleRows;
constexpr float kPageButtonWidth = 124.f;
constexpr float kPageCounterWidth = 96.f;
constexpr float kActionButtonRadius = 16.f;
constexpr int kThemeShopBackdropCacheTarget = 2;
constexpr size_t kTextMeasureCacheLimit = 1024;
constexpr size_t kEllipsizeCacheLimit = 1024;

std::unordered_map<std::string, nxui::Vec2> g_textMeasureCache;
std::unordered_map<std::string, std::string> g_ellipsizeCache;

std::string measureCacheKey(nxui::Font* font, const std::string& text) {
    return std::to_string((std::uintptr_t)font) + "\n"
         + std::to_string(font ? font->revision() : 0) + "\n"
         + text;
}

std::string ellipsizeCacheKey(nxui::Font* font, const std::string& text, float maxWidth, float scale) {
    return std::to_string((std::uintptr_t)font)
         + "\n" + std::to_string(font ? font->revision() : 0)
        + "\n" + std::to_string((int)std::lround(maxWidth * 4.f))
        + "\n" + std::to_string((int)std::lround(scale * 1000.f))
        + "\n" + text;
}

nxui::Vec2 measureTextCached(nxui::Font* font, const std::string& text) {
    if (!font || text.empty())
        return font ? font->measure(text) : nxui::Vec2{};

    std::string key = measureCacheKey(font, text);
    auto it = g_textMeasureCache.find(key);
    if (it != g_textMeasureCache.end())
        return it->second;

    nxui::Vec2 size = font->measure(text);
    if (g_textMeasureCache.size() >= kTextMeasureCacheLimit)
        g_textMeasureCache.clear();
    g_textMeasureCache.emplace(std::move(key), size);
    return size;
}

struct GridLayout {
    nxui::Rect header;
    nxui::Rect searchButton;
    nxui::Rect refreshButton;
    nxui::Rect pagePrevButton;
    nxui::Rect pageCounter;
    nxui::Rect pageNextButton;
    nxui::Rect grid;
    float cardW = 0.f;
    float cardH = 0.f;
};

GridLayout makeGridLayout(const nxui::Rect& content) {
    GridLayout layout;
    layout.header = {
        content.x + kContentInsetX,
        content.y + kContentInsetY,
        content.width - kContentInsetX * 2.f,
        kHeaderHeight
    };
    layout.searchButton = {
        layout.header.right() - kSearchButtonWidth,
        layout.header.y + 10.f,
        kSearchButtonWidth,
        46.f
    };
    layout.refreshButton = {
        layout.searchButton.x - kHeaderButtonGap - kRefreshButtonWidth,
        layout.header.y + 10.f,
        kRefreshButtonWidth,
        46.f
    };
    layout.grid = {
        content.x + kContentInsetX,
        layout.header.bottom() + 12.f,
        content.width - kContentInsetX * 2.f,
        content.height - (layout.header.bottom() - content.y) - 12.f - kFooterHintHeight
    };
    const float pageControlsW = kPageButtonWidth * 2.f + kPageCounterWidth + kHeaderButtonGap * 2.f;
    const float pageControlsX = layout.grid.x + std::max(0.f, (layout.grid.width - pageControlsW) * 0.5f);
    const float pageControlsY = layout.grid.bottom() + 8.f;
    layout.pagePrevButton = {
        pageControlsX,
        pageControlsY,
        kPageButtonWidth,
        46.f
    };
    layout.pageCounter = {
        layout.pagePrevButton.right() + kHeaderButtonGap,
        pageControlsY,
        kPageCounterWidth,
        46.f
    };
    layout.pageNextButton = {
        layout.pageCounter.right() + kHeaderButtonGap,
        pageControlsY,
        kPageButtonWidth,
        46.f
    };
    layout.cardW = (layout.grid.width - kGridGapX * (kGridCols - 1)) / (float)kGridCols;
    layout.cardH = (layout.grid.height - kGridGapY * (kVisibleRows - 1)) / (float)kVisibleRows;
    return layout;
}

nxui::Rect gridCardRect(const GridLayout& layout, int localIndex) {
    int col = localIndex % kGridCols;
    int row = localIndex / kGridCols;
    return {
        layout.grid.x + col * (layout.cardW + kGridGapX),
        layout.grid.y + row * (layout.cardH + kGridGapY),
        layout.cardW,
        layout.cardH
    };
}

nxui::Rect detailDialogRect(const nxui::Rect& content) {
    return {
        content.x + 38.f,
        content.y + 42.f,
        content.width - 76.f,
        content.height - 84.f
    };
}

std::vector<nxui::Rect> detailButtonRects(const nxui::Rect& dialog, int count) {
    std::vector<nxui::Rect> rects;
    if (count <= 0)
        return rects;

    float gap = 14.f;
    float width = count == 1 ? 258.f : 246.f;
    float totalWidth = count * width + (count - 1) * gap;
    float x = dialog.right() - 30.f - totalWidth;
    float y = dialog.bottom() - 68.f;
    rects.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        rects.push_back({x + i * (width + gap), y, width, 46.f});
    }
    return rects;
}

std::vector<nxui::Rect> headerButtonRects(const GridLayout& layout, bool communityTab) {
    std::vector<nxui::Rect> rects;
    if (communityTab)
        rects.push_back(layout.refreshButton);
    rects.push_back(layout.searchButton);
    return rects;
}

std::string ellipsize(nxui::Font* font, const std::string& text, float maxWidth, float scale) {
    if (!font || text.empty() || maxWidth <= 0.f)
        return text;

    std::string cacheKey = ellipsizeCacheKey(font, text, maxWidth, scale);
    auto cached = g_ellipsizeCache.find(cacheKey);
    if (cached != g_ellipsizeCache.end())
        return cached->second;

    if (measureTextCached(font, text).x * scale <= maxWidth)
        return text;

    std::string out = text;
    while (!out.empty() && measureTextCached(font, out + "...").x * scale > maxWidth)
        out.pop_back();

    std::string result = out.empty() ? text : out + "...";
    if (g_ellipsizeCache.size() >= kEllipsizeCacheLimit)
        g_ellipsizeCache.clear();
    g_ellipsizeCache.emplace(std::move(cacheKey), result);
    return result;
}

nxui::Rect fitAspectRect(const nxui::Rect& bounds, float aspect) {
    if (bounds.width <= 0.f || bounds.height <= 0.f || aspect <= 0.f)
        return bounds;

    float width = std::min(bounds.width, bounds.height * aspect);
    float height = width / aspect;
    if (height > bounds.height) {
        height = bounds.height;
        width = height * aspect;
    }

    return {
        bounds.x + (bounds.width - width) * 0.5f,
        bounds.y + (bounds.height - height) * 0.5f,
        width,
        height
    };
}

nxui::Rect detailPreviewRect(const nxui::Rect& dialog) {
    constexpr float kBodyTopInset = 26.f;
    constexpr float kBodyBottomInset = 96.f;
    nxui::Rect bounds = {
        dialog.x + 22.f,
        dialog.y + kBodyTopInset,
        dialog.width * 0.50f,
        dialog.height - kBodyTopInset - kBodyBottomInset
    };
    return fitAspectRect(bounds, kPreviewAspect);
}

struct DetailPreviewControls {
    nxui::Rect prev;
    nxui::Rect counter;
    nxui::Rect next;
};

DetailPreviewControls detailPreviewControls(const nxui::Rect& preview) {
    DetailPreviewControls controls;
    float gap = 10.f;
    float rowY = preview.bottom() + 18.f;
    float rowH = 42.f;
    float prevW = 118.f;
    float counterW = 82.f;
    float nextW = 118.f;
    float totalW = prevW + counterW + nextW + gap * 2.f;
    float startX = preview.x + std::max(0.f, (preview.width - totalW) * 0.5f);

    controls.prev = {startX, rowY, prevW, rowH};
    controls.counter = {controls.prev.right() + gap, rowY, counterW, rowH};
    controls.next = {controls.counter.right() + gap, rowY, nextW, rowH};
    return controls;
}

std::vector<nxui::Rect> detailPreviewControlRects(const DetailPreviewControls& controls,
                                                 const nxui::Rect& preview,
                                                 int screenshotCount) {
    std::vector<nxui::Rect> rects;
    if (screenshotCount <= 0)
        return rects;
    if (screenshotCount > 1)
        rects.push_back(controls.prev);
    rects.push_back(preview);
    if (screenshotCount > 1)
        rects.push_back(controls.next);
    return rects;
}

nxui::Rect lerpRect(const nxui::Rect& from, const nxui::Rect& to, float t) {
    return {
        from.x + (to.x - from.x) * t,
        from.y + (to.y - from.y) * t,
        from.width + (to.width - from.width) * t,
        from.height + (to.height - from.height) * t,
    };
}

nxui::Rect scaledRect(const nxui::Rect& rect, float scale) {
    if (std::abs(scale - 1.f) <= 0.001f) {
        return rect;
    }

    float width = rect.width * scale;
    float height = rect.height * scale;
    return {
        rect.x + (rect.width - width) * 0.5f,
        rect.y + (rect.height - height) * 0.5f,
        width,
        height
    };
}

struct FullscreenOverlayLayout {
    nxui::Rect preview;
    nxui::Rect prev;
    nxui::Rect next;
    nxui::Rect close;
    nxui::Rect counter;
};

FullscreenOverlayLayout makeFullscreenOverlayLayout(const nxui::Rect& content) {
    FullscreenOverlayLayout layout;
    nxui::Rect inner = content.shrunk(18.f);
    nxui::Rect previewBounds = {
        inner.x + 44.f,
        inner.y + 44.f,
        inner.width - 88.f,
        inner.height - 88.f
    };
    layout.preview = fitAspectRect(previewBounds, kPreviewAspect);
    layout.prev = {inner.x + 16.f, layout.preview.y + layout.preview.height * 0.5f - 24.f, 92.f, 48.f};
    layout.next = {inner.right() - 108.f, layout.preview.y + layout.preview.height * 0.5f - 24.f, 92.f, 48.f};
    layout.close = {inner.right() - 128.f, inner.y + 18.f, 112.f, 40.f};
    layout.counter = {inner.x + (inner.width - 84.f) * 0.5f, inner.y + 18.f, 84.f, 40.f};
    return layout;
}

nxui::Rect fitTextureRect(const nxui::Rect& bounds, const nxui::Texture* texture) {
    if (!texture || texture->width() <= 0 || texture->height() <= 0)
        return bounds;

    float texW = (float)texture->width();
    float texH = (float)texture->height();
    float scale = std::min(bounds.width / texW, bounds.height / texH);
    float width = texW * scale;
    float height = texH * scale;
    return {
        bounds.x + (bounds.width - width) * 0.5f,
        bounds.y + (bounds.height - height) * 0.5f,
        width,
        height
    };
}

void drawSpinner(nxui::Renderer& ren, const nxui::Vec2& center, float radius,
                 float time, const nxui::Color& color, float opacity) {
    constexpr int kSegments = 12;
    for (int i = 0; i < kSegments; ++i) {
        float angle = ((float)i / (float)kSegments) * 6.2831853f + time * 5.0f;
        float lead = std::fmod((float)i + time * 12.f, (float)kSegments) / (float)kSegments;
        float alpha = std::clamp(0.12f + lead * 0.88f, 0.f, 1.f) * opacity;
        nxui::Vec2 from = {center.x + std::cos(angle) * (radius - 7.f), center.y + std::sin(angle) * (radius - 7.f)};
        nxui::Vec2 to = {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius};
        ren.drawLine(from, to, color.withAlpha(alpha), 3.f);
    }
}

void drawChip(nxui::Renderer& ren, nxui::Font* font, const nxui::Rect& rect,
              const std::string& text, const nxui::Color& fill, const nxui::Color& border,
              const nxui::Color& textColor, float opacity, float scale = 0.72f) {
    float borderWidth = rect.height >= 42.f ? 1.8f : (rect.height >= 34.f ? 1.5f : 1.3f);
    ren.drawRoundedRect(rect, fill.withAlpha(fill.a * opacity), rect.height * 0.5f);
    ren.drawRoundedRectOutline(rect, border.withAlpha(border.a * opacity), rect.height * 0.5f, borderWidth);
    if (font) {
        std::string fitted = ellipsize(font, text, rect.width - 16.f, scale);
        nxui::Vec2 size = measureTextCached(font, fitted);
        ren.drawText(fitted,
                     {rect.x + (rect.width - size.x * scale) * 0.5f,
                      rect.y + (rect.height - size.y * scale) * 0.5f},
                     font,
                     textColor.withAlpha(opacity),
                     scale);
    }
}

void drawActionButtonChip(nxui::Renderer& ren,
                          nxui::Font* font,
                          const nxui::Rect& rect,
                          const std::string& text,
                          const nxui::Theme* theme,
                          const nxui::Color& textColor,
                          float opacity,
                          float emphasis,
                          float accentMix = -1.f,
                          float scale = 0.72f) {
    auto style = switchu::ui::resolveActionButtonStyle(theme, opacity, emphasis, accentMix);
    nxui::Rect buttonRect = switchu::ui::scaledActionButtonRect(rect, style.scale);
    float radius = std::min(kActionButtonRadius, buttonRect.height * 0.5f);

    switchu::ui::drawActionButtonChrome(ren, rect, radius, style);

    if (font) {
        float textScale = scale * style.scale;
        std::string fitted = ellipsize(font, text, buttonRect.width - 24.f, textScale);
        nxui::Vec2 size = measureTextCached(font, fitted);
        ren.drawText(fitted,
                     {buttonRect.x + (buttonRect.width - size.x * textScale) * 0.5f,
                      buttonRect.y + (buttonRect.height - size.y * textScale) * 0.5f},
                     font,
                     textColor.withAlpha(textColor.a * opacity),
                     textScale);
    }
}

void drawLiquidGlassPanel(nxui::Renderer& ren,
                          const nxui::Theme* theme,
                          const nxui::Rect& rect,
                          float radius,
                          float opacity,
                          float borderBoost = 1.f) {
    if (!theme || opacity <= 0.01f)
        return;

    const auto& tuning = settings::debug::settingsGlassTuning();
    nxui::LiquidGlassSettings savedGlass = ren.liquidGlassSettings();
    auto& glass = ren.liquidGlassSettings();
    glass.refractionIntensity = std::clamp(tuning.refractionIntensity, 0.0f, 1.5f);
    glass.blurIntensity = std::max(0.0f, tuning.shaderBlurIntensity);
    glass.noiseIntensity = 0.0f;
    glass.glowIntensity = std::max(0.0f, tuning.glowIntensity);
    glass.saturation = std::max(0.0f, tuning.saturation);
    glass.opacityMultiplier = 1.0f;
    glass.roughness = std::max(0.0f, tuning.roughness);
    glass.powerFactor = std::max(1.001f, tuning.powerFactor);

    nxui::Rect glassRect = rect.shrunk(std::max(0.0f, tuning.inset));
    float glassRadius = std::max(12.0f, radius - std::max(0.0f, tuning.inset) * 0.5f);
    nxui::Color glassTint = theme->panelBase.withAlpha(theme->mode == nxui::ThemeMode::Dark
        ? std::clamp(tuning.tintAlphaDark, 0.0f, 1.0f)
        : std::clamp(tuning.tintAlphaLight, 0.0f, 1.0f));

    ren.drawLiquidGlass(kThemeShopBackdropCacheTarget,
                        glassRect,
                        glassRadius,
                        glassTint,
                        opacity,
                        std::clamp(tuning.shade, 0.0f, 1.0f));
    ren.drawRoundedRectOutline(glassRect,
                               theme->panelBorder.withAlpha(0.24f * borderBoost * opacity),
                               glassRadius,
                               1.2f);
    ren.drawRoundedRectOutline(glassRect.shrunk(1.5f),
                               theme->panelHighlight.withAlpha(0.08f * opacity),
                               std::max(0.f, glassRadius - 1.5f),
                               1.0f);
    ren.liquidGlassSettings() = savedGlass;
}

void drawPreviewPlaceholder(nxui::Renderer& ren,
                            nxui::Font* smallFont,
                            const nxui::Theme* theme,
                            const nxui::Rect& rect,
                            const std::string& label,
                            bool loading,
                            float time,
                            float opacity) {
    ren.drawRoundedRect(rect, theme->panelBase.withAlpha(0.16f * opacity), 14.f);
    ren.drawRoundedRectOutline(rect, theme->panelBorder.withAlpha(0.18f * opacity), 14.f, 1.f);

    nxui::Rect inner = {rect.x + 4.f, rect.y + 4.f, rect.width - 8.f, rect.height - 8.f};
    ren.drawRoundedRect(inner, theme->background.withAlpha(0.22f * opacity), 12.f);

    nxui::Vec2 center = {inner.x + inner.width * 0.5f, inner.y + inner.height * 0.5f};
    if (loading) {
        drawSpinner(ren, {center.x, center.y - 12.f}, 14.f, time, theme->cursorNormal, opacity);
    }

    if (smallFont && !label.empty()) {
        std::string fitted = ellipsize(smallFont, label, inner.width - 20.f, 0.64f);
        nxui::Vec2 size = measureTextCached(smallFont, fitted);
        float textY = center.y + (loading ? 12.f : -size.y * 0.32f);
        ren.drawText(fitted,
                     {inner.x + (inner.width - size.x * 0.64f) * 0.5f, textY},
                     smallFont,
                     theme->textSecondary.withAlpha(0.92f * opacity),
                     0.64f);
    }
}

void drawBuiltInThemePreview(nxui::Renderer& ren,
                             nxui::Font* smallFont,
                             const nxui::Rect& rect,
                             bool light,
                             float opacity) {
    // The original built-in themes have no wallpaper or screenshot asset.
    // Keep their preview honest and zero-byte: a miniature of SwitchU's own
    // chrome, painted from the dark/light palette rather than a fake photo.
    const nxui::Color background = light ? nxui::Color(0.79f, 0.85f, 0.91f, opacity)
                                        : nxui::Color(0.035f, 0.055f, 0.080f, opacity);
    const nxui::Color panel = light ? nxui::Color(0.94f, 0.97f, 1.00f, 0.72f * opacity)
                                    : nxui::Color(0.14f, 0.19f, 0.25f, 0.78f * opacity);
    const nxui::Color accent = light ? nxui::Color(0.13f, 0.46f, 0.89f, opacity)
                                     : nxui::Color(0.18f, 0.58f, 1.00f, opacity);
    const nxui::Color muted = light ? nxui::Color(0.32f, 0.42f, 0.53f, 0.78f * opacity)
                                    : nxui::Color(0.74f, 0.82f, 0.91f, 0.70f * opacity);

    ren.drawRoundedRect(rect, background, 14.f);
    ren.drawRoundedRectOutline(rect, accent.withAlpha(0.45f * opacity), 14.f, 1.f);
    const nxui::Rect inner = rect.shrunk(7.f);
    const nxui::Rect topBar = {inner.x, inner.y, inner.width, std::max(14.f, inner.height * 0.17f)};
    ren.drawRoundedRect(topBar, panel, 8.f);
    ren.drawRoundedRect({topBar.x + 8.f, topBar.y + topBar.height * 0.38f, topBar.width * 0.32f, 2.f}, accent, 1.f);
    const nxui::Rect sidebar = {inner.x, topBar.bottom() + 5.f, std::max(17.f, inner.width * 0.14f),
                                inner.bottom() - topBar.bottom() - 5.f};
    ren.drawRoundedRect(sidebar, panel, 8.f);
    const float iconSize = std::max(5.f, sidebar.width * 0.38f);
    for (int index = 0; index < 3; ++index) {
        const nxui::Rect icon = {sidebar.x + (sidebar.width - iconSize) * 0.5f,
                                 sidebar.y + 9.f + index * (iconSize + 7.f), iconSize, iconSize};
        ren.drawRoundedRect(icon, index == 0 ? accent : muted.withAlpha(0.48f * opacity), iconSize * 0.35f);
    }
    const nxui::Rect content = {sidebar.right() + 6.f, topBar.bottom() + 5.f,
                                inner.right() - sidebar.right() - 6.f, inner.bottom() - topBar.bottom() - 5.f};
    const nxui::Rect hero = {content.x, content.y, content.width, content.height * 0.58f};
    ren.drawRoundedRect(hero, panel, 9.f);
    ren.drawRoundedRect({hero.x + hero.width * 0.10f, hero.y + hero.height * 0.23f,
                         hero.width * 0.64f, 3.f}, muted, 1.f);
    ren.drawRoundedRect({hero.x + hero.width * 0.10f, hero.y + hero.height * 0.43f,
                         hero.width * 0.43f, 2.f}, muted.withAlpha(0.70f * opacity), 1.f);
    const float cardGap = 4.f;
    const float cardWidth = (content.width - cardGap * 2.f) / 3.f;
    for (int index = 0; index < 3; ++index) {
        const nxui::Rect card = {content.x + index * (cardWidth + cardGap), hero.bottom() + 5.f,
                                 cardWidth, std::max(8.f, content.bottom() - hero.bottom() - 5.f)};
        ren.drawRoundedRect(card, index == 0 ? accent.withAlpha(0.72f * opacity) : panel, 5.f);
    }
    if (smallFont) {
        ren.drawText("SwitchU", {inner.right() - 46.f, topBar.y + 4.f}, smallFont,
                     light ? nxui::Color(0.08f, 0.18f, 0.29f, opacity) : nxui::Color::white().withAlpha(opacity),
                     0.42f);
    }
}

void drawThemePreview(nxui::Renderer& ren,
                      nxui::Font* smallFont,
                      const nxui::Theme* theme,
                      const nxui::Rect& rect,
                      const nxui::Texture* texture,
                      const std::string& placeholderLabel,
                      bool loading,
                      float time,
                      float opacity,
                      int sheetCols = 0,
                      int sheetRows = 0,
                      float sheetFps = 10.f,
                      bool builtInDefault = false,
                      bool builtInLight = false) {
    if (!theme)
        return;

    if (texture && texture->valid()) {
        ren.drawRoundedRect(rect, theme->panelBase.withAlpha(0.14f * opacity), 14.f);
        ren.drawRoundedRectOutline(rect, theme->panelBorder.withAlpha(0.18f * opacity), 14.f, 1.f);
        nxui::Rect inner = {rect.x + 4.f, rect.y + 4.f, rect.width - 8.f, rect.height - 8.f};
        ren.drawRoundedRect(inner, nxui::Color(0.f, 0.f, 0.f, 0.14f * opacity), 12.f);
        if (sheetCols > 0 && sheetRows > 0) {
            // A folha traz os quadros em grade; o cartao percorre um por vez.
            // Anima sem baixar nada alem da propria imagem, e sem guardar um
            // laco inteiro de texturas para uma miniatura.
            const int total = sheetCols * sheetRows;
            const int frame = ((int)(time * sheetFps)) % total;
            const float cw = (float)texture->width() / (float)sheetCols;
            const float ch = (float)texture->height() / (float)sheetRows;
            const nxui::Rect src{(frame % sheetCols) * cw, (frame / sheetCols) * ch, cw, ch};
            // O destino usa a proporcao de uma celula, nao da folha inteira.
            const float cellAspect = cw / ch;
            float dw = inner.width, dh = inner.width / cellAspect;
            if (dh > inner.height) { dh = inner.height; dw = inner.height * cellAspect; }
            const nxui::Rect dest{inner.x + (inner.width - dw) * 0.5f,
                                  inner.y + (inner.height - dh) * 0.5f, dw, dh};
            ren.drawTextureRoundedSub(texture, src, dest, 12.f,
                                      nxui::Color(1.f, 1.f, 1.f, opacity));
            return;
        }
        ren.drawTextureRounded(texture,
                               fitTextureRect(inner, texture),
                               12.f,
                               nxui::Color(1.f, 1.f, 1.f, opacity));
        return;
    }

    if (builtInDefault) {
        drawBuiltInThemePreview(ren, smallFont, rect, builtInLight, opacity);
        return;
    }

    drawPreviewPlaceholder(ren, smallFont, theme, rect, placeholderLabel, loading, time, opacity);
}

std::string transferLabel(const ThemeTransferState& state) {
    if (state.label().empty())
        return {};
    if (state.progress01() < 0.f || state.progress01() > 1.f)
        return state.label();

    int percent = (int)std::lround(state.progress01() * 100.f);
    return state.label() + " (" + std::to_string(percent) + "%)";
}

// Sizes a player can act on, not exact byte counts.
//
// The decimal is dropped above 100 of a unit: "1,2 GB" tells you whether the
// theme fits, "847,3 MB" only makes the line longer. The unit is binary, which
// is what the console's own storage screen counts in, so the two agree.
std::string formatBytes(std::uint64_t bytes) {
    if (bytes == 0)
        return {};
    const char* units[] = {"KB", "MB", "GB"};
    double value = (double)bytes / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 2) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), value >= 100.0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buffer;
}

std::string estimateDownloadSize(const std::string& durationStr) {
    if (durationStr.empty() || durationStr == "0:00")
        return "~4.5 MB";
    int seconds = 0;
    size_t firstColon = durationStr.find(':');
    if (firstColon != std::string::npos) {
        size_t secondColon = durationStr.find(':', firstColon + 1);
        if (secondColon != std::string::npos) {
            int h = std::atoi(durationStr.substr(0, firstColon).c_str());
            int m = std::atoi(durationStr.substr(firstColon + 1, secondColon - firstColon - 1).c_str());
            int s = std::atoi(durationStr.substr(secondColon + 1).c_str());
            seconds = h * 3600 + m * 60 + s;
        } else {
            int m = std::atoi(durationStr.substr(0, firstColon).c_str());
            int s = std::atoi(durationStr.substr(firstColon + 1).c_str());
            seconds = m * 60 + s;
        }
    }
    if (seconds <= 0)
        return "~4.5 MB";

    std::uint64_t bytes = static_cast<std::uint64_t>(seconds) * 24000ULL;
    return "~" + formatBytes(bytes);
}

} // namespace

// Both catalogue tabs render the same way and read the same list; what tells
// them apart is which entries survive the filter below.
bool ThemeShopScreen::isCommunityTab() const {
    return m_tabIndex == 1 || m_tabIndex == 2;
}

bool ThemeShopScreen::isAnimatedTab() const {
    return m_tabIndex == 1;
}

bool ThemeShopScreen::isMusicTab() const {
    return m_tabIndex == 3;
}

bool ThemeShopScreen::stepCataloguePage(int delta) {
    if (!usesCustomContentLayout() || m_detailOpen || !isFullyVisible())
        return false;
    const int before = currentPage();
    stepPage(delta);
    return before != currentPage();
}

// "61 temas - 4,5 GB para baixar - 7,3 GB no cartao", for whichever catalogue
// the open tab is showing.
//
// Both figures are worth having and they are far apart: the frames compress
// hard, so the download is a little over half of what lands on the card. Only
// entries that declare a size contribute -- an older index that never carried
// the field is reported as the themes it has, without a size claim invented on
// its behalf.
std::uint64_t ThemeShopScreen::installedThemeBytes(const std::string& installPath) const {
    if (installPath.empty())
        return 0;
    std::lock_guard<std::mutex> lk(m_installedSizes->mutex);
    auto it = m_installedSizes->bytes.find(installPath);
    return it == m_installedSizes->bytes.end() ? 0 : it->second;
}

// Uma medição por tema, na thread de trabalho, e o resultado fica.
//
// Um tema animado é uma pasta com centenas de quadros; somar isso com o
// desenho parado é a mesma travada que a capa personalizada causava. O tamanho
// aparece um instante depois de a aba abrir, e é o preço certo a pagar.
void ThemeShopScreen::measureInstalledThemes() {
    if (!m_threadPool)
        return;

    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(m_installedSizes->mutex);
        for (const auto& entry : m_allThemeShopEntries) {
            if (entry.installPath.empty())
                continue;
            if (m_installedSizes->bytes.count(entry.installPath))
                continue;
            if (!m_installedSizes->inFlight.insert(entry.installPath).second)
                continue;
            pending.push_back(entry.installPath);
        }
    }
    if (pending.empty())
        return;

    auto sizes = m_installedSizes;
    m_threadPool->submit([sizes, pending]() {
        for (const auto& path : pending) {
            std::uint64_t total = 0;
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(path, ec), end;
                 it != end && !ec; it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec)
                    total += (std::uint64_t)it->file_size(ec);
            }
            std::lock_guard<std::mutex> lk(sizes->mutex);
            sizes->bytes[path] = total;
            sizes->inFlight.erase(path);
        }
    });
}

// "4 temas - 512 MB no cartao". Só conta o que realmente ocupa espaço: os
// embutidos vêm dentro do próprio build e não têm caminho de instalação.
std::string ThemeShopScreen::installedThemeTotals() const {
    int measured = 0;
    std::uint64_t total = 0;
    for (const auto& entry : m_allThemeShopEntries) {
        const std::uint64_t bytes = installedThemeBytes(entry.installPath);
        if (bytes == 0)
            continue;
        ++measured;
        total += bytes;
    }
    if (measured == 0 || total == 0)
        return {};

    auto& i18n = nxui::I18n::instance();
    return std::to_string((int)m_allThemeShopEntries.size()) + " "
        + (m_allThemeShopEntries.size() == 1
               ? i18n.tr("themeshop.catalog.theme_one", "theme")
               : i18n.tr("themeshop.catalog.theme_many", "themes"))
        + " - " + formatBytes(total) + " " + i18n.tr("themeshop.catalog.on_card", "on the SD card");
}

std::string ThemeShopScreen::communityCatalogueTotals() const {
    const bool wantAnimated = isAnimatedTab();
    int themes = 0;
    std::uint64_t download = 0;
    std::uint64_t onCard = 0;
    for (const auto& entry : m_allCommunityEntries) {
        const bool fromOurs = entry.catalogUrl == ThemeCatalogClient::kDefaultCatalogUrl;
        if (fromOurs != wantAnimated)
            continue;
        ++themes;
        download += entry.packageBytes;
        onCard   += entry.installedBytes;
    }
    if (themes == 0)
        return {};

    auto& i18n = nxui::I18n::instance();
    std::string line = std::to_string(themes) + " "
        + (themes == 1 ? i18n.tr("themeshop.catalog.theme_one", "theme")
                       : i18n.tr("themeshop.catalog.theme_many", "themes"));
    if (download > 0)
        line += " - " + formatBytes(download) + " " + i18n.tr("themeshop.catalog.to_download", "to download");
    if (onCard > 0)
        line += " - " + formatBytes(onCard) + " " + i18n.tr("themeshop.catalog.on_card", "on the SD card");
    return line;
}

std::string ThemeShopScreen::installedMusicTotals() const {
    auto& i18n = nxui::I18n::instance();
    std::string musicDir = YouTubeClient::musicDirectory();
    std::error_code ec;
    int songCount = 0;
    std::uint64_t totalBytes = 0;
    if (std::filesystem::is_directory(musicDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(musicDir, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file(ec)) continue;
            std::string filename = entry.path().filename().string();
            if (filename.empty() || filename.front() == '.') continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".mp3" || ext == ".ogg" || ext == ".wav") {
                ++songCount;
                std::error_code szEc;
                totalBytes += entry.file_size(szEc);
            }
        }
    }

    if (songCount == 0)
        return {};

    std::string countStr = std::to_string(songCount) + " "
        + (songCount == 1 ? i18n.tr("themeshop.music.song_one", "song")
                          : i18n.tr("themeshop.music.song_many", "songs"));
    return countStr + " - " + formatBytes(totalBytes) + " "
        + i18n.tr("themeshop.catalog.on_card", "on the SD card");
}

int ThemeShopScreen::currentEntryCount() const {
    if (isMusicTab())
        return (int)m_youTubeClient.trackCount();
    return isCommunityTab() ? (int)m_communityEntries.size() : (int)m_themeShopEntries.size();
}

int ThemeShopScreen::currentSelectedIndex() const {
    if (isMusicTab()) {
        int count = (int)m_youTubeClient.trackCount();
        if (count <= 0) return -1;
        return std::clamp(m_musicSelectedIndex, 0, count - 1);
    }

    if (isCommunityTab()) {
        for (int i = 0; i < (int)m_communityEntries.size(); ++i) {
            if (m_communityEntries[i].id == m_communitySelectedId)
                return i;
        }
        return m_communityEntries.empty() ? -1 : 0;
    }

    for (int i = 0; i < (int)m_themeShopEntries.size(); ++i) {
        if (m_themeShopEntries[i].id == m_themeShopSelectedId)
            return i;
    }
    return m_themeShopEntries.empty() ? -1 : 0;
}

void ThemeShopScreen::setCurrentSelectedIndex(int idx) {
    int count = currentEntryCount();
    if (count <= 0)
        return;

    idx = std::clamp(idx, 0, count - 1);
    if (isMusicTab())
        m_musicSelectedIndex = idx;
    else if (isCommunityTab())
        m_communitySelectedId = m_communityEntries[(size_t)idx].id;
    else
        m_themeShopSelectedId = m_themeShopEntries[(size_t)idx].id;
}

int ThemeShopScreen::pageCount() const {
    int count = currentEntryCount();
    return std::max(1, (count + kEntriesPerPage - 1) / kEntriesPerPage);
}

int ThemeShopScreen::currentPage() const {
    int count = currentEntryCount();
    if (count <= 0)
        return 0;
    int scrollRow = isMusicTab() ? m_musicScrollRow : (isCommunityTab() ? m_communityScrollRow : m_installedScrollRow);
    return std::clamp(scrollRow / kVisibleRows, 0, pageCount() - 1);
}

void ThemeShopScreen::setCurrentPage(int page) {
    int count = currentEntryCount();
    if (count <= 0) {
        currentScrollRowRef() = 0;
        return;
    }

    page = std::clamp(page, 0, pageCount() - 1);
    int target = std::min(count - 1, page * kEntriesPerPage);
    setCurrentSelectedIndex(target);
    currentScrollRowRef() = page * kVisibleRows;
}

void ThemeShopScreen::stepPage(int delta) {
    int before = currentPage();
    setCurrentPage(before + delta);
}

int& ThemeShopScreen::currentScrollRowRef() {
    if (isMusicTab())
        return m_musicScrollRow;
    return isCommunityTab() ? m_communityScrollRow : m_installedScrollRow;
}

void ThemeShopScreen::ensureSelectionVisible() {
    int count = currentEntryCount();
    if (count <= 0) {
        currentScrollRowRef() = 0;
        return;
    }

    int& scrollRow = currentScrollRowRef();
    int maxPageScroll = std::max(0, (pageCount() - 1) * kVisibleRows);
    scrollRow = std::clamp(scrollRow, 0, maxPageScroll);

    int page = std::clamp(scrollRow / kVisibleRows, 0, pageCount() - 1);
    int pageStart = page * kEntriesPerPage;
    int pageEnd = std::min(count, pageStart + kEntriesPerPage);
    int selected = std::max(0, currentSelectedIndex());
    if (selected < pageStart || selected >= pageEnd)
        setCurrentSelectedIndex(std::min(count - 1, pageStart));
}

void ThemeShopScreen::openDetail() {
    if (currentEntryCount() <= 0)
        return;
    m_detailOpen = true;
    m_detailSheetAnim.setImmediate(0.f);
    m_detailSheetAnim.set(1.f, 0.22f, nxui::Easing::outCubic);
    m_detailFullscreen = false;
    m_detailFullscreenAnim.setImmediate(0.f);
    m_contentFocusArea = ContentFocusArea::Grid;
    m_detailButtonIndex = 0;
    m_detailScreenshotIndex = 0;
    m_detailPreviewButtonIndex = 0;
    m_detailFocusArea = (isCommunityTab() && detailScreenshotCount() > 0)
        ? DetailFocusArea::Preview
        : DetailFocusArea::Buttons;
    m_focusArea = FocusArea::Content;

    if (isCommunityTab())
        primeCommunityPreview(currentDetailCommunityPreviewPath());
}

void ThemeShopScreen::closeDetail() {
    m_detailOpen = false;
    m_detailSheetAnim.set(0.f, 0.16f, nxui::Easing::outCubic);
    m_detailFullscreen = false;
    m_detailFullscreenAnim.setImmediate(0.f);
    m_detailFocusArea = DetailFocusArea::Buttons;
    m_detailButtonIndex = 0;
    m_detailPreviewButtonIndex = 0;
    m_detailScreenshotIndex = 0;
    if (!m_packageTransferState.isRunning()) {
        m_packageTransferState.reset();
        m_packageTransferThemeId.clear();
        m_packageTransferInstallMode = false;
    } else if (isCommunityTab()) {
        clearCommunityPreviewCache();
        m_lastPreviewPrimeKey.clear();
    }
}

// Um pacote baixado vira um preset com id "package:<id do catálogo>". A pasta
// de instalação também usa o id do catálogo, então a ligação entre as duas
// listas é essa e não depende do nome, que o autor do tema pode mudar.
const ThemeShopScreen::ThemeShopEntry*
ThemeShopScreen::installedEntryForCatalogue(const std::string& catalogueId) const {
    if (catalogueId.empty())
        return nullptr;
    const std::string packaged = "package:" + catalogueId;
    for (const auto& entry : m_allThemeShopEntries) {
        if (entry.id == packaged || entry.id == catalogueId)
            return &entry;
    }
    return nullptr;
}

int ThemeShopScreen::detailButtonCount() const {
    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (!track)
            return 0;
        return track->isDownloaded ? 2 : 1;
    }

    if (isCommunityTab())
        return selectedCommunityThemeEntry() ? 2 : 0;

    const auto* entry = selectedThemeShopEntry();
    if (!entry)
        return 0;
    return entry->removable ? 2 : 1;
}

void ThemeShopScreen::activateDetailButton(int buttonIndex) {
    auto& i18n = nxui::I18n::instance();

    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (!track)
            return;
        int trackIdx = currentSelectedIndex();
        if (trackIdx < 0)
            return;

        if (track->isDownloaded) {
            if (buttonIndex == 0) {
                // Play Track!
                closeDetail();
                if (m_playMusicCb) {
                    m_playMusicCb(track->localFilePath, track->title);
                }
                requestToast(i18n.tr("themeshop.music.playing", "Playing: ") + track->title, 2.5f);
                return;
            } else if (buttonIndex == 1) {
                // Delete Track!
                closeDetail();
                m_youTubeClient.deleteTrack(static_cast<size_t>(trackIdx));
                requestToast(i18n.tr("themeshop.music.deleted", "Track deleted from SD card."), 2.5f);
                if (m_musicDownloadedCb) {
                    m_musicDownloadedCb();
                }
                return;
            }
        }

        if (track->isDownloading) {
            requestToast(i18n.tr("themeshop.music.download_busy", "Download already in progress."), 2.5f);
            return;
        }

        closeDetail();
        std::string dlgTitle = i18n.tr("themeshop.music.download_title", "Downloading Music");
        std::string initialMsg = i18n.tr("themeshop.music.download_prep", "Preparing MP3 stream...");
        if (m_progressShowCb) {
            m_progressShowCb(dlgTitle, initialMsg, 0.05f);
        }

        auto onProgress = [this](const std::string& msg, float p) {
            if (m_progressUpdateCb) {
                m_progressUpdateCb(msg, p);
            }
        };

        std::string songTitle = track->title;
        auto onComplete = [this, songTitle](bool ok, const std::string& path, const std::string& err) {
            auto& i18nInner = nxui::I18n::instance();
            if (m_progressHideCb) {
                m_progressHideCb();
            }
            if (ok) {
                if (m_musicDownloadedCb) {
                    m_musicDownloadedCb();
                }
                std::string compTitle = i18nInner.tr("themeshop.music.finished_title", "Download Finished!");
                std::string compMsg = "'" + songTitle + "'\n\n"
                    + i18nInner.tr("themeshop.music.finished_msg", "was successfully downloaded to your music library!\n\nWould you like to play it now?");
                std::vector<DialogButtonDef> btns;
                btns.push_back({i18nInner.tr("themeshop.music.play_now", "Play Now"), [this, path, songTitle]() {
                    if (m_playMusicCb) {
                        m_playMusicCb(path, songTitle);
                    }
                }});
                btns.push_back({i18nInner.tr("button.ok", "OK"), [this]() {}});
                requestDialog(compTitle, compMsg, std::move(btns));
            } else {
                requestToast(i18nInner.tr("themeshop.music.download_error", "Download failed: ") + err, 4.0f);
            }
        };

        m_youTubeClient.downloadTrack(static_cast<size_t>(trackIdx), onProgress, onComplete);
        return;
    }

    if (isCommunityTab()) {
        const auto* entry = selectedCommunityThemeEntry();
        if (!entry)
            return;

        if (m_packageTransferState.isRunning()) {
            requestToast(i18n.tr("themeshop.community.transfer_busy",
                                 "Another theme transfer is already running."),
                         2.5f);
            return;
        }

        std::string themeId = entry->id;
        // Já instalado: baixar de novo não é o que alguém quer daqui, e era a
        // única coisa oferecida. As ações passam a ser as mesmas da aba de
        // instalados, sobre o preset que o pacote virou.
        if (const auto* installed = installedEntryForCatalogue(themeId)) {
            const std::string presetId = installed->id;
            clearCommunityPreviewCache();
            m_lastPreviewPrimeKey.clear();
            closeDetail();
            if (buttonIndex == 0) {
                if (m_themeShopApplyCb) m_themeShopApplyCb(presetId);
            } else if (buttonIndex == 1) {
                if (m_themeShopDeleteCb) m_themeShopDeleteCb(presetId);
            }
            return;
        }

        clearCommunityPreviewCache();
        m_lastPreviewPrimeKey.clear();
        if (buttonIndex == 0) {
            if (m_themeShopDownloadCb) {
                m_themeShopDownloadCb(themeId);
            } else {
                requestToast(i18n.tr("themeshop.community.install_pending", "Install flow is not wired yet."), 2.5f);
            }
        } else if (buttonIndex == 1) {
            if (m_themeShopDownloadInstallCb) {
                m_themeShopDownloadInstallCb(themeId);
            } else {
                requestToast(i18n.tr("themeshop.community.download_apply_pending", "Download + apply flow is not wired yet."), 2.5f);
            }
        }
        return;
    }

    const auto* entry = selectedThemeShopEntry();
    if (!entry)
        return;

    std::string themeId = entry->id;
    bool removable = entry->removable;
    closeDetail();
    if (buttonIndex == 0) {
        if (m_themeShopApplyCb)
            m_themeShopApplyCb(themeId);
    } else if (buttonIndex == 1 && removable) {
        if (m_themeShopDeleteCb)
            m_themeShopDeleteCb(themeId);
    }
}

void ThemeShopScreen::updateCustomContent(float dt) {
    if (!usesCustomContentLayout())
        return;

    m_detailSheetAnim.update(std::min(dt, 0.03f));
    m_detailFullscreenAnim.update(std::min(dt, 0.03f));

    if (!m_showing) {
        closeDetail();
        clearCommunityPreviewCache();
        m_lastPreviewPrimeKey.clear();
        return;
    }

    if (isCommunityTab())
        syncFinishedCommunityPreviewLoads();
    else {
        clearCommunityPreviewCache();
        m_lastPreviewPrimeKey.clear();
    }

    // The installed grid asks for its covers from inside the frame; the worker
    // that reads and decodes one answers here, on the thread allowed to upload.
    syncFinishedInstalledPreviewLoads();

    if (isMusicTab()) {
        m_youTubeClient.updateThumbnailTextures();
        m_youTubeClient.trimThumbnailCache();
    }

    int headerButtonCount = isCommunityTab() ? 2 : 1;
    m_headerButtonIndex = std::clamp(m_headerButtonIndex, 0, std::max(0, headerButtonCount - 1));

    if (m_lastCustomTabIndex != m_tabIndex) {
        m_lastCustomTabIndex = m_tabIndex;
        m_contentFocusArea = ContentFocusArea::Grid;
        closeDetail();

        // Both catalogue tabs read one merged list and are told apart by which
        // index each entry came from -- and that split happens in the filter.
        // Without re-running it here the list stayed as whatever the previous
        // tab produced, so Community and Animated showed each other's themes
        // until somebody pressed refresh.
        applySearchFilter();
    }

    if (currentEntryCount() <= 0) {
        // An empty list used to push focus onto the tab bar, which put the
        // refresh button out of reach of the controller: the one screen where
        // refresh is the only useful action was the one where only touch could
        // press it. So focus is allowed to stay in the content area, landing on
        // the header instead of on a grid with nothing in it.
        //
        // Allowed, not forced. Forcing it stole focus back from the tab bar on
        // every frame while a catalogue was still loading, which read as the
        // selector being stuck between tabs until the download finished.
        if (m_focusArea == FocusArea::Content) {
            m_contentFocusArea = ContentFocusArea::Header;
            m_headerButtonIndex = 0;
        }
        currentScrollRowRef() = 0;
        closeDetail();
        if (isCommunityTab()) {
            clearCommunityPreviewCache();
            m_lastPreviewPrimeKey.clear();
        }
        return;
    }

    ensureSelectionVisible();

    if (isCommunityTab() && !m_packageTransferState.isRunning()) {
        clampDetailScreenshotIndex();
        int previewButtonCount = detailScreenshotCount() > 0 ? 1 : 0;
        if (previewButtonCount <= 0) {
            m_detailPreviewButtonIndex = 0;
            if (m_detailFocusArea == DetailFocusArea::Preview)
                m_detailFocusArea = DetailFocusArea::Buttons;
        } else {
            m_detailPreviewButtonIndex = 0;
        }
        std::string primeKey = std::to_string(m_communityRevision)
            + "|" + m_communitySelectedId
            + "|" + std::to_string(m_communityScrollRow)
            + "|" + std::to_string(m_detailOpen ? 1 : 0)
            // A tela cheia pede outra imagem que o detalhe -- a folha grande no
            // lugar da pequena -- e nao constava aqui. Sem isso a chave nao
            // mudava ao expandir, primeVisibleCommunityPreviews nao rodava, e a
            // folha HD ficava em Idle: nunca pedida, e por isso "captura
            // indisponivel" para sempre.
            //
            // Medido no console: "tela cheia pede .../preview_sheet_hd.jpg
            // (estado=0 celulas=5x4)" -- estado 0 e Idle.
            + "|" + std::to_string(m_detailFullscreen ? 1 : 0)
            + "|" + std::to_string(m_detailScreenshotIndex);
        if (primeKey != m_lastPreviewPrimeKey) {
            m_lastPreviewPrimeKey = std::move(primeKey);
            trimCommunityPreviewCache();
            primeVisibleCommunityPreviews();
        } else {
            // Tambem sem nada mudar. A poda so acontecia quando a chave mudava,
            // entao quem abria a aba e esperava as previas carregarem nunca
            // podava nada -- e e exatamente ai que elas chegam. A sessao que
            // derrubou o menu teve zero podas.
            m_previewTrimTimer += dt;
            if (m_previewTrimTimer >= 1.f) {
                m_previewTrimTimer = 0.f;
                trimCommunityPreviewCache();
            }
        }
    } else if (isCommunityTab()) {
        clearCommunityPreviewCache();
        m_lastPreviewPrimeKey.clear();
    }
}

bool ThemeShopScreen::handleCustomPressA() {
    if (m_focusArea == FocusArea::Tabs) {
        if (currentEntryCount() > 0) {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Grid;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }

        if ((isCommunityTab() ? 2 : 1) > 0) {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Header;
            m_headerButtonIndex = isCommunityTab() ? 0 : 0;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        return false;
    }

    if (!m_detailOpen && m_contentFocusArea == ContentFocusArea::Header) {
        if (isCommunityTab() && m_headerButtonIndex == 0) {
            refreshCommunityCatalog();
        } else {
            promptSearchQuery();
        }
        if (m_activateSfxCb) m_activateSfxCb();
        return true;
    }

    if (!m_detailOpen && m_contentFocusArea == ContentFocusArea::Pager) {
        int before = currentPage();
        if (m_pageButtonIndex == 0)
            stepPage(-1);
        else
            stepPage(1);
        if (before != currentPage() && m_navSfxCb)
            m_navSfxCb();
        return true;
    }

    if (m_detailOpen) {
        if (m_detailFullscreen) {
            m_detailFullscreen = false;
            m_detailFullscreenAnim.set(0.f, 0.16f, nxui::Easing::outQuad);
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }

        if (isCommunityTab() && m_detailFocusArea == DetailFocusArea::Preview && detailScreenshotCount() > 0) {
            m_detailFullscreen = true;
            m_detailFullscreenAnim.set(1.f, 0.18f, nxui::Easing::outCubic);
            if (m_activateSfxCb) m_activateSfxCb();
            return true;
        }

        activateDetailButton(m_detailButtonIndex);
        if (m_activateSfxCb) m_activateSfxCb();
        return true;
    }

    if (currentEntryCount() > 0) {
        openDetail();
        if (m_activateSfxCb) m_activateSfxCb();
        return true;
    }

    return false;
}

void ThemeShopScreen::currentAccessibilityParts(std::string& context,
                                                std::string& position,
                                                std::string& summary,
                                                bool& forceRepeat) const {
    if (!usesCustomContentLayout()) {
        TabbedOverlayScreen::currentAccessibilityParts(context, position, summary, forceRepeat);
        return;
    }

    context.clear();
    position.clear();
    summary.clear();
    forceRepeat = false;

    if (m_tabIndex < 0 || m_tabIndex >= (int)m_tabs.size())
        return;

    auto& i18n = nxui::I18n::instance();
    const std::string tabName = m_tabs[(size_t)m_tabIndex].name;
    if (m_focusArea == FocusArea::Tabs) {
        context = i18n.tr("accessibility.context.themes", "Themes");
        if (m_accessibilitySpeakPosition) {
            position = std::to_string(m_tabIndex + 1) + " "
                     + i18n.tr("accessibility.context.of", "of") + " "
                     + std::to_string((int)m_tabs.size());
        }
        summary = i18n.tr("accessibility.settings.tab_prefix", "Tab") + " " + tabName;
        if (m_accessibilitySpeakHints)
            summary += ". " + i18n.tr("accessibility.settings.tab_actions", "A or right to enter. Up and down to change tab. B to close.");
        return;
    }

    context = tabName;

    if (m_detailOpen) {
        std::string themeName;
        std::string author;
        if (isMusicTab()) {
            if (const auto* track = selectedMusicTrack()) {
                themeName = track->title;
                author = track->author;
            }
        } else if (isCommunityTab()) {
            if (const auto* entry = selectedCommunityThemeEntry()) {
                themeName = entry->name;
                author = entry->author;
            }
        } else if (const auto* entry = selectedThemeShopEntry()) {
            themeName = entry->name;
            author = entry->author;
        }
        if (themeName.empty())
            themeName = i18n.tr("settings.tabs.theme", "Theme");

        if (m_detailFullscreen) {
            summary = i18n.tr("themeshop.accessibility.fullscreen_preview", "Fullscreen preview")
                    + ". " + themeName;
            if (m_accessibilitySpeakHints)
                summary += ". " + i18n.tr("themeshop.accessibility.fullscreen_actions", "B to close the preview.");
            return;
        }

        std::string area = (m_detailFocusArea == DetailFocusArea::Preview)
            ? i18n.tr("themeshop.accessibility.preview_area", "Preview")
            : i18n.tr("themeshop.accessibility.actions_area", "Actions");
        summary = i18n.tr("themeshop.accessibility.detail", "Detail")
                + ". " + themeName + ". " + area;
        if (!author.empty())
            summary += ". " + i18n.tr("themeshop.accessibility.author", "Author") + ": " + author;
        if (m_accessibilitySpeakHints)
            summary += ". " + i18n.tr("themeshop.accessibility.detail_actions", "Left and right to change control. A to confirm. B to go back.");
        return;
    }

    if (m_contentFocusArea == ContentFocusArea::Header) {
        summary = isCommunityTab()
            ? i18n.tr("themeshop.accessibility.search_refresh", "Search and refresh")
            : i18n.tr("themeshop.accessibility.search_bar", "Search bar");
        if (m_accessibilitySpeakHints) {
            summary += ". " + (isCommunityTab()
                ? i18n.tr("themeshop.accessibility.header_actions", "Left and right to choose. A to confirm. Down to the grid.")
                : i18n.tr("themeshop.accessibility.search_actions", "A to search. Down to the grid."));
        }
        return;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        summary = i18n.tr("themeshop.accessibility.pagination", "Pagination");
        if (m_accessibilitySpeakHints)
            summary += ". " + i18n.tr("themeshop.accessibility.pagination_actions", "Left and right to change button. A to change page. Up to the grid.");
        return;
    }

    std::string themeName;
    std::string detail;
    if (isMusicTab()) {
        if (const auto* track = selectedMusicTrack()) {
            themeName = track->title;
            detail = track->author + (track->isDownloaded ? ", Downloaded" : "");
        }
    } else if (isCommunityTab()) {
        if (const auto* entry = selectedCommunityThemeEntry()) {
            themeName = entry->name;
            detail = entry->author;
        }
    } else if (const auto* entry = selectedThemeShopEntry()) {
        themeName = entry->name;
        detail = entry->source;
        if (entry->active)
            detail = detail.empty() ? "Actif" : detail + ", actif";
    }

    if (themeName.empty())
        themeName = i18n.tr("themeshop.accessibility.no_theme", "No theme");

    summary = themeName;
    if (!detail.empty())
        summary += ". " + detail;
    if (m_accessibilitySpeakPosition) {
        const int count = currentEntryCount();
        const int selected = currentSelectedIndex();
        if (selected >= 0 && count > 0) {
            position = std::to_string(selected + 1) + " "
                     + i18n.tr("accessibility.context.of", "of") + " "
                     + std::to_string(count);
        }
    }
    if (m_accessibilitySpeakHints)
        summary += ". " + i18n.tr("themeshop.accessibility.grid_actions", "Directional pad to navigate. A to open details. B to return to tabs.");
}

std::string ThemeShopScreen::currentAccessibilitySummary() const {
    std::string context;
    std::string position;
    std::string summary;
    bool forceRepeat = false;
    currentAccessibilityParts(context, position, summary, forceRepeat);
    std::string out = context;
    if (!summary.empty())
        out += (out.empty() ? "" : ". ") + summary;
    if (!position.empty())
        out += (out.empty() ? "" : ". ") + position;
    return out;
}

bool ThemeShopScreen::handleCustomPressB() {
    if (m_detailFullscreen) {
        m_detailFullscreen = false;
        m_detailFullscreenAnim.set(0.f, 0.16f, nxui::Easing::outQuad);
        if (m_closeSfxCb) m_closeSfxCb();
        return true;
    }

    if (m_detailOpen) {
        closeDetail();
        if (m_closeSfxCb) m_closeSfxCb();
        return true;
    }

    if (m_focusArea == FocusArea::Content) {
        m_focusArea = FocusArea::Tabs;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    return false;
}

bool ThemeShopScreen::handleCustomPressX() {
    bool handled = promptSearchQuery();
    if (handled && m_activateSfxCb)
        m_activateSfxCb();
    return handled;
}

bool ThemeShopScreen::handleCustomNavUp() {
    if (m_detailFullscreen)
        return true;
    if (m_detailOpen) {
        if (isCommunityTab() && m_detailFocusArea == DetailFocusArea::Buttons && detailScreenshotCount() > 0) {
            m_detailFocusArea = DetailFocusArea::Preview;
            if (m_navSfxCb) m_navSfxCb();
        }
        return true;
    }
    if (m_focusArea != FocusArea::Content)
        return false;

    if (m_contentFocusArea == ContentFocusArea::Header) {
        m_focusArea = FocusArea::Tabs;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        if (currentEntryCount() > 0) {
            m_contentFocusArea = ContentFocusArea::Grid;
            int pageStart = currentPage() * kEntriesPerPage;
            int target = std::min(currentEntryCount() - 1,
                                  pageStart + (kVisibleRows - 1) * kGridCols + m_pageButtonIndex);
            setCurrentSelectedIndex(target);
            ensureSelectionVisible();
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    int pageStart = currentPage() * kEntriesPerPage;
    if (selected - kGridCols >= pageStart) {
        setCurrentSelectedIndex(selected - kGridCols);
        ensureSelectionVisible();
    } else {
        m_contentFocusArea = ContentFocusArea::Header;
        m_headerButtonIndex = isCommunityTab() ? std::clamp(selected % kGridCols, 0, 1) : 0;
    }
    if (m_navSfxCb) m_navSfxCb();
    return true;
}

bool ThemeShopScreen::handleCustomNavDown() {
    if (m_detailFullscreen)
        return true;
    if (m_detailOpen) {
        if (isCommunityTab() && m_detailFocusArea == DetailFocusArea::Preview && detailButtonCount() > 0) {
            m_detailFocusArea = DetailFocusArea::Buttons;
            if (m_navSfxCb) m_navSfxCb();
        }
        return true;
    }
    if (m_focusArea != FocusArea::Content)
        return false;

    if (m_contentFocusArea == ContentFocusArea::Header) {
        if (currentEntryCount() > 0)
            m_contentFocusArea = ContentFocusArea::Grid;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager)
        return true;

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    int pageEnd = std::min(count, (currentPage() + 1) * kEntriesPerPage);
    if (selected + kGridCols < pageEnd) {
        setCurrentSelectedIndex(selected + kGridCols);
        ensureSelectionVisible();
        if (m_navSfxCb) m_navSfxCb();
    } else {
        m_contentFocusArea = ContentFocusArea::Pager;
        m_pageButtonIndex = std::clamp(selected % kGridCols, 0, 1);
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

bool ThemeShopScreen::handleCustomNavLeft() {
    if (m_detailFullscreen) {
        int before = m_detailScreenshotIndex;
        stepDetailScreenshot(-1);
        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_detailOpen) {
        if (isCommunityTab() && m_detailFocusArea == DetailFocusArea::Preview) {
            int before = m_detailScreenshotIndex;
            stepDetailScreenshot(-1);
            if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
            return true;
        }

        if (m_detailButtonIndex > 0) {
            --m_detailButtonIndex;
            if (m_navSfxCb) m_navSfxCb();
        }
        return true;
    }

    if (m_focusArea != FocusArea::Content)
        return false;

    if (m_contentFocusArea == ContentFocusArea::Header) {
        if (m_headerButtonIndex > 0) {
            --m_headerButtonIndex;
        } else {
            m_focusArea = FocusArea::Tabs;
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        if (m_pageButtonIndex > 0)
            --m_pageButtonIndex;
        else
            m_focusArea = FocusArea::Tabs;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    if (selected % kGridCols > 0) {
        setCurrentSelectedIndex(selected - 1);
    } else {
        m_focusArea = FocusArea::Tabs;
    }
    if (m_navSfxCb) m_navSfxCb();
    return true;
}

bool ThemeShopScreen::handleCustomNavRight() {
    if (m_detailFullscreen) {
        int before = m_detailScreenshotIndex;
        stepDetailScreenshot(1);
        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_detailOpen) {
        if (isCommunityTab() && m_detailFocusArea == DetailFocusArea::Preview) {
            int before = m_detailScreenshotIndex;
            stepDetailScreenshot(1);
            if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
            return true;
        }

        if (m_detailButtonIndex + 1 < detailButtonCount()) {
            ++m_detailButtonIndex;
            if (m_navSfxCb) m_navSfxCb();
        }
        return true;
    }

    if (m_focusArea == FocusArea::Tabs) {
        if (currentEntryCount() > 0) {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Grid;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        if ((isCommunityTab() ? 2 : 1) > 0) {
            m_focusArea = FocusArea::Content;
            m_contentFocusArea = ContentFocusArea::Header;
            m_headerButtonIndex = 0;
            if (m_navSfxCb) m_navSfxCb();
            return true;
        }
        return false;
    }

    if (m_contentFocusArea == ContentFocusArea::Header) {
        int headerButtonCount = isCommunityTab() ? 2 : 1;
        if (m_headerButtonIndex + 1 < headerButtonCount) {
            ++m_headerButtonIndex;
        } else if (currentEntryCount() > 0) {
            m_contentFocusArea = ContentFocusArea::Grid;
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    if (m_contentFocusArea == ContentFocusArea::Pager) {
        if (m_pageButtonIndex < 1)
            ++m_pageButtonIndex;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }

    int count = currentEntryCount();
    if (count <= 0)
        return true;

    int selected = std::max(0, currentSelectedIndex());
    if (selected + 1 < count && selected % kGridCols < kGridCols - 1) {
        setCurrentSelectedIndex(selected + 1);
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

int ThemeShopScreen::hitTestGridCard(const nxui::Rect& content, float x, float y) const {
    GridLayout layout = makeGridLayout(content);
    int count = currentEntryCount();
    if (count <= 0 || !layout.grid.contains(x, y))
        return -1;

    int scrollRow = isCommunityTab() ? m_communityScrollRow : m_installedScrollRow;
    int start = scrollRow * kGridCols;
    int end = std::min(count, start + kGridCols * kVisibleRows);
    for (int i = start; i < end; ++i) {
        int local = i - start;
        if (gridCardRect(layout, local).contains(x, y))
            return i;
    }

    return -1;
}

bool ThemeShopScreen::handleCustomTouch(nxui::Input& input, const nxui::Rect&, const nxui::Rect& tabs, const nxui::Rect& content) {
    auto searchHit = [&](float x, float y) {
        return makeGridLayout(content).searchButton.contains(x, y);
    };
    auto refreshHit = [&](float x, float y) {
        return isCommunityTab() && makeGridLayout(content).refreshButton.contains(x, y);
    };
    auto pagePrevHit = [&](float x, float y) {
        return pageCount() > 1 && makeGridLayout(content).pagePrevButton.contains(x, y);
    };
    auto pageNextHit = [&](float x, float y) {
        return pageCount() > 1 && makeGridLayout(content).pageNextButton.contains(x, y);
    };
    auto detailPreviewHitIndex = [&](float x, float y) {
        if (!isCommunityTab() || !m_detailOpen)
            return -1;

        DetailPreviewControls controls = detailPreviewControls(detailPreviewRect(detailDialogRect(content)));
        nxui::Rect previewRect = detailPreviewRect(detailDialogRect(content));
        auto rects = detailPreviewControlRects(controls, previewRect, detailScreenshotCount());
        for (int i = 0; i < (int)rects.size(); ++i) {
            if (rects[(size_t)i].contains(x, y))
                return i;
        }
        return -1;
    };
    auto fullscreenHitIndex = [&](float x, float y) {
        FullscreenOverlayLayout overlay = makeFullscreenOverlayLayout(content);
        if (detailScreenshotCount() > 1 && overlay.prev.contains(x, y))
            return 0;
        if (detailScreenshotCount() > 1 && overlay.next.contains(x, y))
            return 1;
        if (overlay.close.contains(x, y) || overlay.preview.contains(x, y) || content.contains(x, y))
            return 2;
        return -1;
    };

    if (input.touchDown()) {
        float x = input.touchX();
        float y = input.touchY();
        m_themeTouchTarget = ThemeTouchTarget::None;
        m_themeTouchIndex = -1;
        m_themeTouchStartX = x;
        m_themeTouchStartY = y;

        if (tabs.contains(x, y))
            return false;
        if (!content.contains(x, y) && !m_detailOpen)
            return false;

        if (m_detailOpen) {
            if (m_detailFullscreen) {
                m_themeTouchTarget = ThemeTouchTarget::FullscreenPreview;
                m_themeTouchIndex = fullscreenHitIndex(x, y);
                return true;
            }

            nxui::Rect dialog = detailDialogRect(content);
            if (!dialog.contains(x, y)) {
                m_themeTouchTarget = ThemeTouchTarget::DetailBackdrop;
                return true;
            }

            int previewHit = detailPreviewHitIndex(x, y);
            if (previewHit >= 0) {
                m_themeTouchTarget = ThemeTouchTarget::DetailPreviewControl;
                m_themeTouchIndex = previewHit;
                return true;
            }

            auto buttons = detailButtonRects(dialog, detailButtonCount());
            for (int i = 0; i < (int)buttons.size(); ++i) {
                if (buttons[(size_t)i].contains(x, y)) {
                    m_themeTouchTarget = ThemeTouchTarget::DetailButton;
                    m_themeTouchIndex = i;
                    return true;
                }
            }

            return true;
        }

        if (searchHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::Search;
            return true;
        }

        if (refreshHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::Refresh;
            return true;
        }

        if (pagePrevHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::PagePrev;
            return true;
        }

        if (pageNextHit(x, y)) {
            m_themeTouchTarget = ThemeTouchTarget::PageNext;
            return true;
        }

        int hit = hitTestGridCard(content, x, y);
        if (hit >= 0) {
            m_themeTouchTarget = ThemeTouchTarget::GridCard;
            m_themeTouchIndex = hit;
            return true;
        }

        return content.contains(x, y);
    }

    if (input.isTouching() && m_themeTouchTarget != ThemeTouchTarget::None) {
        float dx = std::abs(input.touchX() - m_themeTouchStartX);
        float dy = std::abs(input.touchY() - m_themeTouchStartY);
        if (dx > 18.f || dy > 18.f) {
            m_themeTouchTarget = ThemeTouchTarget::None;
            m_themeTouchIndex = -1;
        }
        return true;
    }

    if (input.touchUp()) {
        float x = input.touchX();
        float y = input.touchY();
        ThemeTouchTarget target = m_themeTouchTarget;
        int hitIndex = m_themeTouchIndex;
        m_themeTouchTarget = ThemeTouchTarget::None;
        m_themeTouchIndex = -1;

        if (target == ThemeTouchTarget::None)
            return content.contains(x, y);

        switch (target) {
            case ThemeTouchTarget::Search:
                if (searchHit(x, y)) {
                    promptSearchQuery();
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            case ThemeTouchTarget::Refresh:
                if (refreshHit(x, y)) {
                    refreshCommunityCatalog();
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            case ThemeTouchTarget::PagePrev:
                if (pagePrevHit(x, y)) {
                    m_contentFocusArea = ContentFocusArea::Pager;
                    m_pageButtonIndex = 0;
                    int before = currentPage();
                    stepPage(-1);
                    if (before != currentPage() && m_navSfxCb) m_navSfxCb();
                }
                return true;
            case ThemeTouchTarget::PageNext:
                if (pageNextHit(x, y)) {
                    m_contentFocusArea = ContentFocusArea::Pager;
                    m_pageButtonIndex = 1;
                    int before = currentPage();
                    stepPage(1);
                    if (before != currentPage() && m_navSfxCb) m_navSfxCb();
                }
                return true;
            case ThemeTouchTarget::GridCard:
                if (hitTestGridCard(content, x, y) == hitIndex) {
                    bool alreadyFocused = (m_focusArea == FocusArea::Content
                        && m_contentFocusArea == ContentFocusArea::Grid
                        && currentSelectedIndex() == hitIndex);
                    setCurrentSelectedIndex(hitIndex);
                    m_focusArea = FocusArea::Content;
                    m_contentFocusArea = ContentFocusArea::Grid;
                    ensureSelectionVisible();
                    if (alreadyFocused) {
                        openDetail();
                        if (m_activateSfxCb) m_activateSfxCb();
                    } else if (m_navSfxCb) {
                        m_navSfxCb();
                    }
                }
                return true;
            case ThemeTouchTarget::DetailPreviewControl:
                if (hitIndex == detailPreviewHitIndex(x, y)) {
                    if (hitIndex == 0) {
                        if (detailScreenshotCount() > 1) {
                            int before = m_detailScreenshotIndex;
                            stepDetailScreenshot(-1);
                            if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                        }
                    } else if ((hitIndex == 1 && detailScreenshotCount() > 1)
                            || (hitIndex == 0 && detailScreenshotCount() == 1)) {
                        m_detailFocusArea = DetailFocusArea::Preview;
                        m_detailFullscreen = true;
                        m_detailFullscreenAnim.set(1.f, 0.18f, nxui::Easing::outCubic);
                        if (m_activateSfxCb) m_activateSfxCb();
                    } else if (hitIndex == 2 && detailScreenshotCount() > 1) {
                        int before = m_detailScreenshotIndex;
                        stepDetailScreenshot(1);
                        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                    }
                }
                return true;
            case ThemeTouchTarget::DetailButton: {
                nxui::Rect dialog = detailDialogRect(content);
                auto buttons = detailButtonRects(dialog, detailButtonCount());
                if (hitIndex >= 0 && hitIndex < (int)buttons.size() && buttons[(size_t)hitIndex].contains(x, y)) {
                    m_detailButtonIndex = hitIndex;
                    activateDetailButton(hitIndex);
                    if (m_activateSfxCb) m_activateSfxCb();
                }
                return true;
            }
            case ThemeTouchTarget::FullscreenPreview:
                if (hitIndex == fullscreenHitIndex(x, y)) {
                    if (hitIndex == 0) {
                        int before = m_detailScreenshotIndex;
                        stepDetailScreenshot(-1);
                        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                    } else if (hitIndex == 1) {
                        int before = m_detailScreenshotIndex;
                        stepDetailScreenshot(1);
                        if (before != m_detailScreenshotIndex && m_navSfxCb) m_navSfxCb();
                    } else if (hitIndex == 2) {
                        m_detailFullscreen = false;
                        m_detailFullscreenAnim.set(0.f, 0.16f, nxui::Easing::outQuad);
                        if (m_closeSfxCb) m_closeSfxCb();
                    }
                }
                return true;
            case ThemeTouchTarget::DetailBackdrop:
                if (!detailDialogRect(content).contains(x, y)) {
                    closeDetail();
                    if (m_closeSfxCb) m_closeSfxCb();
                }
                return true;
            case ThemeTouchTarget::None:
                return false;
        }
    }

    return false;
}

void ThemeShopScreen::drawCustomContent(nxui::Renderer& ren, const nxui::Rect&, const nxui::Rect& content, float opacity) {
    auto& i18n = nxui::I18n::instance();
    GridLayout layout = makeGridLayout(content);
    float slideT = std::clamp(m_contentSlideAnim.value(), 0.f, 1.f);
    float slideOffset = (1.f - slideT) * 24.f * (float)m_tabSwitchDir;
    float contentOpacity = opacity * slideT;
    int count = currentEntryCount();

    if (m_renderDiagnosticsActive) {
        const ThemeShopEntry* installed = selectedThemeShopEntry();
        const ThemeCatalogClient::Entry* community = selectedCommunityThemeEntry();
        DebugLog::log("[themeshop-render] tab=%d count=%d installedCount=%zu communityCount=%zu selectedInstalled=%s selectedCommunity=%s detail=%d fullscreen=%d focusArea=%d contentFocus=%d active=%d visible=%d opacity=%.2f contentOpacity=%.2f",
                      m_tabIndex,
                      count,
                      m_themeShopEntries.size(),
                      m_communityEntries.size(),
                      installed ? installed->id.c_str() : "<none>",
                      community ? community->id.c_str() : "<none>",
                      m_detailOpen ? 1 : 0,
                      m_detailFullscreen ? 1 : 0,
                      (int)m_focusArea,
                      (int)m_contentFocusArea,
                      isActive() ? 1 : 0,
                      isVisible() ? 1 : 0,
                      opacity,
                      contentOpacity);
    }

    layout.header.y += slideOffset;
    layout.refreshButton.y += slideOffset;
    layout.searchButton.y += slideOffset;
    layout.pagePrevButton.y += slideOffset;
    layout.pageCounter.y += slideOffset;
    layout.pageNextButton.y += slideOffset;
    layout.grid.y += slideOffset;

    // As duas abas de catalogo compartilham a renderizacao, mas nao o assunto:
    // uma traz os temas animados deste projeto e a outra os estaticos do
    // PoloNX. O cabecalho dizia "comunidade" nas duas.
    std::string title, subtitle;
    if (isMusicTab()) {
        if (!m_youtubeSearchQuery.empty()) {
            title = i18n.tr("themeshop.music.results_title", "YouTube Search Results");
            subtitle = i18n.tr("themeshop.music.results_subtitle", "Online songs found for: ") + "'" + m_youtubeSearchQuery + "'";
        } else {
            title = i18n.tr("themeshop.music.installed_title", "Installed Music Library");
            subtitle = i18n.tr("themeshop.music.installed_subtitle", "Your local songs saved in sdmc:/config/OmniLaunch/music/");
        }
    } else if (isAnimatedTab()) {
        title = i18n.tr("themeshop.animated.title", "Animated Themes");
        subtitle = i18n.tr("themeshop.animated.subtitle", "Moving wallpapers, with sound when the theme brings it.");
    } else if (isCommunityTab()) {
        title = i18n.tr("themeshop.community.title", "Browse Community Themes");
        subtitle = i18n.tr("themeshop.community.subtitle", "A visual catalog, not a settings selector.");
    } else {
        title = i18n.tr("themeshop.installed.title", "Installed Theme Library");
        subtitle = i18n.tr("themeshop.installed.subtitle", "Your local themes in a gallery layout.");
    }

    ren.drawText(title, {layout.header.x, layout.header.y + 2.f}, m_font,
                 m_theme->textPrimary.withAlpha(contentOpacity), 1.00f);
    ren.drawText(subtitle, {layout.header.x, layout.header.y + 38.f}, m_smallFont,
                 m_theme->textSecondary.withAlpha(0.92f * contentOpacity), 0.78f);

    // What the whole catalogue costs, before anything is chosen.
    //
    // Counted from the entries as they arrive, so a theme published tomorrow is
    // in this line the next time the console reads the index -- there is no
    // number to keep in step by hand. The search filter is deliberately not
    // applied: this answers "what is there", not "what am I looking at".
    {
        std::string totals;
        if (isMusicTab()) {
            if (m_youtubeSearchQuery.empty()) {
                totals = installedMusicTotals();
            }
        } else if (isCommunityTab()) {
            totals = communityCatalogueTotals();
        } else {
            totals = installedThemeTotals();
        }
        if (!totals.empty()) {
            ren.drawText(totals, {layout.header.x, layout.header.y + 60.f}, m_smallFont,
                         m_theme->textSecondary.withAlpha(0.70f * contentOpacity), 0.68f);
        }
    }

    std::string searchLabel = isMusicTab()
        ? (!m_youtubeSearchQuery.empty() ? m_youtubeSearchQuery : i18n.tr("themeshop.music.search_button", "Search YouTube"))
        : (!m_searchQuery.empty() ? m_searchQuery : i18n.tr("themeshop.search.button", "Search"));
    bool searchSelected = !m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header
        && (!isCommunityTab() || m_headerButtonIndex == 1);
    drawActionButtonChip(ren,
                         m_smallFont,
                         layout.searchButton,
                         searchLabel,
                         m_theme,
                         m_theme->textPrimary,
                         contentOpacity,
                         searchSelected ? 1.f : 0.f,
                         searchSelected ? 1.f : (m_searchQuery.empty() && m_youtubeSearchQuery.empty() ? 0.f : 0.72f),
                         0.78f);

    std::string counterText;
    if (isMusicTab() && m_youTubeClient.isSearching()) {
        counterText = i18n.tr("themeshop.music.searching", "Searching");
    } else if (count > 0) {
        int selected = std::max(0, currentSelectedIndex()) + 1;
        counterText = std::to_string(selected) + " / " + std::to_string(count);
    } else if (isCommunityTab() && m_communityTransferState.isRunning()) {
        counterText = i18n.tr("themeshop.community.counter_loading", "Loading");
    } else {
        counterText = "0 / 0";
    }
    nxui::Vec2 countSize = measureTextCached(m_font, counterText);
    float countRight = (isCommunityTab() ? layout.refreshButton.x : layout.searchButton.x) - 18.f;
    ren.drawText(counterText,
                 {countRight - countSize.x * 0.90f, layout.header.y + 4.f},
                 m_font,
                 m_theme->textPrimary.withAlpha(contentOpacity),
                 0.90f);

    if (!m_packageTransferState.label().empty()) {
        nxui::Color statusFill = m_theme->panelBase.withAlpha(0.18f);
        nxui::Color statusBorder = m_theme->panelBorder.withAlpha(0.28f);
        if (m_packageTransferState.isRunning()) {
            statusFill = m_theme->cursorNormal.withAlpha(0.16f);
            statusBorder = m_theme->cursorNormal.withAlpha(0.42f);
        } else if (m_packageTransferState.isReady()) {
            statusFill = nxui::Color(0.18f, 0.50f, 0.28f, 0.18f);
            statusBorder = nxui::Color(0.34f, 0.92f, 0.52f, 0.42f);
        } else if (m_packageTransferState.hasFailed()) {
            statusFill = nxui::Color(0.52f, 0.18f, 0.16f, 0.20f);
            statusBorder = nxui::Color(0.98f, 0.34f, 0.30f, 0.42f);
        }

        nxui::Rect statusChip = {
            layout.header.x,
            layout.header.bottom() - 6.f,
            std::min(layout.grid.width * 0.64f, 430.f),
            28.f
        };
        drawChip(ren,
                 m_smallFont,
                 statusChip,
                 transferLabel(m_packageTransferState),
                 statusFill,
                 statusBorder,
                 m_theme->textPrimary,
                 contentOpacity,
                 0.70f);
        if (m_packageTransferState.isRunning()) {
            drawSpinner(ren,
                        {statusChip.x + 16.f, statusChip.y + statusChip.height * 0.5f},
                        7.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        contentOpacity);
        }
    }

    if (isCommunityTab()) {
        std::string refreshLabel = m_communityTransferState.isRunning()
            ? i18n.tr("themeshop.community.refreshing", "Refreshing")
            : i18n.tr("themeshop.community.refresh", "Refresh");
        bool refreshSelected = !m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header
            && m_headerButtonIndex == 0;
        drawActionButtonChip(ren,
                             m_smallFont,
                             layout.refreshButton,
                             refreshLabel,
                             m_theme,
                             m_theme->textPrimary,
                             contentOpacity,
                             refreshSelected ? 1.f : 0.f,
                             refreshSelected ? 1.f : 0.68f,
                             0.78f);
        if (m_communityTransferState.isRunning()) {
            drawSpinner(ren,
                        {layout.refreshButton.x + 18.f, layout.refreshButton.y + layout.refreshButton.height * 0.5f},
                        8.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        contentOpacity);
        }
    }

    const int pages = pageCount();
    const int page = currentPage();
    const bool canPagePrev = page > 0;
    const bool canPageNext = page + 1 < pages;
    const bool pagerFocused = !m_detailOpen
        && m_focusArea == FocusArea::Content
        && m_contentFocusArea == ContentFocusArea::Pager;
    drawActionButtonChip(ren,
                         m_smallFont,
                         layout.pagePrevButton,
                         i18n.tr("themeshop.page.prev", "Previous"),
                         m_theme,
                         canPagePrev ? m_theme->textPrimary : m_theme->textSecondary,
                         contentOpacity,
                         pagerFocused && m_pageButtonIndex == 0 ? 1.f : 0.f,
                         canPagePrev ? (pagerFocused && m_pageButtonIndex == 0 ? 1.f : 0.18f) : 0.f,
                         0.74f);
    drawChip(ren,
             m_smallFont,
             layout.pageCounter,
             std::to_string(page + 1) + " / " + std::to_string(pages),
             m_theme->panelBase.withAlpha(0.16f),
             m_theme->panelBorder.withAlpha(0.24f),
             m_theme->textPrimary,
             contentOpacity,
             0.74f);
    drawActionButtonChip(ren,
                         m_smallFont,
                         layout.pageNextButton,
                         i18n.tr("themeshop.page.next", "Next"),
                         m_theme,
                         canPageNext ? m_theme->textPrimary : m_theme->textSecondary,
                         contentOpacity,
                         pagerFocused && m_pageButtonIndex == 1 ? 1.f : 0.f,
                         canPageNext ? (pagerFocused && m_pageButtonIndex == 1 ? 1.f : 0.18f) : 0.f,
                         0.74f);

    if (pagerFocused) {
        nxui::Rect target = m_pageButtonIndex == 0 ? layout.pagePrevButton : layout.pageNextButton;
        m_focusCursor.moveTo(target.expanded(2.f), 20.f, 0.08f);
    }

    if (isCommunityTab() && !m_communityTransferState.label().empty() && m_packageTransferState.label().empty()) {
        ren.drawText(m_communityTransferState.label(),
                     {layout.header.x, layout.header.bottom() - 6.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.78f * contentOpacity),
                     0.72f);
    }

    if (isCommunityTab() && m_communityTransferState.isRunning() && count == 0) {
        nxui::Vec2 center = {layout.grid.x + layout.grid.width * 0.5f, layout.grid.y + layout.grid.height * 0.42f};
        drawSpinner(ren, center, 22.f, m_uiTime, m_theme->cursorNormal, contentOpacity);
        std::string loadingLabel = i18n.tr("themeshop.community.loading", "Loading theme catalog...");
        nxui::Vec2 size = measureTextCached(m_font, loadingLabel);
        ren.drawText(loadingLabel,
                     {center.x - size.x * 0.40f, center.y + 28.f},
                     m_font,
                     m_theme->textPrimary.withAlpha(contentOpacity),
                     0.88f);
        return;
    }

    if (count == 0) {
        nxui::Rect emptyBox = {
            layout.grid.x + layout.grid.width * 0.14f,
            layout.grid.y + layout.grid.height * 0.22f,
            layout.grid.width * 0.72f,
            170.f
        };
        ren.drawRoundedRect(emptyBox, m_theme->panelBase.withAlpha(0.16f * contentOpacity), 20.f);
        ren.drawRoundedRectOutline(emptyBox, m_theme->panelBorder.withAlpha(0.24f * contentOpacity), 20.f, 1.2f);
        std::string emptyTitle, emptySubtitle;
        if (isMusicTab()) {
            if (m_youTubeClient.isSearching()) {
                emptyTitle = i18n.tr("themeshop.music.searching_title", "Searching YouTube...");
                emptySubtitle = i18n.tr("themeshop.music.searching_hint", "Connecting to YouTube and fetching soundtracks...");
            } else if (!m_youtubeSearchQuery.empty()) {
                emptyTitle = i18n.tr("themeshop.music.no_results", "No songs found for this search.");
                emptySubtitle = i18n.tr("themeshop.music.no_results_hint", "Press Search (or X) to try different keywords.");
            } else {
                emptyTitle = i18n.tr("themeshop.music.empty_installed_title", "No Local Songs Found");
                emptySubtitle = i18n.tr("themeshop.music.empty_installed_hint", "Press Search (or X) to find and download songs from YouTube.");
            }
        } else {
            bool searchActive = !m_searchQuery.empty();
            emptyTitle = searchActive
                ? i18n.tr("themeshop.search.no_results", "No themes match this search.")
                : (isCommunityTab()
                    ? i18n.tr("themeshop.community.catalog_empty", "No published themes are listed in the catalog.")
                    : i18n.tr("themeshop.installed.empty", "No installed themes found."));
            emptySubtitle = searchActive
                ? i18n.tr("themeshop.search.no_results_hint", "Press X or use Search to change or clear the filter.")
                : (isCommunityTab() && m_communityTransferState.hasFailed()
                    ? i18n.tr("themeshop.community.catalog_failed", "The catalog could not be fetched. Use Refresh to try again.")
                    : i18n.tr("themeshop.community.empty_hint", "Add themes to the repository and they will appear here."));
        }

        nxui::Vec2 titleSize = measureTextCached(m_font, emptyTitle);
        ren.drawText(emptyTitle,
                     {emptyBox.x + (emptyBox.width - titleSize.x * 0.90f) * 0.5f, emptyBox.y + 40.f},
                     m_font,
                     m_theme->textPrimary.withAlpha(contentOpacity),
                     0.90f);
        nxui::Vec2 subSize = measureTextCached(m_smallFont, emptySubtitle);
        ren.drawText(emptySubtitle,
                     {emptyBox.x + (emptyBox.width - subSize.x * 0.76f) * 0.5f, emptyBox.y + 94.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.90f * contentOpacity),
                     0.76f);
        if (isMusicTab() && m_youTubeClient.isSearching()) {
            drawSpinner(ren,
                        {emptyBox.x + emptyBox.width * 0.5f, emptyBox.y + 135.f},
                        10.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        contentOpacity);
        }
        if (!m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header) {
            auto headerButtons = headerButtonRects(layout, isCommunityTab());
            if (!headerButtons.empty()) {
                int headerIndex = std::clamp(m_headerButtonIndex, 0, (int)headerButtons.size() - 1);
                m_focusCursor.moveTo(headerButtons[(size_t)headerIndex].expanded(2.f), 20.f, 0.08f);
            }
        }
        return;
    }

    int scrollRow = currentScrollRowRef();
    int start = scrollRow * kGridCols;
    int end = std::min(count, start + kGridCols * kVisibleRows);
    int selected = std::max(0, currentSelectedIndex());
    float reveal = std::clamp(m_tabReveal.value(), 0.f, 1.f);

    for (int globalIndex = start; globalIndex < end; ++globalIndex) {
        int localIndex = globalIndex - start;
        nxui::Rect card = gridCardRect(layout, localIndex);
        float delay = std::min(0.42f, localIndex * 0.03f);
        float localReveal = std::clamp((reveal - delay) / 0.30f, 0.f, 1.f);
        float rowOpacity = contentOpacity * localReveal;
        float rise = (1.f - localReveal) * 14.f;
        card.y += rise;

        bool cardSelected = (globalIndex == selected);
        std::string titleText;
        std::string subtitleText;
        // Drawn at the right end of the author's line rather than appended to
        // it: the author is what gets ellipsized when the card is narrow, and
        // the size must not be the half that disappears.
        std::string sizeText;
        std::string versionText;
        const nxui::Texture* previewTexture = nullptr;
        PreviewPhase previewPhase = PreviewPhase::Failed;
        bool previewRequested = false;
        bool builtInDefaultPreview = false;
        bool builtInLightPreview = false;
        bool activeTheme = false;
        bool installedHere = false;
        int sheetCols = 0, sheetRows = 0;
        float sheetFps = 10.f;

        if (isMusicTab()) {
            YouTubeClient::TrackItem track;
            if (m_youTubeClient.getTrack(static_cast<size_t>(globalIndex), track)) {
                titleText = track.title;
                subtitleText = track.author.empty() ? "YouTube" : track.author;
                versionText = track.duration;
                if (track.isDownloading) {
                    int pct = static_cast<int>(track.downloadProgress * 100.f);
                    sizeText = std::to_string(pct) + "%";
                } else if (track.isDownloaded) {
                    sizeText = "✓ " + i18n.tr("themeshop.music.downloaded", "Downloaded");
                    installedHere = true;
                } else {
                    sizeText = estimateDownloadSize(track.duration);
                }
                if (!track.thumbnailUrl.empty()) {
                    previewTexture = m_youTubeClient.thumbnailTexture(track.id);
                    auto phase = m_youTubeClient.thumbnailPhase(track.id);
                    previewPhase = (phase == YouTubeClient::PreviewPhase::Ready) ? PreviewPhase::Ready :
                                   (phase == YouTubeClient::PreviewPhase::Downloaded || phase == YouTubeClient::PreviewPhase::Loading) ? PreviewPhase::Loading :
                                   PreviewPhase::Idle;
                    if (previewPhase == PreviewPhase::Idle) {
                        m_youTubeClient.primeThumbnail(track.id, track.thumbnailUrl);
                        previewPhase = PreviewPhase::Loading;
                    }
                    previewRequested = true;
                } else {
                    previewRequested = false;
                    previewPhase = PreviewPhase::Idle;
                }
            }
        } else if (isCommunityTab()) {
            const auto& entry = m_communityEntries[(size_t)globalIndex];
            titleText = entry.name;
            subtitleText = entry.author.empty() ? i18n.tr("themeshop.community.author_unknown", "Unknown") : entry.author;
            sizeText = formatBytes(entry.packageBytes);
            versionText = entry.version;
            previewTexture = communityPreviewTexture(entry);
            previewPhase = communityPreviewPhase(entry);
            previewRequested = !entry.cover.empty() || !entry.thumbSheet.empty();
            sheetCols = entry.thumbCols;
            sheetRows = entry.thumbRows;
            sheetFps  = entry.thumbFps;
            // Marca o que já está no console, para não ser preciso abrir cada
            // tema só para descobrir que ele já foi baixado.
            installedHere = installedEntryForCatalogue(entry.id) != nullptr;
        } else {
            const auto& entry = m_themeShopEntries[(size_t)globalIndex];
            titleText = entry.name;
            std::string author = entry.author.empty()
                ? i18n.tr("themeshop.community.author_unknown", "Unknown")
                : entry.author;
            subtitleText = entry.source.empty() ? author : (author + " - " + entry.source);
            sizeText = formatBytes(installedThemeBytes(entry.installPath));
            versionText = entry.version;
            activeTheme = entry.active;
            previewRequested = !entry.coverPath.empty();
            builtInDefaultPreview = entry.id == "builtin:Default Dark" || entry.id == "builtin:Default Light";
            builtInLightPreview = entry.id == "builtin:Default Light";
            if (previewRequested) {
                primeInstalledPreview(entry.coverPath);
                previewTexture = installedPreviewTexture(entry.coverPath);
                previewPhase = installedPreviewPhase(entry.coverPath);
            }
        }

        const bool isGridFocused = !m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Grid;
        const bool showSelected = cardSelected && isGridFocused;

        nxui::Color cardFill = m_theme->panelBase.withAlpha((showSelected ? 0.40f : 0.14f) * rowOpacity);
        nxui::Color cardBorder = showSelected
            ? m_theme->cursorNormal.withAlpha(0.95f * rowOpacity)
            : m_theme->panelBorder.withAlpha(0.24f * rowOpacity);
        ren.drawRoundedRect(card, cardFill, 22.f);
        ren.drawRoundedRectOutline(card, cardBorder, 22.f, showSelected ? 3.0f : 1.0f);
        if (showSelected) {
            ren.drawRoundedRectOutline(card.expanded(2.f), m_theme->cursorNormal.withAlpha(0.40f * rowOpacity), 24.f, 2.0f);
        }

        nxui::Rect previewBounds = {card.x + 10.f, card.y + 10.f, card.width - 20.f, card.height - 76.f};
        nxui::Rect preview = fitAspectRect(previewBounds, kPreviewAspect);
        bool previewLoading = previewPhase == PreviewPhase::Loading || previewPhase == PreviewPhase::Downloaded;
        std::string previewLabel = previewRequested
            ? (previewLoading
                ? i18n.tr("themeshop.preview.loading", "Loading screenshot...")
                : i18n.tr("themeshop.preview.unavailable", "Screenshot unavailable"))
            : i18n.tr("themeshop.preview.missing", "No screenshot");
        drawThemePreview(ren,
                         m_smallFont,
                         m_theme,
                         preview,
                         previewTexture,
                         previewLabel,
                         previewLoading,
                         m_uiTime,
                         rowOpacity,
                         sheetCols, sheetRows, sheetFps,
                         builtInDefaultPreview, builtInLightPreview);

        if (!versionText.empty()) {
            float chipWidth = std::max(74.f, std::min(108.f, 32.f + measureTextCached(m_smallFont, versionText).x * 0.64f));
            nxui::Rect versionChip = {preview.right() - chipWidth - 10.f, preview.y + 10.f, chipWidth, 26.f};
            drawChip(ren,
                     m_smallFont,
                     versionChip,
                     versionText,
                     nxui::Color(0.f, 0.f, 0.f, 0.26f),
                     nxui::Color(1.f, 1.f, 1.f, 0.16f),
                     m_theme->textPrimary,
                     rowOpacity,
                     0.64f);
        }

        if (installedHere) {
            const std::string label = i18n.tr("themeshop.community.installed", "Installed");
            float chipWidth = std::max(92.f, std::min(126.f, 32.f + measureTextCached(m_smallFont, label).x * 0.66f));
            nxui::Rect chip = {preview.x + 10.f, preview.y + 10.f, chipWidth, 26.f};
            drawChip(ren,
                     m_smallFont,
                     chip,
                     label,
                     nxui::Color(0.10f, 0.22f, 0.38f, 0.36f),
                     nxui::Color(0.36f, 0.68f, 0.98f, 0.52f),
                     m_theme->textPrimary,
                     rowOpacity,
                     0.66f);
        }

        if (activeTheme) {
            const std::string activeLabel = i18n.tr("themeshop.installed.status_active", "Active");
            float chipWidth = std::max(82.f, std::min(114.f, 32.f + measureTextCached(m_smallFont, activeLabel).x * 0.66f));
            nxui::Rect activeChip = {preview.x + 10.f, preview.y + 10.f, chipWidth, 26.f};
            drawChip(ren,
                     m_smallFont,
                     activeChip,
                     activeLabel,
                     nxui::Color(0.10f, 0.34f, 0.14f, 0.34f),
                     nxui::Color(0.34f, 0.92f, 0.52f, 0.50f),
                     m_theme->textPrimary,
                     rowOpacity,
                     0.66f);
        }

        std::string titleFitted = ellipsize(m_font, titleText, card.width - 18.f, 0.86f);
        ren.drawText(titleFitted,
                     {card.x + 10.f, preview.bottom() + 10.f},
                     m_font,
                     m_theme->textPrimary.withAlpha(rowOpacity),
                     0.86f);
        float subtitleWidth = card.width - 18.f;
        if (!sizeText.empty()) {
            const float sizeWidth = measureTextCached(m_smallFont, sizeText).x * 0.72f;
            ren.drawText(sizeText,
                         {card.right() - 10.f - sizeWidth, card.bottom() - 30.f},
                         m_smallFont,
                         m_theme->textSecondary.withAlpha(0.78f * rowOpacity),
                         0.72f);
            subtitleWidth -= sizeWidth + 10.f;
        }
        std::string subtitleFitted = ellipsize(m_smallFont, subtitleText, subtitleWidth, 0.76f);
        ren.drawText(subtitleFitted,
                     {card.x + 10.f, card.bottom() - 30.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.92f * rowOpacity),
                     0.76f);

        if (!m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Grid && cardSelected) {
            m_focusCursor.moveTo(card.expanded(2.f), 22.f, 0.08f);
        }
    }

    if (!m_detailOpen && m_focusArea == FocusArea::Content && m_contentFocusArea == ContentFocusArea::Header) {
        auto headerButtons = headerButtonRects(layout, isCommunityTab());
        if (!headerButtons.empty()) {
            int headerIndex = std::clamp(m_headerButtonIndex, 0, (int)headerButtons.size() - 1);
            m_focusCursor.moveTo(headerButtons[(size_t)headerIndex].expanded(2.f), 20.f, 0.08f);
        }
    }

    int firstVisible = start + 1;
    int lastVisible = end;
    std::string footer = std::to_string(firstVisible) + "-" + std::to_string(lastVisible) + " / " + std::to_string(count);
    ren.drawText(footer,
                 {layout.grid.x, layout.grid.bottom() + 15.f},
                 m_smallFont,
                 m_theme->textSecondary.withAlpha(0.84f * contentOpacity),
                 0.76f);

    float detailT = std::clamp(m_detailSheetAnim.value(), 0.f, 1.f);
    if (!m_detailOpen && detailT <= 0.01f)
        return;

    float detailOpacity = contentOpacity * detailT;
    ren.drawRoundedRect(content, nxui::Color(0.f, 0.f, 0.f, 0.24f * detailOpacity), 26.f);
    nxui::Rect dialog = detailDialogRect(content);
    dialog = scaledRect(dialog, 0.96f + 0.04f * detailT);
    drawLiquidGlassPanel(ren, m_theme, dialog, 24.f, detailOpacity, 1.4f);

    nxui::Rect preview = detailPreviewRect(dialog);
    int detailSheetCols = 0, detailSheetRows = 0;
    float detailSheetFps = 4.f;
    std::string detailTitle;
    std::string detailSubtitle;
    std::string detailInfoA;
    std::string detailInfoB;
    // O tamanho no cartao ganhou linha propria. Junto com o download numa linha
    // so, a frase passava da largura da coluna e as reticencias comiam
    // justamente a metade que interessa a quem esta decidindo se cabe.
    std::string detailInfoBExtra;
    std::string detailInfoC;
    const nxui::Texture* detailPreviewTexture = nullptr;
    PreviewPhase detailPreviewPhase = PreviewPhase::Failed;
    bool detailPreviewRequested = false;
    bool detailBuiltInDefaultPreview = false;
    bool detailBuiltInLightPreview = false;
    int detailScreenshotTotal = 0;
    DetailPreviewControls previewControls = detailPreviewControls(preview);

    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (!track)
            return;
        detailTitle = track->title;
        detailSubtitle = track->author.empty() ? "YouTube" : track->author;
        detailInfoA = track->isDownloaded ? (i18n.tr("themeshop.music.size", "Size: ") + track->duration)
                                          : (i18n.tr("themeshop.music.duration", "Duration: ") + track->duration + "  •  " + i18n.tr("themeshop.music.est_size", "Est. Size: ") + estimateDownloadSize(track->duration));
        detailInfoB = track->isDownloaded ? i18n.tr("themeshop.music.status_downloaded", "Status: Saved in sdmc:/config/OmniLaunch/music/")
                                          : (i18n.tr("themeshop.music.video_id", "Video ID: ") + track->id);
        detailInfoC = track->isDownloaded
            ? i18n.tr("themeshop.music.status_ready_play", "Status: Ready to play or delete")
            : i18n.tr("themeshop.music.status_ready", "Status: Ready to download (.mp3)");
        if (!track->thumbnailUrl.empty()) {
            detailPreviewTexture = m_youTubeClient.thumbnailTexture(track->id);
            auto phase = m_youTubeClient.thumbnailPhase(track->id);
            detailPreviewPhase = (phase == YouTubeClient::PreviewPhase::Ready) ? PreviewPhase::Ready :
                                 (phase == YouTubeClient::PreviewPhase::Downloaded || phase == YouTubeClient::PreviewPhase::Loading) ? PreviewPhase::Loading :
                                 PreviewPhase::Idle;
            if (detailPreviewPhase == PreviewPhase::Idle) {
                m_youTubeClient.primeThumbnail(track->id, track->thumbnailUrl);
                detailPreviewPhase = PreviewPhase::Loading;
            }
            detailPreviewRequested = true;
        } else {
            detailPreviewRequested = false;
            detailPreviewPhase = PreviewPhase::Idle;
        }
    } else if (isCommunityTab()) {
        const auto* entry = selectedCommunityThemeEntry();
        if (!entry)
            return;
        detailTitle = entry->name;
        detailSubtitle = entry->author.empty() ? i18n.tr("themeshop.community.author_unknown", "Unknown") : entry->author;
        detailInfoA = i18n.tr("themeshop.community.version", "Version") + std::string(": ")
            + (entry->version.empty() ? i18n.tr("themeshop.community.version_unknown", "Unknown") : entry->version);
        // Where the two sizes belong: the detail screen is where a theme is
        // accepted or refused, and it has room for both. The manifest path this
        // line used to carry is a catalogue implementation detail -- it stays
        // as the fallback for an index that declares no sizes.
        const std::string downloadSize = formatBytes(entry->packageBytes);
        const std::string cardSize = formatBytes(entry->installedBytes);
        if (!downloadSize.empty() || !cardSize.empty()) {
            detailInfoB = i18n.tr("themeshop.community.size", "Size") + std::string(": ");
            if (!downloadSize.empty())
                detailInfoB += downloadSize + " " + i18n.tr("themeshop.catalog.to_download", "to download");
            if (!cardSize.empty())
                detailInfoBExtra = cardSize + " " + i18n.tr("themeshop.catalog.on_card", "on the SD card");
        } else {
            detailInfoB = i18n.tr("themeshop.community.manifest", "Manifest") + std::string(": ")
                + (entry->manifest.empty() ? i18n.tr("themeshop.community.manifest_missing", "Not provided") : entry->manifest);
        }
        detailScreenshotTotal = std::max(0, (int)entry->screenshots.size());
        std::string detailPreviewPath = currentDetailCommunityPreviewPath();
        // A folha tambem no detalhe e na tela cheia. Ela e a unica coisa que se
        // move: as capturas do tema sao imagens paradas, e um tema animado
        // apresentado por uma foto parada nao mostra o que ele e.
        if (const auto* selected = selectedCommunityThemeEntry()) {
            // As duas folhas, nao so a pequena. A grande tem a mesma grade de
            // celulas -- muda o tamanho de cada uma -- e comparar so com
            // thumbSheet deixava a tela cheia sem colunas nem linhas, o que faz
            // o desenho tratar a folha inteira como uma imagem so e mostrar as
            // vinte celulas de uma vez.
            const bool isSheet =
                (!selected->thumbSheet.empty()   && detailPreviewPath == selected->thumbSheet)
             || (!selected->thumbSheetHd.empty() && detailPreviewPath == selected->thumbSheetHd);
            if (isSheet) {
                detailSheetCols = selected->thumbCols;
                detailSheetRows = selected->thumbRows;
                detailSheetFps  = selected->thumbFps;
            }
        }

        // Qual imagem a tela cheia pediu e em que estado ela esta. A previa
        // sumia sem deixar rastro: "captura indisponivel" e o log sem uma
        // palavra sobre qual arquivo foi pedido.
        if (m_detailFullscreen && detailPreviewPath != m_lastLoggedDetailPath) {
            m_lastLoggedDetailPath = detailPreviewPath;
            DebugLog::log("[themeshop] tela cheia pede %s (estado=%d celulas=%dx%d)",
                          detailPreviewPath.c_str(), (int)communityPreviewPhase(detailPreviewPath),
                          detailSheetCols, detailSheetRows);
        }
        detailPreviewTexture = communityPreviewTexture(detailPreviewPath);
        detailPreviewPhase = communityPreviewPhase(detailPreviewPath);
        detailPreviewRequested = !detailPreviewPath.empty();
        if (!detailPreviewRequested) {
            detailInfoC = i18n.tr("themeshop.community.cover_missing", "No screenshot has been declared for this theme yet.");
        } else if (detailPreviewTexture) {
            detailInfoC = detailScreenshotTotal > 1
                ? i18n.tr("themeshop.community.cover_present", "Use Left/Right to browse screenshots and A for fullscreen.")
                : i18n.tr("themeshop.community.cover_single", "Press A to open this screenshot in fullscreen.");
        } else if (detailPreviewPhase == PreviewPhase::Loading || detailPreviewPhase == PreviewPhase::Downloaded) {
            detailInfoC = i18n.tr("themeshop.community.cover_loading", "Loading theme screenshot...");
        } else {
            detailInfoC = i18n.tr("themeshop.community.cover_unavailable", "The screenshot could not be loaded. Placeholder shown instead.");
        }
    } else {
        const auto* entry = selectedThemeShopEntry();
        if (!entry)
            return;
        detailTitle = entry->name;
        std::string author = entry->author.empty()
            ? i18n.tr("themeshop.community.author_unknown", "Unknown")
            : entry->author;
        detailSubtitle = entry->source.empty() ? author : (author + " - " + entry->source);
        detailInfoA = i18n.tr("themeshop.installed.status", "Status") + std::string(": ")
            + (entry->active ? i18n.tr("themeshop.installed.status_active", "Active") : i18n.tr("themeshop.installed.status_available", "Available"));
        detailInfoB = i18n.tr("themeshop.installed.sound", "Bundled Sound Pack") + std::string(": ")
            + (entry->soundPreset.empty() ? i18n.tr("themeshop.installed.sound_none", "None") : entry->soundPreset);
        detailInfoC = entry->removable
            ? i18n.tr("themeshop.installed.remove_hint", "This theme can be removed from the console.")
            : i18n.tr("themeshop.installed.builtin_hint", "This is part of the built-in theme set.");
        detailPreviewRequested = !entry->coverPath.empty();
        detailBuiltInDefaultPreview = entry->id == "builtin:Default Dark" || entry->id == "builtin:Default Light";
        detailBuiltInLightPreview = entry->id == "builtin:Default Light";
        if (detailPreviewRequested) {
            primeInstalledPreview(entry->coverPath);
            detailPreviewTexture = installedPreviewTexture(entry->coverPath);
            detailPreviewPhase = installedPreviewPhase(entry->coverPath);
        }
    }

    bool detailPreviewLoading = detailPreviewPhase == PreviewPhase::Loading || detailPreviewPhase == PreviewPhase::Downloaded;
    std::string detailPreviewLabel = detailPreviewRequested
        ? (detailPreviewLoading
            ? i18n.tr("themeshop.preview.loading", "Loading screenshot...")
            : i18n.tr("themeshop.preview.unavailable", "Screenshot unavailable"))
        : i18n.tr("themeshop.preview.missing", "No screenshot");
    drawThemePreview(ren,
                     m_smallFont,
                     m_theme,
                     preview,
                     detailPreviewTexture,
                     detailPreviewLabel,
                     detailPreviewLoading,
                     m_uiTime,
                     detailOpacity,
                     detailSheetCols, detailSheetRows, detailSheetFps,
                     detailBuiltInDefaultPreview, detailBuiltInLightPreview);

    if (isCommunityTab() && detailPreviewRequested) {
        auto previewButtons = detailPreviewControlRects(previewControls, preview, detailScreenshotTotal);
        if (detailScreenshotTotal > 1) {
            bool canGoPrev = m_detailScreenshotIndex > 0;
            bool canGoNext = m_detailScreenshotIndex + 1 < detailScreenshotTotal;
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 previewControls.prev,
                                 i18n.tr("themeshop.preview.prev", "Prev"),
                                 m_theme,
                                 canGoPrev ? m_theme->textPrimary : m_theme->textSecondary,
                                 detailOpacity,
                                 0.f,
                                 canGoPrev ? 0.24f : 0.f,
                                 0.78f);
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 previewControls.next,
                                 i18n.tr("themeshop.preview.next", "Next"),
                                 m_theme,
                                 canGoNext ? m_theme->textPrimary : m_theme->textSecondary,
                                 detailOpacity,
                                 0.f,
                                 canGoNext ? 0.24f : 0.f,
                                 0.78f);
            drawChip(ren,
                     m_smallFont,
                     previewControls.counter,
                     std::to_string(m_detailScreenshotIndex + 1) + " / " + std::to_string(detailScreenshotTotal),
                     nxui::Color(0.f, 0.f, 0.f, 0.22f),
                     m_theme->panelBorder.withAlpha(0.20f),
                     m_theme->textPrimary,
                     detailOpacity,
                     0.68f);
        }

        if (m_detailFocusArea == DetailFocusArea::Preview && !previewButtons.empty()) {
            m_focusCursor.moveTo(preview.expanded(4.f), 20.f, 0.08f);
        }
    }

    float infoBlockHeight = (isCommunityTab() && !m_packageTransferState.label().empty()) ? 272.f : 224.f;
    // A linha extra do tamanho empurra o resto para baixo, entao o bloco cresce
    // junto -- senao a ultima linha sai pela borda do dialogo.
    if (!detailInfoBExtra.empty())
        infoBlockHeight += 32.f;
    nxui::Rect infoBounds = {
        preview.right() + 24.f,
        dialog.y + 26.f,
        dialog.right() - preview.right() - 46.f,
        dialog.height - 122.f
    };
    nxui::Rect info = {
        infoBounds.x,
        infoBounds.y + std::max(0.f, (infoBounds.height - infoBlockHeight) * 0.5f),
        infoBounds.width,
        infoBlockHeight
    };
    ren.drawText(ellipsize(m_font, detailTitle, info.width, 1.10f),
                 {info.x, info.y + 2.f},
                 m_font,
                 m_theme->textPrimary.withAlpha(detailOpacity),
                 1.10f);
    ren.drawText(ellipsize(m_smallFont, detailSubtitle, info.width, 0.84f),
                 {info.x, info.y + 44.f},
                 m_smallFont,
                 m_theme->textSecondary.withAlpha(0.90f * detailOpacity),
                 0.84f);

    ren.drawText(detailInfoA,
                 {info.x, info.y + 108.f},
                 m_smallFont,
                 m_theme->textPrimary.withAlpha(detailOpacity),
                 0.80f);
    ren.drawText(ellipsize(m_smallFont, detailInfoB, info.width, 0.76f),
                 {info.x, info.y + 150.f},
                 m_smallFont,
                 m_theme->textSecondary.withAlpha(0.92f * detailOpacity),
                 0.76f);
    float infoCy = info.y + 190.f;
    if (!detailInfoBExtra.empty()) {
        ren.drawText(ellipsize(m_smallFont, detailInfoBExtra, info.width, 0.76f),
                     {info.x, info.y + 182.f},
                     m_smallFont,
                     m_theme->textSecondary.withAlpha(0.92f * detailOpacity),
                     0.76f);
        infoCy = info.y + 222.f;
    }
    ren.drawText(ellipsize(m_smallFont, detailInfoC, info.width, 0.76f),
                 {info.x, infoCy},
                 m_smallFont,
                 m_theme->textSecondary.withAlpha(0.92f * detailOpacity),
                 0.76f);

    if (isCommunityTab() && !m_packageTransferState.label().empty()) {
        nxui::Color statusColor = m_theme->textSecondary;
        if (m_packageTransferState.isRunning())
            statusColor = m_theme->textPrimary;
        else if (m_packageTransferState.isReady())
            statusColor = nxui::Color(0.62f, 0.96f, 0.72f, 1.f);
        else if (m_packageTransferState.hasFailed())
            statusColor = nxui::Color(1.f, 0.56f, 0.52f, 1.f);

        ren.drawText(ellipsize(m_smallFont, transferLabel(m_packageTransferState), info.width - 24.f, 0.76f),
                     {info.x + 24.f, info.y + 236.f},
                     m_smallFont,
                     statusColor.withAlpha(detailOpacity),
                     0.76f);
        if (m_packageTransferState.isRunning()) {
            drawSpinner(ren,
                        {info.x + 10.f, info.y + 246.f},
                        7.f,
                        m_uiTime,
                        m_theme->cursorNormal,
                        detailOpacity);
        }
    }

    float fullscreenT = std::clamp(m_detailFullscreenAnim.value(), 0.f, 1.f);
    bool fullscreenVisible = m_detailFullscreen || fullscreenT > 0.01f;
    if (fullscreenVisible) {
        FullscreenOverlayLayout fullscreen = makeFullscreenOverlayLayout(content);
        nxui::Rect animatedPreview = lerpRect(preview, fullscreen.preview, fullscreenT);
        ren.drawRoundedRect(content, nxui::Color(0.f, 0.f, 0.f, 0.58f * detailOpacity * fullscreenT), 26.f);
        drawThemePreview(ren,
                         m_smallFont,
                         m_theme,
                         animatedPreview,
                         detailPreviewTexture,
                         detailPreviewLabel,
                         detailPreviewLoading,
                         m_uiTime,
                         detailOpacity,
                         detailSheetCols, detailSheetRows, detailSheetFps);

        drawActionButtonChip(ren,
                             m_smallFont,
                             fullscreen.close,
                             i18n.tr("button.close", "Close"),
                             m_theme,
                             m_theme->textPrimary,
                             detailOpacity * fullscreenT,
                             0.f,
                             0.38f,
                             0.84f);

        if (detailScreenshotTotal > 1) {
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 fullscreen.prev,
                                 i18n.tr("themeshop.preview.prev", "Prev"),
                                 m_theme,
                                 m_theme->textPrimary,
                                 detailOpacity * fullscreenT,
                                 0.f,
                                 0.32f,
                                 0.84f);
            drawActionButtonChip(ren,
                                 m_smallFont,
                                 fullscreen.next,
                                 i18n.tr("themeshop.preview.next", "Next"),
                                 m_theme,
                                 m_theme->textPrimary,
                                 detailOpacity * fullscreenT,
                                 0.f,
                                 0.32f,
                                 0.84f);
            drawChip(ren,
                     m_smallFont,
                     fullscreen.counter,
                     std::to_string(m_detailScreenshotIndex + 1) + " / " + std::to_string(detailScreenshotTotal),
                     nxui::Color(0.f, 0.f, 0.f, 0.50f),
                     nxui::Color(1.f, 1.f, 1.f, 0.24f),
                     m_theme->textPrimary,
                     detailOpacity * fullscreenT,
                     0.70f);
        }

        m_focusCursor.moveTo(fullscreen.close.expanded(2.f), 20.f, 0.08f);
        return;
    }

    std::vector<std::string> buttonLabels;
    if (isMusicTab()) {
        const auto* track = selectedMusicTrack();
        if (track && track->isDownloading) {
            buttonLabels.push_back(i18n.tr("themeshop.music.downloading", "Downloading..."));
        } else if (track && track->isDownloaded) {
            buttonLabels.push_back(i18n.tr("themeshop.music.play_track", "Play Track"));
            buttonLabels.push_back(i18n.tr("themeshop.music.delete_track", "Delete Track"));
        } else {
            buttonLabels.push_back(i18n.tr("themeshop.music.download_action", "Download Track (.mp3)"));
        }
    } else if (isCommunityTab()) {
        const auto* catalogueEntry = selectedCommunityThemeEntry();
        const bool alreadyInstalled =
            catalogueEntry && installedEntryForCatalogue(catalogueEntry->id) != nullptr;
        if (alreadyInstalled) {
            buttonLabels.push_back(i18n.tr("themeshop.installed.apply", "Apply Theme"));
            buttonLabels.push_back(i18n.tr("themeshop.installed.remove", "Remove Theme"));
        } else {
            buttonLabels.push_back(i18n.tr("themeshop.community.install", "Install"));
            buttonLabels.push_back(i18n.tr("themeshop.community.download_apply", "Download + Apply"));
        }
    } else {
        buttonLabels.push_back(i18n.tr("themeshop.installed.apply", "Apply Theme"));
        const auto* entry = selectedThemeShopEntry();
        if (entry && entry->removable)
            buttonLabels.push_back(i18n.tr("themeshop.installed.remove", "Remove Theme"));
    }

    auto buttons = detailButtonRects(dialog, (int)buttonLabels.size());
    bool disableCommunityButtons = isCommunityTab() && m_packageTransferState.isRunning();
    for (int i = 0; i < (int)buttons.size(); ++i) {
        bool selectedButton = !disableCommunityButtons && (i == m_detailButtonIndex) && (m_detailFocusArea == DetailFocusArea::Buttons);
        nxui::Color textColor = disableCommunityButtons ? m_theme->textSecondary : m_theme->textPrimary;
        drawActionButtonChip(ren,
                             m_smallFont,
                             buttons[(size_t)i],
                             buttonLabels[(size_t)i],
                             m_theme,
                             textColor,
                             contentOpacity,
                             selectedButton ? 1.f : 0.f,
                             disableCommunityButtons ? 0.f : (selectedButton ? 1.f : 0.18f),
                             0.84f);
        if (selectedButton) {
            ren.drawRoundedRectOutline(buttons[(size_t)i].expanded(2.f),
                                       m_theme->cursorNormal.withAlpha(0.95f * contentOpacity),
                                       20.f, 2.5f);
            ren.drawRoundedRectOutline(buttons[(size_t)i].expanded(4.f),
                                       m_theme->cursorNormal.withAlpha(0.40f * contentOpacity),
                                       22.f, 1.5f);
        }
    }

    if (!(isCommunityTab() && m_detailFocusArea == DetailFocusArea::Preview && detailPreviewRequested)
        && !buttons.empty() && !disableCommunityButtons) {
        m_focusCursor.moveTo(buttons[(size_t)std::clamp(m_detailButtonIndex, 0, (int)buttons.size() - 1)].expanded(2.f),
                             20.f,
                             0.08f);
    }
}
