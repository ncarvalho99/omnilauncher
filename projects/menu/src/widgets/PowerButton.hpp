#pragma once

#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/Types.hpp>
#include <nxui/core/Input.hpp>
#include <functional>

namespace widgets {

/// Metro top-bar button that opens the power (sleep / shutdown / reboot) dialog.
/// Sits beside the Media Center button; only shown in the Metro view.
class PowerButton : public nxui::GlassWidget {
public:
    PowerButton();
    ~PowerButton() override = default;

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

    void onRender(nxui::Renderer& ren) override;
    bool handleTouch(const nxui::Input& input);

private:
    bool m_focused = false;
    std::function<void()> m_onActivateCb;
    nxui::Texture m_iconTex;
    bool m_iconLoaded = false;
};

} // namespace widgets
