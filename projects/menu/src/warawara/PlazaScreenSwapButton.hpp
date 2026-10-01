#pragma once

#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/Types.hpp>
#include <nxui/core/Input.hpp>
#include <functional>

namespace warawara {

/// Authentic Wii U TV/GamePad screen swap button widget for the top header.
/// Allows instant switching between Home Menu Grid and WaraWara Plaza view.
class PlazaScreenSwapButton : public nxui::GlassWidget {
public:
    PlazaScreenSwapButton();
    ~PlazaScreenSwapButton() override = default;

    void setPlazaActive(bool active);
    bool isPlazaActive() const { return m_plazaActive; }

    void setMetroStyle(bool metro) { m_metroStyle = metro; }
    bool isMetroStyle() const { return m_metroStyle; }

    void onActivate(std::function<void()> cb) {
        m_onActivateCb = cb;
        setOnActivate(cb);
        addAction(static_cast<uint64_t>(nxui::Button::A), cb);
    }
    bool activate() override {
        if (m_onActivateCb) {
            m_onActivateCb();
            return true;
        }
        return false;
    }

    void onFocusGained() override { m_focused = true; }
    void onFocusLost() override { m_focused = false; }
    bool isFocused() const { return m_focused; }

    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

    bool handleTouch(const nxui::Input& input);

private:
    bool m_plazaActive = false;
    bool m_focused = false;
    float m_pulseAnim = 0.0f;
    bool m_hovered = false;
    std::function<void()> m_onActivateCb;
    nxui::Texture m_uIconTex;
    bool m_uIconLoaded = false;
    bool m_metroStyle = false;
    nxui::Texture m_metroUIconTex;
    bool m_metroUIconLoaded = false;
};

} // namespace warawara
