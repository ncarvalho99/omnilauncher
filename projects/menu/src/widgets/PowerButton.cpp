#include "PowerButton.hpp"
#include <nxui/core/Input.hpp>

namespace widgets {

PowerButton::PowerButton() {
    setSize(54.0f, 48.0f);
    setCornerRadius(4.0f);
    setLiquidGlassEnabled(false);
    setBlurEnabled(false);
    setFocusable(true);
    setTag("power_button");
    setAccessibilityRole("button");
    setAccessibilityLabel("Power");
    setAccessibilityHint("A to open the power menu");
    addAction(static_cast<uint64_t>(nxui::Button::A), [this]() {
        if (m_onActivateCb) m_onActivateCb();
    });
}

void PowerButton::onRender(nxui::Renderer& ren) {
    const float alpha = m_opacity * m_panelOpacity;
    if (alpha <= 0.01f) return;

    if (!m_iconLoaded) {
        m_iconLoaded = true;
        if (!m_iconTex.loadFromFile(ren.gpu(), ren,
                "sdmc:/switch/OmniLaunch/icons/metro/shutdown.png", 128)) {
            m_iconTex.loadFromFile(ren.gpu(), ren, "romfs:/icons/metro/shutdown.png", 128);
        }
    }

    // Same Metro tile chrome as the neighbouring Media Center / Plaza buttons.
    ren.drawRoundedRect(m_rect, nxui::Color(0.f, 0.47f, 0.84f, 0.90f * alpha), 4.f);

    if (m_iconTex.valid()) {
        constexpr float iconSize = 28.0f;
        const float cx = m_rect.x + m_rect.width * 0.5f;
        const float cy = m_rect.y + m_rect.height * 0.5f;
        ren.drawTexture(&m_iconTex, {cx - iconSize * 0.5f, cy - iconSize * 0.5f, iconSize, iconSize},
                        nxui::Color::white().withAlpha(alpha));
    }
}

bool PowerButton::handleTouch(const nxui::Input& input) {
    if (!isVisible() || m_opacity <= 0.05f) return false;
    if (input.touchDown() && m_rect.contains(input.touchX(), input.touchY()) && m_onActivateCb) {
        m_onActivateCb();
        return true;
    }
    return false;
}

} // namespace widgets
