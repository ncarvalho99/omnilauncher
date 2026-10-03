#include "WiiUMenuApp.hpp"
#include <fmt/format.h>
#include <zlib.h>
#include "widgets/GlossyIcon.hpp"
#include "DebugLog.hpp"
#include "NsService.hpp"
#include "core/PlayTime.hpp"
#include <switchu/title_footprint.hpp>
#include <switchu/sd_commit.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <nxui/core/I18n.hpp>

namespace {

std::string titleIdHex(std::uint64_t titleId) {
    return fmt::format("{:016X}", titleId);
}

std::string installedDisplayVersion(std::uint64_t titleId) {
#ifdef SWITCHU_MENU
    const Result initRc = switchu::menu::ensureNsService("game-details");
    if (R_FAILED(initRc)) {
        DebugLog::log("[details] ns unavailable title=%016llX rc=0x%08X",
                      static_cast<unsigned long long>(titleId), static_cast<unsigned int>(initRc));
        return {};
    }

    NsApplicationControlData control{};
    u64 actualSize = 0;
    const Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId,
                                                  &control, sizeof(control), &actualSize);
    if (R_SUCCEEDED(rc) && control.nacp.display_version[0] != '\0') {
        DebugLog::log("[details] version title=%016llX value=%s", (unsigned long long)titleId,
                      control.nacp.display_version);
        return control.nacp.display_version;
    }
    DebugLog::log("[details] version unavailable title=%016llX rc=0x%08X size=%llu",
                  (unsigned long long)titleId, (unsigned int)rc, (unsigned long long)actualSize);
#else
    (void)titleId;
#endif
    return {};
}

std::string installedModSummary(std::uint64_t titleId) {
    const std::filesystem::path base = std::filesystem::path("sdmc:/atmosphere/contents") / titleIdHex(titleId);
    std::error_code ec;
    int count = 0;
    for (const char* directory : {"romfs", "exefs", "cheats"}) {
        ec.clear();
        if (std::filesystem::is_directory(base / directory, ec) && !ec)
            ++count;
    }
    auto& i18n = nxui::I18n::instance();
    if (count == 0)
        return i18n.tr("dialog.details_mods_none", "None detected");
    DebugLog::log("[details] mods title=%016llX detected=%d", (unsigned long long)titleId, count);
    return i18n.tr("dialog.details_mods_detected", "Detected") + ": " + std::to_string(count);
}

// Asked of pdm directly rather than read from the sort cache: the dossier is
// one title, opened on purpose, and the cache is only kept fresh while the
// most-played view is in use.
std::string installedPlayTime(std::uint64_t titleId) {
    const auto nanoseconds = switchu::menu::playtime::query(titleId);
    if (nanoseconds && *nanoseconds > 0)
        return switchu::menu::playtime::format(*nanoseconds);
    return nxui::I18n::instance().tr("dialog.details_playtime_unavailable", "Not available yet");
}

} // namespace

#if 0 // Replaced by the folder/widget-aware 1.2 implementation.
bool WiiUMenuApp::isEditableIcon(nxui::Widget* w) const {
    if (!w || w->tag() != "glossy_icon")
        return false;
    auto* icon = static_cast<GlossyIcon*>(w);
    return icon->titleId() != 0;
}
#endif

#if 0 // Replaced by folder/widget-aware 1.2 accessibility descriptions.
std::string WiiUMenuApp::accessibilityContextFor(nxui::Widget* w) const {
    auto& i18n = nxui::I18n::instance();
    if (!w)
        return {};
    if (m_dialog && m_dialog->isActive() && w == m_dialog.get())
        return i18n.tr("accessibility.context.dialog", "Dialog");
    if (m_userSelect && m_userSelect->isActive() && w == m_userSelect.get())
        return i18n.tr("accessibility.context.profile_selection", "Profile selection");
    if (m_settings && m_settings->isActive() && w == m_settings.get())
        return i18n.tr("accessibility.context.settings", "Settings");
    if (m_themeShop && m_themeShop->isActive() && w == m_themeShop.get())
        return i18n.tr("accessibility.context.themes", "Themes");
    if (m_gameGallery && m_gameGallery->isActive() && w == m_gameGallery.get())
        return i18n.tr("dialog.gallery_title", "Gallery");
    if (m_gameMods && m_gameMods->isActive() && w == m_gameMods.get())
        return i18n.tr("dialog.mods_title", "Mods");
    if (m_gameCheats && m_gameCheats->isActive() && w == m_gameCheats.get())
        return i18n.tr("dialog.cheats_title", "Cheats");
    if (m_gameDetails && m_gameDetails->isActive() && w == m_gameDetails.get())
        return i18n.tr("dialog.details_title", "Game details");
    if (w->tag() == "glossy_icon" && m_grid)
        return i18n.tr("accessibility.context.main_menu", "Main menu")
             + ", " + i18n.tr("accessibility.context.page", "page") + " "
             + std::to_string(m_grid->currentPage() + 1)
             + " " + i18n.tr("accessibility.context.of", "of") + " "
             + std::to_string(m_grid->totalPages());
    for (const auto& btn : m_sidebar.leftButtons())
        if (btn.get() == w) return i18n.tr("accessibility.context.left_sidebar", "Left sidebar");
    for (const auto& btn : m_sidebar.rightButtons())
        if (btn.get() == w) return i18n.tr("accessibility.context.right_sidebar", "Right sidebar");
    for (const auto& avatar : m_userAvatarButtons)
        if (avatar.get() == w) return i18n.tr("accessibility.context.user_profiles", "User profiles");
    return {};
}

std::string WiiUMenuApp::accessibilityActionsFor(nxui::Widget* w) const {
    auto& i18n = nxui::I18n::instance();
    if (!w)
        return {};
    if (m_editMode && w->tag() == "glossy_icon")
        return i18n.tr("accessibility.actions.edit_mode", "Directional pad to choose the new position. Y to place. B to cancel.");
    if (w->tag() == "glossy_icon") {
        auto* icon = static_cast<GlossyIcon*>(w);
        if (icon->titleId() == 0)
            return i18n.tr("accessibility.actions.empty_slot", "Directional pad to navigate. ZL or ZR to change page.");
        return icon->isNotLaunchable()
            ? i18n.tr("accessibility.actions.game_blocked", "A to show the reason. Y to move. ZL or ZR to change page.")
            : i18n.tr("accessibility.actions.game_launchable", "A to launch. X for options. Y to move. ZL or ZR to change page.");
    }
    if (m_settings && w == m_settings.get())
        return i18n.tr("accessibility.actions.settings", "Up and down to choose a category. A or right to enter. B to close.");
    if (m_themeShop && w == m_themeShop.get())
        return i18n.tr("accessibility.actions.themes", "Up and down to navigate. A to choose. B to close.");
    if (m_gameGallery && w == m_gameGallery.get())
        return i18n.tr("accessibility.actions.themes", "Up and down to navigate. A to choose. B to close.");
    if (m_gameMods && w == m_gameMods.get())
        return i18n.tr("dialog.mods_hint", "A enable/disable. X remove. B back.");
    if (m_gameCheats && w == m_gameCheats.get())
        return i18n.tr("dialog.cheats_hint", "A toggle  -  X toggle all  -  B back");
    if (m_gameDetails && w == m_gameDetails.get())
        return i18n.tr("dialog.details_actions", "Left and right to select gameplay art. A to expand. B to return.");
    if ((m_dialog && w == m_dialog.get()) || (m_userSelect && w == m_userSelect.get()))
        return i18n.tr("accessibility.actions.dialog", "Left and right to change choice. A to confirm. B to cancel.");
    return {};
}

std::string WiiUMenuApp::accessibilityPositionFor(nxui::Widget* w) const {
    if (!w || !m_config.accessibilitySpeakPosition)
        return {};

    auto& i18n = nxui::I18n::instance();
    if (w->tag() == "glossy_icon" && m_grid) {
        const int global = m_grid->focusedGlobalIndex();
        if (global >= 0) {
            const int cols = std::max(1, m_grid->columns());
            const int rows = std::max(1, m_grid->rowsPerPage());
            const int local = global % std::max(1, m_grid->iconsPerPage());
            const int row = local / cols + 1;
            const int col = local % cols + 1;
            return i18n.tr("accessibility.position.row", "row") + " " + std::to_string(row)
                 + " " + i18n.tr("accessibility.context.of", "of") + " " + std::to_string(rows)
                 + ". " + i18n.tr("accessibility.position.column", "column") + " " + std::to_string(col)
                 + " " + i18n.tr("accessibility.context.of", "of") + " " + std::to_string(cols);
        }
    }

    auto describeLinear = [&](const auto& buttons) -> std::string {
        for (int i = 0; i < (int)buttons.size(); ++i) {
            if (buttons[(size_t)i].get() == w) {
                return std::to_string(i + 1) + " "
                     + i18n.tr("accessibility.context.of", "of") + " "
                     + std::to_string((int)buttons.size());
            }
        }
        return {};
    };

    if (auto text = describeLinear(m_sidebar.leftButtons()); !text.empty())
        return text;
    if (auto text = describeLinear(m_sidebar.rightButtons()); !text.empty())
        return text;

    for (int i = 0; i < (int)m_userAvatarButtons.size(); ++i) {
        if (m_userAvatarButtons[(size_t)i].get() == w) {
            return std::to_string(i + 1) + " "
                 + i18n.tr("accessibility.context.of", "of") + " "
                 + std::to_string((int)m_userAvatarButtons.size());
        }
    }

    return {};
}

void WiiUMenuApp::announceFocusedWidget(nxui::Widget* w) {
    if (!w)
        return;

    std::string hint = accessibilityActionsFor(w);
    std::string position = accessibilityPositionFor(w);

    std::string originalHint = w->accessibilityHint();
    if (m_accessibility.speakHints()) {
        if (!hint.empty())
            w->setAccessibilityHint(hint);
    } else {
        w->setAccessibilityHint({});
    }
    const bool forceRepeat = w->tag() == "glossy_icon";
    std::string summary = w->accessibilitySummary();
    if (summary.empty())
        summary = w->tag();
    m_accessibility.announceStructuredFocus(accessibilityContextFor(w),
                                            position,
                                            summary,
                                            forceRepeat);
    if ((m_accessibility.speakHints() && !hint.empty()) || !m_accessibility.speakHints())
        w->setAccessibilityHint(originalHint);
}
#endif

#if 0 // Replaced by the folder/widget-aware 1.2 editing implementation.
void WiiUMenuApp::startEditGhost(GlossyIcon* sourceIcon) {
    stopEditGhost();
    if (!sourceIcon)
        return;

    if (m_editSourceIndex >= 0)
        m_iconStreamer.setPinnedIndex(m_editSourceIndex);

    m_editSourceIcon = sourceIcon;
    m_editSourceIcon->setOpacity(0.10f);

    auto ghost = std::make_shared<GlossyIcon>();
    ghost->setTag("edit_ghost");
    ghost->setFocusable(false);
    ghost->setTitle(sourceIcon->title());
    ghost->setTitleId(sourceIcon->titleId());
    ghost->setTexture(sourceIcon->texture());
    ghost->setIsGameCard(sourceIcon->isGameCard());
    ghost->setGameCardTexture(sourceIcon->gameCardTexture());
    ghost->setNotLaunchable(sourceIcon->isNotLaunchable());
    ghost->setCornerRadius(sourceIcon->cornerRadius());
    ghost->setBlurEnabled(false);
    ghost->setPanelOpacity(0.84f);
    ghost->setOpacity(0.84f);
    ghost->setScale(1.06f);
    ghost->forceVisible();

    m_editGhostTargetRect = sourceIcon->focusRect().expanded(4.f);
    ghost->setRect(m_editGhostTargetRect);
    m_editGhostPulse = 0.f;

    m_editGhostIcon = ghost;
}

void WiiUMenuApp::stopEditGhost() {
    m_iconStreamer.clearPinnedIndex();

    if (m_editSourceIcon)
        m_editSourceIcon->setOpacity(1.f);
    m_editSourceIcon = nullptr;

    m_editGhostIcon.reset();
    m_editGhostPulse = 0.f;
}

void WiiUMenuApp::updateEditGhost(float dt) {
    if (!m_editMode || !m_editGhostIcon)
        return;

    if (m_cursor && m_cursor->isVisible()) {
        m_editGhostTargetRect = m_cursor->currentRect();
    } else if (auto* cur = focusManager().current()) {
        if (cur->tag() == "glossy_icon")
            m_editGhostTargetRect = cur->focusRect().expanded(4.f);
    }

    m_editGhostPulse += dt;
    float pulse = 0.80f + 0.08f * std::sin(m_editGhostPulse * 8.f);
    m_editGhostIcon->setOpacity(pulse);
    m_editGhostIcon->setPanelOpacity(std::min(1.f, pulse + 0.12f));
    m_editGhostIcon->setScale(1.07f + 0.025f * std::sin(m_editGhostPulse * 7.f));

    m_editGhostIcon->setRect(m_editGhostTargetRect);
}

void WiiUMenuApp::unbindEditActions() {
    if (!m_editBoundIcon)
        return;
    m_editBoundIcon->clearActions();
    m_editBoundIcon = nullptr;
}

void WiiUMenuApp::bindEditActions(GlossyIcon* icon) {
    if (!icon)
        return;
    if (m_editBoundIcon == icon)
        return;

    unbindEditActions();
    m_editBoundIcon = icon;

    icon->addAction(static_cast<uint64_t>(nxui::Button::A), []() {});
    icon->addAction(static_cast<uint64_t>(nxui::Button::B), [this]() {
        exitEditMode();
        m_audio.playSfx(Sfx::ModalHide);
    });
}

void WiiUMenuApp::enterEditMode() {
    auto* cur = focusManager().current();
    if (!isEditableIcon(cur))
        return;

    auto* icon = static_cast<GlossyIcon*>(cur);
    m_editMode = true;
    m_editSourceIndex = m_grid ? m_grid->focusedGlobalIndex() : -1;
    m_editHeldTitle = icon->title();
    startEditGhost(icon);
    bindEditActions(icon);
    m_titlePill->setText(nxui::I18n::instance().tr("game.move_prefix", "Move: ") + m_editHeldTitle);
    m_titlePill->setVisible(true);
    m_accessibility.announce(nxui::I18n::instance().tr(
        "accessibility.move_mode.enter",
        "Moving game: ") + m_editHeldTitle, true, true);
}

void WiiUMenuApp::exitEditMode() {
    if (!m_editMode)
        return;

    m_editMode = false;
    unbindEditActions();
    m_editSourceIndex = -1;
    m_editHeldTitle.clear();
    stopEditGhost();

    auto* cur = focusManager().current();
    if (isEditableIcon(cur)) {
        auto* icon = static_cast<GlossyIcon*>(cur);
        m_titlePill->setText(icon->title());
        m_titlePill->setVisible(true);
    } else {
        m_titlePill->hideAnimated();
    }

    if (m_layoutDirty)
        saveMenuLayout();
}

bool WiiUMenuApp::commitEditModePlacement() {
    if (!m_editMode || !m_grid)
        return false;

    int from = m_editSourceIndex;
    int target = m_grid->focusedGlobalIndex();
    if (from < 0 || target < 0 || from >= m_model.count() || target >= m_model.count())
        return false;
    if (m_model.at(from).titleId == 0)
        return false;

    int oldPage = m_grid->currentPage();
    bool changed = (from != target);
    if (changed) {
        if (!m_layoutSlots.empty() && from < (int)m_layoutSlots.size() && target < (int)m_layoutSlots.size())
            std::swap(m_layoutSlots[from], m_layoutSlots[target]);

        m_model.swapEntries(from, target);
        m_iconStreamer.swapIndices(from, target);
        m_grid->swapSlots(from, target);
        m_editSourceIndex = target;
        m_iconStreamer.setPinnedIndex(m_editSourceIndex);
        m_layoutDirty = true;
    }

    m_grid->focusGlobalIndex(target);

    int newPage = m_grid->currentPage();
    if (changed || newPage != oldPage) {
        m_iconStreamer.onPageChanged(newPage, m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
    }
    for (auto* icon : m_grid->pageIcons()) {
        if (icon)
            icon->forceVisible();
    }

    if (auto* cur = m_grid->focusManager().current())
        focusManager().setFocus(cur);

    if (m_editGhostIcon) {
        if (auto* focused = m_grid->focusManager().current())
            m_editGhostTargetRect = focused->focusRect().expanded(4.f);
    }

    updateCursor();
    return true;
}

bool WiiUMenuApp::moveFocusedIcon(nxui::FocusDirection dir) {
    if (!m_editMode || !m_grid)
        return false;

    int from = m_grid->focusedGlobalIndex();
    if (from < 0 || from >= m_model.count())
        return false;
    if (m_model.at(from).titleId == 0)
        return false;

    int cols = std::max(1, m_grid->columns());
    int rows = std::max(1, m_grid->rowsPerPage());
    int perPage = std::max(1, m_grid->iconsPerPage());
    int totalPages = std::max(1, m_grid->totalPages());

    int page = from / perPage;
    int local = from % perPage;
    int col = local % cols;
    int row = local / cols;

    int target = from;
    switch (dir) {
        case nxui::FocusDirection::LEFT:
            if (col > 0)
                target = from - 1;
            else if (page > 0)
                target = (page - 1) * perPage + row * cols + (cols - 1);
            else
                return false;
            break;
        case nxui::FocusDirection::RIGHT:
            if (col < cols - 1)
                target = from + 1;
            else if (page + 1 < totalPages)
                target = (page + 1) * perPage + row * cols;
            else
                return false;
            break;
        case nxui::FocusDirection::UP:
            if (row > 0)
                target = from - cols;
            else
                return false;
            break;
        case nxui::FocusDirection::DOWN:
            if (row + 1 < rows)
                target = from + cols;
            else
                return false;
            break;
    }

    if (target < 0 || target >= m_model.count())
        return false;
    if (target == from)
        return true;

    if (!m_layoutSlots.empty() && from < (int)m_layoutSlots.size() && target < (int)m_layoutSlots.size())
        std::swap(m_layoutSlots[from], m_layoutSlots[target]);

    m_model.swapEntries(from, target);
    m_iconStreamer.swapIndices(from, target);
    m_grid->swapSlots(from, target);
    m_editSourceIndex = target;
    m_iconStreamer.setPinnedIndex(m_editSourceIndex);
    m_grid->focusGlobalIndex(target);

    int newPage = m_grid->currentPage();
    m_iconStreamer.onPageChanged(newPage, m_grid->iconsPerPage(),
                                 app().gpu(), app().renderer(),
                                 m_grid->allIcons());
    for (auto* icon : m_grid->pageIcons()) {
        if (icon)
            icon->forceVisible();
    }

    if (auto* cur = m_grid->focusManager().current())
        focusManager().setFocus(cur);

    auto* cur = focusManager().current();
    if (isEditableIcon(cur)) {
        auto* icon = static_cast<GlossyIcon*>(cur);
        bindEditActions(icon);
        m_titlePill->setText(nxui::I18n::instance().tr("game.move_prefix", "Move: ") + icon->title());
    }

    m_layoutDirty = true;
    updateCursor();
    return true;
}

void WiiUMenuApp::wireFocusCallback() {
    focusManager().onFocusChanged([this](nxui::Widget*, nxui::Widget* cur) {
        updateCursor();
        announceFocusedWidget(cur);

        if ((m_dialog && m_dialog->isActive()) ||
            (m_themeShop && m_themeShop->isActive()) ||
            (m_gameGallery && m_gameGallery->isActive()) ||
            (m_gameMods && m_gameMods->isActive()) ||
            (m_gameCheats && m_gameCheats->isActive()) ||
            (m_gameDetails && m_gameDetails->isActive()) ||
            (m_settings && m_settings->isActive()) ||
            (m_userSelect && m_userSelect->isActive()))
            return;

        bool suppressSfx = m_suppressNextNavigateSfx;
        m_suppressNextNavigateSfx = false;
        if (!suppressSfx)
            m_audio.playSfx(Sfx::Navigate);

        if (cur && cur->tag() == "glossy_icon") {
            if (m_grid && m_grid->isXmb()) {
                m_grid->syncXmbFocusFromCurrent();
            }
            m_grid->focusManager().setFocus(cur);
            auto* icon = static_cast<GlossyIcon*>(cur);
            auto& i18n = nxui::I18n::instance();
            if (m_editMode) {
                bindEditActions(icon);
                m_editGhostTargetRect = icon->focusRect();
                if (!m_editHeldTitle.empty())
                    m_titlePill->setText(i18n.tr("game.move_prefix", "Move: ") + m_editHeldTitle);
                else if (icon->titleId() != 0)
                    m_titlePill->setText(i18n.tr("game.move_prefix", "Move: ") + icon->title());
                else
                    m_titlePill->setText(i18n.tr("game.move", "Move"));
                m_titlePill->setVisible(true);
                return;
            }
            if (icon->title().empty() || icon->entryKind() == GridEntryKind::Empty) {
                refreshGameArtworkBackdrop(0);
                m_titlePill->hideAnimated();
                return;
            }
            refreshGameArtworkBackdrop(icon->titleId());
#ifdef SWITCHU_MENU
            if (m_launcher.isAppSuspended(icon->titleId())) {
                m_titlePill->setText(icon->title());
            } else
#endif
            m_titlePill->setText(icon->title());
            const bool showTitle = (m_openFolderId != 0) || (m_appLayoutMode != AppLayoutMode::Xmb && m_appLayoutMode != AppLayoutMode::List && m_appLayoutMode != AppLayoutMode::Metro);
            m_titlePill->setVisible(showTitle);
        } else if (cur) {
            if (m_grid)
                m_grid->focusManager().setFocus(nullptr);
            refreshGameArtworkBackdrop(0);
            if (m_editMode)
                exitEditMode();
            for (auto& btn : m_sidebar.leftButtons()) {
                if (btn.get() == cur) { m_titlePill->setText(btn->label()); m_titlePill->setVisible(true); return; }
            }
            for (auto& btn : m_sidebar.rightButtons()) {
                if (btn.get() == cur) { m_titlePill->setText(btn->label()); m_titlePill->setVisible(true); return; }
            }
            for (auto& avatar : m_userAvatarButtons) {
                if (avatar.get() == cur) {
                    m_titlePill->setText(avatar->nickname());
                    m_titlePill->setVisible(!avatar->nickname().empty());
                    return;
                }
            }
            if (m_screenSwapButton && m_screenSwapButton.get() == cur) {
                m_titlePill->setText(m_screenSwapButton->isPlazaActive() ? "Home Menu" : "WaraWara Plaza");
                m_titlePill->setVisible(true);
                return;
            }
            m_titlePill->hideAnimated();
        } else {
            refreshGameArtworkBackdrop(0);
            m_titlePill->hideAnimated();
        }
    });
    updateCursor();
    if (auto* cur = focusManager().current()) {
        if (cur->tag() == "glossy_icon") {
            auto* icon = static_cast<GlossyIcon*>(cur);
            if (icon->titleId() != 0)
                m_titlePill->setText(icon->title());
            refreshGameArtworkBackdrop(icon->titleId());
        }
    }
}
#endif

void WiiUMenuApp::refreshGameArtworkBackdrop(std::uint64_t titleId) {
    if (!m_gameArtworkBackdrop)
        return;
    const std::string path = gallery::GameArtworkStore::pathFor(
        titleId, gallery::ArtworkKind::Background);
    if (path == m_gameArtworkBackdrop->artworkPath())
        return;
    if (path.empty()) {
        m_gameArtworkBackdrop->clearArtwork(&app().gpu());
        return;
    }
    // Only asks. Reading the cover off the card and decoding it happens on a
    // worker, and the image arrives a few frames later through
    // pollPendingArtwork. Doing it here is what made every step onto a game
    // with custom art stall for about half a second.
    m_gameArtworkBackdrop->requestArtwork(m_threadPool, path);
    DebugLog::log("[gallery] background requested: %s", path.c_str());
}

void WiiUMenuApp::raiseOverlay(const std::shared_ptr<nxui::Widget>& overlay) {
    if (!m_overlayLayer || !overlay)
        return;
    m_overlayLayer->removeChild(overlay.get());
    m_overlayLayer->addChild(overlay);
}

bool WiiUMenuApp::isCurrentFocusableWidget(nxui::Widget* w) const {
    if (!w) return false;
    if (m_plazaScreen && m_plazaScreen.get() == w) return false;
    // m_dialog stayed focused-but-unchecked here: the homebrew "Mark as game
    // port" button has closeOnPress=false, so the dialog is still the current
    // focus when its handler opens the text-entry confirm step. Cancelling
    // that keyboard called restoreFocus(), which never recognised m_dialog as
    // a valid target, fell through to a random grid icon, and left the dialog
    // open with no focus owner -- the source of the platform-picker freeze.
    if (m_dialog && m_dialog.get() == w) return w->isFocusable();
    if (m_contextMenu && m_contextMenu.get() == w) return w->isFocusable();
    if (m_gameOptions && m_gameOptions.get() == w) return w->isFocusable();
    if (m_folderOptions && m_folderOptions.get() == w) return w->isFocusable();
    if (m_controllerTest && m_controllerTest.get() == w) return w->isFocusable();
    // The folder header is a control while a folder is open, and renaming from
    // it returns focus here. Without this the restore failed silently, focus
    // stayed on the hidden keyboard, and the selection ring was left framing the
    // panel that had just closed.
    if (m_folderHeader && m_folderHeader.get() == w) return w->isFocusable();
    if (m_textEntry && m_textEntry.get() == w) return w->isFocusable();
    if (m_mediaCenterScreen && m_mediaCenterScreen.get() == w) return w->isFocusable();
    if (m_steamGridDbPicker && m_steamGridDbPicker.get() == w) return w->isFocusable();
    if (m_platformPicker && m_platformPicker.get() == w) return w->isFocusable();
    if (m_gameGallery && m_gameGallery.get() == w) return w->isFocusable();
    if (m_gameMods && m_gameMods.get() == w) return w->isFocusable();
    if (m_gameCheats && m_gameCheats.get() == w) return w->isFocusable();
    if (m_gameDetails && m_gameDetails.get() == w) return w->isFocusable();
    if (m_themeShop && m_themeShop.get() == w) return w->isFocusable();
    if (m_settings && m_settings.get() == w) return w->isFocusable();
    if (m_quickSettings && m_quickSettings.get() == w) return w->isFocusable();
    for (const auto& btn : m_sidebar.leftButtons())
        if (btn.get() == w) return w->isFocusable();
    for (const auto& btn : m_sidebar.rightButtons())
        if (btn.get() == w) return w->isFocusable();
    for (const auto& avatar : m_userAvatarButtons)
        if (avatar.get() == w) return w->isFocusable();
    if (m_grid)
        for (const auto& icon : m_grid->allIcons())
            if (icon.get() == w) return w->isFocusable();
    return false;
}

int WiiUMenuApp::findTitleIndex(uint64_t titleId) const {
    if (titleId == 0)
        return -1;
    for (int i = 0; i < m_model.count(); ++i) {
        if (m_model.at(i).titleId == titleId)
            return i;
    }
    return -1;
}

#if 0 // Replaced by the folder-aware 1.2 implementation.
bool WiiUMenuApp::focusTitle(uint64_t titleId) {
    if (!m_grid)
        return false;

    int idx = findTitleIndex(titleId);
    if (idx < 0)
        return false;

    int oldPage = m_grid->currentPage();
    if (!m_grid->focusGlobalIndex(idx))
        return false;

    if (m_grid->currentPage() != oldPage || titleId != 0) {
        m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
    }

    if (auto* cur = m_grid->focusManager().current())
        focusManager().setFocus(cur);
    updateCursor();
    return true;
}

// Leva o seletor de volta para a grade, seja qual for o jogo.
//
// HOME traz o menu para a frente e o seletor tem de vir junto. Quando nÃ£o hÃ¡
// jogo suspenso, focusTitle nÃ£o tem o que procurar e devolve false, e atÃ© aqui
// isso deixava o cursor parado no botÃ£o da barra lateral que abriu a tela --
// com o nome do jogo escrito embaixo, apontando para outra coisa. Era esse
// desencontro que aparecia no relato.
#endif

bool WiiUMenuApp::focusGridSelection() {
    if (!m_grid)
        return false;

    nxui::Widget* target = m_grid->focusManager().current();
    if (!target) {
        // Sem nada lembrado, o primeiro Ã­cone da pÃ¡gina aberta serve.
        const int first = m_grid->currentPage() * m_grid->iconsPerPage();
        if (!m_grid->focusGlobalIndex(first))
            return false;
        target = m_grid->focusManager().current();
    }
    if (!target)
        return false;

    m_grid->focusManager().setFocus(target);
    focusManager().setFocus(target);
    updateCursor();
    return true;
}

void WiiUMenuApp::markSuspendedIcon(uint64_t titleId) {
    if (!m_grid)
        return;
    for (auto& icon : m_grid->allIcons())
        icon->setSuspended(titleId != 0 && icon->titleId() == titleId);
    if (titleId != 0)
        focusTitle(titleId);

    if (auto* cur = m_grid->focusManager().current()) {
        auto* icon = static_cast<GlossyIcon*>(cur);
        if (m_launcher.isAppSuspended(icon->titleId())) {
            m_titlePill->setText(icon->title());
        } else {
            m_titlePill->setText(icon->title());
        }
    }
}

void WiiUMenuApp::closeActiveOverlays(bool preserveDialog) {
    m_navigator.resetToHome();
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
    if (!preserveDialog && m_preflightEdgePending) {
        DebugLog::log("[diagnostic-preflight-edge] pending dialog cancelled");
        m_preflightEdgePending = false;
        m_preflightEdgeLockObserved = false;
    }
#endif
    if (m_editMode)
        exitEditMode();
    if (m_contextMenu && m_contextMenu->isActive())
        m_contextMenu->hide();
    if (m_userSelect && m_userSelect->isActive())
        m_userSelect->hide();
    if (m_quickSettings && m_quickSettings->isActive())
        m_quickSettings->hide();
    if (!preserveDialog && m_dialog && m_dialog->isActive())
        m_dialog->hide();
    if (m_settings && m_settings->isActive())
        m_settings->hide();
    if (m_themeShop && m_themeShop->isActive())
        m_themeShop->hide();
    if (m_gameGallery && m_gameGallery->isActive())
        m_gameGallery->hide();
    if (m_gameMods && m_gameMods->isActive())
        m_gameMods->hide();
    if (m_gameCheats && m_gameCheats->isActive())
        m_gameCheats->hide();
    if (m_gameDetails && m_gameDetails->isActive())
        m_gameDetails->hide();
    if (m_activityLog && m_activityLog->isActive())
        m_activityLog->hide();
    if (m_plazaScreen && m_plazaScreen->isActive())
        closeWaraWaraPlaza();
    if (m_steamGridDbPicker && m_steamGridDbPicker->isActive())
        m_steamGridDbPicker->hide();
    if (m_mediaCenterScreen && m_mediaCenterScreen->isActive())
        m_mediaCenterScreen->hide();
    if (m_platformPicker && m_platformPicker->isActive())
        m_platformPicker->hide();
    if (m_gameOptions && m_gameOptions->isActive())
        m_gameOptions->hide();
    if (m_folderOptions && m_folderOptions->isActive())
        m_folderOptions->hide();
    if (m_textEntry && m_textEntry->isActive())
        m_textEntry->hide(false);
    if (m_controllerTest && m_controllerTest->isActive())
        m_controllerTest->hide();
    if (m_openFolderId != 0)
        closeFolder();
}

nxui::Widget* WiiUMenuApp::focusRoot() {
    // Nothing behind the lock screen can be navigated or activated. Returning
    // nullptr blocks the frame's whole input dispatch, which is exactly the
    // guarantee this screen has to make; it reads its own presses from the pad.
    if (m_lockScreen.isLocked()) return nullptr;
    if (m_launchAnim && m_launchAnim->isPlaying()) return nullptr;
    if (m_folderCaptureRequested) return nullptr;
    if (m_progressDialog && m_progressDialog->isActive()) return m_progressDialog.get();
    // The on-screen keyboard is a modal overlay that can be opened from dialogs or
    // context menus. It must take precedence over context menus and dialogs so it
    // receives all button and direction inputs while active.
    if (m_textEntry && m_textEntry->isActive()) return m_textEntry.get();
    if (m_contextMenu && m_contextMenu->isActive()) return m_contextMenu.get();
    if (m_platformPicker && m_platformPicker->isActive()) return m_platformPicker.get();
    if (m_dialog && m_dialog->isActive()) return m_dialog.get();
    if (m_steamGridDbPicker && m_steamGridDbPicker->isActive()) return m_steamGridDbPicker.get();
    if (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) return m_mediaCenterScreen.get();
    if (m_controllerTest && m_controllerTest->isActive()) return m_controllerTest.get();
    if (m_folderOptions && m_folderOptions->isActive()) return m_folderOptions.get();
    if (m_gameOptions && m_gameOptions->isActive()) return m_gameOptions.get();
    if (m_gameCheats && m_gameCheats->isActive()) return m_gameCheats.get();
    if (m_gameMods && m_gameMods->isActive()) return m_gameMods.get();
    if (m_gameGallery && m_gameGallery->isActive()) return m_gameGallery.get();
    if (m_gameDetails && m_gameDetails->isActive()) return m_gameDetails.get();
    if (m_activityLog && m_activityLog->isActive()) return m_activityLog.get();
    if (m_plazaScreen && m_plazaScreen->isActive()) return m_plazaScreen.get();
    if (m_themeShop && m_themeShop->isActive()) return m_themeShop.get();
    if (m_settings && m_settings->isActive()) return m_settings.get();
    if (m_quickSettings && m_quickSettings->isActive()) return m_quickSettings.get();
    if (m_userSelect && m_userSelect->isActive()) return m_userSelect.get();
    return &rootBox();
}

void WiiUMenuApp::toggleAccessibilitySpeech() {
    auto& i18n = nxui::I18n::instance();
    const bool enabled = !m_config.accessibilityEnabled;
    m_config.accessibilityEnabled = enabled;
    if (m_settings)
        m_settings->setAccessibilityEnabledState(enabled);
    if (m_themeShop)
        m_themeShop->setAccessibilityVoiceEnabled(enabled);
    if (m_gameGallery)
        m_gameGallery->setAccessibilityVoiceEnabled(enabled);
    if (m_gameMods)
        m_gameMods->setAccessibilityVoiceEnabled(enabled);
    if (m_gameCheats)
        m_gameCheats->setAccessibilityVoiceEnabled(enabled);
    if (m_gameDetails)
        m_gameDetails->setAccessibilityVoiceEnabled(enabled);
    if (m_activityLog)
        m_activityLog->setAccessibilityVoiceEnabled(enabled);
    if (m_gameOptions)
        m_gameOptions->setAccessibilityVoiceEnabled(enabled);
    if (m_folderOptions)
        m_folderOptions->setAccessibilityVoiceEnabled(enabled);

    if (enabled) {
        m_audio.playSfx(Sfx::ThemeToggle);
        m_accessibility.setEnabled(true);
        m_accessibility.announce(i18n.tr("accessibility.speech.enabled",
                                         "Voice guidance enabled."), true, true);
    } else {
        m_accessibility.announceAndDisable(i18n.tr("accessibility.speech.disabled",
                                                   "Voice guidance disabled."));
    }
    m_config.save();
}

void WiiUMenuApp::handleSortShortcutRelease(float dt) {
    auto& input = app().input();
    if (input.isDown(nxui::Button::R)) {
        m_sortShortcutArmed = true;
        m_sortShortcutHeld = 0.f;
    } else if (m_sortShortcutArmed && input.isHeld(nxui::Button::R)) {
        m_sortShortcutHeld += dt;
    }
    if (!input.isUp(nxui::Button::R))
        return;
    const bool armed = m_sortShortcutArmed;
    const float held = m_sortShortcutHeld;
    m_sortShortcutArmed = false;
    m_sortShortcutHeld = 0.f;
    if (!armed)
        return;
    // Only a tap sorts. Holding R is how the homebrew override reaches Sphaira,
    // and a player doing that is not asking for the grid to be reordered when
    // they let go -- which is what made the shortcut feel like it fired twice.
    if (held > kSortShortcutTapSeconds)
        return;
    // Anything with its own R meaning, or any overlay covering the grid, keeps
    // the shortcut out of the way: the sort belongs to the grid alone.
    //
    // focusRoot() is the general test and comes first: it is whatever owns input
    // right now, so an overlay added later cannot forget to appear in the list
    // below. The on-screen keyboard did exactly that, and its L/R page switch
    // reached this shortcut and re-sorted the home grid behind it.
    // A folder is not an overlay -- it is the same grid showing other contents --
    // so focusRoot() is still the root box with one open and the guard below let
    // R through. It sorted the home grid behind the folder, and worse: with a
    // folder open reflowHomeGrid() takes its generic path, which rebuilds the
    // saved slots from whatever m_model holds. That model is the folder's
    // contents, so every home entry missing from the folder -- every widget
    // included -- was cleared out of m_layoutSlots and written to disk. The
    // widgets then came back as 1x1 wherever the filler could place them.
    // The dynamic line renders slot order as a ring, so a sort projected onto
    // the same slots rearranged the row around the cursor and left the icons it
    // moved reloading. Order in that view is the arrangement the owner built,
    // and R has nothing to offer it.
    if (isCarouselLayout())
        return;
    if (m_navigator.route() == switchu::navigation::Route::Home &&
        focusRoot() == &rootBox() && m_grid && m_grid->isXmb()) {
        m_grid->stepXmb(+1, 0);
        return;
    }
    if (m_navigator.route() == switchu::navigation::Route::Home &&
        focusRoot() == &rootBox() && isListLayout()) {
        jumpList(+1);
        return;
    }
    // Taking a title out of a folder briefly lived here, and moved to X so that
    // one button does both halves of the same job. R stays inert inside a
    // folder, for the reason above.
    if (m_openFolderId != 0 ||
        focusRoot() != &rootBox() ||
        (m_dialog && m_dialog->isActive()) ||
        (m_contextMenu && m_contextMenu->isActive()) ||
        (m_themeShop && m_themeShop->isActive()) ||
        (m_gameGallery && m_gameGallery->isActive()) ||
        (m_gameMods && m_gameMods->isActive()) ||
        (m_gameCheats && m_gameCheats->isActive()) ||
        (m_gameDetails && m_gameDetails->isActive()) ||
        (m_steamGridDbPicker && m_steamGridDbPicker->isActive()) ||
        (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) ||
        (m_gameOptions && m_gameOptions->isActive()) ||
        (m_folderOptions && m_folderOptions->isActive()) ||
        (m_controllerTest && m_controllerTest->isActive()) ||
        (m_settings && m_settings->isActive()) ||
        (m_userSelect && m_userSelect->isActive()) ||
        m_editMode) {
        return;
    }
    cycleSortMode();
}

void WiiUMenuApp::handleZlShortcutRelease(float dt) {
    if (!m_zlQuickFlipArmed)
        return;
    auto& input = app().input();
    if (input.isHeld(nxui::Button::ZL)) {
        m_zlQuickFlipHeld += dt;
    }
    if (input.isUp(nxui::Button::ZL)) {
        const bool armed = m_zlQuickFlipArmed;
        const float held = m_zlQuickFlipHeld;
        const bool triggered = m_deletePageTriggered;
        m_zlQuickFlipArmed = false;
        m_zlQuickFlipHeld = 0.f;
        if (armed && !triggered && held < 0.35f) {
            if (m_navigator.route() == switchu::navigation::Route::Home &&
                focusRoot() == &rootBox()) {
                flipPage(-1);
            }
        }
    }
}

bool WiiUMenuApp::handleAccessibilityToggleCombo() {
    if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
        return false;
    auto& input = app().input();
    if (!input.isDown(nxui::Button::Plus) || !input.isDown(nxui::Button::Minus))
        return false;
    if (m_accessibilityToggleComboHeld)
        return true;
    m_accessibilityToggleComboHeld = true;
    m_plusExitPending = false;
    m_plusExitPendingTimer = 0.f;
    toggleAccessibilitySpeech();
    return true;
}

bool WiiUMenuApp::handleFrameDumpShortcut() {
    if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
        return false;
    auto& input = app().input();
    const bool combo = input.isHeld(nxui::Button::ZL) &&
                       input.isHeld(nxui::Button::ZR) &&
                       input.isHeld(nxui::Button::RStick);
    if (!combo) {
        m_frameDumpShortcutHeld = false;
        return false;
    }
    if (m_frameDumpShortcutHeld)
        return true;
    m_frameDumpShortcutHeld = true;

    // Toggle: if recording is already active, stop early
    if (m_frameDumpActive) {
        m_frameDumpActive = false;
        m_frameDumpRemaining = 0;
        app().renderer().setDrawJournalEnabled(false);
        m_audio.playSfx(Sfx::ModalHide);
        DebugLog::log("[frame-dump] stopped early by user (captured %d frames)", m_frameDumpIndex);
        return true;
    }

    // Start 10-second diagnostic recording (200 frames @ 20 FPS, 1 frame every 3 vsyncs)
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::tm tmNow{};
    localtime_r(&now, &tmNow);
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmNow);

    m_frameDumpBatchDir = fmt::format("sdmc:/config/OmniLaunch/frame_dumps/{}", ts);
    std::error_code ec;
    std::filesystem::create_directories(m_frameDumpBatchDir, ec);

    // Drop helper Python script into the directory for 1-click PC extraction and
    // automatic artifact analysis. Finding the glitched frames by eye is what
    // made previous rounds slow and subjective: the attenuation is an exact
    // constant multiply, so it can be detected numerically and matched against
    // the frame's own recorded command list without anyone judging a thumbnail.
    std::ofstream py(m_frameDumpBatchDir + "/extract_frames.py");
    if (py) {
        py << R"PY(import os, glob, struct, zlib

try:
    from PIL import Image
except ImportError:
    Image = None
try:
    import numpy as np
except ImportError:
    np = None

zraws = sorted(glob.glob('frame_*.zraw'))
print(f'Extracting {len(zraws)} frames...')

frames = []
for zf in zraws:
    with open(zf, 'rb') as f:
        data = f.read()
    if len(data) < 16 or data[:4] != b'ZRAW':
        continue
    magic, w, h, raw_sz = struct.unpack('<4sIII', data[:16])
    raw = zlib.decompress(data[16:])
    base = os.path.splitext(zf)[0]
    if Image:
        Image.frombytes('RGBA', (w, h), raw).save(f'{base}.png')
    if np is not None:
        a = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4)[:, :, :3]
        frames.append((base, a.astype(np.float32)))
print('Done extracting PNGs!')


def half_ratio(cur, ref, x0, x1):
    """Median ratio and coherent-pixel fraction, over bright reference pixels.

    The Plaza is animated, so ordinary ratio standard deviation is dominated
    by moving Miis even when >80% of the framebuffer follows one exact scalar.
    Coherence measures the fraction within +/-0.035 of the median instead. The
    thresholds below were replayed against both existing native captures: all
    seven known glitches were found, with zero extra frames.
    """
    c = cur[:, x0:x1].ravel()
    r = ref[:, x0:x1].ravel()
    m = r > 24.0
    if m.sum() < 1000:
        return None, None
    q = c[m] / r[m]
    med = float(np.median(q))
    coherent = float(np.mean(np.abs(q - med) < 0.035))
    return med, coherent


def scan_band(cur, prev, nxt):
    """Locate a contiguous run of attenuated scanlines, if one exists.

    Measured captures show the artifact is not always fullscreen: it covers a
    run of rows starting at row 0 and ending at a cut row that differs per
    occurrence (720, 266 and 25 rows were observed in one capture). A
    whole-frame or half-frame median therefore reports 1.0000 and hides it.
    Comparing row by row against a pixel-stable reference exposes the band and
    its boundary, which is the measurement that distinguishes a scanout-side
    tear from a submitted darkening draw.
    """
    static = (np.all(np.abs(prev - nxt) < 2, axis=2) & (prev.mean(2) > 40))
    ratios = np.full(cur.shape[0], np.nan)
    for y in range(cur.shape[0]):
        m = static[y]
        if m.sum() < 60:
            continue
        ratios[y] = np.median(cur[y][m].ravel() /
                              np.maximum(prev[y][m].ravel(), 1e-6))
    hit = np.nonzero((ratios < 0.95) & ~np.isnan(ratios))[0]
    if hit.size < 3:
        return None
    return int(hit.min()), int(hit.max()), float(np.median(ratios[hit]))


if np is not None and len(frames) >= 3:
    h, w = frames[0][1].shape[:2]
    mid = w // 2
    print('\n=== attenuation analysis ===')
    print('a half is flagged when BOTH neighbours give ratio <0.95, agree')
    print('within 0.02, and >=72% of bright channels follow that scalar.\n')
    flagged = []
    for i in range(1, len(frames) - 1):
        base, cur = frames[i]
        prev, nxt = frames[i - 1][1], frames[i + 1][1]
        row = []
        for name, x0, x1 in (('left', 0, mid), ('right', mid, w)):
            rp, cp = half_ratio(cur, prev, x0, x1)
            rn, cn = half_ratio(cur, nxt, x0, x1)
            if rp is None or rn is None:
                row.append((name, None, None))
                continue
            # Both comparisons must agree, otherwise this is a scene change.
            if (rp < 0.95 and rn < 0.95 and abs(rp - rn) < 0.02 and
                    cp >= 0.72 and cn >= 0.72):
                row.append((name, (rp + rn) / 2.0, min(cp, cn)))
            else:
                row.append((name, None, None))
        band = scan_band(cur, prev, nxt)
        if any(r[1] is not None for r in row) or band:
            flagged.append((base, row, band))

    if not flagged:
        print('no attenuated frames in this capture.')
    for base, row, band in flagged:
        parts = []
        for name, ratio, coherent in row:
            parts.append(f'{name}=clean' if ratio is None
                         else f'{name}=x{ratio:.4f}(coherent {coherent:.1%})')
        print(f'{base}: ' + '  '.join(parts))
        if band:
            first, last, ratio = band
            span = last - first + 1
            kind = 'WHOLE FRAME' if span >= 719 else f'BAND rows {first}..{last}'
            print(f'    {kind}  x{ratio:.4f}  ({span}/720 rows)')
        jp = f'{base}.journal.txt'
        if os.path.exists(jp):
            with open(jp, 'r', errors='replace') as jf:
                lines = jf.read().splitlines()
            head = [l for l in lines[:3]]
            for hl in head:
                print('    ' + hl)
            if lines and 'dropped=0' not in lines[0]:
                print('    INCONCLUSIVE: journal truncated; zero verdict is not evidence')
            # Any backbuffer draw with a partial alpha. The journal's verdict
            # applies the stricter geometry + all-vertices-dark test; printing
            # these lines preserves the raw candidates for manual inspection.
            for l in lines:
                if 'DIMMER' in l:
                    print('    DIMMER ' + l.strip())
            for l in lines:
                if 'PRIMITIVE' in l and 'attenRange=' in l and ' tgt=-1 ' in l:
                    try:
                        hi = float(l.split('attenRange=')[1].split()[0].split('..')[1])
                    except (IndexError, ValueError):
                        continue
                    if hi < 0.99:
                        print('    CANDIDATE ' + l.strip())
        else:
            print('    (no journal for this frame)')
    print('\nverdict dimmer_quads>0 names the exact quad that dims the frame.')
    print('A 0 verdict only counts when unreadable_batches and non_quad_batches')
    print('are also 0; otherwise some geometry could not be tested at all.')
elif np is None:
    print('install numpy for automatic attenuation analysis')

if os.system('ffmpeg -version') == 0:
    os.system('ffmpeg -y -framerate 20 -i frame_%03d.png -c:v libx264 -pix_fmt yuv420p clip.mp4')
    print('Created clip.mp4 successfully!')
)PY";
        py.flush();
    }

    m_frameDumpActive = true;
    m_frameDumpRemaining = 200; // 200 frames @ 20 FPS = 10.0 seconds
    m_frameDumpIndex = 0;
    m_frameDumpCounter = 0;
    // Record every frame, not just the captured ones: a frame is only known to
    // be glitched after its pixels are examined on PC, and by then the chance
    // to have journalled it is gone.
    app().renderer().setDrawJournalEnabled(true);
    app().gpu().requestFrameDump();
    m_audio.playSfx(Sfx::Activate);
    DebugLog::log("[frame-dump] started 10-second recording (200 frames @ 20 FPS) into %s (mkdir: %s)",
                  m_frameDumpBatchDir.c_str(), ec ? ec.message().c_str() : "ok");
    return true;
}

void WiiUMenuApp::syncFrameDumpCapture() {
    if (!m_frameDumpActive)
        return;

    std::vector<std::uint8_t> pixels;
    if (app().gpu().takeFrameDump(pixels)) {
        const int curIndex = m_frameDumpIndex++;
        const std::string outPath = fmt::format("{}/frame_{:03d}.zraw",
                                                m_frameDumpBatchDir, curIndex);

        // The journal still describes the frame whose pixels were just taken:
        // the dump's copy was recorded in that frame's endFrame, and the next
        // beginFrame (which would reset the journal) has not run yet. Writing
        // it here therefore pairs frame_NNN.zraw with frame_NNN.journal.txt by
        // construction rather than by timing luck.
        auto journal = app().renderer().formatDrawJournal();
        const std::string journalPath = fmt::format("{}/frame_{:03d}.journal.txt",
                                                    m_frameDumpBatchDir, curIndex);
        m_threadPool.submit([journalPath, journal = std::move(journal)]() {
            std::ofstream out(journalPath, std::ios::binary);
            if (out) {
                out.write(journal.data(), (std::streamsize)journal.size());
                out.flush();
            }
        });

        // Offload lossless zlib compression and writing to the background thread pool
        m_threadPool.submit([outPath, data = std::move(pixels)]() {
            uLongf compBound = compressBound(static_cast<uLong>(data.size()));
            std::vector<std::uint8_t> compBuf(16 + compBound);
            std::memcpy(compBuf.data(), "ZRAW", 4);
            const uint32_t w = 1280;
            const uint32_t h = 720;
            const uint32_t rawSize = static_cast<uint32_t>(data.size());
            std::memcpy(compBuf.data() + 4, &w, 4);
            std::memcpy(compBuf.data() + 8, &h, 4);
            std::memcpy(compBuf.data() + 12, &rawSize, 4);

            uLongf destLen = compBound;
            if (compress2(compBuf.data() + 16, &destLen, data.data(), data.size(), 1) == Z_OK) {
                std::ofstream out(outPath, std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(compBuf.data()), 16 + destLen);
                    out.flush();
                }
            }
        });

        --m_frameDumpRemaining;
        if (m_frameDumpRemaining <= 0) {
            m_frameDumpActive = false;
            app().renderer().setDrawJournalEnabled(false);
            m_audio.playSfx(Sfx::ModalHide);
            DebugLog::log("[frame-dump] completed 10-second recording in %s (200 frames)",
                          m_frameDumpBatchDir.c_str());
            return;
        }
    }

    // Schedule next frame according to cadence (every 3 vsync frames = 20 FPS)
    if (m_frameDumpActive && !app().gpu().frameDumpBusy()) {
        ++m_frameDumpCounter;
        if (m_frameDumpCounter >= 3) {
            m_frameDumpCounter = 0;
            app().gpu().requestFrameDump();
        }
    }
}


void WiiUMenuApp::wireGlobalActions() {
    auto& root = rootBox();

    root.addAction(static_cast<uint64_t>(nxui::Button::B), [this]() {
        // Actions bubble to the root, so this fires even when an overlay has
        // already handled B. focusRoot() is the general test for "something
        // else owns input", and it replaces a hand-kept list that could not
        // know about screens added later: the on-screen keyboard was missing
        // from it, so cancelling a folder rename with B closed the keyboard
        // and the folder behind it in the same press, dropping the player out
        // to the home grid. isActive() covers the close animation, so the
        // guard still holds on the frame the overlay is dismissed.
        if (focusRoot() != &rootBox())
            return;
        if (m_openFolderId != 0)
            closeFolder();
    });

    root.addAction(static_cast<uint64_t>(nxui::Button::L), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        if (m_navigator.route() == switchu::navigation::Route::Home &&
            focusRoot() == &rootBox() && m_grid && m_grid->isXmb()) {
            m_grid->stepXmb(-1, 0);
            return;
        }
        if (m_navigator.route() == switchu::navigation::Route::Home &&
            focusRoot() == &rootBox() && isListLayout()) {
            jumpList(-1);
            return;
        }
        m_accessibility.repeatLastAnnouncement();
    });

    root.addAction(static_cast<uint64_t>(nxui::Button::LStick), [this]() {
        if (m_editMode) return;
        if (m_quickSettings && m_quickSettings->isActive()) {
            closeQuickSettings();
            return;
        }
        if ((m_dialog && m_dialog->isActive()) ||
            (m_contextMenu && m_contextMenu->isActive()) ||
            (m_themeShop && m_themeShop->isActive()) ||
            (m_gameGallery && m_gameGallery->isActive()) ||
            (m_gameMods && m_gameMods->isActive()) ||
            (m_gameCheats && m_gameCheats->isActive()) ||
            (m_gameDetails && m_gameDetails->isActive()) ||
            (m_settings && m_settings->isActive()) ||
            (m_steamGridDbPicker && m_steamGridDbPicker->isActive()) ||
        (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) ||
            (m_platformPicker && m_platformPicker->isActive()) ||
            (m_gameOptions && m_gameOptions->isActive()) ||
            (m_folderOptions && m_folderOptions->isActive()) ||
            (m_controllerTest && m_controllerTest->isActive()) ||
            (m_userSelect && m_userSelect->isActive())) {
            return;
        }
        openQuickSettings();
    });

    root.addAction(static_cast<uint64_t>(nxui::Button::RStick), [this]() {
        if (m_editMode) return;
        if (app().input().isHeld(nxui::Button::ZL) && app().input().isHeld(nxui::Button::ZR))
            return;
        if ((m_dialog && m_dialog->isActive()) ||
            (m_quickSettings && m_quickSettings->isActive()) ||
            (m_contextMenu && m_contextMenu->isActive()) ||
            (m_themeShop && m_themeShop->isActive()) ||
            (m_gameGallery && m_gameGallery->isActive()) ||
            (m_gameMods && m_gameMods->isActive()) ||
            (m_gameCheats && m_gameCheats->isActive()) ||
            (m_gameDetails && m_gameDetails->isActive()) ||
            (m_settings && m_settings->isActive()) ||
            (m_steamGridDbPicker && m_steamGridDbPicker->isActive()) ||
        (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) ||
            (m_platformPicker && m_platformPicker->isActive()) ||
            (m_gameOptions && m_gameOptions->isActive()) ||
            (m_folderOptions && m_folderOptions->isActive()) ||
            (m_controllerTest && m_controllerTest->isActive()) ||
            (m_userSelect && m_userSelect->isActive())) {
            return;
        }
        auto* cur = focusManager().current();
        if (!cur || cur->tag() != "glossy_icon") return;
        auto* icon = static_cast<GlossyIcon*>(cur);
        const std::uint64_t titleId = icon->titleId();
        if (titleId == 0 || (titleId >> 56) == 0xF1ULL || (titleId >> 56) == 0xF2ULL)
            return;

        const bool currentFav = m_config.isFavorite(titleId);
        m_config.setFavorite(titleId, !currentFav);
        m_config.save();
        switchu::commitSdCard("toggle favorite");

        if (!currentFav) {
            m_audio.playSfx(Sfx::Activate);
        } else {
            m_audio.playSfx(Sfx::ToggleOff);
        }

        icon->setFavorite(!currentFav);
        const int idx = findTitleIndex(titleId);
        if (idx >= 0) {
            m_model.at(idx).isFavorite = !currentFav;
        }

        if (m_config.sortMode == 3 || m_config.sortMode == 4) {
            reflowHomeGrid();
        }
    });

    // R is deliberately not wired as a press action. Holding it over a game is
    // how the homebrew override reaches Sphaira, and reordering the grid on the
    // initial press moved the tiles out from under the player mid-hold. The
    // sort now advances on release instead; see handleSortShortcutRelease.
    // The hint panel still advertises R, which is drawn from sortModeLabel().

    root.addAction(static_cast<uint64_t>(nxui::Button::ZL), [this]() {
        if (m_navigator.route() != switchu::navigation::Route::Home ||
            focusRoot() != &rootBox())
            return;
        if (deletePageAvailable()) {
            m_zlQuickFlipArmed = true;
            m_zlQuickFlipHeld = 0.f;
            return;
        }
        flipPage(-1);
    });
    root.addAction(static_cast<uint64_t>(nxui::Button::ZR), [this]() {
        if (m_navigator.route() != switchu::navigation::Route::Home ||
            focusRoot() != &rootBox())
            return;
        flipPage(+1);
    });
    root.addAction(static_cast<uint64_t>(nxui::Button::Y), [this]() {
        if ((m_dialog && m_dialog->isActive()) ||
            (m_quickSettings && m_quickSettings->isActive()) ||
            (m_contextMenu && m_contextMenu->isActive()) ||
            (m_themeShop && m_themeShop->isActive()) ||
            (m_gameGallery && m_gameGallery->isActive()) ||
            (m_gameMods && m_gameMods->isActive()) ||
            (m_gameCheats && m_gameCheats->isActive()) ||
            (m_gameDetails && m_gameDetails->isActive()) ||
            (m_settings && m_settings->isActive()) ||
            (m_steamGridDbPicker && m_steamGridDbPicker->isActive()) ||
        (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) ||
            (m_platformPicker && m_platformPicker->isActive()) ||
            (m_gameOptions && m_gameOptions->isActive()) ||
            (m_folderOptions && m_folderOptions->isActive()) ||
            (m_controllerTest && m_controllerTest->isActive()) ||
            (m_userSelect && m_userSelect->isActive())) {
            return;
        }

        if (m_appLayoutMode == AppLayoutMode::Metro) {
            cycleCurrentMetroTileSize();
            return;
        }

        if (m_editMode) {
            const std::string movedTitle = m_editHeldTitle;
            bool changed = commitEditModePlacement();
            exitEditMode();
            if (!movedTitle.empty()) {
                auto* focused = focusManager().current();
                std::string summary = nxui::I18n::instance().tr(
                    changed ? "accessibility.move_mode.placed" : "accessibility.move_mode.cancelled",
                    changed ? "Game moved: " : "Move cancelled: ") + movedTitle;
                if (focused) {
                    std::string context = accessibilityContextFor(focused);
                    std::string position = accessibilityPositionFor(focused);
                    if (!position.empty())
                        context = context.empty() ? position : context + ". " + position;
                    if (!context.empty())
                        summary += ". " + context;
                    if (!focused->accessibilitySummary().empty())
                        summary += ". " + focused->accessibilitySummary();
                }
                m_accessibility.announce(summary, true, true);
            }
            m_audio.playSfx(changed ? Sfx::ConfirmPositive : Sfx::ModalHide);
            return;
        }

        auto* cur = focusManager().current();
        if (!isEditableIcon(cur))
            return;

        enterEditMode();
        m_audio.playSfx(Sfx::Activate);
    });
#ifdef SWITCHU_DEBUG_UI
    root.addAction(static_cast<uint64_t>(nxui::Button::Minus), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        if (handleAccessibilityToggleCombo())
            return;
        m_showDebugOverlay = !m_showDebugOverlay;
        DebugLog::log("[debug] ImGui overlay toggled: %d", m_showDebugOverlay ? 1 : 0);
    });
#else
    root.addAction(static_cast<uint64_t>(nxui::Button::Minus), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        handleAccessibilityToggleCombo();
    });
#endif
#ifdef SWITCHU_HOMEBREW
    root.addAction(static_cast<uint64_t>(nxui::Button::Plus), [this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest)
            return;
        if (handleAccessibilityToggleCombo())
            return;
        m_plusExitPending = true;
        m_plusExitPendingTimer = 0.80f;
    });
#endif

#ifdef SWITCHU_MENU
    root.addAction(static_cast<uint64_t>(nxui::Button::X), [this]() {
        if (m_editMode) return;
        if (m_navigator.route() != switchu::navigation::Route::Home ||
            focusRoot() != &rootBox())
            return;

        if (deletePageAvailable()) {
            if (m_openFolderId != 0)
                deleteFolderPage();
            else
                deleteHomePage();
            return;
        }

        auto* cur = focusManager().current();
        if (!cur || cur->tag() != "glossy_icon") return;
        auto* icon = static_cast<GlossyIcon*>(cur);

        if (m_launcher.suspendedTitleId() != 0 &&
            m_launcher.isAppSuspended(icon->titleId())) {
            m_audio.playSfx(Sfx::ModalShow);
            m_dialogReturnFocus = cur;
            auto& i18n = nxui::I18n::instance();
            const auto markCloseRequested = [this]() {
                m_launcher.setAppRunning(false);
                m_launcher.setAppHasForeground(false);
                m_launcher.setSuspendedTitleId(0);
                for (auto& ic : m_grid->allIcons())
                    ic->setSuspended(false);
                if (auto* current = m_grid->focusManager().current()) {
                    auto* focusedIcon = static_cast<GlossyIcon*>(current);
                    m_titlePill->setText(focusedIcon->title());
                }
            };
#ifdef SWITCHU_TERMINATION_QUEUE_TEST
            m_dialog->show(
                "Lifecycle close test",
                "DIAGNOSTIC BUILD. B cancels. Applet close: use after HOME from a game's "
                "full-screen keyboard. Sleep powers down. Force 15s: wait.",
                {
                    {"Applet close", [this, markCloseRequested]() {
                        m_launcher.terminateApplication();
                        markCloseRequested();
                    }, true},
                    {"Duplicate", [this, markCloseRequested]() {
                        m_launcher.terminateApplicationDuplicate();
                        markCloseRequested();
                    }, true},
                    {"Sleep test", [this, markCloseRequested]() {
                        m_launcher.terminateApplicationHold();
                        m_launcher.enterSleep();
                        markCloseRequested();
                    }, true},
                    {"Force 15s", [this, markCloseRequested]() {
                        m_launcher.terminateApplicationForce();
                        markCloseRequested();
                    }, true}
                },
                0,
                {}
            );
#else
            m_dialog->show(
                i18n.tr("game.close_title", "Close game"),
                i18n.tr("game.close_prefix", "Close") + std::string(" ") + icon->title()
                    + i18n.tr("game.close_suffix", "?\nUnsaved progress will be lost."),
                {
                    {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                    {i18n.tr("button.close", "Close"),  [this, markCloseRequested]() {
                        m_launcher.terminateApplication();
                        markCloseRequested();
                    }, true}
                },
                1,
                {}
            );
#endif
            focusManager().setFocus(m_dialog.get());
            return;
        }

        if (m_openFolderId != 0) {
            const std::uint64_t titleId = icon->titleId();
            if (titleId == 0 || (titleId >> 56) == 0xF1ULL || (titleId >> 56) == 0xF2ULL)
                return;
            if (m_folderStore.folderForTitle(titleId) != m_openFolderId)
                return;
            DebugLog::log("[folders] X removes %016llX from folder %u",
                          static_cast<unsigned long long>(titleId), m_openFolderId);
            removeTitleFromFolder(titleId);
            return;
        }
    });
#endif
}

#if 0 // Replaced by the folder/page-arrow-aware 1.2 implementation.
void WiiUMenuApp::handleTouch() {
    constexpr float kSwipeThreshold = 80.f;
    constexpr float kLongPressThreshold = 0.55f;
    constexpr float kLongPressMoveThreshold = 18.f;

    auto& input = app().input();

    auto hitAvatar = [this](float x, float y) -> UserAvatarButton* {
        for (auto& avatar : m_userAvatarButtons) {
            if (avatar && avatar->isVisible() && avatar->hitTest(x, y))
                return avatar.get();
        }
        return nullptr;
    };

    auto focusTouchedIcon = [this](int localHit) -> GlossyIcon* {
        if (!m_grid || localHit < 0)
            return nullptr;

        int global = m_grid->currentPage() * m_grid->iconsPerPage() + localHit;
        if (!m_grid->focusGlobalIndex(global))
            return nullptr;

        auto* cur = m_grid->focusManager().current();
        if (!cur)
            return nullptr;

        focusManager().setFocus(cur);
        if (m_cursor) {
            m_cursor->moveTo(cur->focusRect().expanded(4.f), 0.f);
            m_cursor->setVisible(!m_grid->is3D() && !m_grid->isMetro() && !isListLayout() &&
                                 !m_grid->isXmb() && m_appLayoutMode != AppLayoutMode::Metro);
        } else {
            updateCursor();
        }

        if (!isEditableIcon(cur))
            return nullptr;
        return static_cast<GlossyIcon*>(cur);
    };

    if (input.touchDown()) {
        float tx = input.touchX();
        float ty = input.touchY();
        m_touchAvatarTarget = hitAvatar(tx, ty);
        m_touchAvatarWasFocused = m_touchAvatarTarget && (focusManager().current() == m_touchAvatarTarget);
        if (m_touchAvatarTarget) {
            m_touchHitIndex = -1;
            m_touchOnFocused = false;
            m_touchEditDragActive = false;
            return;
        }

        if (ty <= 80.f && !m_editMode &&
            !(m_dialog && m_dialog->isActive()) &&
            !(m_quickSettings && m_quickSettings->isActive())) {
            openQuickSettings();
            m_touchHitIndex = -1;
            m_touchOnFocused = false;
            m_touchEditDragActive = false;
            return;
        }

        int hit = m_grid->hitTest(tx, ty);
        m_touchHitIndex = hit;
        m_touchOnFocused = false;
        m_touchEditDragActive = false;
        if (hit >= 0) {
            auto icons = m_grid->pageIcons();
            if (hit < (int)icons.size())
                m_touchOnFocused = (icons[hit] == focusManager().current());
        }
    }

    if (input.isTouching() && m_touchHitIndex >= 0) {
        float dx = input.touchDeltaX();
        float dy = input.touchDeltaY();

        if (!m_editMode
            && std::abs(dx) <= kLongPressMoveThreshold
            && std::abs(dy) <= kLongPressMoveThreshold
            && input.touchDuration() >= kLongPressThreshold)
        {
            if (auto* icon = focusTouchedIcon(m_touchHitIndex)) {
                enterEditMode();
                if (m_editMode) {
                    m_touchEditDragActive = true;
                    m_audio.playSfx(Sfx::Activate);
                    m_editGhostTargetRect = icon->focusRect().expanded(4.f);
                }
            }
        }

        if (m_editMode && m_touchEditDragActive) {
            int dragHit = m_grid->hitTest(input.touchX(), input.touchY());
            if (dragHit >= 0)
                focusTouchedIcon(dragHit);
        }
    }

    if (input.touchUp()) {
        if (m_touchAvatarTarget) {
            float dx = input.touchDeltaX();
            float dy = input.touchDeltaY();
            UserAvatarButton* avatar = m_touchAvatarTarget;
            m_touchAvatarTarget = nullptr;
            if (std::abs(dx) < 20.f && std::abs(dy) < 20.f &&
                hitAvatar(input.touchX(), input.touchY()) == avatar)
            {
                focusManager().setFocus(avatar);
                if (!m_touchAvatarWasFocused)
                    avatar->activate();
            }
            m_touchAvatarWasFocused = false;
            return;
        }

        if (m_editMode && m_touchEditDragActive) {
            bool changed = commitEditModePlacement();
            exitEditMode();
            m_audio.playSfx(changed ? Sfx::ConfirmPositive : Sfx::ModalHide);
            m_touchHitIndex = -1;
            m_touchEditDragActive = false;
            return;
        }

        float dx = input.touchDeltaX();
        float dy = input.touchDeltaY();
        if (std::abs(dx) > kSwipeThreshold && std::abs(dx) > std::abs(dy) * 1.5f) {
            int p = m_grid->currentPage() + (dx < 0 ? 1 : -1);
            if (p >= 0 && p < m_grid->totalPages() && !m_grid->isTransitioning()) {
                m_grid->startWaveTransition(p);
                m_audio.playSfx(Sfx::PageChange);
            }
        }
        m_touchHitIndex = -1;
        m_touchEditDragActive = false;
    }
}
#endif

#ifdef SWITCHU_MENU
#if 0 // Replaced by the widget/folder-aware 1.2 implementation.
void WiiUMenuApp::handleSystemAction(SysAction a) {
    switch (a) {
        case SysAction::HomeButton:
            // HOME reaches the menu as a daemon notification rather than a HID
            // button. While locked it is an immediate system-level unlock, not
            // a regular HOME navigation event.
            if (m_lockScreen.isLocked()) {
                m_lockScreen.unlockNow();
                m_lockScreenLowPowerPrimed = false;
                app().setRenderEnabled(true);
                return;
            }
            DebugLog::log("[pump] HomeButton -> UI update");
            m_launcher.setAppHasForeground(false);

            markSuspendedIcon(m_launcher.suspendedTitleId());
            closeActiveOverlays();
            // Sem jogo suspenso nÃ£o hÃ¡ tÃ­tulo para procurar, e antes o seletor
            // simplesmente ficava onde estava -- na barra lateral.
            if (!focusTitle(m_launcher.suspendedTitleId()))
                focusGridSelection();
            break;
        default:
            break;
    }
}
#endif
#endif

#if 0 // Replaced by the dynamic-layout-aware 1.2 implementation.
void WiiUMenuApp::updateCursor() {
    if ((m_themeShop && m_themeShop->isActive()) ||
        (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) ||
        (m_gameGallery && m_gameGallery->isActive()) ||
        (m_gameMods && m_gameMods->isActive()) ||
        (m_gameDetails && m_gameDetails->isActive()) ||
        (m_settings && m_settings->isActive()) ||
        (m_dialog && m_dialog->isActive()) ||
        (m_quickSettings && m_quickSettings->isActive()) ||
        (m_userSelect && m_userSelect->isActive()))
        return;

    auto* cur = focusManager().current();
    if (cur) {
        nxui::Rect fr = cur->focusRect();
        m_cursor->moveTo(fr.expanded(4.f));
        m_cursor->setVisible(true);
    } else {
        m_cursor->setVisible(false);
    }
}
#endif

// Pressing + enters the game dossier directly. The old first dialog cost an
// unnecessary action and hid the very information the player asked for.
void WiiUMenuApp::showIconOptions() {
#ifdef SWITCHU_MENU
    if (m_editMode) return;
    if ((m_dialog && m_dialog->isActive()) || (m_gameMods && m_gameMods->isActive()) ||
        (m_gameCheats && m_gameCheats->isActive()) ||
        (m_gameDetails && m_gameDetails->isActive())) return;

    auto* cur = focusManager().current();
    if (!cur || cur->tag() != "glossy_icon") return;
    auto* icon = static_cast<GlossyIcon*>(cur);

    if (icon->entryKind() == GridEntryKind::Folder) {
        const std::uint32_t folderId = static_cast<std::uint32_t>(icon->titleId() - kFolderTitleIdPrefix);
        showFolderContextMenu(folderId);
        return;
    }

    const int index = findTitleIndex(icon->titleId());
    if (index < 0) return;
    const auto& entry = m_model.at(index);


    const std::uint64_t titleId = entry.titleId;
    const std::string title = entry.title;

    if (!isNativeApplicationId(titleId) && !m_config.isGamePort(titleId)) {
        m_dialogReturnFocus = cur;
        showNonGameOptions(titleId, title);
        return;
    }

    m_gameDetailsReturnFocus = cur;
    showGameDetails(titleId, title, icon->texture());
#endif
}

void WiiUMenuApp::showGamePortPlatformMenu(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_platformPicker) return;
    // The NACP/catalogue title reaching here can be whatever the port author
    // baked into the ROM (author credit, "Render96", resolution tags...) and
    // there was never a way to see or correct it before it was sent to the
    // online catalogue. Confirming it here, prefilled from any earlier
    // correction, is the one general fix that covers every community's
    // packaging convention without hardcoding any of them.
    auto& i18n = nxui::I18n::instance();
    const std::string initial = m_config.gamePortSearchTitle(titleId, title);
    DebugLog::log("[gameport] showGamePortPlatformMenu enter titleId=%016llX title=%s initial=%s",
                  (unsigned long long)titleId, title.c_str(), initial.c_str());
    requestTextEntry(
        i18n.tr("dialog.confirm_search_title", "Confirm search title"),
        i18n.tr("dialog.confirm_search_title_guide", "What this game is called elsewhere"),
        initial, 128, false,
        [this, titleId, title](const std::string& value) {
            DebugLog::log("[gameport] confirm-search-title accept fired titleId=%016llX value=%s",
                          (unsigned long long)titleId, value.c_str());
            const std::string searchTitle = value.empty() ? title : value;
            if (searchTitle != title)
                m_config.setGamePortSearchTitle(titleId, searchTitle);
            if (!m_platformPicker) {
                DebugLog::log("[gameport] confirm-search-title accept aborted: m_platformPicker null");
                return;
            }
            m_platformPickerTitleId = titleId;
            m_platformPickerTitle = title;
            raiseOverlay(m_platformPicker);
            m_audio.playSfx(Sfx::ModalShow);
            m_platformPicker->showForTitle(searchTitle);
            focusManager().setFocus(m_platformPicker.get());
            DebugLog::log("[gameport] platformPicker shown and focused searchTitle=%s", searchTitle.c_str());
        });
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::removeGamePort(std::uint64_t titleId) {
#ifdef SWITCHU_MENU
    m_config.gamePortPlatforms.erase(
        std::remove_if(m_config.gamePortPlatforms.begin(),
                       m_config.gamePortPlatforms.end(),
                       [titleId](const auto& entry) { return entry.first == titleId; }),
        m_config.gamePortPlatforms.end());
    m_config.save();
    if (m_widgetStore.recentActivity().titleId == titleId) {
        m_widgetStore.clearRecentActivity();
        m_widgetStore.save();
    }
    if (m_gameDetails)
        m_gameDetails->hide();
#else
    (void)titleId;
#endif
}

void WiiUMenuApp::showNonGameOptions(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    auto& i18n = nxui::I18n::instance();
    // Same overlay ordering fix the other dialogs carry: the shared dialog is
    // created before the overlays that can sit above it, so it is moved back to
    // the end of the tree before being shown.
    raiseOverlay(m_dialog);
    m_audio.playSfx(Sfx::ModalShow);
    // Homebrew has no dossier, so its folder action lives here. It can be filed
    // like anything else: FolderStore keys on the title id and does not care that
    // the entry is a forwarder.
    m_dialog->show(
        title,
        i18n.tr("dialog.non_game_options_body",
                "Homebrew and ports have no game details, artwork or mods to "
                "manage."),
        {
            {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
            {i18n.tr("dialog.mark_as_game_port", "Mark as game port"), [this, titleId, title]() {
                 showGamePortPlatformMenu(titleId, title);
             }, false},
            {i18n.tr("button.delete", "Delete"), [this, titleId, title]() {
                 confirmDeleteSoftware(titleId, title);
             }, false},
        },
        0, {});
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::showGameOptionsMenu(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    auto& i18n = nxui::I18n::instance();
    m_dialog->show(
        title,
        i18n.tr("dialog.icon_options_body", "Choose what to do with this software."),
        {
            {i18n.tr("dialog.icon_options_info", "Software information"),
             [this, titleId, title]() { showSoftwareInformation(titleId, title); }, true},
            {i18n.tr("dialog.icon_options_customize", "Customize"),
             [this, titleId, title]() { showGameCustomizeMenu(titleId, title); }, true},
            {i18n.tr("dialog.icon_options_delete", "Delete software"),
             [this, titleId, title]() { confirmDeleteSoftware(titleId, title); }, false},
            // closeOnPress, not an action. Cancel had it false with an empty
            // callback, which is a button that does nothing at all: the dialog
            // stayed open and the only way out was B.
            {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
        },
        0, {});
    focusManager().setFocus(m_dialog.get());
#endif
}

void WiiUMenuApp::showGameCustomizeMenu(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    auto& i18n = nxui::I18n::instance();
    m_audio.playSfx(Sfx::ModalShow);
    m_dialog->show(
        i18n.tr("dialog.customize_title", "Customize"),
        title + "\n\n" + i18n.tr("dialog.customize_body",
                                  "Choose how this software appears on the menu."),
        {
            {i18n.tr("dialog.icon_options_gallery", "Gallery"),
             [this, titleId, title]() { showGameGallery(titleId, title); }, true},
            {i18n.tr("dialog.customize_active_art", "Active artwork"),
             [this, titleId, title]() { showGameArtworkStatus(titleId, title); }, true},
            {i18n.tr("dialog.customize_restore_default", "Restore default"),
             [this, titleId, title]() { showGameArtworkRestoreMenu(titleId, title); }, true},
        },
        0,
        [this, titleId, title]() { showGameOptionsMenu(titleId, title); });
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::showGameArtworkStatus(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    // Game Details is added after the shared dialog in the overlay tree.  Put
    // the dialog back at the end before showing it so the modal is visible,
    // rather than merely receiving controller focus behind the dossier.
    raiseOverlay(m_dialog);
    auto& i18n = nxui::I18n::instance();
    const bool customCover = !gallery::GameArtworkStore::pathFor(
        titleId, gallery::ArtworkKind::Cover).empty();
    const bool customBackground = !gallery::GameArtworkStore::pathFor(
        titleId, gallery::ArtworkKind::Background).empty();
    const std::string body = title + "\n\n"
        + i18n.tr(customCover ? "dialog.customize_cover_custom"
                              : "dialog.customize_cover_default",
                  customCover ? "Cover: custom" : "Cover: default")
        + "\n"
        + i18n.tr(customBackground ? "dialog.customize_background_custom"
                                   : "dialog.customize_background_default",
                  customBackground ? "Background: custom" : "Background: default");
    // Details is now the parent view. Returning to the retired Customize
    // dialog here created two dialog transitions in one frame and could leave
    // controller focus without an active widget. This is intentionally a
    // terminal status dialog: both A and B hand focus straight back to Details.
    auto returnToDetails = [this]() {
        if (m_gameDetails && m_gameDetails->isActive())
            focusManager().setFocus(m_gameDetails.get());
    };
    m_dialog->show(i18n.tr("dialog.customize_active_art", "Active artwork"), body,
                   {
                       {i18n.tr("dialog.customize_restore_default", "Restore default"),
                        [this, titleId, title]() { showGameArtworkRestoreMenu(titleId, title); }, false},
                       {i18n.tr("button.ok", "OK"), returnToDetails, true},
                   },
                   0, returnToDetails);
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::showGameArtworkRestoreMenu(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    raiseOverlay(m_dialog);
    auto& i18n = nxui::I18n::instance();
    auto returnToDetails = [this]() {
        if (m_gameDetails && m_gameDetails->isActive())
            focusManager().setFocus(m_gameDetails.get());
    };
    m_dialog->show(i18n.tr("dialog.customize_restore_default", "Restore default"),
                   title + "\n\n" + i18n.tr("dialog.customize_restore_body",
                                               "Choose the artwork to restore."),
                   {
                       {i18n.tr("dialog.customize_restore_cover", "Restore cover"),
                        [this, titleId, title]() {
                            restoreGameArtworkFromOptions(titleId, title, gallery::ArtworkKind::Cover);
                        }, true},
                       {i18n.tr("dialog.customize_restore_background", "Restore background"),
                        [this, titleId, title]() {
                            restoreGameArtworkFromOptions(titleId, title, gallery::ArtworkKind::Background);
                        }, true},
                   },
                   0,
                   returnToDetails);
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::restoreGameArtworkFromOptions(
    std::uint64_t titleId, const std::string& title, gallery::ArtworkKind kind) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    const auto result = gallery::GameArtworkStore::restoreDefault(titleId, kind);
    const bool cover = kind == gallery::ArtworkKind::Cover;
    if (result.ok && cover && m_grid) {
        const StreamPump pump = gridStreamPump();
        m_iconStreamer.forceReload(pump.page, pump.perPage,
                                   app().gpu(), app().renderer(), m_grid->allIcons());
    }
    DebugLog::log("[gallery] options restore %s: title=%016llX ok=%d error=%s",
                  cover ? "cover" : "background", static_cast<unsigned long long>(titleId),
                  result.ok ? 1 : 0, result.error.c_str());
    auto& i18n = nxui::I18n::instance();
    const char* messageKey = result.ok
        ? (cover ? "dialog.gallery_restored_cover" : "dialog.gallery_restored_background")
        : "dialog.gallery_save_failed";
    const char* fallback = result.ok
        ? (cover ? "Default cover restored." : "Default background restored.")
        : "Could not save image.";
    auto returnToDetails = [this]() {
        if (m_gameDetails && m_gameDetails->isActive())
            focusManager().setFocus(m_gameDetails.get());
    };
    m_dialog->show(i18n.tr("dialog.customize_restore_default", "Restore default"),
                   title + "\n\n" + i18n.tr(messageKey, fallback),
                   {{i18n.tr("button.ok", "OK"), returnToDetails, true}},
                   0,
                   returnToDetails);
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
    (void)kind;
#endif
}

void WiiUMenuApp::showSoftwareInformation(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    showGameDetails(titleId, title);
#endif
}

void WiiUMenuApp::showGameDetails(std::uint64_t titleId, const std::string& title,
                                  nxui::Texture* liveCover) {
#ifdef SWITCHU_MENU
    createGameDetails();
    if (!m_gameDetails) return;
    std::vector<std::uint8_t> cover = gallery::GameArtworkStore::loadCover(titleId);
    if (cover.empty()) cover = AppListLoader::loadIconData(titleId);
    if (!m_gameDetailsReturnFocus)
        m_gameDetailsReturnFocus = m_dialogReturnFocus;
    m_dialogReturnFocus = m_gameDetails.get();
    m_audio.playSfx(Sfx::ModalShow);
    // A title marked as a port always searches under the platform it was
    // marked with, even when its title id happens to sit in the native
    // range: that range is only ever a packaging detail for a community
    // port, and letting it win here is what sent titles like a legacy PC
    // GTA V build to the "nintendo-switch" lookup, where it can never match.
    const std::string metadataPlatform = m_config.isGamePort(titleId)
        ? m_config.gamePortPlatform(titleId)
        : (isNativeApplicationId(titleId) ? "nintendo-switch" : std::string());
    const std::string searchTitle = m_config.gamePortSearchTitle(titleId, title);
    m_gameDetails->openForGame(titleId, title, searchTitle, std::move(cover), liveCover,
                                installedDisplayVersion(titleId), installedModSummary(titleId),
                                installedPlayTime(titleId), metadataPlatform,
                                m_config.isGamePort(titleId),
                                m_config.isFavorite(titleId));
    // Game Details is persistent and may already sit below an overlay created
    // later (notably Activity Log). Opening it changed visibility and focus but
    // did not change sibling draw order, so selecting an activity row left the
    // dossier rendered behind the still-active log. Reinsert it at the top just
    // like every other overlay that can be opened from another overlay.
    raiseOverlay(m_gameDetails);
    focusManager().setFocus(m_gameDetails.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::showGameMods(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_gameMods) {
        m_gameMods = std::make_shared<GameModsScreen>();
        if (m_overlayLayer) m_overlayLayer->addChild(m_gameMods);
        m_gameMods->setFont(&m_fontNormal);
        m_gameMods->setSmallFont(&m_fontSmall);
        m_gameMods->setTheme(&m_theme);
        m_gameMods->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
        m_gameMods->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                       m_config.accessibilitySpeakPosition);
        m_gameMods->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
        m_gameMods->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
        m_gameMods->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
        m_gameMods->onAccessibilityAnnouncement([this](const std::string& text) {
            m_accessibility.announce(text);
        });
        m_gameMods->onRequestRemove([this]() { confirmDeleteGameMod(); });
        m_gameMods->onClosed([this]() {
            if (m_gameDetails && m_gameDetails->isActive()) {
                m_suppressNextNavigateSfx = true;
                focusManager().setFocus(m_gameDetails.get());
            } else if (auto* target = m_grid ? m_grid->focusManager().current() : nullptr) {
                focusManager().setFocus(target);
            }
        });
    }
    m_audio.playSfx(Sfx::ModalShow);
    m_gameMods->openForGame(titleId, title);
    focusManager().setFocus(m_gameMods.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::showGameCheats(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_gameCheats) {
        m_gameCheats = std::make_shared<GameCheatsScreen>();
        if (m_overlayLayer) m_overlayLayer->addChild(m_gameCheats);
        m_gameCheats->setFont(&m_fontNormal);
        m_gameCheats->setSmallFont(&m_fontSmall);
        m_gameCheats->setTheme(&m_theme);
        m_gameCheats->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
        m_gameCheats->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                        m_config.accessibilitySpeakPosition);
        m_gameCheats->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
        m_gameCheats->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
        m_gameCheats->onToggleOffSfx([this]() { m_audio.playSfx(Sfx::ToggleOff); });
        m_gameCheats->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
        m_gameCheats->onAccessibilityAnnouncement([this](const std::string& text) {
            m_accessibility.announce(text);
        });
        m_gameCheats->onClosed([this]() {
            if (m_gameDetails && m_gameDetails->isActive()) {
                m_suppressNextNavigateSfx = true;
                focusManager().setFocus(m_gameDetails.get());
            } else if (auto* target = m_grid ? m_grid->focusManager().current() : nullptr) {
                focusManager().setFocus(target);
            }
        });
    }
    raiseOverlay(m_gameCheats);
    m_audio.playSfx(Sfx::ModalShow);
    m_gameCheats->openForGame(titleId, title);
    focusManager().setFocus(m_gameCheats.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::confirmDeleteGameMod() {
#ifdef SWITCHU_MENU
    if (!m_dialog || !m_gameMods || !m_gameMods->isActive()) return;
    const std::string name = m_gameMods->selectedName();
    if (name.empty()) return;
    auto& i18n = nxui::I18n::instance();
    // The Mods screen is newer in the overlay stack; place the confirmation
    // above it so it receives both rendering and controller focus.
    raiseOverlay(m_dialog);
    m_dialogReturnFocus = m_gameMods.get();
    m_dialog->show(i18n.tr("dialog.mods_remove_title", "Remove mod?"),
                   name + "\n\n" + i18n.tr("dialog.mods_remove_body",
                                              "This permanently removes this mod from the SD card."),
                   {
                       {i18n.tr("button.cancel", "Cancel"), []() {}, true},
                       {i18n.tr("dialog.mods_remove", "Remove"), [this]() {
                            if (m_gameMods) m_gameMods->removeSelected();
                        }, true},
                   }, 0, {});
    focusManager().setFocus(m_dialog.get());
#endif
}

void WiiUMenuApp::showGameGallery(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    createGameGallery();
    if (!m_gameGallery)
        return;
    // The normal caller leaves its return target in m_dialogReturnFocus. When
    // Gallery was entered from the dossier this is the dossier itself, so the
    // close callback can resume it instead of exiting to the home grid.
    m_gameGalleryReturnFocus = m_dialogReturnFocus;
    m_dialogReturnFocus = m_gameGallery.get();
    m_audio.playSfx(Sfx::ModalShow);
    m_gameGallery->openForGame(titleId, title);
    focusManager().setFocus(m_gameGallery.get());
#endif
}

void WiiUMenuApp::confirmGameArtwork(
    std::uint64_t titleId, const std::string& title, GameGalleryClient::Category category,
    GameGalleryClient::Asset asset, std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
#ifdef SWITCHU_MENU
    if (!m_dialog || !m_gameGallery || !bytes || bytes->empty())
        return;

    auto& i18n = nxui::I18n::instance();
    const bool cover = category == GameGalleryClient::Category::Grids;
    const std::string message = i18n.tr(
        cover ? "dialog.gallery_apply_cover" : "dialog.gallery_apply_background",
        cover ? "This image will be saved as this software's custom cover."
              : "This image will be saved as this software's custom background.");
    // GameGallery is created after the shared dialog and therefore sits above
    // it in the overlay paint order. Reinsert the dialog before showing it so
    // its focused A/B controls and its visual card always describe one layer.
    raiseOverlay(m_dialog);
    m_dialogReturnFocus = m_gameGallery.get();
    m_audio.playSfx(Sfx::ModalShow);
    m_dialog->show(i18n.tr("dialog.gallery_apply_title", "Use this image?"),
                   title + "\n\n" + message,
                   {
                       {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                       {i18n.tr("dialog.gallery_apply", "Use image"),
                        [this, titleId, category, asset = std::move(asset), bytes = std::move(bytes)]() mutable {
                            startGameArtworkSave(titleId, category, std::move(asset), std::move(bytes));
                        }, true},
                   },
                   0, {});
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
    (void)category;
    (void)asset;
    (void)bytes;
#endif
}

void WiiUMenuApp::startGameArtworkSave(
    std::uint64_t titleId, GameGalleryClient::Category category,
    GameGalleryClient::Asset asset, std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
    if (!m_gameGallery || !bytes || bytes->empty())
        return;
    syncGameArtworkSave();
    if (m_gameArtworkSaveFuture.valid()
        && m_gameArtworkSaveFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        m_gameGallery->requestToast(nxui::I18n::instance().tr(
            "dialog.gallery_saving", "Saving image..."));
        return;
    }

    const auto kind = category == GameGalleryClient::Category::Grids
        ? gallery::ArtworkKind::Cover : gallery::ArtworkKind::Background;
    auto shared = std::make_shared<GameArtworkSaveShared>();
    shared->result.kind = kind;
    m_gameArtworkSave = shared;
    m_gameGallery->requestToast(nxui::I18n::instance().tr(
        "dialog.gallery_saving", "Saving image..."));
    m_gameArtworkSaveFuture = m_threadPool.submit([shared, titleId, kind, asset = std::move(asset), bytes = std::move(bytes)]() {
        const auto result = gallery::GameArtworkStore::save(titleId, kind, asset, *bytes);
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->result = result;
    });
}

void WiiUMenuApp::confirmRestoreGameArtwork(
    std::uint64_t titleId, const std::string& title, GameGalleryClient::Category category) {
#ifdef SWITCHU_MENU
    if (!m_dialog || !m_gameGallery)
        return;

    const bool cover = category == GameGalleryClient::Category::Grids;
    auto& i18n = nxui::I18n::instance();
    raiseOverlay(m_dialog);
    m_dialogReturnFocus = m_gameGallery.get();
    m_audio.playSfx(Sfx::ModalShow);
    m_dialog->show(i18n.tr(cover ? "dialog.gallery_restore_cover_title"
                                 : "dialog.gallery_restore_background_title",
                                 cover ? "Restore default cover?" : "Restore default background?"),
                   title + "\n\n" + i18n.tr(
                       cover ? "dialog.gallery_restore_cover_body"
                             : "dialog.gallery_restore_background_body",
                       cover ? "The custom cover will be removed and the original Switch icon will return."
                             : "The custom background will be removed and the theme background will return."),
                   {
                       {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                       {i18n.tr("dialog.gallery_restore", "Restore default"),
                        [this, titleId, category]() { startGameArtworkRestore(titleId, category); }, true},
                   },
                   0, {});
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
    (void)category;
#endif
}

void WiiUMenuApp::startGameArtworkRestore(
    std::uint64_t titleId, GameGalleryClient::Category category) {
    if (!m_gameGallery)
        return;
    syncGameArtworkSave();
    if (m_gameArtworkSaveFuture.valid()
        && m_gameArtworkSaveFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        m_gameGallery->requestToast(nxui::I18n::instance().tr(
            "dialog.gallery_saving", "Saving image..."));
        return;
    }
    const auto kind = category == GameGalleryClient::Category::Grids
        ? gallery::ArtworkKind::Cover : gallery::ArtworkKind::Background;
    auto shared = std::make_shared<GameArtworkSaveShared>();
    shared->result.kind = kind;
    shared->result.titleId = titleId;
    shared->result.restored = true;
    m_gameArtworkSave = shared;
    m_gameGallery->requestToast(nxui::I18n::instance().tr(
        "dialog.gallery_restoring", "Restoring default..."));
    m_gameArtworkSaveFuture = m_threadPool.submit([shared, titleId, kind]() {
        const auto result = gallery::GameArtworkStore::restoreDefault(titleId, kind);
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->result = result;
    });
}

void WiiUMenuApp::syncGameArtworkSave() {
    if (!m_gameArtworkSaveFuture.valid()
        || m_gameArtworkSaveFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    auto shared = m_gameArtworkSave;
    gallery::ArtworkSaveResult result;
    try {
        m_gameArtworkSaveFuture.get();
    } catch (...) {
        result.error = "unexpected save failure";
    }
    if (shared) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        result = shared->result;
    }
    m_gameArtworkSave.reset();
    if (!m_gameGallery)
        return;

    auto& i18n = nxui::I18n::instance();
    if (result.ok) {
        const bool cover = result.kind == gallery::ArtworkKind::Cover;
        const char* messageKey = result.restored
            ? (cover ? "dialog.gallery_restored_cover" : "dialog.gallery_restored_background")
            : (cover ? "dialog.gallery_saved_cover" : "dialog.gallery_saved_background");
        const char* fallback = result.restored
            ? (cover ? "Default cover restored." : "Default background restored.")
            : (cover ? "Custom cover saved." : "Custom background saved.");
        m_gameGallery->requestToast(i18n.tr(messageKey, fallback), 2.8f);
        DebugLog::log("[gallery] %s %s: %s", result.restored ? "restored" : "saved",
                      cover ? "cover" : "background", result.path.c_str());
        if (cover && m_grid) {
            // The visible icon may already own a streamed system texture. Reload
            // this small page cache so the new cover appears before navigating
            // away and back.
            const StreamPump pump = gridStreamPump();
            m_iconStreamer.forceReload(pump.page, pump.perPage,
                                       app().gpu(), app().renderer(), m_grid->allIcons());
        } else if (!cover) {
            auto* focused = focusManager().current();
            if (focused && focused->tag() == "glossy_icon" &&
                static_cast<GlossyIcon*>(focused)->titleId() == result.titleId)
                refreshGameArtworkBackdrop(result.titleId);
        }
    } else {
        m_gameGallery->requestToast(i18n.tr("dialog.gallery_save_failed",
                                            "Could not save image."), 3.2f);
        DebugLog::log("[gallery] save failed: %s", result.error.c_str());
    }
}

void WiiUMenuApp::confirmDeleteSoftware(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_dialog)
        return;
    // Reached from the dossier, which is added after the shared dialog in the
    // overlay tree. Without moving the dialog back to the end it takes focus
    // but is drawn behind the dossier: the A/B hints change and no card
    // appears, so a delete looks like it is waiting on nothing. Same fix the
    // artwork dialogs already carry.
    raiseOverlay(m_dialog);
    auto& i18n = nxui::I18n::instance();
    // Says what goes and what does not. Save data survives a delete on this
    // console, and somebody about to remove a game they intend to reinstall
    // deserves to know that before they decide, not after.
    std::string body = fmt::format("{}\n\n{}", title,
        i18n.tr("dialog.delete_software_body",
                "This removes the software from the console. It cannot be undone "
                "here -- the software has to be downloaded or installed again."));

    // Name the folders that go with it. A port keeps its content in
    // atmosphere/contents, which is where the space actually is: without this
    // the dialog promised a delete, the card never gained a byte back, and
    // there was no way to tell beforehand that anything would be left behind.
    const auto footprint = switchu::titles::sdFootprint(titleId);
    if (!footprint.empty()) {
        body += "\n\n";
        body += i18n.tr("dialog.delete_software_files",
                        "These folders on the SD card will be removed too:");
        for (const std::string& path : footprint)
            body += "\n" + path;
    }

    m_audio.playSfx(Sfx::ModalShow);
    m_dialog->show(
        i18n.tr("dialog.delete_software", "Delete software?"),
        body,
        {
            {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
            {i18n.tr("button.delete", "Delete"), [this, titleId, title]() {
                 // Was its own inline nsDeleteApplicationCompletely() call, which
                 // removed only registered content, blocked the frame while it
                 // ran, and reported success whenever ns was happy -- so a port
                 // whose files were all still on the card was announced as
                 // deleted. startSoftwareDeletion() is the one implementation:
                 // it runs on a worker behind the progress dialog and sweeps the
                 // SD locations ns knows nothing about.
                 startSoftwareDeletion(titleId, title, false);
             }, false},
        },
        0, {});
    focusManager().setFocus(m_dialog.get());
#endif
}
