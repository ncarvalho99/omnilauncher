#include "IconGrid.hpp"
#include "GlossyIcon.hpp"
#include "GridNavigation.hpp"
#include "core/DebugLog.hpp"
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Animation.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sys/stat.h>
#include <cstdio>
#include <string>
#include <mutex>
#include <unordered_map>


IconGrid::IconGrid() {
    m_focus.onFocusChanged([this](nxui::Widget* /*old*/, nxui::Widget* cur) {
        if (m_onFocusChanged && cur) {
            m_onFocusChanged(cur);
        }
    });
}

namespace {

static std::unordered_map<std::uint64_t, std::string> s_coverPathCache;
static std::mutex s_coverPathMutex;

std::string resolveFlowCoverPath(std::uint64_t titleId) {
    if (titleId == 0) return {};
    {
        std::lock_guard<std::mutex> lock(s_coverPathMutex);
        auto it = s_coverPathCache.find(titleId);
        if (it != s_coverPathCache.end()) return it->second;
    }

    char hexUpper[17];
    char hexLower[17];
    std::snprintf(hexUpper, sizeof(hexUpper), "%016llX", static_cast<unsigned long long>(titleId));
    std::snprintf(hexLower, sizeof(hexLower), "%016llx", static_cast<unsigned long long>(titleId));

    const char* exts[] = {".jpg", ".png", ".jpeg"};
    struct stat st;
    std::string found;

    for (const char* ext : exts) {
        // 1. SwitchU covers root (SteamGridDB 600x900 covers or manual drops)
        std::string p1 = std::string("sdmc:/config/SwitchU/covers/") + hexUpper + ext;
        if (stat(p1.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { found = std::move(p1); break; }
        p1 = std::string("sdmc:/config/SwitchU/covers/") + hexLower + ext;
        if (stat(p1.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { found = std::move(p1); break; }

        // 2. SwitchU game_art cover slot (custom covers applied from Dossier/Gallery)
        std::string p2 = std::string("sdmc:/config/SwitchU/game_art/") + hexUpper + "/cover" + ext;
        if (stat(p2.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { found = std::move(p2); break; }
        p2 = std::string("sdmc:/config/SwitchU/game_art/") + hexLower + "/cover" + ext;
        if (stat(p2.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { found = std::move(p2); break; }

        // 3. sLaunch covers root (shared SD cover art)
        std::string p3 = std::string("sdmc:/slaunch/covers/") + hexUpper + ext;
        if (stat(p3.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { found = std::move(p3); break; }
        p3 = std::string("sdmc:/slaunch/covers/") + hexLower + ext;
        if (stat(p3.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { found = std::move(p3); break; }
    }

    {
        std::lock_guard<std::mutex> lock(s_coverPathMutex);
        s_coverPathCache[titleId] = found;
    }
    return found;
}

float clamp01(float v) {
    return std::clamp(v, 0.f, 1.f);
}

// ---- Flow tuning -----------------------------------------------------------
//
// World units where a case is one unit tall; nxui's focal length turns those
// into pixels. A Switch case is roughly 2:3, so it is taller than it is wide -
// a square extent is what makes a coverflow row read as rotated icons instead
// of boxes on a shelf.
constexpr float kFlowHalfH   = 0.50f;   // half-height
constexpr float kFlowHalfW   = 0.34f;   // half-width (2:3 of the height)
constexpr float kFlowDepth   = 0.030f;  // half-thickness of the case
constexpr float kFlowY       = 0.12f;   // row lifted a little off centre

// Spacing must clear the case's *on-screen* width, which depends on the swing:
// at 34 degrees a case covers 2*halfW*cos(34) = 0.561 world units. Lowering the
// angle without raising the spacing is what makes covers touch.
constexpr float kFlowSpacing  = 0.70f;  // centre-to-centre along the row
constexpr float kFlowSideStep = 0.14f;  // extra shove away from centre
constexpr float kFlowZBase    = 2.35f;  // centre case distance
constexpr float kFlowZBack    = 0.38f;  // how far the sides recede
constexpr float kFlowAngle    = 0.60f;  // max swing, radians (~34 deg)

// Past the first neighbour each case recedes further and turns a little more
// edge-on. Depth alone cannot express this: it shrinks on-screen spacing faster
// than it shrinks the cases, so any recession strong enough to see closes the
// gaps. The extra turn narrows them, buying back the spacing recession costs.
constexpr float kFlowZStep = 0.30f;
constexpr float kFlowAStep = 0.10f;

constexpr int   kFlowVisible = 6;       // cases drawn either side of centre
constexpr float kFlowRunSpin = 0.62f;   // rad/s idle turn for the running title

// Subdivision per surface. Flat artwork full of straight edges shows the
// affine sawtooth worst, so the printed front gets the most.
constexpr int kFlowStripsSide  = 4;
constexpr int kFlowStripsFront = 12;
constexpr int kFlowStripsRefl  = 4;

// Reflection gradient. Mirrored about each case's own bottom edge.
constexpr float kFlowReflTop = 0.27f;   // ~70/255

int flowWrap(int i, int n) {
    if (n <= 0) return 0;
    i %= n;
    return (i < 0) ? i + n : i;
}

float flowMidZ(const nxui::Vec3 q[4]) {
    return 0.25f * (q[0].z + q[1].z + q[2].z + q[3].z);
}

} // namespace

void IconGrid::flowPlace(float p, float& x, float& z, float& angle) {
    const float t = std::clamp(p, -1.f, 1.f);
    angle = -t * kFlowAngle;
    x     = p * kFlowSpacing + t * kFlowSideStep;

    // The swing and the initial fall-back saturate one item out; depth keeps
    // creeping beyond that, so the wall recedes instead of sitting flat.
    const float beyond = std::max(0.f, std::abs(p) - 1.f);
    z = kFlowZBase + std::abs(t) * kFlowZBack + beyond * kFlowZStep;
    if (beyond > 0.f)
        angle += (t < 0.f ? 1.f : -1.f) * beyond * kFlowAStep;
}

void IconGrid::flowFace(float x, float z, float angle,
                        float lx0, float lz0, float lx1, float lz1,
                        float halfH, nxui::Vec3 out[4]) {
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);
    auto put = [&](int i, float lx, float lz, float ly) {
        out[i] = {
            x + lx * ca + lz * sa,
            kFlowY + ly,
            z - lx * sa + lz * ca
        };
    };
    put(0, lx0, lz0,  halfH);   // TL
    put(1, lx1, lz1,  halfH);   // TR
    put(2, lx1, lz1, -halfH);   // BR
    put(3, lx0, lz0, -halfH);   // BL
}

void IconGrid::flowCorners(float x, float z, float angle,
                           float halfW, float halfH, nxui::Vec3 out[4]) {
    flowFace(x, z, angle, -halfW, 0.f, halfW, 0.f, halfH, out);
}

void IconGrid::clearFlowCovers() {
    for (auto& pcd : m_pendingCoverDecodes) {
        try {
            if (pcd.future.valid()) pcd.future.wait();
        } catch (...) {}
    }
    m_flowCovers.clear();
    m_pendingCoverDecodes.clear();
}

void IconGrid::preloadFlowCoversAround(int centerIdx) {
    if (!is3D() || m_allIcons.empty() || !m_threadPool)
        return;
    const int count = static_cast<int>(m_allIcons.size());
    for (int offset = -3; offset <= 3; ++offset) {
        int idx = ((centerIdx + offset) % count + count) % count;
        const auto& icon = m_allIcons[static_cast<size_t>(idx)];
        if (!icon) continue;
        const std::uint64_t tid = icon->titleId();
        if (tid == 0) continue;
        auto& cov = m_flowCovers[tid];
        if (!cov.checked) {
            cov.checked = true;
            cov.loading = true;
            auto decodeState = std::make_shared<CoverDecodeState>();
            auto work = [decodeState, tid]() {
                try {
                    const std::string coverPath = resolveFlowCoverPath(tid);
                    if (!coverPath.empty()) {
                        decodeState->decoded = nxui::Texture::decodeFile(coverPath, 720);
                        decodeState->failed = !decodeState->decoded.valid();
                    } else {
                        decodeState->failed = true;
                    }
                } catch (...) {
                    decodeState->failed = true;
                }
                decodeState->done = true;
            };
            PendingCoverDecode pcd;
            pcd.titleId = tid;
            pcd.state = decodeState;
            pcd.future = m_threadPool->submit(std::move(work));
            m_pendingCoverDecodes.push_back(std::move(pcd));
        }
    }
}

void IconGrid::setup(std::vector<std::shared_ptr<GlossyIcon>> icons,
                     int cols, int rows,
                     float cellW, float cellH,
                     float padX, float padY)
{
    m_allIcons = std::move(icons);
    reconfigureLayout(cols, rows, cellW, cellH, padX, padY);
    if (is3D()) {
        int cur = focusedGlobalIndex();
        preloadFlowCoversAround(cur >= 0 ? cur : 0);
    }
}

void IconGrid::setLayoutMode(AppLayoutMode mode) {
    if (m_layoutMode == mode) return;
    const AppLayoutMode previous = m_layoutMode;
    m_layoutMode = mode;
    // XMB takes the icons of every non-selected category out of the focus tree
    // (see syncXmbChildRects). Those are the same shared GlossyIcon objects the
    // other views use, so leaving XMB without putting them back would carry the
    // non-focusable state into Grid or Flow and make most of the library
    // unreachable there. Restoring is cheap and only runs on the way out.
    if (previous == AppLayoutMode::Xmb) {
        for (auto& icon : m_allIcons) {
            if (!icon) continue;
            // addDirectionAction stores one callback per d-pad/stick button.
            // Repeated XMB layouts overwrite those map entries (they do not
            // accumulate), but leaving them installed would make another view
            // consume every direction press and call stepXmb(), which now
            // returns immediately because the mode is no longer XMB -- a new
            // navigation freeze. Remove precisely the twelve actions XMB owns;
            // normal layouts navigate through FocusManager/customNavigation.
            for (nxui::Button button : {
                     nxui::Button::DLeft, nxui::Button::DRight,
                     nxui::Button::DUp, nxui::Button::DDown,
                     nxui::Button::LStickL, nxui::Button::LStickR,
                     nxui::Button::LStickU, nxui::Button::LStickD,
                     nxui::Button::RStickL, nxui::Button::RStickR,
                     nxui::Button::RStickU, nxui::Button::RStickD}) {
                icon->removeAction(static_cast<uint64_t>(button));
            }
            switch (icon->entryKind()) {
                case GridEntryKind::WidgetContinuation:
                    // The hidden second half of a 2x1 widget is never focusable.
                    icon->setFocusable(false);
                    break;
                case GridEntryKind::Empty:
                    // Padding at the end of the model. The carousel compacts
                    // real entries to the front and must not stop on these;
                    // the paged grid uses them as placeable slots.
                    icon->setFocusable(!isCarousel());
                    break;
                default:
                    icon->setFocusable(true);
                    break;
            }
        }
    }
    int cur = focusedGlobalIndex();
    m_layoutReveal.setImmediate(0.86f);
    m_layoutReveal.set(1.f, 0.24f, nxui::Easing::outCubic);
    if (m_layoutMode == AppLayoutMode::Xmb) {
        layoutXmb();
        if (cur >= 0 && cur < (int)m_allIcons.size() && m_xmbCols.size() > 4) {
            auto& gameCol = m_xmbCols[4];
            for (size_t i = 0; i < gameCol.size(); ++i) {
                if (gameCol[i] == m_allIcons[cur].get()) {
                    m_xmbCol = 4;
                    m_xmbItem = static_cast<int>(i);
                    m_xmbGamesRow = m_xmbItem;
                    m_xmbColScroll = 4.0f;
                    m_xmbItemScroll = static_cast<float>(i);
                    // The column and row just changed, so the rects layoutXmb()
                    // placed are for the old selection. Re-place before focus
                    // so the ring lands on the right tile on the first frame.
                    syncXmbChildRects();
                    m_focus.setFocus(gameCol[i]);
                    break;
                }
            }
        }
        return;
    }
    // Flow/Shelf share the carousel's model, focus bindings and scroll offset:
    // they are different presentations of the same row, so switching between
    // views keeps your place on the same title without re-deriving anything.
    if (isCarousel()) {
        m_lineScrollOffset.setImmediate(cur >= 0 ? static_cast<float>(cur) : 0.f);
        layoutLine();
        if (is3D()) {
            preloadFlowCoversAround(cur >= 0 ? cur : 0);
        }
    } else {
        setPage(cur >= 0 ? cur / std::max(1, iconsPerPage()) : m_page);
        layoutPage();
    }
    if (cur >= 0 && cur < (int)m_allIcons.size() && m_allIcons[cur]->isFocusable()) {
        m_focus.setFocus(m_allIcons[cur].get());
    }
}

bool IconGrid::isDynamicLineScrolling() const {
    return isCarousel()
        && std::abs(m_lineScrollOffset.value() - m_lineScrollOffset.target()) > 0.01f;
}

void IconGrid::setDynamicLineUpTarget(nxui::Widget* target) {
    m_lineUpTarget = target;
    if (!isCarousel())
        return;
    for (auto& icon : m_allIcons) {
        if (icon)
            icon->setCustomNavigation(nxui::FocusDirection::UP, m_lineUpTarget);
    }
}

void IconGrid::setDynamicLineDownTarget(nxui::Widget* target) {
    m_lineDownTarget = target;
    if (!isCarousel())
        return;
    for (auto& icon : m_allIcons) {
        if (icon)
            icon->setCustomNavigation(nxui::FocusDirection::DOWN, m_lineDownTarget);
    }
}

void IconGrid::reconfigureLayout(int cols, int rows,
                                 float cellW, float cellH,
                                 float padX, float padY)
{
    m_cols  = cols;  m_rows = rows;
    m_cellW = cellW; m_cellH = cellH;
    m_padX  = padX;  m_padY  = padY;

    int perPage = iconsPerPage();
    m_totalPages = std::max(1, ((int)m_allIcons.size() + perPage - 1) / perPage);

    float gridW = m_cols * m_cellW + (m_cols - 1) * m_padX;
    float gridH = m_rows * m_cellH + (m_rows - 1) * m_padY;
    m_originX = (m_rect.width  - gridW) * 0.5f + m_rect.x;
    m_originY = (m_rect.height - gridH) * 0.5f + m_rect.y;

    if (m_layoutMode == AppLayoutMode::Xmb)
        layoutXmb();
    else if (isCarousel())
        layoutLine();
    else
        setPage(m_page);
}

void IconGrid::setPage(int page) {
    if (isCarousel() || m_layoutMode == AppLayoutMode::Xmb) return;
    m_page = std::clamp(page, 0, m_totalPages - 1);
    layoutPage();
}

void IconGrid::layoutPage() {
    nxui::Widget* prevFocused = m_focus.current();

    clearChildren();
    int start = m_page * iconsPerPage();
    int end   = std::min(start + iconsPerPage(), (int)m_allIcons.size());

    std::vector<nxui::Widget*> fItems;

    for (int i = start; i < end; ++i) {
        auto& icon = m_allIcons[i];
        icon->setCustomNavigation(nxui::FocusDirection::LEFT, nullptr);
        icon->setCustomNavigation(nxui::FocusDirection::RIGHT, nullptr);
        icon->setCustomNavigation(nxui::FocusDirection::UP, nullptr);
        icon->setCustomNavigation(nxui::FocusDirection::DOWN, m_lineDownTarget);
        int local  = i - start;
        int col    = local % m_cols;
        int row    = local / m_cols;
        float x = m_originX + col * (m_cellW + m_padX);
        float y = m_originY + row * (m_cellH + m_padY);
        const int spanColumns = std::max(1, icon->gridSpanColumns());
        const int spanRows = std::max(1, icon->gridSpanRows());
        icon->setRect({x, y,
                       m_cellW * spanColumns + m_padX * (spanColumns - 1),
                       m_cellH * spanRows + m_padY * (spanRows - 1)});
        // A 2x1 widget renders its content inside one cell on hardware while the
        // move ghost, which bypasses this path, renders it correctly. The rect
        // computed here is the one the tile is told to draw into, so logging it
        // says whether the span is lost before or after this point.
        if (icon->entryKind() == GridEntryKind::Widget)
            DebugLog::log("[widget-rect] span=%dx%d rect=%.0f,%.0f %.0fx%.0f cell=%.0fx%.0f",
                          spanColumns, spanRows, icon->rect().x, icon->rect().y,
                          icon->rect().width, icon->rect().height, m_cellW, m_cellH);
        addChild(icon);
        if (icon->isFocusable())
            fItems.push_back(icon.get());
    }

    bindGridNavigation(start, end);
    bindEdgeActions(start, end);

    m_focus.setGrid(fItems, m_cols);
    if (prevFocused) {
        for (auto* item : fItems) {
            if (item == prevFocused) {
                m_focus.setFocus(prevFocused);
                break;
            }
        }
    }
}

void IconGrid::bindGridNavigation(int start, int end) {
    std::vector<GridNavigationItem> items;
    items.reserve(static_cast<std::size_t>(std::max(0, end - start)));
    for (int index = start; index < end; ++index) {
        const auto& icon = m_allIcons[static_cast<std::size_t>(index)];
        if (!icon || !icon->isFocusable() || !icon->isVisible()) continue;
        const int local = index - start;
        items.push_back({index, local % m_cols, local / m_cols,
                         std::max(1, icon->gridSpanColumns()),
                         std::max(1, icon->gridSpanRows())});
    }

    const auto nearestSideTarget = [](const GlossyIcon& source,
                                      const std::vector<nxui::Widget*>& targets) {
        nxui::Widget* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        const float sourceY = source.focusRect().y + source.focusRect().height * 0.5f;
        for (auto* target : targets) {
            if (!target || !target->isVisible() || !target->isFocusable()) continue;
            const auto rect = target->focusRect();
            const float distance = std::abs(rect.y + rect.height * 0.5f - sourceY);
            if (distance < bestDistance) {
                best = target;
                bestDistance = distance;
            }
        }
        return best;
    };
    const auto bind = [&](GlossyIcon& source, const GridNavigationItem& item,
                          nxui::FocusDirection focusDirection,
                          GridNavigationDirection gridDirection) {
        const int target = findGridNavigationTarget(items, item.index, gridDirection);
        nxui::Widget* destination = target >= 0
            ? m_allIcons[static_cast<std::size_t>(target)].get() : nullptr;
        if (!destination && gridDirection == GridNavigationDirection::Left &&
            item.column == 0)
            destination = nearestSideTarget(source, m_gridLeftTargets);
        if (!destination && gridDirection == GridNavigationDirection::Right &&
            item.column + std::max(1, item.columns) >= m_cols)
            destination = nearestSideTarget(source, m_gridRightTargets);
        if (!destination && gridDirection == GridNavigationDirection::Up &&
            item.row == 0 && m_gridUpTarget && m_gridUpTarget->isVisible() &&
            m_gridUpTarget->isFocusable())
            destination = m_gridUpTarget;
        source.setCustomNavigation(focusDirection,
                                   destination);
    };
    for (const auto& item : items) {
        auto& source = *m_allIcons[static_cast<std::size_t>(item.index)];
        bind(source, item, nxui::FocusDirection::LEFT,
             GridNavigationDirection::Left);
        bind(source, item, nxui::FocusDirection::RIGHT,
             GridNavigationDirection::Right);
        bind(source, item, nxui::FocusDirection::UP,
             GridNavigationDirection::Up);
        bind(source, item, nxui::FocusDirection::DOWN,
             GridNavigationDirection::Down);
    }
}

void IconGrid::setGridUpTarget(nxui::Widget* target) {
    if (m_gridUpTarget == target)
        return;
    m_gridUpTarget = target;
    if (m_layoutMode == AppLayoutMode::Grid)
        layoutPage();
}

void IconGrid::setGridSideTargets(std::vector<nxui::Widget*> left,
                                  std::vector<nxui::Widget*> right) {
    m_gridLeftTargets = std::move(left);
    m_gridRightTargets = std::move(right);
    if (m_layoutMode == AppLayoutMode::Grid)
        layoutPage();
}

nxui::Rect IconGrid::gridSpanRect(int globalIndex, int columns, int rows) const {
    if (globalIndex < 0 || globalIndex >= static_cast<int>(m_allIcons.size()))
        return {};
    // The single-row carousel has its own fixed metrics and animation. Edit
    // ghosts/cursors must follow that displayed rect instead of reconstructing
    // a cell from the configurable grid dimensions.
    if (isCarousel())
        return dynamicIconRect(globalIndex);
    const int local = globalIndex % std::max(1, iconsPerPage());
    const int column = local % std::max(1, m_cols);
    const int row = local / std::max(1, m_cols);
    const int spanColumns = std::max(1, columns);
    const int spanRows = std::max(1, rows);
    return {m_originX + column * (m_cellW + m_padX),
            m_originY + row * (m_cellH + m_padY),
            m_cellW * spanColumns + m_padX * (spanColumns - 1),
            m_cellH * spanRows + m_padY * (spanRows - 1)};
}

void IconGrid::layoutLine() {
    nxui::Widget* prevFocused = m_focus.current();
    clearChildren();

    std::vector<nxui::Widget*> fItems;
    fItems.reserve(m_allIcons.size());
    const int lineCount = static_cast<int>(m_allIcons.size());
    // The line is a carousel: walking off either end comes back around, so the
    // last installed title leads to the first. Empty padding is not focusable in
    // this mode, so the cycle only ever visits real entries. Returns -1 when the
    // line holds at most one of them, which leaves the binding null rather than
    // pointing an icon at itself.
    const auto wrapFocusable = [this, lineCount](int from, int step) {
        for (int offset = 1; offset <= lineCount; ++offset) {
            const int candidate = ((from + step * offset) % lineCount + lineCount)
                                % lineCount;
            if (candidate == from) break;
            if (m_allIcons[(size_t)candidate] &&
                m_allIcons[(size_t)candidate]->isFocusable())
                return candidate;
        }
        return -1;
    };
    for (size_t i = 0; i < m_allIcons.size(); ++i) {
        auto& icon = m_allIcons[i];
        const int left = wrapFocusable((int)i, -1);
        const int right = wrapFocusable((int)i, +1);
        icon->setCustomNavigation(nxui::FocusDirection::LEFT,
                                  left >= 0 ? m_allIcons[(size_t)left].get() : nullptr);
        icon->setCustomNavigation(nxui::FocusDirection::RIGHT,
                                  right >= 0 ? m_allIcons[(size_t)right].get() : nullptr);
        icon->setCustomNavigation(nxui::FocusDirection::UP, m_lineUpTarget);
        icon->setCustomNavigation(nxui::FocusDirection::DOWN, nullptr);
        addChild(icon);
        if (icon->isFocusable())
            fItems.push_back(icon.get());
    }

    m_focus.setGrid(fItems, std::max(1, (int)fItems.size()));
    // Diagnostic for the line coming up with a single visible tile. The model
    // and the streamer indices check out on paper, so what is needed is the
    // count the grid actually built, how many of them can hold focus, and where
    // the carousel offset sits against the focused index.
    // Re-anchor the carousel whenever the ring changes size. The offset is an
    // index into the ring and nothing else: after a rebuild it was left holding
    // the index it had in the previous model, so the switch back from the grid
    // -- 120 entries -- left it at 29 in a ring of 16 with focus on 15. The
    // focused tile then sat two steps off centre and everything else fell below
    // the alpha the carousel fades neighbours out with, which is the single
    // visible tile with nothing either side. Snapped, not animated: there is
    // nothing to travel between when the row it was travelling through is gone.
    const int ringCount = static_cast<int>(m_allIcons.size());
    const int focusedNow = focusedGlobalIndex();
    if (ringCount != m_lineRingCount) {
        m_lineRingCount = ringCount;
        m_lineScrollOffset.setImmediate(focusedNow >= 0
                                            ? static_cast<float>(focusedNow) : 0.f);
    }
    DebugLog::log("[line] icons=%d focusable=%d focused=%d offset=%.2f target=%.2f",
                  ringCount, (int)fItems.size(), focusedNow,
                  m_lineScrollOffset.value(), m_lineScrollOffset.target());

    if (prevFocused) {
        for (auto* item : fItems) {
            if (item == prevFocused) {
                m_focus.setFocus(prevFocused);
                break;
            }
        }
    }
}

void IconGrid::bindEdgeActions(int start, int end) {
    if (m_cols <= 0)
        return;
    for (int i = start; i < end; ++i) {
        nxui::Widget* w = m_allIcons[i].get();
        if (!w) continue;
        const int col = (i - start) % m_cols;
        if (col == m_cols - 1) {
            auto onRight = [this]() -> bool {
                if (m_onEdgePageHold && m_onEdgePageHold(+1))
                    return true;
                if (m_edgePaging && m_onEdgePage) {
                    m_onEdgePage(+1);
                    return true;
                }
                return false;
            };
            w->addPredicateAction(static_cast<uint64_t>(nxui::Button::DRight), onRight);
            w->addPredicateAction(static_cast<uint64_t>(nxui::Button::LStickR), onRight);
            w->addPredicateAction(static_cast<uint64_t>(nxui::Button::RStickR), onRight);
        }
        if (col == 0) {
            auto onLeft = [this]() -> bool {
                if (m_onEdgePageHold && m_onEdgePageHold(-1))
                    return true;
                if (m_edgePaging && m_onEdgePage) {
                    m_onEdgePage(-1);
                    return true;
                }
                return false;
            };
            w->addPredicateAction(static_cast<uint64_t>(nxui::Button::DLeft), onLeft);
            w->addPredicateAction(static_cast<uint64_t>(nxui::Button::LStickL), onLeft);
            w->addPredicateAction(static_cast<uint64_t>(nxui::Button::RStickL), onLeft);
        }
    }
}

void IconGrid::positionPage(int page, float dx) {
    const int start = page * iconsPerPage();
    const int end   = std::min(start + iconsPerPage(), (int)m_allIcons.size());
    for (int i = start; i < end; ++i) {
        const int local = i - start;
        auto& icon = m_allIcons[i];
        const int spanColumns = std::max(1, icon->gridSpanColumns());
        const int spanRows = std::max(1, icon->gridSpanRows());
        icon->setRect({m_originX + (local % m_cols) * (m_cellW + m_padX) + dx,
                       m_originY + (local / m_cols) * (m_cellH + m_padY),
                       m_cellW * spanColumns + m_padX * (spanColumns - 1),
                       m_cellH * spanRows + m_padY * (spanRows - 1)});
    }
}

float IconGrid::pageStride() const {
    const float gridW = m_cols * m_cellW + (m_cols - 1) * m_padX;
    return std::max(m_rect.width, (m_originX - m_rect.x) + gridW + m_padX);
}

int IconGrid::focusedGlobalIndex() const {
    auto* cur = m_focus.current();
    if (!cur)
        return -1;
    for (int i = 0; i < (int)m_allIcons.size(); ++i) {
        if (m_allIcons[i].get() == cur)
            return i;
    }
    return -1;
}

int IconGrid::xmbStreamCenterIndex() const {
    if (m_layoutMode != AppLayoutMode::Xmb || m_xmbCols.size() <= 4)
        return -1;
    const auto& games = m_xmbCols[4];
    if (games.empty())
        return -1;
    const int gamesRow = std::clamp(m_xmbGamesRow, 0, static_cast<int>(games.size()) - 1);
    // While a system category is selected the focused widget is not an app at
    // all, so focusedGlobalIndex() reports -1 and the streamer would be told to
    // centre on index 0 -- evicting every game icon the player had just been
    // looking at. The games column keeps its own row, so the window stays put
    // over the apps while the player visits Settings or Network and is still
    // correct when they come back.
    // Both indices are clamped here rather than trusted: the games column is
    // rebuilt from m_allIcons whenever the model changes (uninstall, folder
    // open, sort, filter) and can shrink under a remembered row.
    const int row = std::clamp(m_xmbItem, 0, static_cast<int>(games.size()) - 1);
    GlossyIcon* target = (m_xmbCol == 4) ? games[row] : games[gamesRow];
    if (!target)
        return -1;
    for (int i = 0; i < (int)m_allIcons.size(); ++i) {
        if (m_allIcons[i].get() == target)
            return i;
    }
    return -1;
}

// Shortest signed distance from `from` to `to` around the line, which is a
// ring. Everything the carousel measures goes through here so that the item
// after the last one is the first, both for placement and for the scroll
// target: stepping off the end then continues in the direction pressed instead
// of rewinding to the other side.
float IconGrid::lineRingDelta(float from, float to) const {
    float delta = to - from;
    const int count = static_cast<int>(m_allIcons.size());
    if (count > 0) {
        const float span = static_cast<float>(count);
        const float half = span * 0.5f;
        while (delta > half) delta -= span;
        while (delta < -half) delta += span;
    }
    return delta;
}

nxui::Rect IconGrid::dynamicIconRect(int index, float* outScale,
                                     float* outOpacity,
                                     float* outDistance) const {
    const float centerX = m_rect.x + m_rect.width * 0.5f;
    // Leave a dedicated control strip below the profiles, then place the app
    // carousel in the lower half of the HOME scene.
    const float centerY = m_rect.y + m_rect.height * 0.66f;
    const float offset = m_lineScrollOffset.value();
    // Carousel sizing is intentionally independent from the configurable
    // grid rows/columns. Changing the grid density must not resize single row.
    constexpr float baseCellW = 150.f;
    constexpr float baseCellH = 150.f;
    // The old extra 36 px made neighbouring apps feel disconnected. A small,
    // stable gutter keeps the row compact even when grid padding is reconfigured.
    const float lineSpacing = baseCellW + std::max(8.f, m_padX * 0.4f);

    float d = lineRingDelta(offset, static_cast<float>(index));
    const float absD = std::abs(d);
    float s = 1.f;
    float a = 1.f;
    if (absD <= 1.0f) {
        // Position drives the visual state: the departing icon now shrinks as
        // it leaves centre while the incoming icon travels and grows into it.
        const float centerBlend = 1.f - absD;
        const float smoothBlend = centerBlend * centerBlend * (3.f - 2.f * centerBlend);
        s = 0.82f + smoothBlend * (1.36f - 0.82f);
        a = 0.76f + smoothBlend * 0.24f;
    } else {
        s = std::max(0.54f, 0.82f - (absD - 1.0f) * 0.12f);
        a = std::max(0.0f, 0.76f - (absD - 1.0f) * 0.24f);
    }

    const float reveal = clamp01(m_layoutReveal.value());
    const float revealScale = 0.94f + reveal * 0.06f;
    s *= revealScale;
    a *= reveal;

    const float liftT = std::min(absD, 1.f);
    const float smoothLift = liftT * liftT * (3.f - 2.f * liftT);
    const float sideLift = 32.f * smoothLift;
    const float w = baseCellW * s;
    const float h = baseCellH * s;
    const float x = centerX + d * lineSpacing - w * 0.5f;
    const float y = centerY - h * 0.5f - sideLift;

    if (outScale) *outScale = s;
    if (outOpacity) *outOpacity = a;
    if (outDistance) *outDistance = absD;
    return {x, y, w, h};
}

nxui::Rect IconGrid::focusedDisplayRect() const {
    if (m_layoutMode == AppLayoutMode::Xmb) {
        // Exactly the rect renderXmb draws the focused tile into, so the
        // selection ring tracks the tile through its zoom instead of framing a
        // fixed box that no longer matches it.
        return xmbItemRect(m_xmbItem);
    }
    if (isCarousel()) {
        const int focused = focusedGlobalIndex();
        if (focused >= 0) {
            if (is3D())
                return projected3DIconRect(focused);
            return dynamicIconRect(focused);
        }
    }
    if (auto* cur = m_focus.current())
        return cur->focusRect();
    return {};
}

bool IconGrid::focusGlobalIndex(int idx) {
    if (idx < 0 || idx >= (int)m_allIcons.size())
        return false;
    if (!m_allIcons[idx] || !m_allIcons[idx]->isFocusable())
        return false;

    if (isCarousel()) {
        m_focus.setFocus(m_allIcons[idx].get());
        if (is3D()) {
            preloadFlowCoversAround(idx);
        }
        // Target the congruent value nearest the current offset, so a wrap moves
        // one step rather than scrolling the length of the line. The offset is
        // allowed outside [0, count) for this; every reader goes through
        // lineRingDelta().
        const float from = m_lineScrollOffset.value();
        m_lineScrollOffset.set(from + lineRingDelta(from, static_cast<float>(idx)),
                               kLineScrollDuration, nxui::Easing::outCubic);
        return true;
    }

    int perPage = iconsPerPage();
    if (perPage <= 0)
        return false;

    int wantedPage = idx / perPage;
    if (wantedPage != m_page)
        setPage(wantedPage);

    m_focus.setFocus(m_allIcons[idx].get());
    return true;
}

bool IconGrid::swapSlots(int a, int b) {
    if (a < 0 || b < 0 || a >= (int)m_allIcons.size() || b >= (int)m_allIcons.size())
        return false;
    if (a == b)
        return true;

    std::swap(m_allIcons[a], m_allIcons[b]);
    if (isCarousel())
        layoutLine();
    else
        layoutPage();
    return true;
}

std::vector<GlossyIcon*> IconGrid::pageIcons() const {
    std::vector<GlossyIcon*> out;
    if (m_layoutMode == AppLayoutMode::Xmb) {
        // "The icons on screen" for the bar is the neighbourhood of the
        // selection in the games column, matching the window the streamer is
        // asked to keep resident. The paged branch would have returned the
        // first page's worth regardless of where the selection actually is.
        if (m_xmbCols.size() > 4) {
            const auto& col = m_xmbCols[4];
            const int center = std::clamp(m_xmbCol == 4 ? m_xmbItem : 0,
                                          0, std::max(0, (int)col.size() - 1));
            const int start = std::max(0, center - 4);
            const int end = std::min((int)col.size(), center + 5);
            for (int i = start; i < end; ++i)
                if (col[i]) out.push_back(col[i]);
        }
        return out;
    }
    if (isCarousel()) {
        int cur = focusedGlobalIndex();
        int center = cur >= 0 ? cur : 0;
        int start = std::max(0, center - 4);
        int end = std::min((int)m_allIcons.size(), center + 5);
        for (int i = start; i < end; ++i)
            out.push_back(m_allIcons[i].get());
        return out;
    }
    int start = m_page * iconsPerPage();
    int end   = std::min(start + iconsPerPage(), (int)m_allIcons.size());
    for (int i = start; i < end; ++i) out.push_back(m_allIcons[i].get());
    return out;
}

bool IconGrid::projectPoint3D(const nxui::Vec3& p, float screenW, float screenH, nxui::Vec2& out) {
    constexpr float kFocal = 900.f;
    constexpr float kNearZ = 0.05f;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.z < kNearZ)
        return false;
    out.x = screenW * 0.5f + (p.x * kFocal) / p.z;
    out.y = screenH * 0.5f - (p.y * kFocal) / p.z;
    return std::isfinite(out.x) && std::isfinite(out.y);
}

bool IconGrid::pointInQuad(float px, float py, const nxui::Vec2 pts[4]) {
    bool hasPos = false;
    bool hasNeg = false;
    for (int i = 0; i < 4; ++i) {
        const nxui::Vec2& a = pts[i];
        const nxui::Vec2& b = pts[(i + 1) % 4];
        float cross = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
        if (cross > 0.001f) hasPos = true;
        else if (cross < -0.001f) hasNeg = true;
        if (hasPos && hasNeg)
            return false;
    }
    return true;
}

int IconGrid::hitTestFlow(float screenX, float screenY) const {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return -1;
    const float scroll = m_lineScrollOffset.value();
    const int centre = (int)std::round(scroll);

    struct HitCand {
        int index;
        float p;
        float z;
    };
    std::vector<HitCand> order;
    order.reserve(16);
    for (int i = centre - 6; i <= centre + 6; ++i) {
        float p = (float)i - scroll;
        float fx, fz, fang;
        flowPlace(p, fx, fz, fang);
        order.push_back({flowWrap(i, n), p, fz});
    }

    // Near to far (smallest z first)
    std::sort(order.begin(), order.end(), [](const HitCand& a, const HitCand& b) {
        return a.z < b.z;
    });

    const float halfW = kFlowHalfW;
    const float halfH = kFlowHalfH;
    const float depth = kFlowDepth;

    for (const auto& c : order) {
        float fx, fz, fang;
        flowPlace(c.p, fx, fz, fang);
        if (c.index >= 0 && c.index < (int)m_allIcons.size() && m_allIcons[c.index]->isSuspended())
            fang += m_flowClock * kFlowRunSpin;

        // 1. Front face
        nxui::Vec3 front3D[4];
        flowFace(fx, fz, fang, -halfW, 0.f, halfW, 0.f, halfH, front3D);
        nxui::Vec2 front2D[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            if (!projectPoint3D(front3D[k], 1280.f, 720.f, front2D[k])) {
                ok = false;
                break;
            }
        }
        if (ok && pointInQuad(screenX, screenY, front2D))
            return c.index;

        // 2. Left spine
        if (fang > 0.05f) {
            nxui::Vec3 spine3D[4];
            flowFace(fx, fz, fang, -halfW, 0.f, -halfW, 2.f * depth, halfH, spine3D);
            ok = true;
            for (int k = 0; k < 4; ++k) {
                if (!projectPoint3D(spine3D[k], 1280.f, 720.f, front2D[k])) {
                    ok = false;
                    break;
                }
            }
            if (ok && pointInQuad(screenX, screenY, front2D))
                return c.index;
        }

        // 3. Right edge
        if (fang < -0.05f) {
            nxui::Vec3 spine3D[4];
            flowFace(fx, fz, fang, halfW, 0.f, halfW, 2.f * depth, halfH, spine3D);
            ok = true;
            for (int k = 0; k < 4; ++k) {
                if (!projectPoint3D(spine3D[k], 1280.f, 720.f, front2D[k])) {
                    ok = false;
                    break;
                }
            }
            if (ok && pointInQuad(screenX, screenY, front2D))
                return c.index;
        }
    }
    return -1;
}

int IconGrid::hitTestShelf(float screenX, float screenY) const {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return -1;
    const float scroll = m_lineScrollOffset.value();
    const int centre = (int)std::round(scroll);

    struct HitCand {
        int index;
        float d;
        float z;
    };
    std::vector<HitCand> order;
    order.reserve(16);
    for (int i = centre - 3; i <= centre + 8; ++i) {
        float d = (float)i - scroll;
        float x, y, z, a;
        shelfPlace(d, x, y, z, a);
        if (a <= 0.05f) continue;
        order.push_back({flowWrap(i, n), d, z});
    }

    // Near to far (smallest z first)
    std::sort(order.begin(), order.end(), [](const HitCand& a, const HitCand& b) {
        return a.z < b.z;
    });

    const float halfW = 0.44f;
    const float halfH = 0.66f;

    for (const auto& c : order) {
        float x, y, z, a;
        shelfPlace(c.d, x, y, z, a);

        const nxui::Vec3 quad[4] = {
            { x - halfW, y + halfH, z },
            { x + halfW, y + halfH, z },
            { x + halfW, y - halfH, z },
            { x - halfW, y - halfH, z }
        };

        nxui::Vec2 quad2D[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            if (!projectPoint3D(quad[k], 1280.f, 720.f, quad2D[k])) {
                ok = false;
                break;
            }
        }
        if (ok && pointInQuad(screenX, screenY, quad2D))
            return c.index;
    }
    return -1;
}

int IconGrid::hitTestDeck(float screenX, float screenY) const {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return -1;
    const float scroll = m_lineScrollOffset.value();
    const int centre = (int)std::round(scroll);

    struct HitCand {
        int index;
        float d;
        float z;
    };
    std::vector<HitCand> order;
    order.reserve(16);
    for (int i = centre - 5; i <= centre + 5; ++i) {
        float d = (float)i - scroll;
        float x, y, z, ang, a;
        deckPlace(d, x, y, z, ang, a);
        if (a <= 0.05f) continue;
        order.push_back({flowWrap(i, n), d, z});
    }

    // Near to far (smallest z first)
    std::sort(order.begin(), order.end(), [](const HitCand& a, const HitCand& b) {
        return a.z < b.z;
    });

    const float halfW = 0.36f;
    const float halfH = 0.54f;

    for (const auto& c : order) {
        float x, y, z, ang, a;
        deckPlace(c.d, x, y, z, ang, a);

        nxui::Vec3 quad[4];
        deckCorners(x, y, z, ang, halfW, halfH, quad);

        nxui::Vec2 quad2D[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            if (!projectPoint3D(quad[k], 1280.f, 720.f, quad2D[k])) {
                ok = false;
                break;
            }
        }
        if (ok && pointInQuad(screenX, screenY, quad2D))
            return c.index;
    }
    return -1;
}

nxui::Rect IconGrid::projected3DIconRect(int index) const {
    if (index < 0 || index >= (int)m_allIcons.size())
        return {};

    const float offset = m_lineScrollOffset.value();
    const float d = lineRingDelta(offset, static_cast<float>(index));

    nxui::Vec3 quad[4];
    if (m_layoutMode == AppLayoutMode::Flow) {
        float fx, fz, fang;
        flowPlace(d, fx, fz, fang);
        const float halfW = kFlowHalfW;
        const float halfH = kFlowHalfH;
        flowFace(fx, fz, fang, -halfW, 0.f, halfW, 0.f, halfH, quad);
    } else if (m_layoutMode == AppLayoutMode::Shelf) {
        float x, y, z, a;
        shelfPlace(d, x, y, z, a);
        const float halfW = 0.44f;
        const float halfH = 0.66f;
        quad[0] = { x - halfW, y + halfH, z };
        quad[1] = { x + halfW, y + halfH, z };
        quad[2] = { x + halfW, y - halfH, z };
        quad[3] = { x - halfW, y - halfH, z };
    } else if (m_layoutMode == AppLayoutMode::Deck) {
        float x, y, z, ang, a;
        deckPlace(d, x, y, z, ang, a);
        const float halfW = 0.36f;
        const float halfH = 0.54f;
        deckCorners(x, y, z, ang, halfW, halfH, quad);
    } else if (m_layoutMode == AppLayoutMode::Cover) {
        float x, y, z, a;
        coverPlace(d, x, y, z, a);
        const auto& icon = m_allIcons[(size_t)index];
        const std::uint64_t tid = icon ? icon->titleId() : 0;
        bool hasCover = false;
        if (tid != 0) {
            auto it = m_flowCovers.find(tid);
            if (it != m_flowCovers.end() && it->second.available && it->second.texture.valid()) {
                hasCover = true;
            }
        }
        const float halfW = hasCover ? 0.44f : 0.5625f;
        const float halfH = hasCover ? 0.66f : 0.5625f;
        coverCorners(x, y, z, halfW, halfH, quad);
    } else {
        return dynamicIconRect(index);
    }

    nxui::Vec2 p2D[4];
    for (int k = 0; k < 4; ++k) {
        if (!projectPoint3D(quad[k], 1280.f, 720.f, p2D[k]))
            return dynamicIconRect(index);
    }

    float minX = std::min({p2D[0].x, p2D[1].x, p2D[2].x, p2D[3].x});
    float maxX = std::max({p2D[0].x, p2D[1].x, p2D[2].x, p2D[3].x});
    float minY = std::min({p2D[0].y, p2D[1].y, p2D[2].y, p2D[3].y});
    float maxY = std::max({p2D[0].y, p2D[1].y, p2D[2].y, p2D[3].y});

    return nxui::Rect{minX, minY, std::max(1.f, maxX - minX), std::max(1.f, maxY - minY)};
}

int IconGrid::hitTest(float screenX, float screenY) const {
    if (m_layoutMode == AppLayoutMode::Flow) {
        return hitTestFlow(screenX, screenY);
    }
    if (m_layoutMode == AppLayoutMode::Shelf) {
        return hitTestShelf(screenX, screenY);
    }
    if (m_layoutMode == AppLayoutMode::Deck) {
        return hitTestDeck(screenX, screenY);
    }
    if (m_layoutMode == AppLayoutMode::Cover) {
        return hitTestCover(screenX, screenY);
    }
    // XMB touch is resolved in findTopHit(): a tap there changes the selected
    // category or row, which is a mutation, and hitTest() is const by contract
    // for every other view. The old code cast the const away from here instead.
    if (m_layoutMode == AppLayoutMode::Xmb)
        return -1;

    if (m_layoutMode == AppLayoutMode::DynamicLine) {
        for (int i = 0; i < (int)m_allIcons.size(); ++i) {
            nxui::Rect r = dynamicIconRect(i);
            if (r.contains(screenX, screenY))
                return i;
        }
        return -1;
    }
    int start = m_page * iconsPerPage();
    int end   = std::min(start + iconsPerPage(), (int)m_allIcons.size());
    for (int i = start; i < end; ++i) {
        nxui::Rect r = m_allIcons[i]->focusRect();
        if (r.contains(screenX, screenY))
            return i - start;
    }
    return -1;
}

nxui::Widget* IconGrid::findTopHit(float x, float y) {
    if (!isVisible()) return nullptr;
    if (m_layoutMode == AppLayoutMode::Xmb) {
        // hitTestXmb moves the selection (and activates on a second tap on the
        // same row) and reports nothing back: the caller must not then treat
        // the tap as a launch of whatever the selection happens to be, which is
        // how a single touch on the bar could start an app the player only
        // meant to highlight.
        hitTestXmb(x, y);
        if (m_xmbCol >= 0 && m_xmbCol < static_cast<int>(m_xmbCols.size())) {
            auto& col = m_xmbCols[m_xmbCol];
            if (m_xmbItem >= 0 && m_xmbItem < static_cast<int>(col.size()))
                return col[m_xmbItem];
        }
        return nullptr;
    }
    int hit = hitTest(x, y);
    if (hit < 0) return nullptr;
    if (isCarousel()) {
        if (hit >= 0 && hit < (int)m_allIcons.size())
            return m_allIcons[hit].get();
    } else {
        int global = m_page * iconsPerPage() + hit;
        if (global >= 0 && global < (int)m_allIcons.size())
            return m_allIcons[global].get();
    }
    return nullptr;
}

void IconGrid::startAppearAnimation() {
    if (m_layoutMode == AppLayoutMode::Xmb) {
        // XMB draws one long column, not a page. Taking the paged branch below
        // only started the appear animation for the first cols*rows icons, so
        // everything past that stayed at zero opacity: after returning from a
        // game the bar came back with blank tiles that only filled in once the
        // player scrolled far enough to rebuild them.
        for (auto& icon : m_allIcons) {
            if (icon) icon->forceVisible();
        }
        for (auto& sys : m_xmbSystemIcons) {
            if (sys) sys->forceVisible();
        }
        return;
    }
    if (isCarousel()) {
        int cur = focusedGlobalIndex();
        int center = cur >= 0 ? cur : 0;
        for (int i = 0; i < (int)m_allIcons.size(); ++i) {
            float dist = static_cast<float>(std::abs(i - center));
            float delay = std::min(0.40f, dist * 0.06f);
            m_allIcons[i]->startAppear(delay);
        }
        return;
    }
    int start = m_page * iconsPerPage();
    int end   = std::min(start + iconsPerPage(), (int)m_allIcons.size());
    int maxDist = (m_cols - 1) + (m_rows - 1);
    for (int i = start; i < end; ++i) {
        int local = i - start;
        int col   = local % m_cols;
        int row   = local / m_cols;
        float t   = maxDist > 0 ? (float)(col + row) / maxDist : 0.f;
        float delay = t * 0.40f;
        m_allIcons[i]->startAppear(delay);
    }
}

void IconGrid::startPageTransition(int targetPage) {
    if (isCarousel()) return;   // neither carousel view pages

    targetPage = std::clamp(targetPage, 0, m_totalPages - 1);
    if (targetPage == m_page) return;

    const int fromPage = m_page;
    const int oldGlobalFocus = focusedGlobalIndex();
    const int wantedLocalCell = oldGlobalFocus >= 0
        ? oldGlobalFocus % std::max(1, iconsPerPage()) : 0;
    setPage(targetPage);

    // Preserve the logical cell when paging. A continuation cell belonging to
    // a large widget is not focusable, so choose the closest real anchor
    // instead of accepting FocusManager's unrelated first-item fallback.
    const int pageStart = targetPage * iconsPerPage();
    const int pageEnd = std::min(pageStart + iconsPerPage(),
                                 static_cast<int>(m_allIcons.size()));
    int best = -1;
    int bestDistance = std::numeric_limits<int>::max();
    const int wantedColumn = wantedLocalCell % std::max(1, m_cols);
    const int wantedRow = wantedLocalCell / std::max(1, m_cols);
    const nxui::Vec2 wantedCenter{
        m_originX + wantedColumn * (m_cellW + m_padX) + m_cellW * 0.5f,
        m_originY + wantedRow * (m_cellH + m_padY) + m_cellH * 0.5f};
    for (int index = pageStart; index < pageEnd; ++index) {
        if (!m_allIcons[static_cast<std::size_t>(index)] ||
            !m_allIcons[static_cast<std::size_t>(index)]->isFocusable())
            continue;
        if (m_allIcons[static_cast<std::size_t>(index)]->focusRect().contains(
                wantedCenter.x, wantedCenter.y)) {
            best = index;
            break;
        }
        const int local = index - pageStart;
        const int distance = std::abs(local % std::max(1, m_cols) - wantedColumn) +
                             std::abs(local / std::max(1, m_cols) - wantedRow);
        if (distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    if (best >= 0)
        m_focus.setFocus(m_allIcons[static_cast<std::size_t>(best)].get());

    if (!m_slideTransition) {
        m_sliding = false;
        startAppearAnimation();
        if (m_onPageSwitched) m_onPageSwitched();
        return;
    }

    m_slidePrevPage = fromPage;
    m_slideDir = (targetPage > fromPage) ? 1 : -1;
    m_slideT = 0.f;
    m_sliding = true;

    const int start = m_page * iconsPerPage();
    const int end   = std::min(start + iconsPerPage(), (int)m_allIcons.size());
    for (int i = start; i < end; ++i)
        m_allIcons[i]->forceVisible();

    const float stride = pageStride();
    m_slideInDx  = stride * (float)m_slideDir;
    m_slideOutDx = 0.f;
    positionPage(m_page, m_slideInDx);

    if (m_onPageSwitched) m_onPageSwitched();
}

void IconGrid::startWaveTransition(int targetPage) {
    startPageTransition(targetPage);
}

void IconGrid::onUpdate(float dt) {
    m_layoutReveal.update(dt);
    if (m_layoutMode == AppLayoutMode::Xmb) {
        const float prevColScroll = m_xmbColScroll;
        const float prevItemScroll = m_xmbItemScroll;
        m_xmbColScroll += (static_cast<float>(m_xmbCol) - m_xmbColScroll) * 0.22f;
        m_xmbItemScroll += (static_cast<float>(m_xmbItem) - m_xmbItemScroll) * 0.26f;
        if (std::abs(static_cast<float>(m_xmbCol) - m_xmbColScroll) < 0.003f)
            m_xmbColScroll = static_cast<float>(m_xmbCol);
        if (std::abs(static_cast<float>(m_xmbItem) - m_xmbItemScroll) < 0.003f)
            m_xmbItemScroll = static_cast<float>(m_xmbItem);
        // Every XMB rect is a function of these two scroll values, and they move
        // every frame while the bar settles. renderXmb() recomputes from them
        // live, so without re-placing the children here the widget rects would
        // describe the position the selection was in before the animation
        // started -- a touch during the slide would land on the wrong row, and
        // the selection ring would sit off the tile it is framing.
        if (prevColScroll != m_xmbColScroll || prevItemScroll != m_xmbItemScroll)
            syncXmbChildRects();
        return;
    }
    if (isCarousel()) {
        // Sampled once per frame so every case in the Flow row is placed
        // against the same instant. Wraps harmlessly; only its fractional
        // progression matters to the idle turn.
        if (m_layoutMode == AppLayoutMode::Flow) {
            m_flowClock += dt;
            if (m_flowClock > 3600.f) m_flowClock -= 3600.f;
        }
        m_lineScrollOffset.update(dt);
        int cur = focusedGlobalIndex();
        if (cur >= 0 &&
            std::abs(lineRingDelta(m_lineScrollOffset.target(),
                                   static_cast<float>(cur))) > 0.001f) {
            const float from = m_lineScrollOffset.value();
            m_lineScrollOffset.set(from + lineRingDelta(from, static_cast<float>(cur)),
                                   kLineScrollDuration,
                                   nxui::Easing::outCubic);
        }

        // Every rect on the line is a pure function of the scroll offset, the
        // reveal value and the grid rect. At rest all three are constant, so
        // recomputing them each frame produced identical values for the whole
        // installed library. Recompute only when one of those inputs moved.
        const float offsetNow = m_lineScrollOffset.value();
        const float revealNow = m_layoutReveal.value();
        const bool layoutDirty =
            m_lineLayoutCacheCount != (int)m_allIcons.size()
            || std::abs(m_lineLayoutCacheOffset - offsetNow) > 0.0001f
            || std::abs(m_lineLayoutCacheReveal - revealNow) > 0.0001f
            || std::abs(m_lineLayoutCacheRect.x - m_rect.x) > 0.0001f
            || std::abs(m_lineLayoutCacheRect.y - m_rect.y) > 0.0001f
            || std::abs(m_lineLayoutCacheRect.width - m_rect.width) > 0.0001f
            || std::abs(m_lineLayoutCacheRect.height - m_rect.height) > 0.0001f;

        if (layoutDirty) {
            // Flow/Shelf/Deck position items in world space at draw time and never
            // read these rects directly, but they are kept in sync with the projected
            // 3D bounds anyway: the edit cursor, focus ring, touch feedback and
            // accessibility all query focusedDisplayRect and rect() through this path.
            if (is3D()) {
                for (int i = 0; i < (int)m_allIcons.size(); ++i) {
                    m_allIcons[i]->setRect(projected3DIconRect(i));
                }
            } else {
                for (int i = 0; i < (int)m_allIcons.size(); ++i) {
                    m_allIcons[i]->setRect(dynamicIconRect(i));
                }
            }
            m_lineLayoutCacheCount = (int)m_allIcons.size();
            m_lineLayoutCacheOffset = offsetNow;
            m_lineLayoutCacheReveal = revealNow;
            m_lineLayoutCacheRect = m_rect;
        }
        return;
    }

    if (!m_sliding)
        return;

    m_slideT += dt;
    const float t = std::clamp(m_slideT / kSlideDuration, 0.f, 1.f);
    const float eased = nxui::Easing::outCubic(t);
    const float stride = pageStride();

    m_slideInDx  = (1.f - eased) * stride * (float)m_slideDir;
    m_slideOutDx = m_slideInDx - stride * (float)m_slideDir;
    positionPage(m_page, m_slideInDx);

    if (t >= 1.f) {
        m_sliding = false;
        m_slideInDx = m_slideOutDx = 0.f;
        positionPage(m_page, 0.f);
    }
}

void IconGrid::renderPageAt(nxui::Renderer& ren, int page, float dx) {
    const int start = page * iconsPerPage();
    const int end   = std::min(start + iconsPerPage(), (int)m_allIcons.size());
    for (int i = start; i < end; ++i) {
        auto& icon = m_allIcons[i];
        const nxui::Rect saved = icon->rect();
        const int local = i - start;
        const int spanColumns = std::max(1, icon->gridSpanColumns());
        const int spanRows = std::max(1, icon->gridSpanRows());
        icon->setRect({m_originX + (local % m_cols) * (m_cellW + m_padX) + dx,
                       m_originY + (local / m_cols) * (m_cellH + m_padY),
                       m_cellW * spanColumns + m_padX * (spanColumns - 1),
                       m_cellH * spanRows + m_padY * (spanRows - 1)});
        icon->render(ren);
        icon->setRect(saved);
    }
}

void IconGrid::renderDynamicLine(nxui::Renderer& ren) {
    ren.pushClipRect(m_rect);

    // The focused index is a linear scan over every icon. Reading it inside the
    // loop made the whole pass quadratic in the installed title count for a
    // value that cannot change while the loop runs.
    const int focusedIndex = focusedGlobalIndex();

    auto& candidates = m_lineRenderScratch;
    candidates.clear();
    candidates.reserve(m_allIcons.size());

    for (int i = 0; i < (int)m_allIcons.size(); ++i) {
        float s = 1.f;
        float a = 1.f;
        float absD = 0.f;
        const nxui::Rect r = dynamicIconRect(i, &s, &a, &absD);
        if (absD > 4.5f && i != focusedIndex) continue;
        const float d = r.center().x - m_rect.center().x;
        candidates.push_back({i, absD, d, s, a, r});
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.absD > rhs.absD;
    });

    for (const auto& c : candidates) {
        auto& icon = m_allIcons[c.index];
        const nxui::Rect savedRect = icon->rect();
        const float savedOp = icon->opacity();

        icon->setRect(c.rect);
        icon->setOpacity(savedOp * c.a);
        icon->render(ren);

        icon->setRect(savedRect);
        icon->setOpacity(savedOp);
    }

    ren.popClipRect();
}

void IconGrid::renderFlow(nxui::Renderer& ren) {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return;

    // Allow reflections and floor plane to reach the bottom of the screen (y=720),
    // keeping clipping bounded below the top HUD (y=60).
    ren.pushClipRect(nxui::Rect{0.f, 60.f, 1280.f, 660.f});

    const float reveal = clamp01(m_layoutReveal.value());

    // Dark glossy floor plane grounding cases visually behind reflections
    const float floorTop = 490.f;
    const float floorHeight = 720.f - floorTop;
    ren.drawGradientRect(
        nxui::Rect{0.f, floorTop, 1280.f, floorHeight},
        nxui::Color(0.04f, 0.05f, 0.08f, 0.55f * reveal),
        nxui::Color(0.01f, 0.01f, 0.02f, 0.85f * reveal)
    );
    ren.drawRect(
        nxui::Rect{0.f, floorTop, 1280.f, 1.5f},
        nxui::Color(1.f, 1.f, 1.f, 0.08f * reveal)
    );

    // Harvest completed asynchronous cover decodes (up to 2 uploads per frame)
    int uploads = 0;
    for (auto it = m_pendingCoverDecodes.begin(); it != m_pendingCoverDecodes.end() && uploads < 2;) {
        if (it->state && it->state->done.load()) {
            try {
                if (it->future.valid()) it->future.get();
            } catch (...) {}
            auto covIt = m_flowCovers.find(it->titleId);
            if (covIt != m_flowCovers.end()) {
                if (!it->state->failed.load() && it->state->decoded.valid()) {
                    if (covIt->second.texture.loadFromDecoded(ren.gpu(), ren, it->state->decoded)) {
                        covIt->second.available = true;
                    }
                }
                covIt->second.loading = false;
            }
            it = m_pendingCoverDecodes.erase(it);
            ++uploads;
        } else {
            ++it;
        }
    }

    const int focusedIndex = focusedGlobalIndex();
    const float scroll = m_lineScrollOffset.value();

    // The drawn range is virtual: it may run below zero or past the end, and is
    // folded to a real item only when its content is needed. That keeps the row
    // continuous across a wrap.
    const int centre = (int)std::lround(scroll);

    auto& order = m_flowRenderScratch;
    order.clear();
    order.reserve((size_t)kFlowVisible * 2 + 1);
    for (int i = centre - kFlowVisible; i <= centre + kFlowVisible; ++i) {
        const float p = (float)i - scroll;
        float fx, fz, fang;
        flowPlace(p, fx, fz, fang);
        order.push_back({flowWrap(i, n), p, fz});
    }

    // Outside-in. There is no depth buffer, so draw order is the only depth
    // information there is: the centre case is nearest and must land last.
    std::sort(order.begin(), order.end(), [](const FlowCandidate& a, const FlowCandidate& b) {
        return a.z > b.z;
    });

    for (const auto& c : order) {
        auto& icon = m_allIcons[(size_t)c.index];
        if (!icon) continue;

        float fx, fz, fang;
        flowPlace(c.p, fx, fz, fang);

        const bool isSel = (c.index == focusedIndex);

        // The running title turns slowly on its own so it can be picked out of
        // the row at a glance. Added on top of any other rotation.
        if (icon->isSuspended())
            fang += m_flowClock * kFlowRunSpin;

        const float halfW = kFlowHalfW;
        const float halfH = kFlowHalfH;
        const float depth = kFlowDepth;

        // Watertight case geometry: front, left spine, right edge, and back
        // are constructed via flowFace with shared corner coordinates.
        nxui::Vec3 front[4];
        flowFace(fx, fz, fang, -halfW, 0.f, halfW, 0.f, halfH, front);

        nxui::Vec3 faceL[4];
        flowFace(fx, fz, fang, -halfW, 0.f, -halfW, 2.f * depth, halfH, faceL);

        nxui::Vec3 faceR[4];
        flowFace(fx, fz, fang, halfW, 0.f, halfW, 2.f * depth, halfH, faceR);

        nxui::Vec3 back[4];
        flowFace(fx, fz, fang, halfW, 2.f * depth, -halfW, 2.f * depth, halfH, back);

        // Proximity lighting. This is a large part of why the centre case reads
        // as selected. Side faces are lit independently of the printed front:
        // they are grey plastic and have nothing to do with the artwork.
        //
        // The selected case additionally lifts above the saturated proximity
        // value, so the centre item stays distinguishable from its immediate
        // neighbours once the row has settled and both are at prox ~1.
        const float prox = std::max(0.f, 1.f - std::abs(c.p));
        // Selected art gets a small additive lift, clamped before the float is
        // converted to uint8_t by SDL2. Without the clamp 1.07*255 narrows to a
        // dark byte on that backend instead of saturating to white.
        const float selectedLift = isSel ? 0.08f : 0.f;
        const float lit      = std::clamp((150.f + 105.f * prox) / 255.f
                                          + selectedLift, 0.f, 1.f);
        const float blankLit = std::clamp((18.f + 14.f * prox) / 255.f
                                          + selectedLift * 0.25f, 0.f, 1.f);
        const float sideLit  = std::clamp((70.f + 60.f * prox) / 255.f
                                          + selectedLift * 0.35f, 0.f, 1.f);

        const float alpha = m_opacity * icon->opacity() * reveal;
        if (alpha <= 0.f) continue;

        const nxui::Color artTint {lit, lit, lit, alpha};
        const nxui::Color blankCol{blankLit, blankLit, blankLit, alpha};
        const nxui::Color sideCol {sideLit, sideLit, sideLit, alpha};
        const nxui::Color backCol {sideLit * 0.9f, sideLit * 0.9f, sideLit * 0.9f, alpha};

        // Query 2:3 portrait cover art cache. If available, the cover spans the
        // full front face. If absent or loading, 1:1 square icon is inset.
        const std::uint64_t tid = icon->titleId();
        nxui::Texture* coverTex = nullptr;
        if (tid != 0) {
            auto& cov = m_flowCovers[tid];
            if (!cov.checked) {
                cov.checked = true;
                cov.loading = true;
                if (m_threadPool) {
                    auto decodeState = std::make_shared<CoverDecodeState>();
                    auto work = [decodeState, tid]() {
                        try {
                            const std::string coverPath = resolveFlowCoverPath(tid);
                            if (!coverPath.empty()) {
                                decodeState->decoded = nxui::Texture::decodeFile(coverPath, 720);
                                decodeState->failed = !decodeState->decoded.valid();
                            } else {
                                decodeState->failed = true;
                            }
                        } catch (...) {
                            decodeState->failed = true;
                        }
                        decodeState->done = true;
                    };
                    PendingCoverDecode pcd;
                    pcd.titleId = tid;
                    pcd.state = decodeState;
                    pcd.future = m_threadPool->submit(std::move(work));
                    m_pendingCoverDecodes.push_back(std::move(pcd));
                }
            }
            if (cov.available && cov.texture.valid()) {
                coverTex = &cov.texture;
            }
        }

        // 1:1 square icon inset centered on the front and back faces, keeping exact square
        // aspect ratio without vertical distortion.
        const float iconHalf = halfW * 0.82f;
        nxui::Vec3 iconQuad[4];
        flowFace(fx, fz, fang, -iconHalf, 0.001f, iconHalf, 0.001f, iconHalf, iconQuad);

        nxui::Vec3 iconBack[4];
        flowFace(fx, fz, fang, iconHalf, 2.f * depth - 0.001f, -iconHalf, 2.f * depth - 0.001f, iconHalf, iconBack);

        // The four faces are sorted by their actual depth rather than by rules
        // read off the angle. The cases sit well off to either side, so
        // perspective slides faces past one another at rotations the sine and
        // cosine know nothing about; sorting a convex box far-to-near is
        // correct at every angle and needs no winding convention.
        struct FaceOrder { float z; int which; };
        FaceOrder faces[4] = {
            {flowMidZ(front), 0},
            {flowMidZ(back),  1},
            {flowMidZ(faceL), 2},
            {flowMidZ(faceR), 3},
        };
        std::sort(std::begin(faces), std::end(faces),
                  [](const FaceOrder& a, const FaceOrder& b) { return a.z > b.z; });

        nxui::Texture* art = icon->texture();
        const bool showingFront = flowMidZ(front) <= flowMidZ(back);
        const bool showingBack  = flowMidZ(back)  <= flowMidZ(front);

        for (const auto& f : faces) {
            switch (f.which) {
                case 0:
                    // A case is a solid object: the bare case is drawn first and
                    // the artwork is printed on top of it, so a title with no
                    // art is a blank case rather than a hole.
                    ren.drawQuad3D(nullptr, front, blankCol, 1.f, 1.f, false, kFlowStripsSide);
                    if (showingFront) {
                        if (coverTex) {
                            ren.drawQuad3D(coverTex, front, artTint, 1.f, 1.f, false, kFlowStripsFront);
                        } else if (art && art->valid()) {
                            ren.drawQuad3D(art, iconQuad, artTint, 1.f, 1.f, false, kFlowStripsFront);
                        }
                    }
                    break;
                case 1:
                    ren.drawQuad3D(nullptr, back, backCol, 1.f, 1.f, false, kFlowStripsSide);
                    if (showingBack) {
                        if (coverTex) {
                            ren.drawQuad3D(coverTex, back, artTint, 1.f, 1.f, false, kFlowStripsFront);
                        } else if (art && art->valid()) {
                            ren.drawQuad3D(art, iconBack, artTint, 1.f, 1.f, false, kFlowStripsFront);
                        }
                    }
                    break;
                case 2: ren.drawQuad3D(nullptr, faceL, sideCol, 1.f, 1.f, false, kFlowStripsSide); break;
                case 3: ren.drawQuad3D(nullptr, faceR, sideCol, 1.f, 1.f, false, kFlowStripsSide); break;
            }
        }

        // Reflection. Mirrored about the case's OWN bottom edge rather than a
        // fixed floor, so it stays edge to edge at any scale, and every face is
        // mirrored so the reflection keeps the same silhouette. The corner
        // order reverses because the mirror flips the winding. The cover
        // texture is reused; no mirrored copy is ever allocated.
        const float floorY = kFlowY - halfH;
        auto mirror = [floorY](const nxui::Vec3 src[4], nxui::Vec3 out[4]) {
            for (int k = 0; k < 4; ++k) {
                const int j = 3 - k;
                out[k] = {src[j].x, 2.f * floorY - src[j].y, src[j].z};
            }
        };

        // Mirror and draw the SAME far-to-near face order as the case. A fixed
        // front-only reflection lies when a suspended title turns its back to
        // the viewer, and ordering sides independently can paint a farther edge
        // over the visible face.
        const float reflTop = kFlowReflTop;
        nxui::Vec3 m[4];
        for (const auto& f : faces) {
            const nxui::Vec3* src = f.which == 0 ? front
                                   : f.which == 1 ? back
                                   : f.which == 2 ? faceL : faceR;
            const nxui::Color col = f.which == 0 ? blankCol
                                   : f.which == 1 ? backCol : sideCol;
            mirror(src, m);
            ren.drawQuad3D(nullptr, m, col, reflTop, 0.f, true, kFlowStripsRefl);
            if (f.which == 0 && showingFront) {
                if (coverTex) {
                    ren.drawQuad3D(coverTex, m, artTint, reflTop, 0.f, true, kFlowStripsRefl);
                } else if (art && art->valid()) {
                    nxui::Vec3 mIcon[4];
                    mirror(iconQuad, mIcon);
                    ren.drawQuad3D(art, mIcon, artTint, reflTop, 0.f, true, kFlowStripsRefl);
                }
            } else if (f.which == 1 && showingBack) {
                if (coverTex) {
                    ren.drawQuad3D(coverTex, m, artTint, reflTop, 0.f, true, kFlowStripsRefl);
                } else if (art && art->valid()) {
                    nxui::Vec3 mIcon[4];
                    mirror(iconBack, mIcon);
                    ren.drawQuad3D(art, mIcon, artTint, reflTop, 0.f, true, kFlowStripsRefl);
                }
            }
        }
    }

    ren.popClipRect();
}

void IconGrid::shelfPlace(float d, float& x, float& y, float& z, float& a) {
    if (d >= 0.0f) {
        x = -1.45f + d * 0.78f;
        y =  0.30f + d * 0.12f;
        z =  3.60f + d * 1.00f;
        a = std::clamp(1.0f - d * 0.16f, 0.0f, 1.0f);
    } else { // sliding out to the left
        x = -1.45f + d * 1.90f;
        y =  0.30f;
        z =  3.60f + d * 0.30f;
        a = std::clamp(1.0f + d * 1.20f, 0.0f, 1.0f);
    }
}

void IconGrid::renderShelf(nxui::Renderer& ren) {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return;

    ren.pushClipRect(nxui::Rect{0.f, 60.f, 1280.f, 660.f});

    const float reveal = clamp01(m_layoutReveal.value());

    // Dark glossy floor plane grounding cases visually behind reflections
    const float floorTop = 460.f;
    const float floorHeight = 720.f - floorTop;
    ren.drawGradientRect(
        nxui::Rect{0.f, floorTop, 1280.f, floorHeight},
        nxui::Color(0.04f, 0.05f, 0.08f, 0.60f * reveal),
        nxui::Color(0.01f, 0.01f, 0.02f, 0.90f * reveal)
    );
    ren.drawRect(
        nxui::Rect{0.f, floorTop, 1280.f, 1.5f},
        nxui::Color(1.f, 1.f, 1.f, 0.08f * reveal)
    );

    // Harvest completed asynchronous cover decodes (up to 2 uploads per frame)
    int uploads = 0;
    for (auto it = m_pendingCoverDecodes.begin(); it != m_pendingCoverDecodes.end() && uploads < 2;) {
        if (it->state && it->state->done.load()) {
            try {
                if (it->future.valid()) it->future.get();
            } catch (...) {}
            auto covIt = m_flowCovers.find(it->titleId);
            if (covIt != m_flowCovers.end()) {
                if (!it->state->failed.load() && it->state->decoded.valid()) {
                    if (covIt->second.texture.loadFromDecoded(ren.gpu(), ren, it->state->decoded)) {
                        covIt->second.available = true;
                    }
                }
                covIt->second.loading = false;
            }
            it = m_pendingCoverDecodes.erase(it);
            ++uploads;
        } else {
            ++it;
        }
    }

    const int focusedIndex = focusedGlobalIndex();
    const float scroll = m_lineScrollOffset.value();
    const int centre = (int)std::lround(scroll);

    auto& order = m_flowRenderScratch;
    order.clear();
    order.reserve(16);
    for (int i = centre - 3; i <= centre + 8; ++i) {
        const float d = (float)i - scroll;
        float x, y, z, a;
        shelfPlace(d, x, y, z, a);
        if (a <= 0.01f) continue;
        order.push_back({flowWrap(i, n), d, z});
    }

    // Far to near: the row recedes to the right, so the furthest items land first.
    std::sort(order.begin(), order.end(), [](const FlowCandidate& a, const FlowCandidate& b) {
        return a.z > b.z;
    });

    for (const auto& c : order) {
        auto& icon = m_allIcons[(size_t)c.index];
        if (!icon) continue;

        float x, y, z, a;
        shelfPlace(c.p, x, y, z, a);

        const bool isSel = (c.index == focusedIndex);

        const float halfW = 0.44f;
        const float halfH = 0.66f;

        const nxui::Vec3 quad[4] = {
            { x - halfW, y + halfH, z },
            { x + halfW, y + halfH, z },
            { x + halfW, y - halfH, z },
            { x - halfW, y - halfH, z }
        };

        const float floorY = y - halfH - 0.04f;
        const nxui::Vec3 refl[4] = {
            { x - halfW, floorY, z },
            { x + halfW, floorY, z },
            { x + halfW, floorY - 2.f * halfH, z },
            { x - halfW, floorY - 2.f * halfH, z }
        };

        const float prox = std::max(0.0f, 1.0f - std::abs(c.p));
        const float selectedLift = isSel ? 0.08f : 0.f;
        const float lit = std::clamp((150.f + 105.f * prox) / 255.f + selectedLift, 0.f, 1.f);
        const float blankLit = std::clamp((20.f + 15.f * prox) / 255.f + selectedLift * 0.25f, 0.f, 1.f);

        const float alpha = m_opacity * icon->opacity() * reveal * a;
        if (alpha <= 0.01f) continue;

        const nxui::Color artTint {lit, lit, lit, alpha};
        const nxui::Color blankCol{blankLit, blankLit, blankLit, alpha};

        // Query 2:3 portrait cover art cache.
        const std::uint64_t tid = icon->titleId();
        nxui::Texture* coverTex = nullptr;
        if (tid != 0) {
            auto& cov = m_flowCovers[tid];
            if (!cov.checked) {
                cov.checked = true;
                cov.loading = true;
                if (m_threadPool) {
                    auto decodeState = std::make_shared<CoverDecodeState>();
                    auto work = [decodeState, tid]() {
                        try {
                            const std::string coverPath = resolveFlowCoverPath(tid);
                            if (!coverPath.empty()) {
                                decodeState->decoded = nxui::Texture::decodeFile(coverPath, 720);
                                decodeState->failed = !decodeState->decoded.valid();
                            } else {
                                decodeState->failed = true;
                            }
                        } catch (...) {
                            decodeState->failed = true;
                        }
                        decodeState->done = true;
                    };
                    PendingCoverDecode pcd;
                    pcd.titleId = tid;
                    pcd.state = decodeState;
                    pcd.future = m_threadPool->submit(std::move(work));
                    m_pendingCoverDecodes.push_back(std::move(pcd));
                }
            }
            if (cov.available && cov.texture.valid()) {
                coverTex = &cov.texture;
            }
        }

        nxui::Texture* art = icon->texture();

        // 1:1 square icon inset centered on front face when 2:3 vertical cover is not present.
        const float iconHalf = halfW * 0.88f;
        const nxui::Vec3 iconQuad[4] = {
            { x - iconHalf, y + iconHalf, z - 0.001f },
            { x + iconHalf, y + iconHalf, z - 0.001f },
            { x + iconHalf, y - iconHalf, z - 0.001f },
            { x - iconHalf, y - iconHalf, z - 0.001f }
        };
        const nxui::Vec3 iconRefl[4] = {
            { x - iconHalf, floorY - (halfH - iconHalf), z },
            { x + iconHalf, floorY - (halfH - iconHalf), z },
            { x + iconHalf, floorY - (halfH + iconHalf), z },
            { x - iconHalf, floorY - (halfH + iconHalf), z }
        };

        // 1. Draw floor reflection
        const float reflTop = 0.32f * alpha;
        ren.drawQuad3D(nullptr, refl, blankCol, reflTop, 0.f, true, 8);
        if (coverTex) {
            ren.drawQuad3D(coverTex, refl, artTint, reflTop, 0.f, true, 8);
        } else if (art && art->valid()) {
            ren.drawQuad3D(art, iconRefl, artTint, reflTop, 0.f, true, 8);
        }

        // 2. Draw card face
        ren.drawQuad3D(nullptr, quad, blankCol, 1.f, 1.f, false, 8);
        if (coverTex) {
            ren.drawQuad3D(coverTex, quad, artTint, 1.f, 1.f, false, 8);
        } else if (art && art->valid()) {
            ren.drawQuad3D(art, iconQuad, artTint, 1.f, 1.f, false, 8);
        }

        // 3. Selection glowing border (Xbox 360 style)
        if (isSel) {
            nxui::Vec2 tl, br;
            if (ren.project3D(quad[0], tl) && ren.project3D(quad[2], br)) {
                const float fx0 = tl.x - 4.f;
                const float fy0 = tl.y - 4.f;
                const float fw  = (br.x - tl.x) + 8.f;
                const float fh  = (br.y - tl.y) + 8.f;

                const nxui::Color frameCol{0.25f, 0.72f, 1.0f, 0.95f * alpha};
                ren.drawRect(nxui::Rect{fx0, fy0, fw, 3.5f}, frameCol);
                ren.drawRect(nxui::Rect{fx0, fy0 + fh - 3.5f, fw, 3.5f}, frameCol);
                ren.drawRect(nxui::Rect{fx0, fy0, 3.5f, fh}, frameCol);
                ren.drawRect(nxui::Rect{fx0 + fw - 3.5f, fy0, 3.5f, fh}, frameCol);

                // Soft outer glow outline
                const nxui::Color glowCol{0.25f, 0.72f, 1.0f, 0.35f * alpha};
                ren.drawRect(nxui::Rect{fx0 - 2.f, fy0 - 2.f, fw + 4.f, 2.f}, glowCol);
                ren.drawRect(nxui::Rect{fx0 - 2.f, fy0 + fh, fw + 4.f, 2.f}, glowCol);
                ren.drawRect(nxui::Rect{fx0 - 2.f, fy0 - 2.f, 2.f, fh + 4.f}, glowCol);
                ren.drawRect(nxui::Rect{fx0 + fw, fy0 - 2.f, 2.f, fh + 4.f}, glowCol);
            }
        }
    }

    ren.popClipRect();
}

void IconGrid::deckPlace(float d, float& x, float& y, float& z, float& ang, float& a) {
    const float absD = std::abs(d);
    const float sign = (d >= 0.f) ? 1.f : -1.f;

    // Focused card sits prominently at x = 0, y = -0.48f, z = 2.70f (closer to camera).
    // Non-focused cards sit back at z = 3.30f, y = -0.54f, spaced by ~0.84 world units.
    if (absD <= 1.0f) {
        const float t = absD; // 0 = centered, 1 = neighbor
        const float smooth = t * t * (3.f - 2.f * t);
        x = sign * (smooth * 0.84f);
        y = -0.48f - smooth * 0.06f;
        z = 2.70f + smooth * 0.60f;
        ang = -sign * (smooth * 0.12f); // gentle yaw tilt towards center
        a = 1.0f;
    } else {
        const float extra = absD - 1.0f;
        x = sign * (0.84f + extra * 0.76f);
        y = -0.54f;
        z = 3.30f + extra * 0.12f; // subtle curve away
        ang = -sign * std::min(0.24f, 0.12f + extra * 0.03f);
        a = std::clamp(1.0f - extra * 0.22f, 0.0f, 1.0f);
    }
}

void IconGrid::deckCorners(float x, float y, float z, float ang,
                           float halfW, float halfH, nxui::Vec3 out[4]) {
    const float cosA = std::cos(ang);
    const float sinA = std::sin(ang);
    // Ordered TL, TR, BR, BL
    out[0] = { x - halfW * cosA, y + halfH, z + halfW * sinA };
    out[1] = { x + halfW * cosA, y + halfH, z - halfW * sinA };
    out[2] = { x + halfW * cosA, y - halfH, z - halfW * sinA };
    out[3] = { x - halfW * cosA, y - halfH, z + halfW * sinA };
}

void IconGrid::renderDeck(nxui::Renderer& ren) {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return;

    ren.pushClipRect(nxui::Rect{0.f, 60.f, 1280.f, 660.f});

    const float reveal = clamp01(m_layoutReveal.value());

    // Dark sleek gradient behind the card ribbon across the bottom half
    const float ribbonTop = 330.f;
    const float ribbonHeight = 720.f - ribbonTop;
    ren.drawGradientRect(
        nxui::Rect{0.f, ribbonTop, 1280.f, ribbonHeight},
        nxui::Color(0.02f, 0.03f, 0.05f, 0.10f * reveal),
        nxui::Color(0.01f, 0.01f, 0.03f, 0.88f * reveal)
    );
    ren.drawRect(
        nxui::Rect{0.f, ribbonTop + 20.f, 1280.f, 1.0f},
        nxui::Color(1.f, 1.f, 1.f, 0.05f * reveal)
    );

    // Harvest completed asynchronous cover decodes (up to 2 uploads per frame)
    int uploads = 0;
    for (auto it = m_pendingCoverDecodes.begin(); it != m_pendingCoverDecodes.end() && uploads < 2;) {
        if (it->state && it->state->done.load()) {
            try {
                if (it->future.valid()) it->future.get();
            } catch (...) {}
            auto covIt = m_flowCovers.find(it->titleId);
            if (covIt != m_flowCovers.end()) {
                if (!it->state->failed.load() && it->state->decoded.valid()) {
                    if (covIt->second.texture.loadFromDecoded(ren.gpu(), ren, it->state->decoded)) {
                        covIt->second.available = true;
                    }
                }
                covIt->second.loading = false;
            }
            it = m_pendingCoverDecodes.erase(it);
            ++uploads;
        } else {
            ++it;
        }
    }

    const float scroll = m_lineScrollOffset.value();
    const int centre = (int)std::round(scroll);
    const int focusedIndex = focusedGlobalIndex();

    struct DeckCandidate {
        int index;
        float d;
        float z;
    };
    std::vector<DeckCandidate> order;
    order.reserve(16);
    for (int i = centre - 5; i <= centre + 5; ++i) {
        const float d = (float)i - scroll;
        float x, y, z, ang, a;
        deckPlace(d, x, y, z, ang, a);
        if (a <= 0.01f) continue;
        order.push_back({flowWrap(i, n), d, z});
    }

    // Far to near (largest z first)
    std::sort(order.begin(), order.end(), [](const DeckCandidate& a, const DeckCandidate& b) {
        return a.z > b.z;
    });

    for (const auto& c : order) {
        auto& icon = m_allIcons[(size_t)c.index];
        if (!icon) continue;

        float x, y, z, ang, a;
        deckPlace(c.d, x, y, z, ang, a);

        const bool isSel = (c.index == focusedIndex);

        const float halfW = 0.36f;
        const float halfH = 0.54f;

        nxui::Vec3 quad[4];
        deckCorners(x, y, z, ang, halfW, halfH, quad);

        const float prox = std::max(0.0f, 1.0f - std::abs(c.d));
        const float selectedLift = isSel ? 0.08f : 0.f;
        const float lit = std::clamp((160.f + 95.f * prox) / 255.f + selectedLift, 0.f, 1.f);
        const float blankLit = std::clamp((24.f + 16.f * prox) / 255.f + selectedLift * 0.25f, 0.f, 1.f);

        const float alpha = m_opacity * icon->opacity() * reveal * a;
        if (alpha <= 0.01f) continue;

        const nxui::Color artTint {lit, lit, lit, alpha};
        const nxui::Color blankCol{blankLit, blankLit, blankLit, alpha};

        // Query 2:3 portrait cover art cache
        const std::uint64_t tid = icon->titleId();
        bool hasCover = false;
        nxui::Texture* coverTex = nullptr;
        if (tid != 0) {
            auto& cov = m_flowCovers[tid];
            if (!cov.checked) {
                cov.checked = true;
                cov.loading = true;
                if (m_threadPool) {
                    auto decodeState = std::make_shared<CoverDecodeState>();
                    auto work = [decodeState, tid]() {
                        try {
                            const std::string coverPath = resolveFlowCoverPath(tid);
                            if (!coverPath.empty()) {
                                decodeState->decoded = nxui::Texture::decodeFile(coverPath, 720);
                                decodeState->failed = !decodeState->decoded.valid();
                            } else {
                                decodeState->failed = true;
                            }
                        } catch (...) {
                            decodeState->failed = true;
                        }
                        decodeState->done = true;
                    };
                    PendingCoverDecode pcd;
                    pcd.titleId = tid;
                    pcd.state = decodeState;
                    pcd.future = m_threadPool->submit(std::move(work));
                    m_pendingCoverDecodes.push_back(std::move(pcd));
                }
            }
            if (cov.available && cov.texture.valid()) {
                hasCover = true;
                coverTex = &cov.texture;
            }
        }

        // Draw soft floor shadow
        const float floorY = y - halfH - 0.02f;
        nxui::Vec3 shadowQuad[4];
        deckCorners(x, floorY, z, ang, halfW * 0.96f, halfH * 0.25f, shadowQuad);
        ren.drawQuad3D(nullptr, shadowQuad, nxui::Color(0.f, 0.f, 0.f, 0.45f * alpha),
                       0.45f * alpha, 0.0f);

        // If selected: draw SteamOS signature cyan/blue glowing accent frame!
        if (isSel) {
            nxui::Vec3 glowQuad[4];
            const float glowW = halfW + 0.028f;
            const float glowH = halfH + 0.028f;
            deckCorners(x, y, z + 0.005f, ang, glowW, glowH, glowQuad);
            // Signature SteamOS neon cyan-blue
            const nxui::Color glowCol{0.10f, 0.65f, 0.98f, 0.88f * alpha};
            ren.drawQuad3D(nullptr, glowQuad, glowCol, 0.88f * alpha, 0.88f * alpha);
        }

        // Dark card chassis / backplate
        ren.drawQuad3D(nullptr, quad, blankCol);

        if (hasCover && coverTex) {
            // Full 2:3 vertical cover art
            ren.drawQuad3D(coverTex, quad, artTint);
        } else {
            // 1:1 square icon cleanly inset inside 2:3 card frame without vertical distortion
            const float iconHalf = halfW * 0.78f;
            nxui::Vec3 iconQuad[4];
            // Positioned neatly in upper-center of the card
            deckCorners(x, y + 0.06f, z - 0.004f, ang, iconHalf, iconHalf, iconQuad);
            if (icon->texture()) {
                ren.drawQuad3D(icon->texture(), iconQuad, artTint);
            }
        }
    }

    ren.popClipRect();
}

void IconGrid::coverPlace(float d, float& x, float& y, float& z, float& a) {
    const float absD = std::abs(d);
    // At z = 3.0f and focal length 960, one screen width (1280px) equals 4.0 world units.
    // Neighboring items are spaced exactly 4.0 units apart (1 screen width), matching sLaunch.
    x = d * 4.0f;
    // Cover center sits slightly above mid-screen (screenY = 300px)
    y = 0.1875f;
    // Subtle z-receding during transition slide gives a refined depth cue
    z = 3.0f + std::min(absD, 1.0f) * 0.15f;
    a = std::clamp(1.0f - absD * 0.65f, 0.0f, 1.0f);
}

void IconGrid::coverCorners(float x, float y, float z,
                            float halfW, float halfH, nxui::Vec3 out[4]) {
    // Ordered TL, TR, BR, BL
    out[0] = { x - halfW, y + halfH, z };
    out[1] = { x + halfW, y + halfH, z };
    out[2] = { x + halfW, y - halfH, z };
    out[3] = { x - halfW, y - halfH, z };
}

int IconGrid::hitTestCover(float screenX, float screenY) const {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return -1;
    const float scroll = m_lineScrollOffset.value();
    const int centre = (int)std::round(scroll);
    const int offsets[] = {0, 1, -1};
    for (int off : offsets) {
        int idx = centre + off;
        const float d = (float)idx - scroll;
        if (std::abs(d) > 1.25f) continue;
        int wrapped = flowWrap(idx, n);
        auto& icon = m_allIcons[(size_t)wrapped];
        if (!icon) continue;

        float x, y, z, a;
        coverPlace(d, x, y, z, a);

        const std::uint64_t tid = icon->titleId();
        bool hasCover = false;
        if (tid != 0) {
            auto it = m_flowCovers.find(tid);
            if (it != m_flowCovers.end() && it->second.available && it->second.texture.valid()) {
                hasCover = true;
            }
        }
        const float halfW = hasCover ? 0.44f : 0.5625f;
        const float halfH = hasCover ? 0.66f : 0.5625f;

        nxui::Vec3 quad[4];
        coverCorners(x, y, z, halfW, halfH, quad);

        nxui::Vec2 p2D[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            if (!projectPoint3D(quad[k], 1280.f, 720.f, p2D[k])) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        if (pointInQuad(screenX, screenY, p2D)) {
            return wrapped;
        }
    }
    return -1;
}

void IconGrid::renderCover(nxui::Renderer& ren) {
    const int n = (int)m_allIcons.size();
    if (n <= 0) return;

    ren.pushClipRect(nxui::Rect{0.f, 60.f, 1280.f, 660.f});

    const float reveal = clamp01(m_layoutReveal.value());

    // Dark sleek floor plane grounding the cover reflection
    const float floorTop = 490.f;
    const float floorHeight = 720.f - floorTop;
    ren.drawGradientRect(
        nxui::Rect{0.f, floorTop, 1280.f, floorHeight},
        nxui::Color(0.02f, 0.03f, 0.05f, 0.08f * reveal),
        nxui::Color(0.01f, 0.01f, 0.03f, 0.70f * reveal)
    );
    ren.drawRect(
        nxui::Rect{0.f, floorTop, 1280.f, 1.0f},
        nxui::Color(1.f, 1.f, 1.f, 0.06f * reveal)
    );

    // Harvest completed asynchronous cover decodes (up to 2 uploads per frame)
    int uploads = 0;
    for (auto it = m_pendingCoverDecodes.begin(); it != m_pendingCoverDecodes.end() && uploads < 2;) {
        if (it->state && it->state->done.load()) {
            try {
                if (it->future.valid()) it->future.get();
            } catch (...) {}
            auto covIt = m_flowCovers.find(it->titleId);
            if (covIt != m_flowCovers.end()) {
                if (!it->state->failed.load() && it->state->decoded.valid()) {
                    if (covIt->second.texture.loadFromDecoded(ren.gpu(), ren, it->state->decoded)) {
                        covIt->second.available = true;
                    }
                }
                covIt->second.loading = false;
            }
            it = m_pendingCoverDecodes.erase(it);
            ++uploads;
        } else {
            ++it;
        }
    }

    const float scroll = m_lineScrollOffset.value();
    const int centre = (int)std::round(scroll);
    const int focusedIndex = focusedGlobalIndex();

    struct CoverCandidate {
        int index;
        float d;
        float absD;
    };
    std::vector<CoverCandidate> order;
    order.reserve(3);
    for (int off = -1; off <= 1; ++off) {
        int idx = centre + off;
        const float d = (float)idx - scroll;
        if (std::abs(d) > 1.25f) continue;
        order.push_back({flowWrap(idx, n), d, std::abs(d)});
    }

    // Draw farther items first so centered cover paints last (on top)
    std::sort(order.begin(), order.end(), [](const CoverCandidate& a, const CoverCandidate& b) {
        return a.absD > b.absD;
    });

    for (const auto& c : order) {
        auto& icon = m_allIcons[(size_t)c.index];
        if (!icon) continue;

        float x, y, z, a;
        coverPlace(c.d, x, y, z, a);

        const bool isSel = (c.index == focusedIndex);

        // Query 2:3 portrait cover art cache
        const std::uint64_t tid = icon->titleId();
        bool hasCover = false;
        nxui::Texture* coverTex = nullptr;
        if (tid != 0) {
            auto& cov = m_flowCovers[tid];
            if (!cov.checked) {
                cov.checked = true;
                cov.loading = true;
                if (m_threadPool) {
                    auto decodeState = std::make_shared<CoverDecodeState>();
                    auto work = [decodeState, tid]() {
                        try {
                            const std::string coverPath = resolveFlowCoverPath(tid);
                            if (!coverPath.empty()) {
                                decodeState->decoded = nxui::Texture::decodeFile(coverPath, 720);
                                decodeState->failed = !decodeState->decoded.valid();
                            } else {
                                decodeState->failed = true;
                            }
                        } catch (...) {
                            decodeState->failed = true;
                        }
                        decodeState->done = true;
                    };
                    PendingCoverDecode pcd;
                    pcd.titleId = tid;
                    pcd.state = decodeState;
                    pcd.future = m_threadPool->submit(std::move(work));
                    m_pendingCoverDecodes.push_back(std::move(pcd));
                }
            }
            if (cov.available && cov.texture.valid()) {
                hasCover = true;
                coverTex = &cov.texture;
            }
        }

        const float halfW = hasCover ? 0.44f : 0.5625f;
        const float halfH = hasCover ? 0.66f : 0.5625f;

        nxui::Vec3 quad[4];
        coverCorners(x, y, z, halfW, halfH, quad);

        const float prox = std::max(0.0f, 1.0f - c.absD);
        const float selectedLift = isSel ? 0.08f : 0.f;
        const float lit = std::clamp((170.f + 85.f * prox) / 255.f + selectedLift, 0.f, 1.f);
        const float blankLit = std::clamp((26.f + 18.f * prox) / 255.f + selectedLift * 0.25f, 0.f, 1.f);

        const float alpha = m_opacity * icon->opacity() * reveal * a;
        if (alpha <= 0.01f) continue;

        const nxui::Color artTint {lit, lit, lit, alpha};
        const nxui::Color blankCol{blankLit, blankLit, blankLit, alpha};

        // Floor reflection (fading downwards)
        const float floorY = y - halfH - 0.03f;
        const float reflH = halfH * 0.45f;
        nxui::Vec3 reflQuad[4];
        reflQuad[0] = { x - halfW, floorY, z };
        reflQuad[1] = { x + halfW, floorY, z };
        reflQuad[2] = { x + halfW, floorY - reflH, z };
        reflQuad[3] = { x - halfW, floorY - reflH, z };

        if (hasCover && coverTex) {
            ren.drawQuad3D(coverTex, reflQuad, artTint, 0.35f * alpha, 0.0f, true);
        } else if (icon->texture()) {
            ren.drawQuad3D(icon->texture(), reflQuad, artTint, 0.35f * alpha, 0.0f, true);
        }

        // Soft floor drop shadow
        nxui::Vec3 shadowQuad[4];
        coverCorners(x, floorY + 0.01f, z, halfW * 0.98f, halfH * 0.15f, shadowQuad);
        ren.drawQuad3D(nullptr, shadowQuad, nxui::Color(0.f, 0.f, 0.f, 0.45f * alpha),
                       0.45f * alpha, 0.0f);

        // Selection glow frame
        if (isSel) {
            nxui::Vec3 glowQuad[4];
            const float glowW = halfW + 0.028f;
            const float glowH = halfH + 0.028f;
            coverCorners(x, y, z + 0.005f, glowW, glowH, glowQuad);
            // Signature cyan/blue accent glow (matching Deck/Shelf)
            const nxui::Color glowCol{0.10f, 0.65f, 0.98f, 0.88f * alpha};
            ren.drawQuad3D(nullptr, glowQuad, glowCol, 0.88f * alpha, 0.88f * alpha);
        }

        // Card chassis / plate
        ren.drawQuad3D(nullptr, quad, blankCol);

        // Artwork
        if (hasCover && coverTex) {
            ren.drawQuad3D(coverTex, quad, artTint);
        } else if (icon->texture()) {
            ren.drawQuad3D(icon->texture(), quad, artTint);
        }
    }

    ren.popClipRect();
}

void IconGrid::setXmbContext(const XmbContext& ctx) {
    m_xmbContext = ctx;
    if (m_layoutMode == AppLayoutMode::Xmb) {
        layoutXmb();
    }
}

void IconGrid::syncXmbFocusFromCurrent() {
    if (m_layoutMode != AppLayoutMode::Xmb || m_xmbCols.empty()) return;
    nxui::Widget* cur = m_focus.current();
    if (!cur) return;
    for (size_t c = 0; c < m_xmbCols.size(); ++c) {
        for (size_t i = 0; i < m_xmbCols[c].size(); ++i) {
            if (m_xmbCols[c][i] == cur) {
                m_xmbCol = static_cast<int>(c);
                m_xmbItem = static_cast<int>(i);
                if (m_xmbCol == 4)
                    m_xmbGamesRow = m_xmbItem;
                syncXmbChildRects();
                if (m_onFocusChanged)
                    m_onFocusChanged(cur);
                return;
            }
        }
    }
}

void IconGrid::stepXmb(int dCol, int dItem) {
    if (m_layoutMode != AppLayoutMode::Xmb || m_xmbCols.empty()) return;

    const int colCount = static_cast<int>(m_xmbCols.size());
    bool moved = false;

    if (dCol != 0) {
        // Walk past empty categories instead of stopping dead on one, and stop
        // at the ends rather than wrapping: on the real bar the row has a first
        // and a last category, and clamping is what makes the ends findable.
        const int stepDir = (dCol > 0) ? 1 : -1;
        int probe = m_xmbCol;
        for (int guard = 0; guard < colCount; ++guard) {
            probe += stepDir;
            if (probe < 0 || probe >= colCount)
                break;
            if (m_xmbCols[probe].empty())
                continue;
            m_xmbCol = probe;
            // Land on the first entry of the new category. Carrying the old
            // row index across columns of different lengths is what made the
            // selection appear to jump to an unrelated item. The games column
            // is the exception: it is the long one the player scrolls through,
            // so returning to it restores where they were.
            m_xmbItem = (m_xmbCol == 4)
                ? std::clamp(m_xmbGamesRow, 0,
                             std::max(0, static_cast<int>(m_xmbCols[4].size()) - 1))
                : 0;
            moved = true;
            break;
        }
    }

    if (dItem != 0 && m_xmbCol >= 0 && m_xmbCol < colCount) {
        auto& col = m_xmbCols[m_xmbCol];
        if (!col.empty()) {
            const int newItem =
                std::clamp(m_xmbItem + dItem, 0, static_cast<int>(col.size()) - 1);
            if (newItem != m_xmbItem) {
                m_xmbItem = newItem;
                moved = true;
            }
        }
    }

    if (!moved)
        return;

    if (m_xmbCol == 4)
        m_xmbGamesRow = m_xmbItem;

    // Rects first, focus second: the focus callback reads focusRect() to place
    // the selection ring, so moving focus before the rects are current would
    // draw the ring at the previous position for a frame.
    syncXmbChildRects();
    auto& col = m_xmbCols[m_xmbCol];
    if (!col.empty() && m_xmbItem < static_cast<int>(col.size())) {
        m_focus.setFocus(col[m_xmbItem]);
        if (m_onFocusChanged)
            m_onFocusChanged(col[m_xmbItem]);
    }
}

bool IconGrid::xmbFocusIsAppEntry() const {
    if (m_layoutMode != AppLayoutMode::Xmb) return false;
    // Column 4 is the one built from m_allIcons; every other column holds the
    // synthesized system shortcuts, which own no title and must not reach the
    // grid's app-only paths.
    return m_xmbCol == 4;
}

float IconGrid::xmbRowOffset(float d) {
    constexpr float kXmbSpacingV   = 42.67f;
    constexpr float kXmbAboveItem  = -1.0f;
    constexpr float kXmbActiveItem =  3.0f;
    constexpr float kXmbUnderItem  =  5.0f;

    const float active = kXmbSpacingV * kXmbActiveItem;
    if (d <= -1.0f) return kXmbSpacingV * (d + kXmbAboveItem);
    if (d >=  1.0f) return kXmbSpacingV * (d + kXmbUnderItem);
    if (d < 0.0f) {
        const float edge = kXmbSpacingV * (-1.0f + kXmbAboveItem);
        return active + (edge - active) * (-d);
    }
    const float edge = kXmbSpacingV * (1.0f + kXmbUnderItem);
    return active + (edge - active) * d;
}

nxui::Rect IconGrid::xmbCategoryRect(int columnIndex) const {
    const float d = static_cast<float>(columnIndex) - m_xmbColScroll;
    const float prox = std::max(0.0f, 1.0f - std::abs(d));
    const float zoom = kXmbZoomPassive + (kXmbZoomActive - kXmbZoomPassive) * prox;
    const float sz = kXmbIconBase * zoom;
    const float cx = kXmbAnchorX + d * kXmbSpacingH;
    return {cx - sz * 0.5f, kXmbTabY - sz * 0.5f, sz, sz};
}

int IconGrid::hitTestXmb(float screenX, float screenY) {
    if (m_xmbCols.empty()) return -1;

    // A tap on a category selects it; it never activates. Selecting a category
    // and launching whatever happened to sit at index 0 of it was how a touch
    // on the bar could fire an applet the player never picked.
    if (screenY >= kXmbTabY - 50.f && screenY <= kXmbTabY + 50.f) {
        for (size_t c = 0; c < m_xmbCols.size(); ++c) {
            const float cx = kXmbAnchorX +
                             (static_cast<float>(c) - m_xmbColScroll) * kXmbSpacingH;
            if (std::abs(screenX - cx) <= kXmbSpacingH * 0.5f) {
                if (m_xmbCols[c].empty())
                    return -1;
                if (static_cast<int>(c) != m_xmbCol) {
                    m_xmbCol = static_cast<int>(c);
                    m_xmbItem = (m_xmbCol == 4)
                        ? std::clamp(m_xmbGamesRow, 0,
                                     std::max(0, static_cast<int>(m_xmbCols[4].size()) - 1))
                        : 0;
                    syncXmbChildRects();
                    m_focus.setFocus(m_xmbCols[m_xmbCol][m_xmbItem]);
                    if (m_onFocusChanged)
                        m_onFocusChanged(m_xmbCols[m_xmbCol][m_xmbItem]);
                }
                // Handled as selection-only. findTopHit() will return the grid,
                // not the newly focused item, so FocusManager cannot interpret
                // this same tap-up as an activation.
                return 0;
            }
        }
        return -1;
    }

    // Items: first tap moves the selection onto the row, a second tap on the
    // already-selected row activates it. The band is the drawn rect widened to
    // the label, so tapping a title works as well as tapping its icon.
    if (m_xmbCol >= 0 && m_xmbCol < static_cast<int>(m_xmbCols.size())) {
        auto& col = m_xmbCols[m_xmbCol];
        for (size_t i = 0; i < col.size(); ++i) {
            if (!col[i]) continue;
            const nxui::Rect r = xmbItemRect(static_cast<int>(i));
            const float cy = r.y + r.height * 0.5f;
            const float halfBand = std::max(22.f, r.height * 0.5f);
            if (std::abs(screenY - cy) <= halfBand &&
                screenX >= kXmbAnchorX - kXmbIconBase &&
                screenX <= 1200.f) {
                // Do not activate here. findTopHit() returns this widget to
                // FocusManager, whose touch-up path implements the expected
                // rule: first tap focuses; tapping an already-focused item
                // fires A/activate. Activating during hit-testing fired once on
                // touch-down and then a second time on touch-up.
                if (static_cast<int>(i) != m_xmbItem) {
                    m_xmbItem = static_cast<int>(i);
                    if (m_xmbCol == 4)
                        m_xmbGamesRow = m_xmbItem;
                    syncXmbChildRects();
                    m_focus.setFocus(col[m_xmbItem]);
                    if (m_onFocusChanged)
                        m_onFocusChanged(col[m_xmbItem]);
                }
                return -1;
            }
        }
    }
    return -1;
}

void IconGrid::layoutXmb() {
    clearChildren();

    if (m_xmbSystemIcons.empty()) {
        m_xmbSystemIcons.resize(11);
        for (auto& sys : m_xmbSystemIcons) {
            sys = std::make_shared<GlossyIcon>();
            sys->setTag("glossy_icon");
            sys->setFocusable(true);
            sys->setVisible(true);
            sys->setCornerRadius(10.f);
        }
    }

    auto bindSysAction = [](const std::shared_ptr<GlossyIcon>& icon,
                            const std::string& title,
                            const std::string& hint,
                            nxui::Texture* tex,
                            std::function<void()> act) {
        icon->setTitle(title);
        icon->setAccessibilityHint(hint);
        icon->setTexture(tex);
        icon->clearActions();
        icon->setOnActivate(act);
        if (act) {
            icon->addAction(static_cast<uint64_t>(nxui::Button::A), act);
        }
    };

    // Settings column items
    bindSysAction(m_xmbSystemIcons[0], "Settings", "System and launcher preferences", m_xmbContext.texSettings, m_xmbContext.onOpenSettings);
    bindSysAction(m_xmbSystemIcons[1], "Theme Shop", "Customize themes and sounds", m_xmbContext.texThemes, m_xmbContext.onOpenThemeShop);
    bindSysAction(m_xmbSystemIcons[2], "Controllers", "Pair controllers and change grip", m_xmbContext.texControllers, m_xmbContext.onOpenControllers);
    bindSysAction(m_xmbSystemIcons[3], "Power", "Sleep, restart or turn off console", m_xmbContext.texPower, m_xmbContext.onOpenPower);

    // Media column items
    bindSysAction(m_xmbSystemIcons[4], "Album", "Screenshots and captured videos", m_xmbContext.texAlbum, m_xmbContext.onOpenAlbum);
    bindSysAction(m_xmbSystemIcons[5], "Media Center", "Play music and explore media", m_xmbContext.texMediaCenter, m_xmbContext.onOpenMediaCenter);

    // User column items
    bindSysAction(m_xmbSystemIcons[6], "User Page", "Account profile and activity log", m_xmbContext.texUser, m_xmbContext.onOpenUserPage);
    bindSysAction(m_xmbSystemIcons[7], "Mii Editor", "Create and manage Mii characters", m_xmbContext.texMii, m_xmbContext.onOpenMiiEditor);

    // Network column items.
    nxui::Texture* netTex = m_xmbContext.texNetwork ? m_xmbContext.texNetwork : m_xmbContext.texSettings;
    bindSysAction(m_xmbSystemIcons[8], "Internet Settings", "Configure Wi-Fi connections", netTex, m_xmbContext.onOpenNetConnect);
    // The system browser is not wired yet. The old placeholder called
    // launchNetConnect(), so selecting "Web Browser" opened Internet Settings
    // again -- a duplicate action presented as a feature. Keep the prepared
    // slot out of the column until a real Web applet launch path exists.
    bindSysAction(m_xmbSystemIcons[9], "Web Browser", "Browse the Internet",
                  m_xmbContext.texBrowser ? m_xmbContext.texBrowser : netTex,
                  m_xmbContext.onOpenWebBrowser);

    // Homebrew column items
    nxui::Texture* hbTex = m_xmbContext.texHomebrew ? m_xmbContext.texHomebrew : m_xmbContext.texAlbum;
    bindSysAction(m_xmbSystemIcons[10], "Homebrew Menu", "Launch homebrew applications (.nro)", hbTex, m_xmbContext.onOpenHbMenu);

    // Assemble 6 columns
    m_xmbCols.clear();
    m_xmbCols.resize(6);

    m_xmbColNames = {"Settings", "Media", "User", "Network", "Games", "Homebrew"};
    m_xmbColIcons = {
        m_xmbContext.texSettings,
        m_xmbContext.texAlbum,
        m_xmbContext.texUser,
        m_xmbContext.texNetwork ? m_xmbContext.texNetwork : m_xmbContext.texSettings,
        m_xmbContext.texGames,
        m_xmbContext.texHomebrew ? m_xmbContext.texHomebrew : m_xmbContext.texAlbum
    };

    m_xmbCols[0] = { m_xmbSystemIcons[0].get(), m_xmbSystemIcons[1].get(), m_xmbSystemIcons[2].get(), m_xmbSystemIcons[3].get() };
    m_xmbCols[1] = { m_xmbSystemIcons[4].get(), m_xmbSystemIcons[5].get() };
    m_xmbCols[2] = { m_xmbSystemIcons[6].get(), m_xmbSystemIcons[7].get() };
    m_xmbCols[3] = { m_xmbSystemIcons[8].get() };
    if (m_xmbContext.onOpenWebBrowser)
        m_xmbCols[3].push_back(m_xmbSystemIcons[9].get());

    std::vector<GlossyIcon*> gameIcons;
    gameIcons.reserve(m_allIcons.size());
    for (auto& icon : m_allIcons) {
        if (icon && icon->isFocusable() && (icon->titleId() != 0 || icon->entryKind() == GridEntryKind::Folder)) {
            gameIcons.push_back(icon.get());
        }
    }
    if (gameIcons.empty()) {
        for (auto& icon : m_allIcons) {
            if (icon) gameIcons.push_back(icon.get());
        }
    }
    m_xmbCols[4] = std::move(gameIcons);

    m_xmbCols[5] = { m_xmbSystemIcons[10].get() };

    for (auto& sys : m_xmbSystemIcons) {
        addChild(sys);
    }
    for (auto& icon : m_allIcons) {
        if (icon) addChild(icon);
    }

    // Navigation.
    //
    // The first version wired a custom-navigation graph between individual
    // icons and let FocusManager resolve the d-pad through it. On hardware that
    // left the bar feeling locked: the graph pointed LEFT/RIGHT at icons in
    // other columns, but those icons still carried the previous view's rects
    // and a stale focusable state, so a move either resolved to a widget that
    // is not drawn anywhere near the bar or was rejected outright -- and the
    // player saw nothing happen at all.
    //
    // Direction actions are checked on the focused widget *before* any spatial
    // search (see Application::tickFocusNavigation), so binding them here makes
    // the d-pad a direct call into stepXmb: LEFT/RIGHT change category, UP/DOWN
    // change row, and no geometry is consulted to decide where a press goes.
    for (auto& col : m_xmbCols) {
        for (GlossyIcon* cur : col) {
            if (!cur) continue;
            cur->setCustomNavigation(nxui::FocusDirection::UP, nullptr);
            cur->setCustomNavigation(nxui::FocusDirection::DOWN, nullptr);
            cur->setCustomNavigation(nxui::FocusDirection::LEFT, nullptr);
            cur->setCustomNavigation(nxui::FocusDirection::RIGHT, nullptr);
            cur->addDirectionAction(nxui::FocusDirection::LEFT,  [this]() { stepXmb(-1, 0); });
            cur->addDirectionAction(nxui::FocusDirection::RIGHT, [this]() { stepXmb(+1, 0); });
            cur->addDirectionAction(nxui::FocusDirection::UP,    [this]() { stepXmb(0, -1); });
            cur->addDirectionAction(nxui::FocusDirection::DOWN,  [this]() { stepXmb(0, +1); });
        }
    }

    if (m_xmbCol < 0 || m_xmbCol >= static_cast<int>(m_xmbCols.size()))
        m_xmbCol = 4;
    if (m_xmbCols[m_xmbCol].empty())
        m_xmbCol = 0;

    auto& activeCol = m_xmbCols[m_xmbCol];
    if (m_xmbItem < 0 || m_xmbItem >= static_cast<int>(activeCol.size()))
        m_xmbItem = 0;

    // The games column was just rebuilt from the current model and may be
    // shorter than it was, so the remembered row is re-clamped here -- at the
    // one place its bound can change -- rather than at each use site.
    m_xmbGamesRow = m_xmbCols.size() > 4
        ? std::clamp(m_xmbGamesRow, 0, std::max(0, static_cast<int>(m_xmbCols[4].size()) - 1))
        : 0;
    if (m_xmbCol == 4)
        m_xmbGamesRow = m_xmbItem;

    m_xmbColScroll = static_cast<float>(m_xmbCol);
    m_xmbItemScroll = static_cast<float>(m_xmbItem);

    // Every icon in the tree keeps whatever rect the previous view left on it,
    // and renderXmb draws from its own maths instead of those rects. The
    // selection ring, the spatial focus search and touch hit-testing all read
    // Widget::focusRect(), so without this pass they were working against the
    // paged grid's geometry: the ring framed a cell that is not on screen in
    // this view -- which is what read as "focus jumped to the whole screen" --
    // and d-pad LEFT/RIGHT resolved against those stale rects instead of the
    // columns. Placing every icon where XMB actually draws it makes all three
    // agree with the picture.
    syncXmbChildRects();

    if (!activeCol.empty()) {
        m_focus.setFocus(activeCol[m_xmbItem]);
        if (m_onFocusChanged)
            m_onFocusChanged(activeCol[m_xmbItem]);
    }
}

// The single source of truth for where an XMB icon *is*. renderXmb() draws the
// same rects from the same constants; keeping the placement here means focus,
// touch and the cursor cannot drift away from what the player sees.
void IconGrid::syncXmbChildRects() {
    if (m_layoutMode != AppLayoutMode::Xmb || m_xmbCols.empty())
        return;

    // Off-view icons are parked far outside the screen AND made non-focusable
    // rather than left at their old rect: the spatial search in FocusManager
    // walks every focusable widget in the tree, so an inactive column's icons
    // sitting at grid coordinates would win the nearest-neighbour test and
    // steal LEFT/RIGHT. Only the active column takes part in navigation.
    for (size_t c = 0; c < m_xmbCols.size(); ++c) {
        const bool active = (static_cast<int>(c) == m_xmbCol);
        auto& col = m_xmbCols[c];
        for (size_t i = 0; i < col.size(); ++i) {
            GlossyIcon* item = col[i];
            if (!item) continue;
            if (!active) {
                item->setFocusable(false);
                item->setRect({-4000.f, -4000.f, kXmbIconBase, kXmbIconBase});
                continue;
            }
            item->setFocusable(true);
            item->setRect(xmbItemRect(static_cast<int>(i)));
        }
    }
}

nxui::Rect IconGrid::xmbItemRect(int itemIndex) const {
    const float d = static_cast<float>(itemIndex) - m_xmbItemScroll;
    const float prox = std::max(0.0f, 1.0f - std::abs(d));
    const float zoom = kXmbZoomPassive + (kXmbZoomActive - kXmbZoomPassive) * prox;
    const float sz = kXmbIconBase * zoom;
    const float cy = kXmbMarginTop + kXmbIconBase * 0.5f + xmbRowOffset(d);
    return {kXmbAnchorX - sz * 0.5f, cy - sz * 0.5f, sz, sz};
}

void IconGrid::renderXmb(nxui::Renderer& ren) {
    if (m_xmbCols.empty()) return;
    ren.pushClipRect(m_rect);

    constexpr float kXmbAlphaActive = 1.0f;
    constexpr float kXmbAlphaPassive = 0.65f;
    constexpr float kXmbLabelLeft = 57.f;
    constexpr float kXmbFadeEnd = 60.f;
    constexpr float kXmbFadeStart = 181.f;
    constexpr float kXmbFadeBotEnd = 700.f;
    constexpr float kXmbFadeBotStart = 580.f;

    // 1. Draw Category Header (Screen Title at top-left)
    if (m_xmbContext.fontNormal && m_xmbCol >= 0 && m_xmbCol < static_cast<int>(m_xmbColNames.size())) {
        const std::string& catName = m_xmbColNames[m_xmbCol];
        ren.drawText(catName, {88.f, 92.f}, m_xmbContext.fontNormal, nxui::Color(1.f, 1.f, 1.f, 0.95f), 1.15f);

        if (m_xmbContext.fontSmall && !m_xmbCols[m_xmbCol].empty()) {
            char countBuf[64];
            std::snprintf(countBuf, sizeof(countBuf), "%zu items", m_xmbCols[m_xmbCol].size());
            ren.drawText(countBuf, {88.f, 132.f}, m_xmbContext.fontSmall, nxui::Color(0.72f, 0.75f, 0.82f, 0.75f), 0.68f);
        }
    }

    // 2. Category Row (Horizontal)
    for (size_t c = 0; c < m_xmbCols.size(); ++c) {
        float d = static_cast<float>(c) - m_xmbColScroll;
        float cx = kXmbAnchorX + d * kXmbSpacingH;
        if (cx < -100.f || cx > 1280.f + 100.f) continue;

        float prox = std::max(0.0f, 1.0f - std::abs(d));
        float zoom = kXmbZoomPassive + (kXmbZoomActive - kXmbZoomPassive) * prox;
        float alpha = 0.50f + 0.50f * prox;
        const nxui::Rect iconRect = xmbCategoryRect(static_cast<int>(c));

        if (prox > 0.6f) {
            float glowA = (prox - 0.6f) / 0.4f * 0.40f;
            ren.drawRoundedRect(iconRect.expanded(10.f * prox), nxui::Color(0.20f, 0.55f, 0.95f, glowA), 18.f);
        }

        nxui::Texture* catTex = (c < m_xmbColIcons.size()) ? m_xmbColIcons[c] : nullptr;
        if (catTex && catTex->valid()) {
            ren.drawTextureRounded(catTex, iconRect, 10.f * zoom, nxui::Color::white().withAlpha(alpha));
        } else {
            ren.drawRoundedRect(iconRect, nxui::Color(0.2f, 0.25f, 0.35f, alpha * 0.8f), 10.f * zoom);
        }
    }

    // 3. Entry Column (Vertical)
    if (m_xmbCol >= 0 && m_xmbCol < static_cast<int>(m_xmbCols.size())) {
        auto& col = m_xmbCols[m_xmbCol];
        const int count = static_cast<int>(col.size());
        const int firstv = std::max(0, static_cast<int>(m_xmbItemScroll) - 4);
        const int lastv = std::min(count - 1, static_cast<int>(m_xmbItemScroll) + 8);
        const float textX = kXmbAnchorX + kXmbIconBase * 0.5f + kXmbLabelLeft;

        for (int i = firstv; i <= lastv; ++i) {
            float d = static_cast<float>(i) - m_xmbItemScroll;
            float cy = kXmbMarginTop + kXmbIconBase * 0.5f + xmbRowOffset(d);
            if (cy >= kXmbFadeBotEnd) break;
            if (cy < kXmbFadeEnd) continue;

            float fade = 1.0f;
            if (cy < kXmbFadeStart) {
                fade = (cy - kXmbFadeEnd) / (kXmbFadeStart - kXmbFadeEnd);
            } else if (cy > kXmbFadeBotStart) {
                fade = (kXmbFadeBotEnd - cy) / (kXmbFadeBotEnd - kXmbFadeBotStart);
            }
            fade = std::clamp(fade, 0.f, 1.f);

            bool sel = (i == m_xmbItem && std::abs(d) < 0.5f);
            float prox = std::max(0.0f, 1.0f - std::abs(d));
            float zoom = kXmbZoomPassive + (kXmbZoomActive - kXmbZoomPassive) * prox;
            float al = kXmbAlphaPassive + (kXmbAlphaActive - kXmbAlphaPassive) * prox;
            float a = fade * al;
            float sz = kXmbIconBase * zoom;

            // Same rect the focus ring and touch use: xmbItemRect() is the one
            // definition, so the picture and the hit box cannot disagree.
            const nxui::Rect itemRect = xmbItemRect(i);

            GlossyIcon* item = col[i];
            nxui::Texture* tex = item ? item->texture() : nullptr;
            if (tex && tex->valid()) {
                ren.drawTextureRounded(tex, itemRect, 8.f * zoom, nxui::Color::white().withAlpha(a));
            } else {
                ren.drawRoundedRect(itemRect, nxui::Color(0.25f, 0.30f, 0.40f, a * 0.7f), 8.f * zoom);
            }

            if (item && item->isSuspended()) {
                nxui::Rect badgeRect{itemRect.x + itemRect.width - 10.f, itemRect.y - 2.f, 9.f, 9.f};
                ren.drawRoundedRect(badgeRect, nxui::Color(0.20f, 0.85f, 0.45f, a), 4.5f);
            }

            if (item && item->isGameCard()) {
                nxui::Rect gcRect{itemRect.x - 2.f, itemRect.y - 2.f, 9.f, 9.f};
                ren.drawRoundedRect(gcRect, nxui::Color(0.95f, 0.35f, 0.25f, a), 3.f);
            }

            if (m_xmbContext.fontNormal && item) {
                const std::string& title = item->title();
                nxui::Color titleColor = sel ? nxui::Color(1.0f, 1.0f, 1.0f, a)
                                             : nxui::Color(0.74f, 0.76f, 0.82f, a * 0.85f);
                float fontScale = sel ? 0.86f : 0.65f;
                ren.drawText(title, {textX, cy - 8.f}, m_xmbContext.fontNormal, titleColor, fontScale);
            }

            if (sel && item && m_xmbContext.fontSmall) {
                std::string sublabel = item->playtimeBadge();
                if (item->isSuspended()) {
                    sublabel = "Running";
                } else if (item->isGameCard() && sublabel.empty()) {
                    sublabel = "Game Card";
                } else if (sublabel.empty() && item->accessibilityHint().length() > 0) {
                    sublabel = item->accessibilityHint();
                }

                if (!sublabel.empty()) {
                    nxui::Color subColor(0.70f, 0.74f, 0.82f, a * 0.80f);
                    ren.drawText(sublabel, {textX, cy + 18.f}, m_xmbContext.fontSmall, subColor, 0.58f);
                }
            }
        }
    }

    ren.popClipRect();
}

void IconGrid::render(nxui::Renderer& ren) {
    if (!m_visible || m_opacity <= 0.f) return;

    if (m_layoutMode == AppLayoutMode::DynamicLine) {
        renderDynamicLine(ren);
        return;
    }

    if (m_layoutMode == AppLayoutMode::Flow) {
        renderFlow(ren);
        return;
    }

    if (m_layoutMode == AppLayoutMode::Shelf) {
        renderShelf(ren);
        return;
    }

    if (m_layoutMode == AppLayoutMode::Deck) {
        renderDeck(ren);
        return;
    }

    if (m_layoutMode == AppLayoutMode::Cover) {
        renderCover(ren);
        return;
    }

    if (m_layoutMode == AppLayoutMode::Xmb) {
        renderXmb(ren);
        return;
    }

    if (!m_children.empty() && ren.gpu().offscreenReady())
        ren.captureToOffscreen(true);

    const float reveal = clamp01(m_layoutReveal.value());
    if (reveal < 0.999f) {
        for (auto& c : m_children) {
            const float savedOp = c->opacity();
            c->setOpacity(savedOp * reveal);
            c->render(ren);
            c->setOpacity(savedOp);
        }
        return;
    }

    if (m_sliding) {
        ren.pushClipRect(m_rect);
        renderPageAt(ren, m_slidePrevPage, m_slideOutDx);
        for (auto& c : m_children) c->render(ren);
        ren.popClipRect();
        return;
    }

    for (auto& c : m_children) c->render(ren);
}

void IconGrid::onRender(nxui::Renderer&) {
}
