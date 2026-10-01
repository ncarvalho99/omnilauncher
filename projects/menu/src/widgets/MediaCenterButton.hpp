#pragma once

#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/Types.hpp>
#include <nxui/core/Input.hpp>
#include <functional>

namespace widgets {

/// Top header button widget for opening the SwitchU Multimedia Center.
/// Sits beside the WaraWara Plaza screen swap button.
class MediaCenterButton : public nxui::GlassWidget {
public:
    MediaCenterButton();
    ~MediaCenterButton() override = default;

    void setPlaying(bool playing);
    bool isPlaying() const { return m_playing; }

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
    bool m_playing = false;
    bool m_focused = false;
    float m_animTime = 0.0f;
    std::function<void()> m_onActivateCb;
    nxui::Texture m_iconTex;
    bool m_iconLoaded = false;
    bool m_metroStyle = false;
    nxui::Texture m_metroMusicTex;
    bool m_metroMusicLoaded = false;
};

} // namespace widgets
