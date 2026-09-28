#include "TitlePillWidget.hpp"
#include <nxui/core/Renderer.hpp>
#include <algorithm>

TitlePillWidget::TitlePillWidget() {
    setLiquidGlassShaderEnabled(false);
}

void TitlePillWidget::setText(const std::string& text, float screenWidth) {
    if (m_text == text && m_layoutInitialized)
        return;

    const bool wasVisible = isVisible();
    m_hideOnCollapse = false;
    setVisible(true);
    m_text = text;
    sizeToFit();
    setCornerRadius(m_rect.height * 0.5f);
    float targetW = m_rect.width;
    float targetX = (screenWidth - targetW) * 0.5f;

    if (!m_layoutInitialized) {
        m_layoutInitialized = true;
        m_animX.setImmediate(targetX);
        m_animW.setImmediate(targetW);
    } else {
        if (!wasVisible) {
            float seedW = std::min(targetW, 54.f);
            m_animW.setImmediate(seedW);
            m_animX.setImmediate((screenWidth - seedW) * 0.5f);
        }
        m_animX.set(targetX, 0.22f, nxui::Easing::outCubic);
        m_animW.set(targetW, 0.22f, nxui::Easing::outCubic);
        m_textReveal.setImmediate(0.32f);
        m_textReveal.set(1.f, 0.18f, nxui::Easing::outCubic);
    }

    m_rect.x = m_animX.value();
    m_rect.width = m_animW.value();
}

void TitlePillWidget::hideAnimated(float screenWidth) {
    if (!isVisible() && !m_hideOnCollapse)
        return;

    if (!m_layoutInitialized) {
        setVisible(false);
        return;
    }

    const float seedW = std::max(0.f, m_animW.value());
    m_animW.set(seedW, 0.01f, nxui::Easing::outCubic);
    m_animX.set((screenWidth - seedW) * 0.5f, 0.01f, nxui::Easing::outCubic);
    m_animW.set(0.f, 0.18f, nxui::Easing::outCubic);
    m_animX.set(screenWidth * 0.5f, 0.18f, nxui::Easing::outCubic);
    m_textReveal.set(0.f, 0.12f, nxui::Easing::outCubic);
    m_hideOnCollapse = true;
    setVisible(true);
}

void TitlePillWidget::setTargetY(float y, bool immediate) {
    if (immediate) {
        m_animY.setImmediate(y);
        m_rect.y = y;
    } else {
        m_animY.set(y, 0.22f, nxui::Easing::outCubic);
    }
}

void TitlePillWidget::onContentUpdate(float dt) {
    m_animY.update(dt);
    m_rect.y = m_animY.value();

    if (m_layoutInitialized) {
        m_rect.x = m_animX.value();
        m_rect.width = m_animW.value();
        if (m_hideOnCollapse && m_animW.value() <= 0.5f) {
            m_hideOnCollapse = false;
            m_text.clear();
            setVisible(false);
        }
    }
}

void TitlePillWidget::onContentRender(nxui::Renderer& ren) {
    if (!m_font || m_text.empty()) return;

    nxui::Rect cr = contentRect();
    nxui::Vec2 textSz = m_font->measure(m_text);
    float tx = cr.x + (cr.width  - textSz.x) * 0.5f;
    float ty = cr.y + (cr.height - textSz.y) * 0.5f;
    float reveal = std::clamp(m_textReveal.value(), 0.f, 1.f);
    ren.pushClipRect(cr);
    ren.drawText(m_text, {tx, ty + (1.f - reveal) * 3.f}, m_font,
                 m_textColor.withAlpha(m_opacity * reveal), 1.f);
    ren.popClipRect();
}

nxui::Vec2 TitlePillWidget::computeContentSize() const {
    if (!m_font || m_text.empty()) return {0.f, 0.f};
    return m_font->measure(m_text);
}
