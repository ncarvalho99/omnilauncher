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
    // Both carousel views share one model: a single wrapping row driven by
    // m_lineScrollOffset, with the same focus bindings and the same per-item
    // rects. They differ only in how that row is drawn. Layout, navigation,
    // hit-testing, paging and animation must therefore test this rather than
    // DynamicLine alone, or Flow silently falls back to paged-grid behaviour.
    bool isCarousel() const {
        return m_layoutMode == AppLayoutMode::DynamicLine
            || m_layoutMode == AppLayoutMode::Flow;
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

    int hitTest(float screenX, float screenY) const;
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
    std::function<void(int)> m_onEdgePage;
    std::function<bool(int)> m_onEdgePageHold;
};
