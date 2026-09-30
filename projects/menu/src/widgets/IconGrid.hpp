#pragma once
#include <nxui/widgets/Widget.hpp>
#include <nxui/focus/FocusManager.hpp>
#include <nxui/core/Types.hpp>
#include <nxui/core/Animation.hpp>
#include "core/AppLayoutMode.hpp"
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <future>
#include <atomic>
#include <cstdint>
#include <nxui/core/Texture.hpp>

namespace nxui {
class ThreadPool;
class Font;
}

class GlossyIcon;

class IconGrid : public nxui::Widget {
public:
    IconGrid();

    void setup(std::vector<std::shared_ptr<GlossyIcon>> icons,
               int cols, int rows,
               float cellW, float cellH,
               float padX, float padY);
    void reconfigureLayout(int cols, int rows,
                           float cellW, float cellH,
                           float padX, float padY);

    void setLayoutMode(AppLayoutMode mode);
    AppLayoutMode layoutMode() const { return m_layoutMode; }
    bool isDynamicLine() const { return m_layoutMode == AppLayoutMode::DynamicLine; }
    bool isFlow() const { return m_layoutMode == AppLayoutMode::Flow; }
    bool isShelf() const { return m_layoutMode == AppLayoutMode::Shelf; }
    bool isDeck() const { return m_layoutMode == AppLayoutMode::Deck; }
    bool isCover() const { return m_layoutMode == AppLayoutMode::Cover; }
    bool isXmb() const { return m_layoutMode == AppLayoutMode::Xmb; }
    bool is3D() const {
        return m_layoutMode == AppLayoutMode::Flow
            || m_layoutMode == AppLayoutMode::Shelf
            || m_layoutMode == AppLayoutMode::Deck
            || m_layoutMode == AppLayoutMode::Cover;
    }
    // All carousel views share one model: a single wrapping row driven by
    // m_lineScrollOffset, with the same focus bindings and the same per-item
    // rects. They differ only in how that row is drawn. Layout, navigation,
    // hit-testing, paging and animation must therefore test this rather than
    // DynamicLine alone.
    bool isCarousel() const {
        return m_layoutMode == AppLayoutMode::DynamicLine
            || m_layoutMode == AppLayoutMode::Flow
            || m_layoutMode == AppLayoutMode::Shelf
            || m_layoutMode == AppLayoutMode::Deck
            || m_layoutMode == AppLayoutMode::Cover;
    }
    bool isDynamicLineScrolling() const;
    void setDynamicLineUpTarget(nxui::Widget* target);
    // Where UP goes from the top row of the paged grid. Only the sides had a
    // target, so the folder header could not be reached with the d-pad.
    void setGridUpTarget(nxui::Widget* target);
    void setDynamicLineDownTarget(nxui::Widget* target);

    void setPage(int page);
    int  currentPage()  const { return m_page; }
    int  totalPages()   const { return m_totalPages; }
    int  columns()      const { return m_cols; }
    int  rowsPerPage()  const { return m_rows; }
    int  iconsPerPage() const { return m_cols * m_rows; }

    nxui::FocusManager& focusManager() { return m_focus; }
    const std::vector<std::shared_ptr<GlossyIcon>>& allIcons() const { return m_allIcons; }

    std::vector<GlossyIcon*> pageIcons() const;

    void setThreadPool(nxui::ThreadPool* pool) { m_threadPool = pool; }
    void clearFlowCovers();
    void preloadFlowCoversAround(int centerIdx);

    struct XmbContext {
        nxui::Font* fontNormal = nullptr;
        nxui::Font* fontSmall = nullptr;

        nxui::Texture* texSettings = nullptr;
        nxui::Texture* texThemes = nullptr;
        nxui::Texture* texControllers = nullptr;
        nxui::Texture* texPower = nullptr;
        nxui::Texture* texAlbum = nullptr;
        nxui::Texture* texMediaCenter = nullptr;
        nxui::Texture* texUser = nullptr;
        nxui::Texture* texMii = nullptr;
        nxui::Texture* texNetwork = nullptr;
        nxui::Texture* texBrowser = nullptr;
        nxui::Texture* texGames = nullptr;
        nxui::Texture* texHomebrew = nullptr;

        std::function<void()> onOpenSettings;
        std::function<void()> onOpenThemeShop;
        std::function<void()> onOpenControllers;
        std::function<void()> onOpenPower;
        std::function<void()> onOpenAlbum;
        std::function<void()> onOpenMediaCenter;
        std::function<void()> onOpenUserPage;
        std::function<void()> onOpenMiiEditor;
        std::function<void()> onOpenNetConnect;
        std::function<void()> onOpenWebBrowser;
        std::function<void()> onOpenHbMenu;

        nxui::Widget* upTarget = nullptr;
        std::vector<nxui::Widget*> leftTargets;
        std::vector<nxui::Widget*> rightTargets;
    };

    // XMB geometry. Rendering, focus rects and touch hit-testing all derive
    // from these, so a tweak here moves the picture and everything that has to
    // line up with it together.
    static constexpr float kXmbAnchorX     = 352.f;
    static constexpr float kXmbSpacingH    = 128.f;
    static constexpr float kXmbTabY        = 223.5f;
    static constexpr float kXmbIconBase    = 85.f;
    static constexpr float kXmbMarginTop   = 181.f;
    static constexpr float kXmbZoomActive  = 1.0f;
    static constexpr float kXmbZoomPassive = 0.55f;

    void setXmbContext(const XmbContext& ctx);
    void stepXmb(int dCol, int dItem);
    void setXmbPosition(int col, int item = -1);
    int xmbCol() const { return m_xmbCol; }
    int xmbItem() const { return m_xmbItem; }
    int xmbColumnCount() const { return static_cast<int>(m_xmbCols.size()); }
    // Index into m_allIcons that the icon streamer should centre its window on
    // while XMB is active, even when a system category currently holds focus.
    // -1 when there is nothing to centre on.
    int xmbStreamCenterIndex() const;
    void syncXmbFocusFromCurrent();
    // True when the focused XMB entry is a real app/folder tile rather than one
    // of the synthesized system shortcuts, so the app can keep the grid-only
    // behaviour (title pill, options menu, edit mode) off the system columns.
    bool xmbFocusIsAppEntry() const;

    int hitTest(float screenX, float screenY) const;
    nxui::Widget* findTopHit(float x, float y) override;
    nxui::Rect focusedDisplayRect() const;
    nxui::Rect gridSpanRect(int globalIndex, int columns, int rows) const;
    void setGridSideTargets(std::vector<nxui::Widget*> left,
                            std::vector<nxui::Widget*> right);

    int focusedGlobalIndex() const;
    bool focusGlobalIndex(int idx);
    bool swapSlots(int a, int b);

    void startAppearAnimation();

    void startPageTransition(int targetPage);
    void startWaveTransition(int targetPage);
    bool isTransitioning() const { return m_sliding; }

    void setSlideTransition(bool enabled) { m_slideTransition = enabled; }
    void setEdgePaging(bool enabled) { m_edgePaging = enabled; }
    void onEdgePage(std::function<void(int dir)> cb) { m_onEdgePage = std::move(cb); }
    void onEdgePageHold(std::function<bool(int dir)> cb) { m_onEdgePageHold = std::move(cb); }

    void onPageSwitched(std::function<void()> cb) { m_onPageSwitched = std::move(cb); }
    void onFocusChanged(std::function<void(nxui::Widget*)> cb) { m_onFocusChanged = std::move(cb); }

    void render(nxui::Renderer& ren) override;

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

private:
    void layoutPage();
    void layoutLine();
    void positionPage(int page, float dx);
    void renderPageAt(nxui::Renderer& ren, int page, float dx);
    void renderDynamicLine(nxui::Renderer& ren);

    // Flow: a 3D coverflow row drawn with nxui's drawQuad3D primitive.
    //
    // Focus, navigation and activation are the carousel's: Flow reuses
    // layoutLine()'s LEFT/RIGHT wrap bindings and the same GlossyIcon
    // callbacks, so it is a presentation layer over the identical model and
    // adds no lifecycle path of its own. Only the drawing differs.
    void renderFlow(nxui::Renderer& ren);
    // Shelf: an Xbox 360 NXE-style 3D library row with prominent front-left
    // selection, receding diagonal perspective, reflections, and glow frame.
    void renderShelf(nxui::Renderer& ren);
    static void shelfPlace(float d, float& x, float& y, float& z, float& a);
    // Deck: a SteamOS/Steam Deck style card ribbon with prominent hero artwork,
    // card depth pop-out, and glowing accent frame.
    void renderDeck(nxui::Renderer& ren);
    static void deckPlace(float d, float& x, float& y, float& z, float& ang, float& a);
    static void deckCorners(float x, float y, float z, float ang,
                            float halfW, float halfH, nxui::Vec3 out[4]);
    // Cover: sLaunch-style fullscreen single-cover showcase with screen-width
    // horizontal slide paging, reflection, glow frame, and 2:3 or 1:1 artwork.
    void renderCover(nxui::Renderer& ren);
    static void coverPlace(float d, float& x, float& y, float& z, float& a);
    static void coverCorners(float x, float y, float z,
                             float halfW, float halfH, nxui::Vec3 out[4]);

    // XMB: PSP/PS3 cross-media bar with horizontal categories and vertical items.
    void layoutXmb();
    void renderXmb(nxui::Renderer& ren);
    int hitTestXmb(float screenX, float screenY);
    static float xmbRowOffset(float d);
    // Places every icon at the rect XMB actually draws it into, and takes the
    // inactive columns out of the focus tree. Called whenever the column, the
    // item or the model changes.
    void syncXmbChildRects();
    nxui::Rect xmbItemRect(int itemIndex) const;
    nxui::Rect xmbCategoryRect(int columnIndex) const;

    int hitTestFlow(float screenX, float screenY) const;
    int hitTestShelf(float screenX, float screenY) const;
    int hitTestDeck(float screenX, float screenY) const;
    int hitTestCover(float screenX, float screenY) const;
    static bool projectPoint3D(const nxui::Vec3& p, float screenW, float screenH, nxui::Vec2& out);
    static bool pointInQuad(float px, float py, const nxui::Vec2 pts[4]);
    nxui::Rect projected3DIconRect(int index) const;
    // Places one case by its signed distance from the row centre, saturating at
    // one item out. That saturation is what gives coverflow a single upright
    // face against a receding wall rather than a smooth arc.
    static void flowPlace(float p, float& x, float& z, float& angle);
    // A face of the 3D case box in its own local coordinates: lx runs across the face,
    // lz is depth. Watertight geometry so front, left spine, right edge, and back
    // share identical coordinates at their seams.
    static void flowFace(float x, float z, float angle,
                         float lx0, float lz0, float lx1, float lz1,
                         float halfH, nxui::Vec3 out[4]);
    // Corners of a case at (x, z) swung by angle, ordered TL, TR, BR, BL.
    static void flowCorners(float x, float z, float angle,
                            float halfW, float halfH, nxui::Vec3 out[4]);
    void bindEdgeActions(int start, int end);
    void bindGridNavigation(int start, int end);
    nxui::Rect dynamicIconRect(int index, float* outScale = nullptr,
                               float* outOpacity = nullptr,
                               float* outDistance = nullptr) const;
    float pageStride() const;

    // Carousel render scratch. Reused across frames so drawing the line does
    // not allocate every frame, and each survivor keeps the rect that was
    // already computed for it instead of recomputing an identical one.
    struct RenderCandidate {
        int index;
        float absD;
        float d;
        float s;
        float a;
        nxui::Rect rect;
    };
    mutable std::vector<RenderCandidate> m_lineRenderScratch;

    // Flow render scratch: the visible virtual range, ordered outside-in. There
    // is no depth buffer, so this ordering is the only depth information the
    // painter has.
    struct FlowCandidate {
        int   index;      // folded index into m_allIcons
        float p;          // signed distance from the row centre, in items
        float z;          // depth after placement, for the outside-in sort
    };
    mutable std::vector<FlowCandidate> m_flowRenderScratch;
    // Wall-clock seconds, sampled once per frame so every case in the row is
    // placed against the same instant.
    float m_flowClock = 0.f;

    struct FlowCoverEntry {
        nxui::Texture texture;
        bool checked = false;
        bool available = false;
        bool loading = false;
    };
    std::unordered_map<std::uint64_t, FlowCoverEntry> m_flowCovers;
    nxui::ThreadPool* m_threadPool = nullptr;
    struct CoverDecodeState {
        nxui::DecodedImage decoded;
        std::atomic<bool> done{false};
        std::atomic<bool> failed{false};
    };
    struct PendingCoverDecode {
        std::uint64_t titleId = 0;
        std::shared_ptr<CoverDecodeState> state;
        std::future<void> future;
    };
    std::vector<PendingCoverDecode> m_pendingCoverDecodes;

    // Inputs the carousel layout was last computed against. Used to skip the
    // full-library rect rebuild while the line is at rest.
    int m_lineLayoutCacheCount = -1;
    float m_lineLayoutCacheOffset = 0.f;
    float m_lineLayoutCacheReveal = -1.f;
    nxui::Rect m_lineLayoutCacheRect{};

    std::vector<std::shared_ptr<GlossyIcon>> m_allIcons;
    nxui::FocusManager m_focus;

    AppLayoutMode m_layoutMode = AppLayoutMode::Grid;
    // Ring size the carousel offset was last anchored against. The offset is an
    // index into that ring, so it means nothing once the ring changes size.
    int m_lineRingCount = -1;
    float lineRingDelta(float from, float to) const;
    nxui::AnimatedFloat m_lineScrollOffset{0.f};
    nxui::AnimatedFloat m_layoutReveal{1.f};
    nxui::Widget* m_lineUpTarget = nullptr;
    nxui::Widget* m_gridUpTarget = nullptr;
    nxui::Widget* m_lineDownTarget = nullptr;
    std::vector<nxui::Widget*> m_gridLeftTargets;
    std::vector<nxui::Widget*> m_gridRightTargets;

    // XMB Cross-Media Bar state
    XmbContext m_xmbContext;
    std::vector<std::shared_ptr<GlossyIcon>> m_xmbSystemIcons;
    std::vector<std::vector<GlossyIcon*>> m_xmbCols;
    std::vector<std::string> m_xmbColNames;
    std::vector<nxui::Texture*> m_xmbColIcons;
    int m_xmbCol = 4;
    int m_xmbItem = 0;
    // Row last selected in the games column. Kept so leaving the games category
    // and coming back restores the player's place, and so the icon streamer has
    // somewhere sensible to centre while a system category is selected.
    int m_xmbGamesRow = 0;
    float m_xmbColScroll = 4.0f;
    float m_xmbItemScroll = 0.0f;

    int m_cols = 5, m_rows = 3;
    int m_page = 0, m_totalPages = 1;
    float m_cellW = 200, m_cellH = 200;
    float m_padX  = 20,  m_padY  = 20;
    float m_originX = 0, m_originY = 0;
    static constexpr float kLineScrollDuration = 0.34f;

    bool  m_slideTransition = false;
    bool  m_edgePaging      = false;
    bool  m_sliding         = false;
    int   m_slidePrevPage   = 0;
    int   m_slideDir        = 1;
    float m_slideT          = 0.f;
    float m_slideInDx       = 0.f;
    float m_slideOutDx      = 0.f;
    static constexpr float kSlideDuration = 0.30f;

    std::function<void()> m_onPageSwitched;
    std::function<void(nxui::Widget*)> m_onFocusChanged;
    std::function<void(int)> m_onEdgePage;
    std::function<bool(int)> m_onEdgePageHold;
};
