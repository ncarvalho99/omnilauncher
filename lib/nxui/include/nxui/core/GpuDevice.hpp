#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>

#ifdef NXUI_BACKEND_DEKO3D
#include <deko3d.hpp>
#include <switch.h>
#else
// SDL2 backend — forward declare what we need
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
#include <switch.h>
#endif

namespace nxui {

#ifdef NXUI_BACKEND_DEKO3D
inline constexpr uint32_t kGpuAlign = DK_MEMBLOCK_ALIGNMENT;   // 0x1000

struct GpuPool {
    dk::UniqueMemBlock block;
    uint32_t size  = 0;
    uint32_t used  = 0;
    void*    cpuBase = nullptr;
    DkGpuAddr gpuBase = 0;

    bool create(dk::Device dev, uint32_t sz, uint32_t flags) {
        size = (sz + kGpuAlign - 1) & ~(kGpuAlign - 1);
        block = dk::MemBlockMaker{dev, size}.setFlags(flags).create();
        used = 0;
        cpuBase = nullptr;
        gpuBase = 0;
        if (!block) {
            size = 0;
            return false;
        }
        if (flags & DkMemBlockFlags_CpuUncached)
            cpuBase = block.getCpuAddr();
        gpuBase = block.getGpuAddr();
        return true;
    }

    uint32_t alloc(uint32_t bytes, uint32_t align = 256) {
        uint32_t off = (used + align - 1) & ~(align - 1);
        if (off + bytes > size) return UINT32_MAX;
        used = off + bytes;
        return off;
    }

    DkGpuAddr gpuAddr(uint32_t off = 0) const { return gpuBase + off; }
    void*     cpuAddr(uint32_t off = 0) const { return (uint8_t*)cpuBase + off; }
};
#endif // NXUI_BACKEND_DEKO3D

class GpuDevice {
public:
    static constexpr int FB_WIDTH  = 1280;
    static constexpr int FB_HEIGHT = 720;
    static constexpr int NUM_FB    = 2;

    static constexpr int MAX_TEXTURES   = 2048;
    static constexpr int MAX_SAMPLERS   = 16;
    // The vertex grew from 32 to 56 bytes to carry the rounded-shape mask, and
    // the cap came down to match. Every rounded shape is a quad now instead of
    // a 36-segment fan, so the busiest measured frame uses 1884 vertices where
    // it once used 25002: 32768 is seventeen times the observed peak, and the
    // arena still shrinks from 2.00 MB to 1.75 MB per frame slot.
    static constexpr int MAX_VERTICES   = 32768;
    static constexpr int VTX_BUF_SIZE   = MAX_VERTICES * 56;
    static constexpr int IDX_BUF_SIZE   = 256;
    static constexpr int VS_UBO_SIZE    = 256;
    static constexpr int FS_UBO_SIZE    = 256;

    // Fragment uniforms are written per draw call. With a single buffer every
    // pushConstants overwrote memory the previous draw was still reading, so
    // the GPU had to finish each draw before the next could be set up — no
    // pipelining at all. Measured at roughly 0.9ms per draw call on the
    // settings overlay, which is what put it at 31ms a frame.
    //
    // A ring of slots lets consecutive draws use distinct memory. 512 covers
    // the busiest measured frame (137 draws) with room to spare, at 128 KB.
    static constexpr int FS_UBO_RING    = 512;
    // The projection matrix has exactly the same hazard and never got the same
    // treatment. updateProjection() and bindRenderTarget() both memcpy a
    // different ortho matrix into one 256-byte allocation, while the frame's
    // command list is not submitted until endFrame — so the last CPU write of
    // the frame silently applied to every draw already recorded in it. It stays
    // invisible only because the blur passes use a clip-space vertex shader
    // that ignores projection; any offscreen pass that did use it would have
    // rendered the whole frame through the wrong matrix. A small ring removes
    // the aliasing: render-target switches per frame are few.
    static constexpr int VS_UBO_RING    = 32;
    static constexpr int CMD_BUF_SIZE   = 256 * 1024;
    static constexpr int CODE_POOL_SIZE = 256 * 1024;

    bool initialize();
    void shutdown();

    int  beginFrame();
    void endFrame();
    void waitIdle();

    // Arms a lossless diagnostic capture of the next fully-rendered frame.
    // The GPU copies RGBA8 pixels into a CPU-visible block before present;
    // takeFrameDump() returns them after queue completion on the following
    // update. Only one capture can be armed or pending at a time.
    bool requestFrameDump();
    bool takeFrameDump(std::vector<std::uint8_t>& rgba);
    bool frameDumpBusy() const { return m_frameDumpArmed || m_frameDumpPending; }

    // Application shutdown destroys a large texture graph. Waiting for the
    // same queue once per texture turns teardown into a chain of redundant
    // GPU-wide barriers. Drain it once before bulk destruction, then let each
    // texture release only its descriptor and memory.
    void beginBulkTeardown();
    bool bulkTeardownActive() const { return m_bulkTeardown; }

    // Split of the two blocking waits in beginFrame, in nanoseconds.
    // Large acquire with small fence means the GPU is keeping up and we are
    // simply waiting on the display. Large fence means the GPU is the
    // bottleneck, and the cost is in what we submit.
    uint64_t lastAcquireNs()   const { return m_lastAcquireNs; }
    uint64_t lastFenceWaitNs() const { return m_lastFenceWaitNs; }

    // Uploads are packed into fenced staging/command slots and submitted before
    // the frame that can sample them. These counters show whether work batched
    // and whether wrapping the ring ever blocked the CPU.
    uint32_t lastFrameUploads()       const { return m_lastFrameUploads; }
    uint32_t lastFrameUploadBatches() const { return m_lastFrameUploadBatches; }
    uint64_t lastFrameUploadWaitNs()  const { return m_lastFrameUploadWaitNs; }

    // Presentation timing, in nanoseconds, for the frame just submitted.
    //
    // The Plaza artifact attenuates a contiguous band of scanlines from row 0
    // to a cut row that differs per occurrence, with the band holding *current*
    // frame content multiplied by a scalar. That rules out both a submitted
    // dimming draw and stale buffer content, and points at what happens between
    // submission and scanout. These counters make the presentation interval
    // observable so a frame that missed its deadline can be told apart from one
    // that met it: a tear caused by present racing scanout must correlate with
    // an anomalous interval, and an artifact that appears at a perfectly normal
    // interval cannot be blamed on present timing.
    uint64_t lastPresentIntervalNs() const { return m_lastPresentIntervalNs; }
    uint64_t lastSubmitToPresentNs() const { return m_lastSubmitToPresentNs; }
    uint64_t lastFrameCpuNs()        const { return m_lastFrameCpuNs; }

    // Result of the dual-readback self-test performed by takeFrameDump: the
    // same framebuffer image is copied into two buffers back to back in one
    // command list, and the buffers are compared byte for byte. Zero mismatches
    // means the readback path is reproducible and the captured pixels are what
    // the GPU image holds. Non-zero means the capture itself is unstable, and
    // any attenuation measured from these dumps is suspect.
    uint32_t lastDumpMismatchBytes() const { return m_lastDumpMismatchBytes; }
    uint32_t lastDumpFirstMismatch() const { return m_lastDumpFirstMismatch; }
    uint32_t lastDumpMaxDelta()      const { return m_lastDumpMaxDelta; }

    int  width()  const { return FB_WIDTH; }
    int  height() const { return FB_HEIGHT; }

#ifdef NXUI_BACKEND_DEKO3D
    // deko3d-specific accessors
    dk::Device   device()  const { return m_dev; }
    dk::Queue    queue()   const { return m_queue; }
    dk::CmdBuf  cmdBuf()  const { return m_cmdbuf[m_slot]; }
    int          slot()    const { return m_slot; }

    GpuPool& codePool()  { return m_codePool; }
    GpuPool& dataPool()  { return m_dataPool; }
    GpuPool& imagePool() { return m_imagePool; }

    DkGpuAddr imgDescGpuAddr()  const { return m_dataPool.gpuAddr(m_imgDescOff); }
    DkGpuAddr samDescGpuAddr()  const { return m_dataPool.gpuAddr(m_samDescOff); }
    void*     imgDescCpuAddr()  const { return m_dataPool.cpuAddr(m_imgDescOff); }
    void*     samDescCpuAddr()  const { return m_dataPool.cpuAddr(m_samDescOff); }

    DkGpuAddr vtxGpuAddr(int frame)   const { return m_dataPool.gpuAddr(m_vtxOff[frame]); }
    void*     vtxCpuAddr(int frame)   const { return m_dataPool.cpuAddr(m_vtxOff[frame]); }
    DkGpuAddr idxGpuAddr(int frame)   const { return m_dataPool.gpuAddr(m_idxOff[frame]); }
    void*     idxCpuAddr(int frame)   const { return m_dataPool.cpuAddr(m_idxOff[frame]); }
    DkGpuAddr vsUboGpuAddr(int frame) const { return m_dataPool.gpuAddr(m_vsUboOff[frame]); }
    void*     vsUboCpuAddr(int frame) const { return m_dataPool.cpuAddr(m_vsUboOff[frame]); }
    DkGpuAddr fsUboGpuAddr(int frame) const { return m_dataPool.gpuAddr(m_fsUboOff[frame]); }
    void*     fsUboCpuAddr(int frame) const { return m_dataPool.cpuAddr(m_fsUboOff[frame]); }
    // Next free slot in this frame's fragment-uniform ring. Wraps rather than
    // overflowing; a frame busy enough to wrap simply reintroduces the old
    // aliasing for its tail instead of corrupting memory.
    DkGpuAddr nextFsUboGpuAddr(int frame) {
        const uint32_t idx = m_fsUboRingPos[frame];
        m_fsUboRingPos[frame] = (idx + 1u) % FS_UBO_RING;
        return m_dataPool.gpuAddr(m_fsUboOff[frame] + idx * FS_UBO_SIZE);
    }
    void resetFsUboRing(int frame) { m_fsUboRingPos[frame] = 0; }

    // Same contract as the fragment ring above, for the projection matrix.
    // Returns the CPU and GPU address of a slot no recorded draw is still
    // pointing at, so a mid-frame projection change cannot rewrite the matrix
    // an earlier draw in the same frame was recorded with.
    uint32_t nextVsUboSlot(int frame) {
        const uint32_t idx = m_vsUboRingPos[frame];
        m_vsUboRingPos[frame] = (idx + 1u) % VS_UBO_RING;
        return idx;
    }
    DkGpuAddr vsUboGpuAddrAt(int frame, uint32_t idx) const {
        return m_dataPool.gpuAddr(m_vsUboOff[frame] + idx * VS_UBO_SIZE);
    }
    void* vsUboCpuAddrAt(int frame, uint32_t idx) const {
        return m_dataPool.cpuAddr(m_vsUboOff[frame] + idx * VS_UBO_SIZE);
    }
    void resetVsUboRing(int frame) { m_vsUboRingPos[frame] = 0; }

    struct ImageAlloc {
        dk::MemBlock block;
        uint32_t     offset = 0;
        bool valid() const { return (bool)block; }
    };

    ImageAlloc allocImageFromPool(uint32_t size, uint32_t alignment = 0);
    void resetImagePool();
    dk::UniqueMemBlock allocImageMemory(uint32_t size);
    void freeImageMemory(uint32_t size);

    // Icons and theme artwork together. Raised from the original 32 MB once the
    // console was asked instead of assumed: the menu process holds 458 MB and
    // had 220 MB free, so that figure was bounding nothing the hardware cared
    // about. It still bounds something worth bounding -- runaway image
    // allocation is what the User Break crash reports came from.
    //
    // 112 e a beira, nao um numero folgado. Medido no console, percorrendo o
    // catalogo de temas com o teto temporariamente em 144:
    //
    //   imagem 101.7 MB   sobrevive
    //   imagem 108.1 MB   o processo morre, sem relatorio de falha
    //
    // Subir o teto nao deu memoria ao console: so tirou a recusa que impedia de
    // chegar la. As previas da loja pararam de falhar e passaram a derrubar o
    // menu, que e a troca errada.
    //
    // O papel de parede animado leva 71 destes 112, entao o que sobra para
    // fontes, icones de 120 jogos e as imagens do catalogo e pouco. O que fez
    // isso caber nao foi mexer no teto e sim parar de manter o cache de previas
    // inteiro na GPU -- ver syncFinishedCommunityPreviewLoads.
    static constexpr uint64_t kDefaultImageBudget = 112u * 1024u * 1024u;

    // O teto em uso. Comeca no valor acima e o dono do processo pode subi-lo
    // depois de saber quanto heap conseguiu -- e o heap que manda, nao um
    // numero escolhido aqui: toda imagem sai dele.
    // Para onde o deko3d relata os erros dele. Sem isso eles matam o processo
    // em silencio.
    using DebugSink = void (*)(const char*);
    static DebugSink debugSink()            { return s_debugSink; }
    static void setDebugSink(DebugSink s)   { s_debugSink = s; }

    static uint64_t imageBudget()          { return s_imageBudget; }
    static void setImageBudget(uint64_t b) { s_imageBudget = b; }

    uint64_t imageMemoryUsed() const { return m_imageMemUsed; }
    uint64_t imageMemoryBudget() const { return imageBudget(); }
    uint64_t imageMemoryAvailable() const {
        return m_imageMemUsed < imageBudget()
            ? imageBudget() - m_imageMemUsed : 0;
    }

    // expectedBytes is what the caller says this image should occupy; 0 means
    // the uncompressed w*h*4. It exists because the short-buffer check below is
    // the guard that turns a bad upload into a failure instead of an svcBreak,
    // and a block-compressed image legitimately carries a fraction of that.
    // Source bytes are copied before return. GPU completion is asynchronous;
    // beginFrame/endFrame submit batches before their drawing commands, and
    // waitIdle flushes any pending batch before waiting.
    bool uploadTexture(dk::Image& dst, const void* pixels, uint32_t size,
                       uint32_t width, uint32_t height, uint64_t expectedBytes = 0);

    // Two classes of offscreen target, because the two users want opposite
    // things. The icon and sidebar glass recapture the scene every single
    // frame, so that capture has to be cheap and does not need detail — it is
    // sampled through a small refracting panel. Settings, the power menu and
    // the account dialog capture once per open and cache it, and they stretch
    // what they captured across the screen, where half resolution is plainly
    // visible as softness no blur tuning can remove.
    //
    // Making them all full resolution cost the home screen 1.8ms a frame and
    // 60 fps became 54. So the per-frame one stays half.
    static constexpr int NUM_OFFSCREEN = 7;

    static constexpr int OFF_SCENE    = 0;   // half res, captured every frame
    static constexpr int OFF_DIALOG   = 1;   // full res, dialog backdrop cache
    static constexpr int OFF_SETTINGS = 2;   // full res, settings backdrop cache
    static constexpr int OFF_SHARP_A  = 3;   // full res, sharp capture and blur
    static constexpr int OFF_SHARP_B  = 4;   // full res, blur scratch
    // Half res, and the pair for it is OFF_SCENE: the wallpaper is drawn into
    // the scene target, blurred against this one, and drawn back, all before
    // anything captures the scene for the icon glass.
    static constexpr int OFF_BG_BLUR  = 5;
    // Full res, the scene as captured before the panel blurred it. With the
    // home scene skipped under an open overlay nothing paints the margin around
    // the panel, and painting it with the blurred copy frosted the whole screen.
    static constexpr int OFF_SCENE_FROZEN = 6;

    static constexpr bool offscreenIsFullRes(int i) {
        return i != OFF_SCENE && i != OFF_BG_BLUR;
    }
    static constexpr int  offscreenWidth(int i)  { return offscreenIsFullRes(i) ? FB_WIDTH  : FB_WIDTH  / 2; }
    static constexpr int  offscreenHeight(int i) { return offscreenIsFullRes(i) ? FB_HEIGHT : FB_HEIGHT / 2; }

    dk::Image&       offscreenImage(int i)       { return m_offImages[i]; }
    const dk::Image& offscreenImage(int i) const { return m_offImages[i]; }
    bool offscreenReady() const { return m_offscreenReady; }

    dk::Image&       fbImage(int i)       { return m_fbImages[i]; }
    const dk::Image& fbImage(int i) const { return m_fbImages[i]; }
    dk::Image&       dsImage()            { return m_dsImage; }
    const dk::Image& dsImage() const      { return m_dsImage; }
#else
    // SDL2-specific accessors
    SDL_Renderer* sdlRenderer() const { return m_sdlRenderer; }
    SDL_Window*   sdlWindow()   const { return m_sdlWindow; }
    int           slot()        const { return m_slot; }

    // Stubs for pool-based allocation (SDL2 uses SDL_CreateTexture directly)
    void resetImagePool() {}
    static constexpr int NUM_OFFSCREEN = 7;
    bool offscreenReady() const { return false; }
    uint64_t imageMemoryUsed() const { return 0; }
    uint64_t imageMemoryBudget() const { return UINT64_MAX; }
    uint64_t imageMemoryAvailable() const { return UINT64_MAX; }
#endif

private:
#ifdef NXUI_BACKEND_DEKO3D
    static constexpr int      UPLOAD_SLOT_COUNT = 4;
    static constexpr uint32_t UPLOAD_CMD_BUF_SIZE = 64u * 1024u;
    static constexpr uint32_t UPLOAD_STAGING_SIZE = 8192u * 1024u;
    static constexpr uint32_t UPLOAD_COPIES_PER_BATCH = 32;

    void createFramebuffers();
    void createDepthStencil();
    void createOffscreenTargets();
    void beginUploadBatch();
    void submitUploadBatch();
    void retireSubmittedUploads();

    dk::UniqueDevice    m_dev;
    dk::UniqueQueue     m_queue;
    dk::UniqueCmdBuf    m_cmdbuf[NUM_FB];
    dk::UniqueCmdBuf    m_uploadCmdbuf[UPLOAD_SLOT_COUNT];
    dk::UniqueSwapchain m_swapchain;

    GpuPool m_fbPool;
    GpuPool m_dsPool;
    GpuPool m_cmdPool[NUM_FB];
    GpuPool m_uploadCmdPool[UPLOAD_SLOT_COUNT];
    GpuPool m_codePool;
    GpuPool m_dataPool;
    GpuPool m_imagePool;
    GpuPool m_uploadStagingPool[UPLOAD_SLOT_COUNT];
    dk::UniqueMemBlock m_frameDumpBuffers[2];

    // Normal icons, glyphs, and BC1 frames share a fixed arena per slot. A
    // larger upload owns one temporary block until the slot fence signals.
    dk::UniqueMemBlock m_uploadTempStaging[UPLOAD_SLOT_COUNT];
    dk::Fence m_uploadFences[UPLOAD_SLOT_COUNT];
    bool m_uploadInFlight[UPLOAD_SLOT_COUNT] {};
    uint32_t m_uploadCopyCount[UPLOAD_SLOT_COUNT] {};
    int m_uploadSlot = 0;
    bool m_uploadBatchOpen = false;

    dk::Image  m_fbImages[NUM_FB];
    dk::Image  m_dsImage;
    dk::Image  m_offImages[NUM_OFFSCREEN];
    GpuPool    m_offPool;
    bool       m_offscreenReady = false;

    // Per-slot fences: signalled in endFrame, waited in beginFrame, so that
    // the CPU never overwrites command/vertex memory the GPU is still reading.
    dk::Fence  m_frameFences[NUM_FB];
    uint32_t m_vtxOff[NUM_FB] {};
    uint32_t m_idxOff[NUM_FB] {};
    uint32_t m_vsUboOff[NUM_FB] {};
    uint32_t m_fsUboOff[NUM_FB] {};
    uint32_t m_fsUboRingPos[NUM_FB] {};
    uint32_t m_vsUboRingPos[NUM_FB] {};
    uint32_t m_imgDescOff = 0;
    uint32_t m_samDescOff = 0;

    static uint64_t s_imageBudget;
    static DebugSink s_debugSink;
    uint64_t m_imageMemUsed = 0;
    uint64_t m_poolMemUsed  = 0;
    static constexpr uint32_t kImageChunkSize = 2u * 1024u * 1024u;
    struct ImagePoolChunk {
        dk::UniqueMemBlock block;
        uint32_t size = 0;
        uint32_t used = 0;
    };
    std::vector<ImagePoolChunk> m_imageChunks;
#else
    SDL_Window*   m_sdlWindow   = nullptr;
    SDL_Renderer* m_sdlRenderer = nullptr;
#endif
    int m_slot = -1;
    bool m_frameDumpArmed = false;
    bool m_frameDumpPending = false;

    // Presentation timing. See lastPresentIntervalNs().
    uint64_t m_prevPresentTick = 0;
    uint64_t m_frameBeginTick = 0;
    uint64_t m_lastPresentIntervalNs = 0;
    uint64_t m_lastSubmitToPresentNs = 0;
    uint64_t m_lastFrameCpuNs = 0;

    // Dual-readback self-test results. See lastDumpMismatchBytes().
    uint32_t m_lastDumpMismatchBytes = 0;
    uint32_t m_lastDumpFirstMismatch = UINT32_MAX;
    uint32_t m_lastDumpMaxDelta = 0;
    uint32_t m_frameUploads = 0;
    uint32_t m_lastFrameUploads = 0;
    uint32_t m_frameUploadBatches = 0;
    uint32_t m_lastFrameUploadBatches = 0;
    uint64_t m_frameUploadWaitNs = 0;
    uint64_t m_lastFrameUploadWaitNs = 0;
    uint64_t m_lastAcquireNs = 0;
    uint64_t m_lastFenceWaitNs = 0;
    bool m_bulkTeardown = false;
};

} // namespace nxui
