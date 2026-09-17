#pragma once
#include <nxui/core/Types.hpp>
#include <nxui/core/GpuDevice.hpp>
#ifdef NXUI_BACKEND_DEKO3D
#include <deko3d.hpp>
#endif
#include <cstdint>
#include <vector>
#include <string>

namespace nxui {

class Texture;
class Font;

struct Vertex2D {
    float x, y;         // Position
    float u, v;         // Texture coordinate
    float r, g, b, a;   // Color (premultiplied alpha)
    // Rounded-shape mask, evaluated in the fragment shader. Describing it here
    // rather than in the fragment uniform block is what lets a run of rounded
    // shapes share a draw call; when it lived in the block, each shape closed
    // the batch. Zero radius means the vertex belongs to an unmasked shape,
    // which is every other draw.
    float sx, sy;       // Position relative to the shape centre, pixels
    float hx, hy;       // Shape half extent, pixels
    float rad;          // Corner radius, pixels
    float thick;        // Stroke width, pixels; 0 fills
};
static_assert(sizeof(Vertex2D) == 56);

struct VsUniforms {
    float projection[16];   // Ortho matrix (top-left origin)
};

struct FsUniforms {
    int32_t useTexture;
    float   param1;
    float   param2;
    float   param3;
    float   extra[4 * 4 * 3];
};

// Shader program IDs
enum class ShaderProgram {
    Basic,
    Backdrop,
    BlurH,
    BlurV,
    Wave,
    LiquidGlass,
    Gradient,
    Count
};

struct LiquidGlassSettings {
    float refractionIntensity = 0.08f;
    float blurIntensity = 0.65f;
    float noiseIntensity = 0.0f;

    float glowIntensity = 0.22f;
    float saturation = 0.96f;
    float opacityMultiplier = 0.28f;
    float roughness = 0.015f;

    float animSpeed = 0.0f;
    float time = 0.0f;
    float powerFactor = 6.0f;
    float fPower = 1.0f;

    float refA = 0.7f;
    float refB = 2.3f;
    float refC = 5.2f;
    float refD = 6.9f;

    float glowWeight = 0.14f;
    float glowBias = -0.02f;
    float glowEdge0 = 0.12f;
    float glowEdge1 = -0.08f;

    Color tintBoost {1.f, 1.f, 1.f, 0.8f};
};

// Renderer
class Renderer {
public:
    explicit Renderer(GpuDevice& gpu);
    ~Renderer();

    /// Set the base directory for compiled shader (.dksh) files.
    /// Must be called BEFORE initialize(). Default: "romfs:/shaders/"
    static void setShaderBasePath(const std::string& path) { s_shaderBasePath = path; }
    static const std::string& shaderBasePath() { return s_shaderBasePath; }

    bool initialize();

    // Frame scope
    void beginFrame();

    // Per-frame GPU submission counters, sampled at the previous beginFrame.
    // The settings overlay drops to 10-15 fps and the cause is not obvious from
    // reading the code — every glass panel forces a flush and a pipeline
    // rebind, but so does the cheaper frosted path, so the two guesses I made
    // from structure alone were both wrong. These make it measurable.
    uint32_t lastFrameDrawCalls()     const { return m_lastFrameDrawCalls; }
    uint32_t lastFramePipelineBinds() const { return m_lastFramePipelineBinds; }
    uint32_t lastFrameVertices()      const { return m_lastFrameVertices; }

    // Fullscreen work that does not scale with draw count. GPU time sits near
    // 30ms whether the frame submits 119 draws or 144, which is the signature
    // of a fixed cost. applyBlur runs 15 iterations of two fullscreen passes
    // in this overlay, and captureToOffscreen blits the framebuffer — counting
    // them says whether the cache that is supposed to stop them is working.
    uint32_t lastFrameBlurPasses() const { return m_lastFrameBlurPasses; }
    uint32_t lastFrameCaptures()   const { return m_lastFrameCaptures; }

    // Keep the offscreen backdrop capture across frames. Only set this while
    // whatever the glass samples is genuinely static, or the refraction will
    // show a stale scene.
    void setHoldOffscreenCapture(bool hold) { m_holdOffscreenCapture = hold; }
    bool holdOffscreenCapture() const       { return m_holdOffscreenCapture; }

    // --- Diagnostic draw journal -------------------------------------------
    //
    // The Plaza dimming artifact reproduces losslessly in the native GPU
    // framebuffer dump: whole frames (or the exact right half, split at
    // x = 640.0) come back multiplied by a constant factor with every pixel of
    // UI geometry still intact. A likely author is a CPU-recorded dimming draw
    // (a fullscreen translucent black quad from a scrim/fade path); alternatives
    // include a textured/computed draw or a GPU state/synchronisation hazard.
    // Reading the code cannot tell which path executed in a glitched frame.
    //
    // The journal records what the CPU actually submitted for the frame the
    // dump captured, so the dumped pixels can be read against their own
    // command list instead of against a hypothesis. If a glitched frame's
    // journal contains a fullscreen dark drawRect, that caller-level path is
    // identified. If it does not, that specific mechanism is excluded for the
    // frame; textured/computed draws and unobserved GPU state remain possible,
    // while recorded scissor, target, clear and swapchain slot narrow them.
    //
    // Recording is off unless a dump is running, so the normal path pays one
    // predicted branch per draw and allocates nothing: the backing store is
    // reserved once in the constructor.
    enum class JournalKind : uint16_t {
        Primitive = 0, Batch, Dimmer, Clear, BindTarget, RestoreTarget,
        ClipPush, ClipPop, Capture, BlurPass, Present,
    };

    struct DrawJournalEntry {
        JournalKind kind = JournalKind::Primitive;
        uint16_t shader  = 0;
        int16_t  target  = -1;      // -1 = backbuffer, else offscreen index
        int16_t  texSlot = -1;
        uint32_t verts   = 0;
        float    x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;   // batch bounds
        float    minR = 0.f, maxR = 0.f;
        float    minG = 0.f, maxG = 0.f;
        float    minB = 0.f, maxB = 0.f;
        float    minA = 0.f, maxA = 0.f;                    // all-vertex ranges
        float    minU = 0.f, maxU = 0.f;
        float    minV = 0.f, maxV = 0.f;                    // sampled UV range
        float    sx = 0.f, sy = 0.f, sw = 0.f, sh = 0.f;   // scissor in effect
    };

    void setDrawJournalEnabled(bool on) { m_journalEnabled = on; }
    bool drawJournalEnabled() const     { return m_journalEnabled; }

    // Names the widget currently drawing, so a corrupted vertex can be blamed
    // on a caller rather than on the geometry helper it happened to pass
    // through. Every sample recorded so far reported `site=quad`, which
    // identifies addQuad and no further. The tag is a borrowed string literal,
    // not a copy: callers pass a compile-time constant that outlives the frame.
    void setDrawTag(const char* tag) { m_drawTag = tag ? tag : ""; }
    const char* drawTag() const      { return m_drawTag; }

    // Scoped form, so an early return cannot leave a stale tag behind.
    struct DrawTagScope {
        Renderer&   r;
        const char* prev;
        DrawTagScope(Renderer& rr, const char* t) : r(rr), prev(rr.m_drawTag) {
            r.setDrawTag(t);
        }
        ~DrawTagScope() { r.m_drawTag = prev; }
    };

    // Per-frame scene population, reported by the screen that owns it. The
    // artifact grows more frequent the longer Plaza runs, so whatever
    // accumulates has to be measurable per frame to be identified.
    void setPlazaCounts(uint32_t miis, uint32_t pedestals, uint32_t bubbles) {
        m_sceneMiis = miis; m_scenePedestals = pedestals;
        m_sceneBubbles = bubbles;
    }

    // The background layer owns the shape field and ambient Miis; the Plaza
    // screen owns the rest. Split so neither clears the other's counts.
    void setBackgroundCounts(uint32_t ambient, uint32_t shapes) {
        m_sceneAmbient = ambient; m_sceneShapes = shapes;
    }

    // Free-form probe line, for a widget to report its own state at the moment
    // it emits something the journal flagged.
    //
    // The corrupted geometry is now pinned to MiiFigure's floor shadow, and
    // every corrupted value is a small constant scaled by exactly 2^64. What
    // the journal cannot see is which input produced it: the renderer only
    // receives the finished coordinates. The widget writes its own inputs here
    // so the faulty variable is named rather than inferred. Lines are dropped
    // once the per-frame budget is reached, and the drop is reported.
    void addProbe(const char* text) {
        if (!m_journalEnabled || !text) return;
        if (m_probes.size() >= kProbeCap) { ++m_probesDropped; return; }
        m_probes.emplace_back(text);
    }
    bool probeBudgetLeft() const {
        return m_journalEnabled && m_probes.size() < kProbeCap;
    }

    // Monotonic frame counter, incremented by beginFrame. Written into the
    // journal header so a dumped frame can be proven to line up with the
    // journal that claims to describe it, rather than assumed to.
    uint64_t frameSerial() const { return m_frameSerial; }

    // Human-readable dump of the journal as it stands. Called after
    // takeFrameDump() and before the next beginFrame(), it describes exactly
    // the frame whose pixels were just retrieved.
    std::string formatDrawJournal() const;

    void endFrame();

    // 2D drawing
    void drawRect(const Rect& r, const Color& c);
    void drawRectOutline(const Rect& r, const Color& c, float thickness = 1.f);
    void drawRoundedRect(const Rect& r, const Color& c, float radius);
    void drawRoundedRectOutline(const Rect& r, const Color& c, float radius, float thickness = 1.f);
    void drawFrostedInset(const Rect& r, const Color& tint, const Color& border,
                          const Color& highlight, float radius, float opacity = 1.f);
    void drawCircle(const Vec2& center, float radius, const Color& c, int segments = 32);
    void drawTriangle(const Vec2& p1, const Vec2& p2, const Vec2& p3, const Color& c);
    void drawLine(const Vec2& from, const Vec2& to, const Color& c, float thickness = 1.f);
    void drawGradientRect(const Rect& r, const Color& top, const Color& bottom);
    void drawTexture(const Texture* tex, const Rect& dest, const Color& tint = Color::white());
    void drawTextureSub(const Texture* tex, const Rect& src, const Rect& dest, const Color& tint = Color::white());
    void drawTextureRounded(const Texture* tex, const Rect& dest, float radius, const Color& tint = Color::white());
    void drawTextureSubRounded(const Texture* tex, const Rect& src, const Rect& dest,
                               float radius, const Color& tint = Color::white());
    void drawTextureRoundedSub(const Texture* tex, const Rect& src, const Rect& dest,
                               float radius, const Color& tint = Color::white());
    void drawText(const std::string& text, const Vec2& pos, Font* font, const Color& color, float scale = 1.f);

    // 3D quads
    //
    // A pinhole camera at the origin looking down +Z, with Y up in view space,
    // projected on the CPU. Vertex2D carries no depth and the pipeline has no
    // depth buffer, so these emit ordinary screen-space triangles into the same
    // batch as every other draw: correctness depends entirely on the caller's
    // draw order, painting far to near.
    //
    // Positions are in world units. A perspective-correct textured quad needs
    // subdivision because the vertex format cannot carry w, so the mapping is
    // corrected geometrically instead - see drawQuad3D.
    static constexpr float kFocal = 900.f;   // pixels, over a 720-tall layout
    static constexpr float kNearZ = 0.05f;

    // Projects one world-space point to layout-space pixels. Returns false for
    // a non-finite point or one on/behind the near plane.
    bool project3D(const Vec3& p, Vec2& out) const;

    // A textured quad in world space, subdivided so affine interpolation stays
    // close enough to projective to be invisible.
    //
    // `corners` is ordered top-left, top-right, bottom-right, bottom-left.
    // A null `tex` fills with flat colour. `strips` is the horizontal
    // subdivision, clamped to [1, 32]; the vertical subdivision is chosen from
    // how much the quad's depth actually varies. `alphaTop`/`alphaBottom` are
    // the final per-vertex alpha endpoints (the tint supplies RGB), which is
    // what draws a reflection. `flipV` mirrors the texture vertically. `uv`, when
    // given, is the {u0, v0, u1, v1} sub-rect to sample.
    void drawQuad3D(const Texture* tex, const Vec3 corners[4], const Color& tint,
                    float alphaTop = 1.f, float alphaBottom = 1.f,
                    bool flipV = false, int strips = 1, const float uv[4] = nullptr);

    // Post-processing
    void captureToOffscreen(bool reuseIfValid = false);
    void captureToOffscreenSharp();
    void copyOffscreen(int srcTarget, int dstTarget);
    void drawOffscreen(int target, const Rect& dest, const Color& tint = Color::white());
    void drawOffscreenRounded(int target, const Rect& dest, float radius,
                              const Color& tint = Color::white());
    void drawLiquidGlass(int target, const Rect& panelRect, float radius,
                         const Color& tint, float opacity = 1.f, float shade = 0.f);
    void applyBlur(float radius = 1.0f, int passes = 2);
    void applyBlurBetween(int a, int b, float radius, int passes);

    // Draws into an offscreen while keeping screen coordinates: the viewport is
    // the target's own size but the projection stays the screen's, so a layer
    // laid out in 1280x720 lands scaled onto a 640x360 target instead of being
    // clipped to its top-left quarter.
    void beginScreenSpaceTarget(int offscreenIdx, const Color& clear = Color(0.f, 0.f, 0.f, 0.f));
    void endScreenSpaceTarget();
    void applyWave(float time, float amplitude, float frequency);

    // Switch shader program (flushes current batch)
    void useShader(ShaderProgram prog);

    // Push custom FS uniforms (flushes current batch)
    void pushFsUniforms(const FsUniforms& fs);

    // Scissor/clip stack
    void pushClipRect(const Rect& r);
    void popClipRect();

    // Screen dimensions
    int width()  const { return m_gpu.width(); }
    int height() const { return m_gpu.height(); }

    // Flush current batch
    void flush();

    LiquidGlassSettings& liquidGlassSettings() { return m_liquidGlassSettings; }
    const LiquidGlassSettings& liquidGlassSettings() const { return m_liquidGlassSettings; }
    void resetLiquidGlassSettings();
    void setLiquidGlassDebugRawBackdrop(bool enabled) { m_liquidGlassDebugRawBackdrop = enabled; }
    bool liquidGlassDebugRawBackdrop() const { return m_liquidGlassDebugRawBackdrop; }

    // Debug
    uint32_t vertexCount() const { return m_vtxCount; }
    void setBoxWireframeEnabled(bool enabled) { m_boxWireframeEnabled = enabled; }
    bool boxWireframeEnabled() const { return m_boxWireframeEnabled; }

    // Texture descriptor management
#ifdef NXUI_BACKEND_DEKO3D
    int registerTexture(const dk::ImageView& view);
    // Devolve o slot para reuso. Sem isto o registrador so avancava: cada tema
    // aplicado consumia um slot por quadro e nunca os liberava, entao a sexta
    // troca de tema esgotava os 2048 e as texturas seguintes nasciam invalidas.
    void releaseTextureSlot(int slot);
    void updateTexture(int slot, const dk::ImageView& view);
    void reclaimReleasedTextureSlotsAfterIdle();
#endif
    void bindTexture(int slot);
    void resetTextureSlots();

    GpuDevice& gpu() { return m_gpu; }

    // Offscreen descriptor slots
    int offscreenDescSlot(int target) const { return m_offDescSlot[target]; }

private:
    // Emit geometry helpers
    // Rounded fills are a single quad masked by the fragment shader. addVertex
    // stamps this onto every vertex it emits; zero radius means no mask, which
    // is the state every other draw runs in.
    Vec2  m_shapeCentre {0.f, 0.f};
    Vec2  m_shapeHalf {0.f, 0.f};
    float m_shapeRadius = 0.f;
    float m_shapeThickness = 0.f;

    // Redundant-state suppression for flush(). The command buffer is rebuilt
    // every frame, so all of it resets in beginFrame; nothing here may outlive
    // a frame.
    static constexpr uint32_t kFsPushBytes = sizeof(int32_t) * 4;
    static constexpr int kTexSlotUnset = -2;   // -1 is the untextured slot
    unsigned char m_fsCache[kFsPushBytes] {};
    bool m_fsCacheValid = false;
    int  m_boundTexSlot = kTexSlotUnset;
    bool m_vtxBufferBound = false;

    // Emits `quad` with the corner mask active, then clears it. A non-zero
    // thickness strokes a band inside the edge instead of filling the shape.
    void drawRoundedMasked(const Rect& dest, float radius, const Color& c,
                           const Rect& uv, float thickness = 0.f);

    // Arms and disarms the mask around a run of quads, for shapes that cover
    // themselves with more than one.
    void beginShape(const Rect& dest, float radius, float thickness);
    void endShape();

    uint32_t m_frameDrawCalls = 0;
    uint32_t m_framePipelineBinds = 0;
    uint32_t m_peakVtxCount = 0;
    uint32_t m_lastFrameDrawCalls = 0;
    uint32_t m_lastFramePipelineBinds = 0;
    uint32_t m_lastFrameVertices = 0;
    uint32_t m_frameBlurPasses = 0;
    uint32_t m_frameCaptures = 0;
    uint32_t m_lastFrameBlurPasses = 0;
    uint32_t m_lastFrameCaptures = 0;
    bool     m_holdOffscreenCapture = false;

    // Diagnostic draw journal. See setDrawJournalEnabled.
    // Measured Plaza frames stay well below this even with one record per
    // drawRect plus one per submitted batch. The cap is deliberately generous
    // because truncating before a late overlay would make a zero verdict
    // inconclusive; if it is ever exceeded the header says `dropped=N` and the
    // analyser must not treat that frame as negative evidence.
    static constexpr size_t kJournalCap = 2048;
    bool     m_journalEnabled = false;
    uint64_t m_frameSerial = 0;
    int      m_journalTarget = -1;             // current render target
    Rect     m_journalScissor {0.f, 0.f, 0.f, 0.f};
    // Cached-memory shadow of the vertex fields the journal inspects, written
    // alongside each vertex so the journal never re-reads uncached GPU memory.
    // See addVertex for why that re-read cannot be trusted.
    struct VtxShadow { float x, y, r, g, b, a; };
    static constexpr uint32_t kVtxShadowCap = 16384;
    std::vector<VtxShadow> m_vtxShadow;
    uint32_t m_journalNonQuadBatches = 0;
    uint32_t m_journalUnshadowed = 0;

    // First few vertices that arrive at addVertex already non-finite or wildly
    // out of range, captured as raw bits together with the state that produced
    // them. See addVertex for why this is recorded at the entry point.
    struct BadVertex {
        uint32_t xBits = 0, yBits = 0;
        uint32_t vtxIndex = 0, batchStart = 0;
        uint16_t site = 0, shader = 0;
        const char* tag = "";
        float radius = 0.f, thickness = 0.f;
        float centreX = 0.f, centreY = 0.f;
        float halfX = 0.f, halfY = 0.f;
        float a = 0.f;
    };
    static constexpr uint32_t kBadVertexSamples = 8;
    BadVertex m_journalBadVertex[kBadVertexSamples] {};
    uint32_t  m_journalBadSamples = 0;
    uint32_t  m_journalBadVertices = 0;

    // Identifies which emission helper is currently adding vertices, so a bad
    // coordinate can be attributed to a caller rather than guessed at.
    enum class EmitSite : uint16_t {
        None = 0, Quad, QuadGrad, Quad3D, RoundedMasked, RoundedOutline,
        Circle, Triangle, Line, Text, Offscreen, Glass, Blur,
    };
    EmitSite m_emitSite = EmitSite::None;
    const char* m_drawTag = "";

    uint32_t m_sceneMiis = 0, m_scenePedestals = 0, m_sceneBubbles = 0;
    uint32_t m_sceneAmbient = 0, m_sceneShapes = 0;

    static constexpr size_t kProbeCap = 24;
    std::vector<std::string> m_probes;
    uint32_t m_probesDropped = 0;

    // Sets the emission site for the duration of one helper and restores the
    // previous value, so nested helpers report the innermost one.
    struct EmitSiteScope {
        Renderer& r;
        EmitSite  prev;
        EmitSiteScope(Renderer& rr, EmitSite s) : r(rr), prev(rr.m_emitSite) {
            r.m_emitSite = s;
        }
        ~EmitSiteScope() { r.m_emitSite = prev; }
    };
    bool     m_journalBackbufferClearSeen = false;
    Color    m_journalBackbufferClear {0.f, 0.f, 0.f, 0.f};
    uint32_t m_journalDropped = 0;             // entries past the cap
    std::vector<DrawJournalEntry> m_journal;

    void journalReset();
    void journalPush(const DrawJournalEntry& e);
    void journalEvent(JournalKind kind, const Color& c = Color{0.f, 0.f, 0.f, 0.f});

    void addVertex(float x, float y, float u, float v, const Color& c);
    void addQuad(float x0, float y0, float x1, float y1,
                 float u0, float v0, float u1, float v1, const Color& c);
    void addQuadGrad(float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1,
                     const Color& cTop, const Color& cBot);

#ifdef NXUI_BACKEND_DEKO3D
    bool loadShaders();
    void setupSampler();
    void updateProjection();
    void bindRenderTarget(int offscreenIdx, float logicalW = 0.f, float logicalH = 0.f);
    void restoreRenderTarget();
#endif

    GpuDevice& m_gpu;

#ifdef NXUI_BACKEND_DEKO3D
    static constexpr int SHADER_COUNT = (int)ShaderProgram::Count;
    dk::Shader m_vertShaders[SHADER_COUNT];
    dk::Shader m_fragShaders[SHADER_COUNT];
    ShaderProgram m_curShader = ShaderProgram::Basic;
#endif

    // Batching state
    Vertex2D*  m_vtxBase   = nullptr;
    uint32_t   m_vtxCount  = 0;
    uint32_t   m_vtxBatchStart = 0;
    int        m_curTexSlot = -1;
    bool       m_texturing  = false;

    // Clip stack
    std::vector<Rect> m_clipStack;

    // Texture descriptor tracking
    int m_nextDescSlot = 0;
    std::vector<int> m_freeDescSlots;
    static constexpr int WHITE_TEX_SLOT = 0;

    // Offscreen target descriptor slots
    int m_offDescSlot[GpuDevice::NUM_OFFSCREEN] = {};

#ifdef NXUI_BACKEND_DEKO3D
    dk::Image          m_whiteImage;
    dk::UniqueMemBlock m_whiteMemBlock;

    // Track whether any image/sampler descriptor has been written
    // via CPU memcpy since the last GPU-side barrier.  When true,
    // flush() inserts barrier(DkBarrier_None, DkInvalidateFlags_Descriptors)
    // before the next draw call so the GPU re-reads from memory.
    bool m_descDirty = false;
#endif

#ifdef NXUI_BACKEND_SDL2
    // SDL2 backend: textures tracked by slot for binding.
    std::vector<SDL_Texture*> m_texSlots;
    SDL_Texture* m_boundTex = nullptr;
    // All SDL geometry texture-state transitions go through this helper. A
    // pending batch has exactly one texture, so changing state without flushing
    // retroactively changes how every queued vertex is submitted.
    void setSdlGeometryTexture(SDL_Texture* texture);

    // 1x1 opaque white fallback for flat-colour geometry. SDL documents that
    // SDL_RenderGeometry accepts a null texture, but not every backend actually
    // draws those triangles; modulating a real white pixel is deterministic.
    SDL_Texture* m_whiteSdlTex = nullptr;

    // Vertex buffer (CPU-side for SDL2)
    std::vector<Vertex2D> m_vtxBuf;
#endif

    bool m_boxWireframeEnabled = false;
    bool m_liquidGlassDebugRawBackdrop = false;
    bool m_reusableOffscreenCaptureValid = false;
    LiquidGlassSettings m_liquidGlassSettings;

    static inline std::string s_shaderBasePath = "romfs:/shaders/";
};

} // namespace nxui
