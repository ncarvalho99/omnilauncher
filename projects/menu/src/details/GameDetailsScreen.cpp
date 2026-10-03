#include "GameDetailsScreen.hpp"

#include "themeshop/ThemeHttp.hpp"
#include "core/GridModel.hpp"

#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

std::string join(const std::vector<std::string>& values, const char* separator = ", ") {
    std::string result;
    for (const auto& value : values) {
        if (value.empty()) continue;
        if (!result.empty()) result += separator;
        result += value;
    }
    return result;
}

std::string metadataPlatformLabel(const std::string& slug) {
    if (slug == "nintendo-switch") return "Nintendo Switch";
    if (slug == "pc") return "PC";
    if (slug == "playstation") return "PlayStation";
    if (slug == "playstation-2") return "PlayStation 2";
    if (slug == "playstation-3") return "PlayStation 3";
    if (slug == "psp") return "PSP";
    if (slug == "playstation-vita") return "PS Vita";
    if (slug == "dreamcast") return "Dreamcast";
    if (slug == "nintendo-64") return "Nintendo 64";
    if (slug == "gamecube") return "GameCube";
    if (slug == "wii") return "Wii";
    if (slug == "nintendo-ds") return "Nintendo DS";
    if (slug == "game-boy-advance") return "Game Boy Advance";
    return slug;
}

std::string hours(float value) {
    if (value <= 0.f) return "—";
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(value < 10.f ? 1 : 0) << value << " h";
    return stream.str();
}

void drawStar(nxui::Renderer& ren, const nxui::Vec2& center, float outerRadius, const nxui::Color& color) {
    const float innerRadius = outerRadius * 0.382f;
    constexpr int kPoints = 5;
    nxui::Vec2 outer[kPoints];
    nxui::Vec2 inner[kPoints];
    constexpr float kPi = 3.14159265358979323846f;
    for (int i = 0; i < kPoints; ++i) {
        float angleOuter = -kPi * 0.5f + i * (2.f * kPi / kPoints);
        float angleInner = angleOuter + (kPi / kPoints);
        outer[i] = { center.x + outerRadius * std::cos(angleOuter),
                     center.y + outerRadius * std::sin(angleOuter) };
        inner[i] = { center.x + innerRadius * std::cos(angleInner),
                     center.y + innerRadius * std::sin(angleInner) };
    }
    for (int i = 0; i < kPoints; ++i) {
        int prevInner = (i + kPoints - 1) % kPoints;
        ren.drawTriangle(center, outer[i], inner[i], color);
        ren.drawTriangle(center, inner[prevInner], outer[i], color);
    }
}


nxui::Rect coverTextureSource(const nxui::Texture& texture, const nxui::Rect& destination) {
    const float textureAspect = (float)texture.width() / (float)texture.height();
    const float destinationAspect = destination.width / destination.height;
    if (textureAspect > destinationAspect) {
        const float width = texture.height() * destinationAspect;
        return {(texture.width() - width) * 0.5f, 0.f, width, (float)texture.height()};
    }
    const float height = texture.width() / destinationAspect;
    return {0.f, (texture.height() - height) * 0.5f, (float)texture.width(), height};
}

} // namespace

GameDetailsScreen::GameDetailsScreen()
    : TabbedOverlayScreen(ScreenMode::GameOptions) {
}

void GameDetailsScreen::openForGame(std::uint64_t titleId, std::string title,
                                    std::string searchTitle,
                                    std::vector<std::uint8_t> activeCover,
                                    nxui::Texture* liveCover,
                                    std::string displayVersion, std::string modSummary,
                                    std::string playTime, std::string metadataPlatform,
                                    bool isGamePort, bool isFavorite) {
    m_titleId = titleId;
    m_title = std::move(title);
    m_searchTitle = searchTitle.empty() ? m_title : std::move(searchTitle);
    m_metadataPlatform = std::move(metadataPlatform);
    m_isGamePort = isGamePort;
    m_isFavorite = isFavorite;
    m_displayVersion = std::move(displayVersion);
    m_modSummary = std::move(modSummary);
    m_playTime = std::move(playTime);
    m_coverBytes = std::move(activeCover);
    m_coverTexture = {};
    m_liveCover = liveCover;
    m_coverUploadPending = !m_coverBytes.empty();
    m_snapshot = {};
    m_seenRevision = 0;
    m_selectedScreenshot = 0;
    m_selectedAction = 0;
    m_summaryScrollLine = 0;
    m_focusZone = FocusZone::Screenshots;
    m_imageExpanded = false;
    clearImages();
    show();
    // One empty tab reserves the existing left rail for the cover and local
    // facts, while the custom content owns every visible interaction.
    m_tabIndex = 0;
    m_focusArea = FocusArea::Content;
    m_localOnly = m_metadataPlatform.empty();
    if (m_pool && !m_localOnly)
        m_client.load(*m_pool, m_searchTitle, m_metadataPlatform);
}

void GameDetailsScreen::buildTabs() {
    m_tabs.clear();
    m_tabs.push_back({"", {}});
}

void GameDetailsScreen::resumeFromChild() {
    show();
    m_tabIndex = 0;
    m_focusArea = FocusArea::Content;
    m_focusZone = FocusZone::Actions;
    m_selectedAction = 0;
}

void GameDetailsScreen::updateSearchTitle(std::string searchTitle) {
    m_searchTitle = searchTitle.empty() ? m_title : std::move(searchTitle);
    m_snapshot = {};
    m_seenRevision = 0;
    m_localOnly = m_metadataPlatform.empty();
    if (m_pool && !m_localOnly)
        m_client.load(*m_pool, m_searchTitle, m_metadataPlatform);
}

void GameDetailsScreen::clearImages() {
    if (m_gpu && !m_images.empty())
        m_gpu->waitIdle();
    m_images.clear();
}

void GameDetailsScreen::queueImage(const std::string& url, int maxSide) {
    if (!m_pool || url.empty()) return;
    auto& slot = m_images[url];
    if (!slot) slot = std::make_shared<ImageState>();
    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->phase != ImagePhase::Idle) return;
        slot->phase = ImagePhase::Loading;
        slot->maxSide = maxSide;
    }
    slot->future = m_pool->submit([slot, url]() {
        try {
            auto bytes = themeshop::http::getBytes(url);
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->bytes = std::move(bytes);
            slot->phase = slot->bytes.empty() ? ImagePhase::Failed : ImagePhase::Downloaded;
        } catch (...) {
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->bytes.clear();
            slot->phase = ImagePhase::Failed;
        }
    });
}

void GameDetailsScreen::syncImageUploads() {
    if (!m_gpu || !m_renderer) return;
    int uploads = 0;
    for (auto& [_, state] : m_images) {
        if (state->future.valid() && state->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try { state->future.get(); } catch (...) {}
        }
        std::vector<std::uint8_t> bytes;
        int maxSide = 1024;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (uploads >= 1 || state->phase != ImagePhase::Downloaded) continue;
            bytes = state->bytes;
            maxSide = state->maxSide;
        }
        if (bytes.empty()) continue;
        ++uploads;
        nxui::Texture texture;
        const bool ok = texture.loadFromMemory(*m_gpu, *m_renderer, bytes.data(), bytes.size(), maxSide);
        std::lock_guard<std::mutex> lock(state->mutex);
        if (ok) {
            state->texture = std::move(texture);
            state->phase = ImagePhase::Ready;
        } else {
            state->bytes.clear();
            state->phase = ImagePhase::Failed;
        }
    }
}

const nxui::Texture* GameDetailsScreen::imageTexture(const std::string& url) const {
    const auto it = m_images.find(url);
    if (it == m_images.end()) return nullptr;
    std::lock_guard<std::mutex> lock(it->second->mutex);
    return it->second->phase == ImagePhase::Ready && it->second->texture.valid()
        ? &it->second->texture : nullptr;
}

GameDetailsScreen::ImagePhase GameDetailsScreen::imagePhase(const std::string& url) const {
    const auto it = m_images.find(url);
    if (it == m_images.end()) return ImagePhase::Idle;
    std::lock_guard<std::mutex> lock(it->second->mutex);
    return it->second->phase;
}

void GameDetailsScreen::syncLocalCover() {
    if (!m_coverUploadPending || !m_gpu || !m_renderer) return;
    m_coverUploadPending = false;
    if (!m_coverBytes.empty()
        && !m_coverTexture.loadFromMemory(*m_gpu, *m_renderer, m_coverBytes.data(), m_coverBytes.size(), 512)) {
        m_coverBytes.clear();
    }
    std::vector<std::uint8_t>().swap(m_coverBytes);
}

void GameDetailsScreen::clampScreenshotSelection() {
    if (m_snapshot.screenshots.empty()) {
        m_selectedScreenshot = 0;
        return;
    }
    m_selectedScreenshot = std::clamp(m_selectedScreenshot, 0, (int)m_snapshot.screenshots.size() - 1);
}

nxui::Rect GameDetailsScreen::heroRect(const nxui::Rect& content) const {
    return {content.x + 18.f, content.y + (m_metadataPlatform.empty() ? 75.f : 96.f),
            content.width - 36.f, m_metadataPlatform.empty() ? 190.f : 169.f};
}

nxui::Rect GameDetailsScreen::thumbnailRect(const nxui::Rect& content, int index, int total) const {
    const float gap = 12.f;
    const float available = content.width - 36.f;
    const int visible = std::max(1, std::min(5, total));
    const float width = (available - gap * (visible - 1)) / visible;
    return {content.x + 18.f + index * (width + gap), content.y + 276.f, width, 52.f};
}

nxui::Rect GameDetailsScreen::fitTexture(const nxui::Rect& rect, const nxui::Texture& texture) {
    const float scale = std::min(rect.width / (float)texture.width(), rect.height / (float)texture.height());
    const float width = texture.width() * scale;
    const float height = texture.height() * scale;
    return {rect.x + (rect.width - width) * 0.5f, rect.y + (rect.height - height) * 0.5f, width, height};
}

std::string GameDetailsScreen::ellipsize(nxui::Font* font, const std::string& text, float maxWidth, float scale) {
    if (!font || text.empty() || font->measure(text).x * scale <= maxWidth) return text;
    std::string result = text;
    while (!result.empty() && font->measure(result + "...").x * scale > maxWidth)
        result.pop_back();
    return result.empty() ? "..." : result + "...";
}

std::vector<std::string> GameDetailsScreen::wrapText(nxui::Font* font, const std::string& text,
                                                      float maxWidth, float scale, int maxLines) {
    std::vector<std::string> lines;
    if (!font || text.empty() || maxLines <= 0) return lines;
    std::istringstream stream(text);
    std::string word;
    while (stream >> word) {
        if (lines.empty()) lines.push_back(word);
        else {
            std::string candidate = lines.back() + " " + word;
            if (font->measure(candidate).x * scale <= maxWidth) lines.back() = std::move(candidate);
            else {
                if ((int)lines.size() >= maxLines) {
                    lines.back() = ellipsize(font, lines.back() + " " + word, maxWidth, scale);
                    break;
                }
                lines.push_back(word);
            }
        }
    }
    return lines;
}

void GameDetailsScreen::updateCustomContent(float) {
    if (!m_showing) {
        m_imageExpanded = false;
        return;
    }
    const auto next = m_client.snapshot();
    if (next.revision != m_seenRevision) {
        m_seenRevision = next.revision;
        m_snapshot = next;
        m_selectedScreenshot = 0;
        m_imageExpanded = false;
        clearImages();
    } else {
        m_snapshot = next;
    }
    clampScreenshotSelection();
    syncLocalCover();
    if (m_snapshot.phase == GameMetadataClient::Phase::Ready) {
        for (int i = 0; i < (int)m_snapshot.screenshots.size() && i < 5; ++i)
            queueImage(m_snapshot.screenshots[(size_t)i], 1024);
        if (m_imageExpanded && !m_snapshot.screenshots.empty())
            queueImage(m_snapshot.screenshots[(size_t)m_selectedScreenshot], 1280);
    }
    syncImageUploads();
}

bool GameDetailsScreen::handleCustomPressA() {
    if (m_focusZone == FocusZone::Actions) {
        activateAction();
        return true;
    }
    if (m_focusZone == FocusZone::Summary)
        return true;
    // Only a network failure is worth asking about again. A title the catalogue
    // does not have stays absent, and the service remembers that answer for a
    // day, so retrying it would spend a request to be told the same thing.
    if (!m_localOnly && m_snapshot.phase == GameMetadataClient::Phase::Failed) {
        if (m_pool) m_client.load(*m_pool, m_searchTitle, m_metadataPlatform);
        if (m_activateSfxCb) m_activateSfxCb();
        return true;
    }
    if (m_snapshot.phase != GameMetadataClient::Phase::Ready || m_snapshot.screenshots.empty())
        return true;
    m_imageExpanded = !m_imageExpanded;
    queueImage(m_snapshot.screenshots[(size_t)m_selectedScreenshot], 1280);
    if (m_activateSfxCb) m_activateSfxCb();
    return true;
}

bool GameDetailsScreen::handleCustomPressB() {
    if (m_imageExpanded) {
        m_imageExpanded = false;
        if (m_navSfxCb) m_navSfxCb();
    } else if (m_focusZone == FocusZone::Summary) {
        m_focusZone = FocusZone::Screenshots;
        m_summaryScrollLine = 0;
        if (m_navSfxCb) m_navSfxCb();
    } else {
        hide();
    }
    return true;
}

bool GameDetailsScreen::handleCustomNavUp() {
    if (m_focusZone == FocusZone::Summary) {
        if (m_summaryScrollLine > 0)
            --m_summaryScrollLine;
        else
            m_focusZone = FocusZone::Screenshots;
        if (m_navSfxCb) m_navSfxCb();
    } else if (m_focusZone == FocusZone::Actions) {
        if (m_selectedAction == 0)
            m_focusZone = FocusZone::Screenshots;
        else
            --m_selectedAction;
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

bool GameDetailsScreen::handleCustomNavDown() {
    if (m_focusZone == FocusZone::Screenshots && !m_imageExpanded) {
        m_focusZone = FocusZone::Summary;
        if (m_navSfxCb) m_navSfxCb();
    } else if (m_focusZone == FocusZone::Summary) {
        ++m_summaryScrollLine;
        if (m_navSfxCb) m_navSfxCb();
    } else if (m_focusZone == FocusZone::Actions) {
        const int maxAction = static_cast<int>(railActions().size()) - 1;
        if (maxAction >= 0)
            m_selectedAction = std::min(maxAction, m_selectedAction + 1);
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

bool GameDetailsScreen::handleCustomNavLeft() {
    if (!m_imageExpanded && m_focusZone == FocusZone::Summary) {
        m_focusZone = FocusZone::Actions;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }
    if (!m_imageExpanded && m_focusZone == FocusZone::Screenshots) {
        if (m_selectedScreenshot > 0) {
            --m_selectedScreenshot;
        } else {
            m_focusZone = FocusZone::Actions;
        }
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }
    if (m_focusZone == FocusZone::Actions) return true;
    if (!m_snapshot.screenshots.empty()) {
        m_selectedScreenshot = (m_selectedScreenshot - 1 + (int)m_snapshot.screenshots.size())
            % (int)m_snapshot.screenshots.size();
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

bool GameDetailsScreen::handleCustomNavRight() {
    if (!m_imageExpanded && m_focusZone == FocusZone::Summary) {
        m_focusZone = FocusZone::Screenshots;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }
    if (!m_imageExpanded && m_focusZone == FocusZone::Actions) {
        m_focusZone = FocusZone::Screenshots;
        if (m_navSfxCb) m_navSfxCb();
        return true;
    }
    if (m_focusZone == FocusZone::Actions) return true;
    if (!m_snapshot.screenshots.empty()) {
        m_selectedScreenshot = (m_selectedScreenshot + 1) % (int)m_snapshot.screenshots.size();
        if (m_navSfxCb) m_navSfxCb();
    }
    return true;
}

// The labels and what they do, built together. They used to be two lists kept
// in step by hand -- a vector of strings and a switch on the index -- where the
// port branch already made the same index mean two different things, and any
// new action had to be inserted into both in the same place.
std::vector<GameDetailsScreen::RailAction> GameDetailsScreen::railActions() const {
    auto& i18n = nxui::I18n::instance();
    std::vector<RailAction> actions;
    actions.push_back({i18n.tr("dialog.icon_options_gallery", "Gallery"), m_openGalleryCb});
    actions.push_back({i18n.tr("dialog.customize_active_art", "Active artwork"), m_showArtworkCb});
    actions.push_back({i18n.tr("dialog.details_manage_mods", "Manage mods"), m_manageModsCb});
    actions.push_back({i18n.tr("dialog.details_cheats", "Cheats"), m_cheatsCb});
    if (!m_isGamePort) {
        actions.push_back({i18n.tr("dialog.details_rename", "Rename"), m_renameCb});
    }
    if (m_isGamePort) {
        actions.push_back({i18n.tr("dialog.port_options", "Port options"), m_portOptionsCb});
    }
    actions.push_back({i18n.tr("dialog.icon_options_delete", "Delete software"), m_deleteSoftwareCb});
    return actions;
}

std::vector<std::string> GameDetailsScreen::actionLabels() const {
    std::vector<std::string> labels;
    for (const auto& action : railActions())
        labels.push_back(action.label);
    return labels;
}

void GameDetailsScreen::activateAction() {
    const auto actions = railActions();
    if (m_selectedAction < 0 || (std::size_t)m_selectedAction >= actions.size())
        return;
    if (actions[(std::size_t)m_selectedAction].callback)
        actions[(std::size_t)m_selectedAction].callback();
}

std::string GameDetailsScreen::onlineStatusMessage() const {
    auto& i18n = nxui::I18n::instance();
    // A finished search that found nothing used to fall through to the loading
    // text, so a port or homebrew appeared to load for ever. Each outcome now
    // says what actually happened.
    if (m_localOnly || (m_snapshot.phase == GameMetadataClient::Phase::Ready && !m_snapshot.found))
        return i18n.tr("dialog.details_no_online", "This title has no entry in the online catalogue.");
    if (m_snapshot.phase == GameMetadataClient::Phase::Failed)
        return i18n.tr("dialog.details_offline", "Online details are unavailable.");
    return i18n.tr("dialog.details_loading", "Loading game details...");
}

std::string GameDetailsScreen::currentAccessibilitySummary() const {
    auto& i18n = nxui::I18n::instance();
    if (m_imageExpanded)
        return i18n.tr("dialog.details_image_expanded", "Expanded gameplay image. Press B to return.");
    if (m_localOnly || m_snapshot.phase != GameMetadataClient::Phase::Ready)
        return m_title + ". " + onlineStatusMessage();
    if (m_snapshot.screenshots.empty())
        return m_title + ". " + onlineStatusMessage();
    if (m_focusZone == FocusZone::Actions)
        return i18n.tr("dialog.details_actions", "Left and right to select gameplay art. A to expand. B to return.");
    if (m_focusZone == FocusZone::Summary)
        return i18n.tr("dialog.details_description", "About this game");
    return m_title + ". " + std::to_string(m_selectedScreenshot + 1) + " "
        + i18n.tr("accessibility.context.of", "of") + " "
        + std::to_string(m_snapshot.screenshots.size()) + ". "
        + i18n.tr("dialog.details_image_hint", "Press A to expand gameplay image.");
}

void GameDetailsScreen::drawCustomContent(nxui::Renderer& ren, const nxui::Rect& panel,
                                          const nxui::Rect& content, float opacity) {
    if (!m_theme || !m_font || !m_smallFont) return;
    auto& i18n = nxui::I18n::instance();
    const nxui::Color primary = m_theme->textPrimary.withAlpha(opacity);
    const nxui::Color secondary = m_theme->textSecondary.withAlpha(0.84f * opacity);
    const nxui::Color subtle = m_theme->textSecondary.withAlpha(0.60f * opacity);

    // The left rail is deliberately a "control deck", not an empty tab rail.
    // Its first card is the exact texture the player sees on the home grid;
    // the following actions replace the old intermediary + dialog.
    const nxui::Rect rail = {panel.x + 18.f, panel.y + 18.f, 254.f, panel.height - 36.f};
    ren.drawRoundedRect(rail, m_theme->panelBase.withAlpha(0.10f * opacity), 20.f);
    ren.drawRoundedRectOutline(rail, m_theme->panelBorder.withAlpha(0.16f * opacity), 20.f, 1.f);
    const nxui::Rect cover = {rail.x + 22.f, rail.y + 20.f, rail.width - 44.f, 218.f};
    ren.drawRoundedRect(cover.expanded(4.f), m_theme->panelBase.withAlpha(0.20f * opacity), 16.f);
    ren.drawRoundedRectOutline(cover.expanded(4.f), m_theme->panelBorder.withAlpha(0.26f * opacity), 16.f, 1.f);
    if (m_coverTexture.valid()) {
        ren.drawTextureRounded(&m_coverTexture, fitTexture(cover, m_coverTexture), 14.f,
                               nxui::Color::white().withAlpha(opacity));
    } else if (m_liveCover && m_liveCover->valid()) {
        ren.drawTextureRounded(m_liveCover, fitTexture(cover, *m_liveCover), 14.f,
                               nxui::Color::white().withAlpha(opacity));
    } else {
        ren.drawRoundedRect(cover, m_theme->background.withAlpha(0.42f * opacity), 14.f);
        ren.drawText(i18n.tr("dialog.details_cover_loading", "Loading cover..."),
                     {cover.x + 18.f, cover.y + cover.height * 0.48f}, m_smallFont, subtle, 0.68f);
    }

    const std::vector<std::string> actions = actionLabels();
    constexpr float kActionsTop = 244.f;
    constexpr float kActionRowH = 36.f;
    for (int i = 0; i < (int)actions.size(); ++i) {
        const nxui::Rect action = {rail.x + 18.f, rail.y + kActionsTop + i * kActionRowH, rail.width - 36.f, 32.f};
        const bool selected = m_focusZone == FocusZone::Actions && m_selectedAction == i;
        if (selected) {
            ren.drawRoundedRect(action, m_theme->panelBase.withAlpha(0.20f * opacity), 11.f);
            ren.drawRoundedRectOutline(action, m_theme->cursorNormal.withAlpha(0.76f * opacity), 11.f, 2.f);
            m_focusCursor.moveTo(action, 10.f, 0.08f);
        } else {
            ren.drawRoundedRect(action, m_theme->panelBase.withAlpha(0.07f * opacity), 11.f);
            ren.drawRoundedRectOutline(action, m_theme->panelBorder.withAlpha(0.14f * opacity), 11.f, 1.f);
        }
        ren.drawText(ellipsize(m_smallFont, actions[(size_t)i], action.width - 26.f, 0.68f),
                     {action.x + 13.f, action.y + 9.f}, m_smallFont, selected ? primary : secondary, 0.64f);
    }
    const float railX = rail.x + 22.f;
    const float railW = rail.width - 44.f;
    auto railFact = [&](float y, const std::string& label, const std::string& value) {
        ren.drawText(label, {railX, y}, m_smallFont, subtle, 0.56f);
        ren.drawText(ellipsize(m_smallFont, value.empty() ? "—" : value, railW, 0.66f),
                     {railX, y + 17.f}, m_smallFont, secondary, 0.66f);
    };
    // Keep the facts in the rail's remaining space. A port adds one action
    // ("Port options"), so the three fixed fact rows can crowd the panel;
    // selecting the highest-priority facts that fit prevents both overlap
    // and panel overflow.
    const float actionsBottom = rail.y + kActionsTop + actions.size() * kActionRowH;
    constexpr float kFactGap = 18.f;
    constexpr float kFactRowH = 46.f;
    const float factsAvailable = rail.bottom() - actionsBottom - kFactGap;
    const int factCount = std::clamp((int)std::floor(factsAvailable / kFactRowH), 0, 3);
    const float factsTop = actionsBottom + kFactGap;
    // Play time comes before mods. When space is tight, the row dropped is
    // the play time -- reported as a port whose dossier showed no play time
    // at all while the grid badge showed 37 hours. "Mods: none detected" is
    // the one worth losing of the two.
    if (factCount >= 1)
        railFact(factsTop, i18n.tr("dialog.details_version", "Version"), m_displayVersion);
    if (factCount >= 2)
        railFact(factsTop + kFactRowH, i18n.tr("dialog.details_playtime", "Play time"), m_playTime);
    if (factCount >= 3)
        railFact(factsTop + 2.f * kFactRowH, i18n.tr("dialog.details_mods", "Mods"), m_modSummary);

    const nxui::Rect main = {panel.x + 292.f, panel.y + 10.f, panel.width - 312.f, panel.height - 20.f};
    const std::string title = m_snapshot.title.empty() ? m_title : m_snapshot.title;
    const std::string titleText = ellipsize(m_font, title, main.width - (m_isFavorite ? 270.f : 230.f), 1.08f);
    ren.drawText(titleText, {main.x + 18.f, main.y + 18.f}, m_font, primary, 1.08f);
    if (m_isFavorite) {
        float titleW = m_font ? m_font->measure(titleText).x * 1.08f : 0.f;
        drawStar(ren, {main.x + 24.f + titleW + 12.f, main.y + 32.f}, 9.f,
                 nxui::Color(1.0f, 0.84f, 0.20f, 0.98f * opacity));
    }
    const float scoreX = main.right() - 256.f;
    const bool onlineMatch = m_snapshot.phase == GameMetadataClient::Phase::Ready && m_snapshot.found;
    const std::string publisherNames = join(m_snapshot.publishers);
    const std::string source = onlineMatch
        ? i18n.tr("dialog.details_publishers", "Publishers") + ": "
            + (publisherNames.empty() ? "-" : publisherNames)
        : i18n.tr("dialog.details_local", "Local software details");
    ren.drawText(ellipsize(m_smallFont, source, scoreX - main.x - 28.f, 0.76f),
                  {main.x + 20.f, main.y + 51.f}, m_smallFont, secondary, 0.76f);
    if (!m_metadataPlatform.empty())
        ren.drawText(ellipsize(m_smallFont, "Metadata platform: "
                               + metadataPlatformLabel(m_metadataPlatform),
                               scoreX - main.x - 28.f, 0.68f),
                     {main.x + 20.f, main.y + 75.f}, m_smallFont, subtle, 0.68f);


    auto scoreChip = [&](float x, const std::string& label, const std::string& value, bool enabled) {
        const nxui::Rect chip = {x, main.y + 17.f, 112.f, 50.f};
        ren.drawRoundedRect(chip, m_theme->panelBase.withAlpha((enabled ? 0.18f : 0.07f) * opacity), 12.f);
        ren.drawRoundedRectOutline(chip, m_theme->panelBorder.withAlpha(0.20f * opacity), 12.f, 1.f);
        ren.drawText(label, {chip.x + 10.f, chip.y + 7.f}, m_smallFont, subtle, 0.50f);
        ren.drawText(value, {chip.x + 10.f, chip.y + 24.f}, m_font,
                     enabled ? primary : subtle, 0.70f);
    };
    scoreChip(scoreX, "METASCORE", m_snapshot.metascore >= 0 ? std::to_string(m_snapshot.metascore) : "—",
              m_snapshot.metascore >= 0);
    std::ostringstream userScore;
    if (m_snapshot.userscore >= 0.f) userScore << std::fixed << std::setprecision(1) << m_snapshot.userscore;
    scoreChip(scoreX + 126.f, "USER", m_snapshot.userscore >= 0.f ? userScore.str() : "—",
              m_snapshot.userscore >= 0.f);

    if (m_imageExpanded && !m_snapshot.screenshots.empty()) {
        m_focusCursor.setOpacity(0.f);
        const nxui::Rect image = main.shrunk(26.f);
        const auto& url = m_snapshot.screenshots[(size_t)m_selectedScreenshot];
        if (const auto* texture = imageTexture(url)) {
            ren.drawTextureRounded(texture, fitTexture(image, *texture), 18.f,
                                   nxui::Color::white().withAlpha(opacity));
        } else {
            ren.drawRoundedRect(image, m_theme->panelBase.withAlpha(0.18f * opacity), 18.f);
            ren.drawText(i18n.tr("dialog.details_loading", "Loading game details..."),
                         {image.x + 28.f, image.y + image.height * 0.5f}, m_font, primary, 0.9f);
        }
        return;
    }

    // A large animated focus rectangle is useful for images and actions, but
    // would cross the copy and fact-card boundaries while reading a synopsis.
    m_focusCursor.setOpacity(m_focusZone == FocusZone::Summary ? 0.f : 1.f);
    const nxui::Rect hero = heroRect(main);
    if (m_snapshot.phase == GameMetadataClient::Phase::Ready && !m_snapshot.screenshots.empty()) {
        const auto& url = m_snapshot.screenshots[(size_t)m_selectedScreenshot];
        if (const auto* texture = imageTexture(url)) {
            ren.drawTextureRounded(texture, fitTexture(hero, *texture), 15.f,
                                   nxui::Color::white().withAlpha(opacity));
        } else {
            ren.drawRoundedRect(hero, m_theme->panelBase.withAlpha(0.14f * opacity), 15.f);
        }
        const int total = std::min(5, (int)m_snapshot.screenshots.size());
        for (int i = 0; i < total; ++i) {
            const nxui::Rect thumb = thumbnailRect(main, i, total);
            const bool selected = i == m_selectedScreenshot;
            ren.drawRoundedRect(thumb, m_theme->panelBase.withAlpha((selected ? 0.18f : 0.08f) * opacity), 9.f);
            nxui::Rect imageRect = thumb.shrunk(3.f);
            if (const auto* texture = imageTexture(m_snapshot.screenshots[(size_t)i])) {
                // A thumbnail strip is a visual sequence, not a masonry wall.
                // Fill one consistent 16:9 cell and crop only its edges so
                // every card and the selection cursor stay on the same line.
                ren.drawTextureRoundedSub(texture, coverTextureSource(*texture, imageRect), imageRect, 7.f,
                                          nxui::Color::white().withAlpha(opacity));
            }
            ren.drawRoundedRectOutline(imageRect, (selected ? m_theme->cursorNormal : m_theme->panelBorder)
                                      .withAlpha((selected ? 0.76f : 0.18f) * opacity), 9.f, selected ? 2.f : 1.f);
            if (selected && m_focusZone == FocusZone::Screenshots)
                m_focusCursor.moveTo(imageRect, 8.f, 0.08f);
        }
    } else {
        ren.drawRoundedRect(hero, m_theme->panelBase.withAlpha(0.12f * opacity), 15.f);
        ren.drawText(onlineStatusMessage(), {hero.x + 26.f, hero.y + hero.height * 0.48f},
                     m_font, primary, 0.88f);
    }

    // Compact catalogue facts live in a two-line ribbon between the gameplay
    // strip and the reading area. It keeps the dossier scan-friendly while the
    // synopsis and story can use one controller-scrollable column below.
    const float metadataY = main.y + 341.f;
    const float metadataGap = 14.f;
    const float metadataW = (main.width - 40.f - metadataGap) * 0.5f;
    auto metadata = [&](float x, float y, const std::string& label, const std::string& value) {
        const std::string text = label + ": " + (value.empty() ? "-" : value);
        ren.drawText(ellipsize(m_smallFont, text, metadataW, 0.57f), {x, y},
                     m_smallFont, subtle, 0.57f);
    };
    metadata(main.x + 20.f, metadataY, i18n.tr("dialog.details_release", "Release"),
             m_snapshot.releaseDate);
    metadata(main.x + 20.f + metadataW + metadataGap, metadataY,
             i18n.tr("dialog.details_genres", "Genres"), join(m_snapshot.genres));
    metadata(main.x + 20.f, metadataY + 19.f, i18n.tr("dialog.details_themes", "Themes"),
             join(m_snapshot.themes));
    metadata(main.x + 20.f + metadataW + metadataGap, metadataY + 19.f,
             i18n.tr("dialog.details_game_modes", "Game modes"), join(m_snapshot.gameModes));

    const float textY = main.y + 386.f;
    const float factsY = main.bottom() - 76.f;
    const std::string summary = m_snapshot.summary.empty()
        ? i18n.tr("dialog.details_summary_pending", "A description will appear when the online catalogue has a match.")
        : m_snapshot.summary;
    const std::string storyline = m_snapshot.storyline.empty()
        ? i18n.tr("dialog.details_story_pending", "The catalogue has no story for this game yet.")
        : m_snapshot.storyline;
    // Keep the facts as a fixed visual anchor.  The synopsis owns the remaining
    // space and becomes a discreet, controller-scrollable reading area. The
    // story is a second labelled section in that same reading flow, so neither
    // text is silently truncated to fit a fixed card.
    std::vector<std::pair<std::string, bool>> lines;
    auto appendSection = [&](const std::string& heading, const std::string& body) {
        lines.emplace_back(heading, true);
        for (const auto& line : wrapText(m_smallFont, body, main.width - 40.f, 0.66f, 64))
            lines.emplace_back(line, false);
        lines.emplace_back("", false);
    };
    appendSection(i18n.tr("dialog.details_description", "About this game"), summary);
    appendSection(i18n.tr("dialog.details_story", "Story"), storyline);
    const nxui::Rect summaryClip = {main.x + 18.f, textY + 22.f, main.width - 36.f,
                                    std::max(36.f, factsY - 24.f - (textY + 34.f))};
    const int visibleLines = std::max(1, static_cast<int>((summaryClip.height - 4.f) / 19.f));
    const int maxScrollLine = std::max(0, static_cast<int>(lines.size()) - visibleLines);
    m_summaryScrollLine = std::clamp(m_summaryScrollLine, 0, maxScrollLine);
    if (m_focusZone == FocusZone::Summary) {
        ren.drawRoundedRectOutline(summaryClip.expanded(2.f),
                                   m_theme->cursorNormal.withAlpha(0.56f * opacity),
                                   8.f, 1.f);
        if (maxScrollLine > 0) {
            const float trackHeight = std::max(16.f, summaryClip.height - 12.f);
            const float thumbHeight = std::max(12.f, trackHeight * visibleLines / (float)lines.size());
            const float progress = maxScrollLine > 0 ? m_summaryScrollLine / (float)maxScrollLine : 0.f;
            const nxui::Rect scrollThumb = {summaryClip.right() - 4.f,
                                             summaryClip.y + 6.f + (trackHeight - thumbHeight) * progress,
                                             2.f, thumbHeight};
            ren.drawRoundedRect(scrollThumb, m_theme->cursorNormal.withAlpha(0.76f * opacity), 1.f);
        }
    }
    ren.pushClipRect(summaryClip);
    for (int i = 0; i < visibleLines && m_summaryScrollLine + i < (int)lines.size(); ++i)
        ren.drawText(lines[(size_t)(m_summaryScrollLine + i)].first,
                     {main.x + 20.f, summaryClip.y + 3.f + i * 19.f}, m_smallFont,
                     lines[(size_t)(m_summaryScrollLine + i)].second ? primary : secondary,
                     lines[(size_t)(m_summaryScrollLine + i)].second ? 0.72f : 0.66f);
    ren.popClipRect();

    // The time estimates form one explicit group; without this heading the
    // three hour values read as unrelated statistics.
    ren.drawText(i18n.tr("dialog.details_time_to_beat", "Time to beat"),
                 {main.x + 20.f, factsY - 20.f}, m_smallFont, subtle, 0.56f);

    // The global controller-hint panel occupies the lower-right corner. Leave
    // a quiet margin there so its glass never covers the final fact card.
    const float factsRight = main.right() - 170.f;
    const float factsWidth = std::max(180.f, factsRight - (main.x + 18.f));
    const float factW = (factsWidth - 18.f) / 3.f;
    auto fact = [&](int index, const std::string& label, const std::string& value) {
        const nxui::Rect card = {main.x + 18.f + index * (factW + 9.f), factsY, factW, 60.f};
        ren.drawRoundedRect(card, m_theme->panelBase.withAlpha(0.11f * opacity), 11.f);
        ren.drawRoundedRectOutline(card, m_theme->panelBorder.withAlpha(0.16f * opacity), 11.f, 1.f);
        ren.pushClipRect(card.shrunk(5.f));
        ren.drawText(ellipsize(m_smallFont, label, card.width - 24.f, 0.50f),
                     {card.x + 12.f, card.y + 9.f}, m_smallFont, subtle, 0.50f);
        ren.drawText(ellipsize(m_smallFont, value.empty() ? "—" : value, card.width - 24.f, 0.69f),
                     {card.x + 12.f, card.y + 29.f}, m_smallFont, primary, 0.69f);
        ren.popClipRect();
    };
    fact(0, i18n.tr("dialog.details_hastily", "Hastily"), hours(m_snapshot.hoursHastily));
    fact(1, i18n.tr("dialog.details_time_main", "Main"), hours(m_snapshot.hoursMain));
    fact(2, i18n.tr("dialog.details_completely", "Complete"), hours(m_snapshot.hoursCompletionist));
}
