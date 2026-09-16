#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/Font.hpp>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <iterator>

namespace nxui {

static void ortho(float* m, float w, float h) {
    std::memset(m, 0, 16 * sizeof(float));
    m[ 0] =  2.f / w;
    m[ 5] = -2.f / h;
    m[10] = -1.f;
    m[12] = -1.f;
    m[13] =  1.f;
    m[15] =  1.f;
}

Renderer::Renderer(GpuDevice& gpu) : m_gpu(gpu) {
    m_clipStack.reserve(8);
    // Reserved once, here, so that recording a frame never allocates: an
    // allocation inside the render loop would perturb the very timing the
    // journal exists to observe.
    m_journal.reserve(kJournalCap);
    m_vtxShadow.resize(kVtxShadowCap);
    resetLiquidGlassSettings();
}
Renderer::~Renderer() {}

void Renderer::resetLiquidGlassSettings() {
    m_liquidGlassSettings = LiquidGlassSettings{};
}

bool Renderer::initialize() {
    std::printf("[Renderer] Loading shaders...\n");
    if (!loadShaders()) {
        std::printf("[Renderer] Shader initialization FAILED\n");
        return false;
    }
    std::printf("[Renderer] Setting up sampler...\n");
    setupSampler();

    std::printf("[Renderer] Creating 1x1 white texture...\n");
    {
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{m_gpu.device()}
            .setFlags(0)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(1, 1)
            .initialize(layout);

        m_whiteMemBlock = m_gpu.allocImageMemory(layout.getSize());
        if (!m_whiteMemBlock) {
            static constexpr const char* msg =
                "[Renderer] White texture allocation FAILED";
            std::fprintf(stderr, "%s\n", msg);
            if (GpuDevice::debugSink())
                GpuDevice::debugSink()(msg);
            return false;
        }
        m_whiteImage.initialize(layout, m_whiteMemBlock, 0);

        uint32_t white = 0xFFFFFFFF;
        if (!m_gpu.uploadTexture(m_whiteImage, &white, 4, 1, 1)) {
            static constexpr const char* msg =
                "[Renderer] White texture upload FAILED";
            std::fprintf(stderr, "%s\n", msg);
            if (GpuDevice::debugSink())
                GpuDevice::debugSink()(msg);
            return false;
        }

        dk::ImageView view{m_whiteImage};
        int slot = registerTexture(view);
        std::printf("[Renderer] White texture registered at slot %d\n", slot);
    }

    if (m_gpu.offscreenReady()) {
        for (int i = 0; i < GpuDevice::NUM_OFFSCREEN; ++i) {
            dk::ImageView view{m_gpu.offscreenImage(i)};
            m_offDescSlot[i] = registerTexture(view);
            std::printf("[Renderer] Offscreen %d registered at slot %d\n", i, m_offDescSlot[i]);
        }
    }

    std::printf("[Renderer] Init complete\n");
    return true;
}

bool Renderer::loadShaders() {
    auto loadDksh = [&](dk::Shader& out, const std::string& path) {
        auto tryLoad = [&](const std::string& candidatePath) {
            std::printf("[Renderer] Loading shader: %s\n", candidatePath.c_str());
            std::ifstream f(candidatePath, std::ios::binary | std::ios::ate);
            if (!f.is_open()) {
                std::printf("[Renderer] FAILED to open shader: %s\n", candidatePath.c_str());
                return false;
            }
            const std::streamoff sz = f.tellg();
            f.seekg(0, std::ios::beg);

            if (sz <= 0) {
                std::printf("[Renderer] Invalid shader size for %s\n", candidatePath.c_str());
                return false;
            }

            uint32_t off = m_gpu.codePool().alloc(static_cast<uint32_t>(sz), DK_SHADER_CODE_ALIGNMENT);
            if (off == UINT32_MAX) {
                std::printf("[Renderer] Code pool alloc FAILED for %lld bytes\n", static_cast<long long>(sz));
                return false;
            }

            void* dst = m_gpu.codePool().cpuAddr(off);
            if (!f.read(static_cast<char*>(dst), static_cast<std::streamsize>(sz))) {
                std::printf("[Renderer] Short shader read for %s\n", candidatePath.c_str());
                return false;
            }

            dk::ShaderMaker{m_gpu.codePool().block, off}.initialize(out);
            return true;
        };

        if (tryLoad(path))
            return true;

        static constexpr const char* kRomfsShaderBase = "romfs:/shaders/";
        if (path.rfind(kRomfsShaderBase, 0) == 0)
            return false;

        std::string fileName = path;
        std::size_t slash = fileName.find_last_of('/');
        if (slash != std::string::npos)
            fileName = fileName.substr(slash + 1);

        std::string fallbackPath = std::string(kRomfsShaderBase) + fileName;
        std::printf("[Renderer] Retrying shader from fallback path: %s\n", fallbackPath.c_str());
        return tryLoad(fallbackPath);
    };

    bool ok = true;

    ok = loadDksh(m_vertShaders[(int)ShaderProgram::Basic], s_shaderBasePath + "basic_vsh.dksh") && ok;
    ok = loadDksh(m_fragShaders[(int)ShaderProgram::Basic], s_shaderBasePath + "basic_fsh.dksh") && ok;

    ok = loadDksh(m_vertShaders[(int)ShaderProgram::Backdrop], s_shaderBasePath + "basic_vsh.dksh") && ok;
    ok = loadDksh(m_fragShaders[(int)ShaderProgram::Backdrop], s_shaderBasePath + "backdrop_fsh.dksh") && ok;

    ok = loadDksh(m_vertShaders[(int)ShaderProgram::BlurH], s_shaderBasePath + "blur_pass_vsh.dksh") && ok;
    ok = loadDksh(m_fragShaders[(int)ShaderProgram::BlurH], s_shaderBasePath + "blur_h_fsh.dksh") && ok;

    ok = loadDksh(m_vertShaders[(int)ShaderProgram::BlurV], s_shaderBasePath + "blur_pass_vsh.dksh") && ok;
    ok = loadDksh(m_fragShaders[(int)ShaderProgram::BlurV], s_shaderBasePath + "blur_v_fsh.dksh") && ok;

    ok = loadDksh(m_vertShaders[(int)ShaderProgram::Wave], s_shaderBasePath + "pass_vsh.dksh") && ok;
    ok = loadDksh(m_fragShaders[(int)ShaderProgram::Wave], s_shaderBasePath + "wave_fsh.dksh") && ok;

    ok = loadDksh(m_vertShaders[(int)ShaderProgram::LiquidGlass], s_shaderBasePath + "pass_vsh.dksh") && ok;
    ok = loadDksh(m_fragShaders[(int)ShaderProgram::LiquidGlass], s_shaderBasePath + "liquid_glass_fsh.dksh") && ok;

    ok = loadDksh(m_vertShaders[(int)ShaderProgram::Gradient], s_shaderBasePath + "basic_vsh.dksh") && ok;
    ok = loadDksh(m_fragShaders[(int)ShaderProgram::Gradient], s_shaderBasePath + "gradient_fsh.dksh") && ok;

    return ok;
}

void Renderer::setupSampler() {
    dk::SamplerDescriptor samDesc;
    samDesc.initialize(
        dk::Sampler{}
            .setFilter(DkFilter_Linear, DkFilter_Linear)
            .setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge)
    );
    std::memcpy(m_gpu.samDescCpuAddr(), &samDesc, sizeof(samDesc));
}

int Renderer::registerTexture(const dk::ImageView& view) {
    if (m_nextDescSlot >= GpuDevice::MAX_TEXTURES && m_freeDescSlots.empty()) {
        std::fprintf(stderr, "[Renderer] texture descriptor overflow: slot=%d max=%d\n",
                     m_nextDescSlot, GpuDevice::MAX_TEXTURES);
        return -1;
    }
    int slot;
    if (!m_freeDescSlots.empty()) {
        slot = m_freeDescSlots.back();
        m_freeDescSlots.pop_back();
    } else {
        slot = m_nextDescSlot++;
    }
    dk::ImageDescriptor desc;
    desc.initialize(view);
    auto* base = static_cast<dk::ImageDescriptor*>(m_gpu.imgDescCpuAddr());
    base[slot] = desc;
    m_descDirty = true;   // GPU descriptor cache must be invalidated before next draw
    return slot;
}

void Renderer::releaseTextureSlot(int slot) {
    // Os slots reservados no inicio (o branco e os offscreen) nunca voltam.
    if (slot <= 0 || slot >= m_nextDescSlot)
        return;
    m_freeDescSlots.push_back(slot);
}

void Renderer::reclaimReleasedTextureSlotsAfterIdle() {
    // Texture retirement in this fork already waits for GPU idle before the
    // descriptor slot and backing memory are released.
}

void Renderer::updateTexture(int slot, const dk::ImageView& view) {
    dk::ImageDescriptor desc;
    desc.initialize(view);
    auto* base = static_cast<dk::ImageDescriptor*>(m_gpu.imgDescCpuAddr());
    base[slot] = desc;
    m_descDirty = true;   // GPU descriptor cache must be invalidated before next draw
}

void Renderer::resetTextureSlots() {
    int firstFree = 1;
    if (m_gpu.offscreenReady())
        firstFree = 1 + GpuDevice::NUM_OFFSCREEN;
    m_nextDescSlot = firstFree;
    m_curTexSlot = -1;
    m_freeDescSlots.clear();

    if (m_gpu.offscreenReady()) {
        for (int i = 0; i < GpuDevice::NUM_OFFSCREEN; ++i) {
            dk::ImageView view{m_gpu.offscreenImage(i)};
            dk::ImageDescriptor d;
            d.initialize(view);
            auto* base = static_cast<dk::ImageDescriptor*>(m_gpu.imgDescCpuAddr());
            base[m_offDescSlot[i]] = d;
        }
    }
}

void Renderer::bindTexture(int slot) {
    if (slot != m_curTexSlot) {
        bool newTexturing = (slot >= 0);
        if (newTexturing != m_texturing || slot != m_curTexSlot)
            flush();
        m_curTexSlot = slot;
        m_texturing = newTexturing;
    }
}

void Renderer::useShader(ShaderProgram prog) {
    if (prog == m_curShader) return;
    ++m_framePipelineBinds;
    flush();
    m_fsCacheValid = false;
    m_boundTexSlot = kTexSlotUnset;
    m_curShader = prog;
    int idx = (int)prog;
    auto cmd = m_gpu.cmdBuf();
    cmd.bindShaders(DkStageFlag_GraphicsMask, {&m_vertShaders[idx], &m_fragShaders[idx]});
}

void Renderer::pushFsUniforms(const FsUniforms& fs) {
    flush();
    int slot = m_gpu.slot();
    auto cmd = m_gpu.cmdBuf();
    auto fsUboAddr = m_gpu.nextFsUboGpuAddr(slot);
    cmd.pushConstants(fsUboAddr, GpuDevice::FS_UBO_SIZE, 0, sizeof(fs), &fs);
    cmd.bindUniformBuffer(DkStage_Fragment, 1, fsUboAddr, GpuDevice::FS_UBO_SIZE);
    // A caller wrote the whole block behind flush()'s back, so what it thinks
    // is bound is no longer what is bound.
    m_fsCacheValid = false;
}

void Renderer::beginFrame() {
    int slot = m_gpu.slot();
    m_vtxBase  = static_cast<Vertex2D*>(m_gpu.vtxCpuAddr(slot));
    m_vtxCount = 0;
    m_vtxBatchStart = 0;
    m_lastFrameDrawCalls = m_frameDrawCalls;
    m_lastFramePipelineBinds = m_framePipelineBinds;
    m_lastFrameVertices = m_peakVtxCount;
    m_lastFrameBlurPasses = m_frameBlurPasses;
    m_lastFrameCaptures = m_frameCaptures;
    m_frameDrawCalls = 0;
    m_framePipelineBinds = 0;
    m_peakVtxCount = 0;
    m_frameBlurPasses = 0;
    m_frameCaptures = 0;
    m_gpu.resetFsUboRing(slot);
    m_gpu.resetVsUboRing(slot);
    m_curTexSlot = -1;
    m_texturing  = false;
    m_curShader  = ShaderProgram::Basic;
    m_shapeRadius = 0.f;
    m_shapeThickness = 0.f;
    m_fsCacheValid = false;
    m_boundTexSlot = kTexSlotUnset;
    m_vtxBufferBound = false;
    m_clipStack.clear();
    ++m_frameSerial;
    if (m_journalEnabled) journalReset();
    // Normally the capture cannot outlive a frame: the framebuffer it copies is
    // redrawn every frame. An overlay that knows nothing behind it has changed
    // can hold it, which skips a 1280x720 to 640x360 blit and two full pipeline
    // barriers per frame — a fixed cost that matters because vsync makes this a
    // cliff, not a slope. The settings overlay measured exactly 30.0 fps, so the
    // frame is only just over the 16.67ms budget.
    if (!m_holdOffscreenCapture)
        m_reusableOffscreenCaptureValid = false;

    auto cmd = m_gpu.cmdBuf();

    dk::ImageView colorTarget{m_gpu.fbImage(slot)};
    dk::ImageView dsTarget{m_gpu.dsImage()};
    cmd.bindRenderTargets(&colorTarget, &dsTarget);

    cmd.setViewports(0, DkViewport{0.f, 0.f,
        (float)m_gpu.width(), (float)m_gpu.height(), 0.f, 1.f});
    cmd.setScissors(0, DkScissor{0, 0, (uint32_t)m_gpu.width(), (uint32_t)m_gpu.height()});

    cmd.clearColor(0, DkColorMask_RGBA, 0.05f, 0.08f, 0.15f, 1.f);
    cmd.clearDepthStencil(false, 0.f, 0xFF, 0);
    if (m_journalEnabled) {
        m_journalTarget = -1;
        m_journalScissor = {0.f, 0.f, (float)m_gpu.width(), (float)m_gpu.height()};
        m_journalBackbufferClearSeen = true;
        m_journalBackbufferClear = {0.05f, 0.08f, 0.15f, 1.f};
        journalEvent(JournalKind::Clear, m_journalBackbufferClear);
    }

    int idx = (int)ShaderProgram::Basic;
    cmd.bindShaders(DkStageFlag_GraphicsMask, {&m_vertShaders[idx], &m_fragShaders[idx]});

    cmd.bindColorState(dk::ColorState{}.setBlendEnable(0, true));
    dk::BlendState blendState;
    blendState.setFactors(DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
                          DkBlendFactor_One, DkBlendFactor_InvSrcAlpha);
    DkBlendState rawBlendState = blendState;
    cmd.bindBlendStates(0, rawBlendState);

    cmd.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false));
    cmd.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));

    static const std::array<DkVtxAttribState, 6> attribs = {{
        DkVtxAttribState{0, 0, offsetof(Vertex2D, x), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(Vertex2D, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(Vertex2D, r), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(Vertex2D, sx), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(Vertex2D, hx), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        DkVtxAttribState{0, 0, offsetof(Vertex2D, rad), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    }};
    static const DkVtxBufferState bufState = {sizeof(Vertex2D), 0};
    cmd.bindVtxAttribState(attribs);
    cmd.bindVtxBufferState(bufState);

    cmd.bindImageDescriptorSet(m_gpu.imgDescGpuAddr(), GpuDevice::MAX_TEXTURES);
    cmd.bindSamplerDescriptorSet(m_gpu.samDescGpuAddr(), GpuDevice::MAX_SAMPLERS);

    // Invalidate the GPU's descriptor cache so it re-reads all descriptors
    // that were updated via CPU memcpy since the last frame.
    cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    m_descDirty = false;

    updateProjection();
}

void Renderer::endFrame() {
    flush();
}

void Renderer::updateProjection() {
    int slot = m_gpu.slot();
    // A fresh ring slot per projection change: the previously recorded draws
    // keep pointing at the matrix they were recorded with.
    const uint32_t ring = m_gpu.nextVsUboSlot(slot);
    auto* ubo = static_cast<uint8_t*>(m_gpu.vsUboCpuAddrAt(slot, ring));
    VsUniforms vs;
    ortho(vs.projection, (float)m_gpu.width(), (float)m_gpu.height());
    std::memcpy(ubo, &vs, sizeof(vs));

    auto cmd = m_gpu.cmdBuf();
    cmd.bindUniformBuffer(DkStage_Vertex, 0, m_gpu.vsUboGpuAddrAt(slot, ring),
                          GpuDevice::VS_UBO_SIZE);
}

void Renderer::journalReset() {
    m_journal.clear();
    m_journalDropped = 0;
    m_journalTarget = -1;
    m_journalScissor = {0.f, 0.f, (float)m_gpu.width(), (float)m_gpu.height()};
    m_journalBackbufferClearSeen = false;
    m_journalNonQuadBatches = 0;
    m_journalUnshadowed = 0;
    m_journalBadVertices = 0;
    m_journalBadSamples = 0;
    m_probes.clear();
    m_probesDropped = 0;
    m_journalBackbufferClear = {0.f, 0.f, 0.f, 0.f};
}

void Renderer::journalPush(const DrawJournalEntry& e) {
    // Past the cap the journal stops recording rather than reallocating, and
    // counts what it dropped. A truncated journal is still evidence as long as
    // it is honest about being truncated; a journal that reallocated mid-frame
    // would have changed the frame it was measuring.
    if (m_journal.size() >= kJournalCap) { ++m_journalDropped; return; }
    m_journal.push_back(e);
}

void Renderer::journalEvent(JournalKind kind, const Color& c) {
    DrawJournalEntry e;
    e.kind   = kind;
    e.target = (int16_t)m_journalTarget;
    e.minR = e.maxR = c.r; e.minG = e.maxG = c.g;
    e.minB = e.maxB = c.b; e.minA = e.maxA = c.a;
    e.sx = m_journalScissor.x;     e.sy = m_journalScissor.y;
    e.sw = m_journalScissor.width; e.sh = m_journalScissor.height;
    journalPush(e);
}

void Renderer::flush() {
    uint32_t batchVerts = m_vtxCount - m_vtxBatchStart;
    if (batchVerts == 0) return;
    ++m_frameDrawCalls;

    // Record the batch before it is submitted. The bounds come from the
    // vertices actually in the arena, not from the caller's rect, so a draw
    // that reached the GPU with different geometry than its caller intended
    // still shows up here as what the GPU will receive.
    if (m_journalEnabled) {
        DrawJournalEntry e;
        e.kind    = JournalKind::Batch;
        e.shader  = (uint16_t)m_curShader;
        e.target  = (int16_t)m_journalTarget;
        e.texSlot = (int16_t)((m_texturing && m_curTexSlot >= 0) ? m_curTexSlot : WHITE_TEX_SLOT);
        e.verts   = batchVerts;

        // Batch bounds also come from the cached shadow, for the same reason the
        // per-quad scan below does: re-reading the uncached arena yields
        // corrupted exponents. UV is not shadowed, so it is reported as zero
        // rather than as an untrustworthy re-read.
        const uint32_t sEnd = std::min(m_vtxBatchStart + batchVerts, kVtxShadowCap);
        if (m_vtxBatchStart < sEnd) {
            const VtxShadow& v0 = m_vtxShadow[m_vtxBatchStart];
            float minX = v0.x, minY = v0.y, maxX = v0.x, maxY = v0.y;
            e.minR = e.maxR = v0.r; e.minG = e.maxG = v0.g;
            e.minB = e.maxB = v0.b; e.minA = e.maxA = v0.a;
            for (uint32_t i = m_vtxBatchStart + 1; i < sEnd; ++i) {
                const VtxShadow& v = m_vtxShadow[i];
                minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
                minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
                e.minR = std::min(e.minR, v.r); e.maxR = std::max(e.maxR, v.r);
                e.minG = std::min(e.minG, v.g); e.maxG = std::max(e.maxG, v.g);
                e.minB = std::min(e.minB, v.b); e.maxB = std::max(e.maxB, v.b);
                e.minA = std::min(e.minA, v.a); e.maxA = std::max(e.maxA, v.a);
            }
            e.x0 = minX; e.y0 = minY; e.x1 = maxX; e.y1 = maxY;
        } else {
            ++m_journalUnshadowed;
        }

        // Scan the batch one six-vertex quad at a time, and record any single
        // quad that could itself dim the framebuffer.
        //
        // The whole-batch min/max above cannot answer that question, and
        // believing it could was an error in entries 122, 124 and 128. A
        // fullscreen black quad at alpha 0.2 batched together with thirteen
        // opaque quads yields maxRGB = 1.0 and maxA = 1.0 for the batch, so a
        // test requiring *every* vertex to be dark and translucent rejects it.
        // Frame 073's journal contains exactly that shape: v=84, colour range
        // 0.000..1.000, alpha range 0.200..1.000 — and 1 - 0.200 = 0.800 is the
        // measured attenuation. Per-quad scanning closes that hole.
        //
        // Only whole quads are examined; a batch whose vertex count is not a
        // multiple of six also contains triangles or line segments, which
        // cannot cover the framebuffer on their own and are counted separately
        // rather than ignored silently.
        if (m_journalTarget == -1 && batchVerts >= 6) {
            const float fbW = (float)m_gpu.width();
            const float fbH = (float)m_gpu.height();
            for (uint32_t q = 0; q + 6 <= batchVerts; q += 6) {
                const uint32_t base = m_vtxBatchStart + q;
                if (base + 6 > kVtxShadowCap) { ++m_journalUnshadowed; continue; }
                const VtxShadow* v = &m_vtxShadow[base];
                float qx0 = v[0].x, qy0 = v[0].y, qx1 = v[0].x, qy1 = v[0].y;
                float mr = v[0].r, mg = v[0].g, mb = v[0].b;
                float minA = v[0].a, maxA = v[0].a;
                for (int k = 1; k < 6; ++k) {
                    qx0 = std::min(qx0, v[k].x); qx1 = std::max(qx1, v[k].x);
                    qy0 = std::min(qy0, v[k].y); qy1 = std::max(qy1, v[k].y);
                    mr = std::max(mr, v[k].r);
                    mg = std::max(mg, v[k].g);
                    mb = std::max(mb, v[k].b);
                    minA = std::min(minA, v[k].a);
                    maxA = std::max(maxA, v[k].a);
                }
                const float qw = qx1 - qx0, qh = qy1 - qy0;
                if (!(std::fabs(qx0) < 1e6f && std::fabs(qy0) < 1e6f &&
                      qw < 1e6f && qh < 1e6f)) continue;
                // Cover the visible framebuffer in both axes, be dark at every
                // corner, and be partially transparent at every corner.
                if (qx0 > 0.5f || qy0 > 0.5f || qx1 < fbW - 0.5f || qy1 < fbH - 0.5f)
                    continue;
                if ((mr + mg + mb) > 0.35f) continue;
                if (!(minA > 0.004f && maxA < 0.999f)) continue;
                DrawJournalEntry d;
                d.kind    = JournalKind::Dimmer;
                d.shader  = e.shader;
                d.target  = e.target;
                d.texSlot = e.texSlot;
                d.verts   = 6;
                d.x0 = qx0; d.y0 = qy0; d.x1 = qx1; d.y1 = qy1;
                d.minR = d.maxR = mr; d.minG = d.maxG = mg; d.minB = d.maxB = mb;
                d.minA = minA; d.maxA = maxA;
                d.sx = m_journalScissor.x;     d.sy = m_journalScissor.y;
                d.sw = m_journalScissor.width; d.sh = m_journalScissor.height;
                journalPush(d);
            }
            if (batchVerts % 6u != 0u) ++m_journalNonQuadBatches;
        }
        e.sx = m_journalScissor.x;     e.sy = m_journalScissor.y;
        e.sw = m_journalScissor.width; e.sh = m_journalScissor.height;
        journalPush(e);
    }

    auto cmd = m_gpu.cmdBuf();
    int slot = m_gpu.slot();

    // If any image descriptor was written via CPU memcpy since the last
    // barrier, invalidate the GPU's descriptor cache NOW, before the
    // draw call that may reference the updated descriptor.
    if (m_descDirty) {
        cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
        m_descDirty = false;
        m_boundTexSlot = kTexSlotUnset;   // the handle may now point elsewhere
    }

    // Nothing below is re-emitted unless it actually changed.

    // Backdrop shares the block so that offscreen captures get the same corner
    // mask as everything else. The other programs push their own uniforms.
    if (m_curShader == ShaderProgram::Basic || m_curShader == ShaderProgram::Backdrop) {
        FsUniforms fs = {};
        fs.useTexture = m_texturing ? 1 : 0;

        // Now that the shape rides on the vertex, the block holds one flag, so
        // consecutive draws almost always match. The binding survives from the
        // previous draw, so an identical block means neither the push nor the
        // rebind has to happen.
        if (!m_fsCacheValid || std::memcmp(m_fsCache, &fs, kFsPushBytes) != 0) {
            std::memcpy(m_fsCache, &fs, kFsPushBytes);
            m_fsCacheValid = true;
            auto fsUboAddr = m_gpu.nextFsUboGpuAddr(slot);
            cmd.pushConstants(fsUboAddr, GpuDevice::FS_UBO_SIZE, 0, kFsPushBytes, &fs);
            cmd.bindUniformBuffer(DkStage_Fragment, 1, fsUboAddr, GpuDevice::FS_UBO_SIZE);
        }
    }

    int texSlot = (m_texturing && m_curTexSlot >= 0) ? m_curTexSlot : WHITE_TEX_SLOT;
    if (texSlot != m_boundTexSlot) {
        cmd.bindTextures(DkStage_Fragment, 0, dkMakeTextureHandle(texSlot, 0));
        m_boundTexSlot = texSlot;
    }

    // The whole arena is bound once per frame and each batch is addressed by
    // its first vertex, rather than rebinding a window of it per draw.
    if (!m_vtxBufferBound) {
        cmd.bindVtxBuffer(0, m_gpu.vtxGpuAddr(slot), GpuDevice::VTX_BUF_SIZE);
        m_vtxBufferBound = true;
    }

    cmd.draw(DkPrimitive_Triangles, batchVerts, 1, m_vtxBatchStart, 0);

    m_vtxBatchStart = m_vtxCount;
}

// Render target switching

void Renderer::bindRenderTarget(int offscreenIdx, float logicalW, float logicalH) {
    flush();
    auto cmd = m_gpu.cmdBuf();
    dk::ImageView colorTarget{m_gpu.offscreenImage(offscreenIdx)};
    cmd.bindRenderTargets(&colorTarget);

    const uint32_t offW = (uint32_t)GpuDevice::offscreenWidth(offscreenIdx);
    const uint32_t offH = (uint32_t)GpuDevice::offscreenHeight(offscreenIdx);
    cmd.setViewports(0, DkViewport{0.f, 0.f, (float)offW, (float)offH, 0.f, 1.f});
    cmd.setScissors(0, DkScissor{0, 0, offW, offH});
    if (m_journalEnabled) {
        m_journalTarget = offscreenIdx;
        m_journalScissor = {0.f, 0.f, (float)offW, (float)offH};
        journalEvent(JournalKind::BindTarget);
    }

    // Zero means "address it in its own pixels", which is what the blur passes
    // want. A caller drawing a screen-space layer passes the screen size, and
    // the viewport above scales it down to fit.
    const float projW = logicalW > 0.f ? logicalW : (float)offW;
    const float projH = logicalH > 0.f ? logicalH : (float)offH;

    int slot = m_gpu.slot();
    VsUniforms vs;
    ortho(vs.projection, projW, projH);
    const uint32_t ring = m_gpu.nextVsUboSlot(slot);
    auto* ubo = static_cast<uint8_t*>(m_gpu.vsUboCpuAddrAt(slot, ring));
    std::memcpy(ubo, &vs, sizeof(vs));
    cmd.bindUniformBuffer(DkStage_Vertex, 0, m_gpu.vsUboGpuAddrAt(slot, ring),
                          GpuDevice::VS_UBO_SIZE);
}

void Renderer::restoreRenderTarget() {
    flush();
    auto cmd = m_gpu.cmdBuf();
    int slot = m_gpu.slot();
    dk::ImageView colorTarget{m_gpu.fbImage(slot)};
    dk::ImageView dsTarget{m_gpu.dsImage()};
    cmd.bindRenderTargets(&colorTarget, &dsTarget);

    cmd.setViewports(0, DkViewport{0.f, 0.f,
        (float)m_gpu.width(), (float)m_gpu.height(), 0.f, 1.f});
    if (m_clipStack.empty()) {
        cmd.setScissors(0, DkScissor{0, 0, (uint32_t)m_gpu.width(), (uint32_t)m_gpu.height()});
    } else {
        auto& r = m_clipStack.back();
        cmd.setScissors(0, DkScissor{(uint32_t)std::max(0.f, r.x), (uint32_t)std::max(0.f, r.y),
                                     (uint32_t)std::max(0.f, r.width), (uint32_t)std::max(0.f, r.height)});
    }

    if (m_journalEnabled) {
        m_journalTarget = -1;
        m_journalScissor = m_clipStack.empty()
            ? Rect{0.f, 0.f, (float)m_gpu.width(), (float)m_gpu.height()}
            : m_clipStack.back();
        journalEvent(JournalKind::RestoreTarget);
    }

    updateProjection();
}

void Renderer::captureToOffscreen(bool reuseIfValid) {
    if (reuseIfValid && m_reusableOffscreenCaptureValid) {
        return;
    }

    flush();
    ++m_frameCaptures;
    if (m_journalEnabled) journalEvent(JournalKind::Capture);
    auto cmd = m_gpu.cmdBuf();
    int slot = m_gpu.slot();

    // Ensure all current framebuffer writes are visible to the 2D blit engine
    // before we capture the scene behind the glass widget.
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);

    dk::ImageView src{m_gpu.fbImage(slot)};
    dk::ImageView dst{m_gpu.offscreenImage(GpuDevice::OFF_SCENE)};
    DkImageRect srcRect{
        0,
        0,
        0,
        (uint32_t)m_gpu.width(),
        (uint32_t)m_gpu.height(),
        1,
    };
    DkImageRect dstRect{
        0,
        0,
        0,
        (uint32_t)GpuDevice::offscreenWidth(GpuDevice::OFF_SCENE),
        (uint32_t)GpuDevice::offscreenHeight(GpuDevice::OFF_SCENE),
        1,
    };
    cmd.blitImage(src, srcRect, dst, dstRect, 0);

    // Barrier: ensure the blit completes before subsequent reads of offscreen 0
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);

    m_reusableOffscreenCaptureValid = reuseIfValid;
}

// The panels that cache their backdrop capture it at full resolution, into
// targets the per-frame scene capture never touches. They pay for it once per
// open; the scene capture pays every frame and stays half res.
void Renderer::captureToOffscreenSharp() {
    flush();
    ++m_frameCaptures;
    auto cmd = m_gpu.cmdBuf();
    int slot = m_gpu.slot();

    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);

    dk::ImageView src{m_gpu.fbImage(slot)};
    dk::ImageView dst{m_gpu.offscreenImage(GpuDevice::OFF_SHARP_A)};
    DkImageRect rect{
        0, 0, 0,
        (uint32_t)m_gpu.width(),
        (uint32_t)m_gpu.height(),
        1,
    };
    cmd.blitImage(src, rect, dst, rect, 0);
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
}

void Renderer::copyOffscreen(int srcTarget, int dstTarget) {
    if (!m_gpu.offscreenReady()) return;
    if (srcTarget < 0 || srcTarget >= GpuDevice::NUM_OFFSCREEN) return;
    if (dstTarget < 0 || dstTarget >= GpuDevice::NUM_OFFSCREEN) return;
    if (srcTarget == dstTarget) return;

    flush();

    auto cmd = m_gpu.cmdBuf();
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);

    dk::ImageView src{m_gpu.offscreenImage(srcTarget)};
    dk::ImageView dst{m_gpu.offscreenImage(dstTarget)};
    DkImageRect rect{
        0,
        0,
        0,
        (uint32_t)GpuDevice::offscreenWidth(dstTarget),
        (uint32_t)GpuDevice::offscreenHeight(dstTarget),
        1,
    };
    cmd.blitImage(src, rect, dst, rect, 0);
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
}

void Renderer::drawOffscreen(int target, const Rect& dest, const Color& tint) {
    useShader(ShaderProgram::Backdrop);
    bindTexture(m_offDescSlot[target]);
    addQuad(dest.x, dest.y, dest.right(), dest.bottom(), 0, 0, 1, 1, tint);
    flush();
    useShader(ShaderProgram::Basic);
}

void Renderer::drawOffscreenRounded(int target, const Rect& dest, float radius, const Color& tint) {
    if (target < 0 || target >= GpuDevice::NUM_OFFSCREEN) return;
    useShader(ShaderProgram::Backdrop);

    // The capture is screen sized, so the quad samples the sub-rect it covers.
    const float u0 = dest.x / (float)m_gpu.width();
    const float v0 = dest.y / (float)m_gpu.height();
    const float u1 = dest.right() / (float)m_gpu.width();
    const float v1 = dest.bottom() / (float)m_gpu.height();
    const Rect uv{u0, v0, u1 - u0, v1 - v0};

    bindTexture(m_offDescSlot[target]);
    if (radius <= 0.f) {
        addQuad(dest.x, dest.y, dest.right(), dest.bottom(), u0, v0, u1, v1, tint);
        flush();
    } else {
        drawRoundedMasked(dest, std::min(radius, std::min(dest.width, dest.height) * 0.5f),
                          tint, uv);
    }
    useShader(ShaderProgram::Basic);
}

void Renderer::drawLiquidGlass(int target, const Rect& panelRect, float radius,
                               const Color& tint, float opacity, float shade) {
    if (opacity <= 0.01f || target < 0 || target >= GpuDevice::NUM_OFFSCREEN) return;
    if (!m_gpu.offscreenReady()) {
        drawRoundedRect(panelRect, tint.withAlpha(tint.a * opacity), radius);
        return;
    }

    useShader(ShaderProgram::LiquidGlass);

    const auto& lg = m_liquidGlassSettings;
    auto clamp01 = [](float value) {
        return std::clamp(value, 0.0f, 1.0f);
    };

    FsUniforms fs = {};
    fs.useTexture = 1;
    fs.param1 = lg.refractionIntensity;
    fs.param2 = lg.blurIntensity;
    fs.param3 = lg.noiseIntensity;

    fs.extra[0] = lg.glowIntensity;
    fs.extra[1] = lg.saturation;
    fs.extra[2] = clamp01(lg.opacityMultiplier);
    fs.extra[3] = lg.roughness;

    fs.extra[4] = lg.animSpeed;
    fs.extra[5] = lg.time;
    fs.extra[6] = lg.powerFactor;
    fs.extra[7] = lg.fPower;

    fs.extra[8] = lg.refA;
    fs.extra[9] = lg.refB;
    fs.extra[10] = lg.refC;
    fs.extra[11] = lg.refD;

    fs.extra[12] = lg.glowWeight;
    fs.extra[13] = lg.glowBias;
    fs.extra[14] = lg.glowEdge0;
    fs.extra[15] = lg.glowEdge1;

    fs.extra[16] = tint.r * lg.tintBoost.r;
    fs.extra[17] = tint.g * lg.tintBoost.g;
    fs.extra[18] = tint.b * lg.tintBoost.b;
    fs.extra[19] = tint.a * lg.tintBoost.a;

    fs.extra[20] = panelRect.x;
    fs.extra[21] = panelRect.y;
    fs.extra[22] = panelRect.width;
    fs.extra[23] = panelRect.height;

    fs.extra[24] = (float)m_gpu.width();
    fs.extra[25] = (float)m_gpu.height();
    fs.extra[26] = clamp01(shade);
    // Corner radius in pixels. This function has accepted a radius since it was
    // written and never passed it on, so the shader built its corner purely
    // from powerFactor — a superellipse exponent. The settings overlay sets
    // that to 20, which is very nearly a sharp rectangle with a kink at the
    // corner: the roundness reported as "not linear".
    fs.extra[27] = std::max(0.0f, radius);

    pushFsUniforms(fs);
    bindTexture(m_offDescSlot[target]);
    addQuad(panelRect.x, panelRect.y, panelRect.right(), panelRect.bottom(), 0, 0, 1, 1,
            Color::white().withAlpha(opacity));
    flush();

    useShader(ShaderProgram::Basic);
}

void Renderer::beginScreenSpaceTarget(int offscreenIdx, const Color& clear) {
    bindRenderTarget(offscreenIdx, (float)m_gpu.width(), (float)m_gpu.height());
    m_gpu.cmdBuf().clearColor(0, DkColorMask_RGBA, clear.r, clear.g, clear.b, clear.a);
}

void Renderer::endScreenSpaceTarget() {
    restoreRenderTarget();
}

void Renderer::applyBlur(float radius, int passes) {
    // The blur works on the sharp pair now, so the scene capture it used to
    // clobber survives — no need to make the next frame recapture.
    applyBlurBetween(GpuDevice::OFF_SHARP_A, GpuDevice::OFF_SHARP_B, radius, passes);
}

// Ping-pongs between two targets of the same size, leaving the result in `a`.
// The wallpaper blur needs its own half-resolution pair, and running it through
// the full-resolution one would cost four times the pixels for something drawn
// every frame rather than once per panel.
void Renderer::applyBlurBetween(int a, int b, float radius, int passes) {
    if (!m_gpu.offscreenReady()) return;
    if (a < 0 || b < 0 || a >= GpuDevice::NUM_OFFSCREEN || b >= GpuDevice::NUM_OFFSCREEN) return;

    // The shader takes nine samples spaced `radius` texels apart, so it is only
    // a Gaussian while those samples are near neighbours. At radius 9 they land
    // 9, 18, 27 and 36 texels out with nothing read between, which is point
    // sampling a grid rather than blurring, and repeating it reinforces that
    // grid into the visible squares reported when the glass sharpness slider
    // goes down.
    //
    // Width comes from passes instead: sigma grows with the square root of the
    // count, so the spread is capped where the kernel still overlaps itself and
    // the rest is bought in repetitions. The cap on those keeps a slider at its
    // softest from turning one panel open into a visible hitch -- past it the
    // result is smooth but stops getting softer, which is the better failure.
    constexpr float kMaxSpread = 2.5f;
    constexpr int   kMaxPasses = 24;
    if (radius > kMaxSpread && passes > 0) {
        const float widen = radius / kMaxSpread;
        passes = std::min(kMaxPasses, (int)std::ceil(passes * widen * widen));
        radius = kMaxSpread;
    }

    m_frameBlurPasses += passes * 2;
    if (m_journalEnabled) journalEvent(JournalKind::BlurPass);

    const float offW = (float)GpuDevice::offscreenWidth(a);
    const float offH = (float)GpuDevice::offscreenHeight(a);

    for (int p = 0; p < passes; ++p) {
        // H blur: a -> b
        bindRenderTarget(b);
        m_gpu.cmdBuf().clearColor(0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 0.f);
        useShader(ShaderProgram::BlurH);
        FsUniforms fs = {};
        fs.useTexture = 1;
        fs.param1 = radius;
        fs.param2 = 1.f / offW;
        fs.param3 = 1.f / offH;
        pushFsUniforms(fs);
        bindTexture(m_offDescSlot[a]);
        addQuad(-1.f, -1.f, 1.f, 1.f, 0, 0, 1, 1, Color::white());
        flush();
        m_gpu.cmdBuf().barrier(DkBarrier_Full, DkInvalidateFlags_Image);

        // V blur: b -> a
        bindRenderTarget(a);
        m_gpu.cmdBuf().clearColor(0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 0.f);
        useShader(ShaderProgram::BlurV);
        pushFsUniforms(fs);
        bindTexture(m_offDescSlot[b]);
        addQuad(-1.f, -1.f, 1.f, 1.f, 0, 0, 1, 1, Color::white());
        flush();
        m_gpu.cmdBuf().barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    }

    restoreRenderTarget();
    useShader(ShaderProgram::Basic);
}

void Renderer::applyWave(float time, float amplitude, float frequency) {
    if (!m_gpu.offscreenReady()) return;

    useShader(ShaderProgram::Wave);
    FsUniforms fs = {};
    fs.useTexture = 1;
    fs.param1 = time;
    fs.param2 = amplitude;
    fs.param3 = frequency;
    pushFsUniforms(fs);

    Rect dest = {0, 0, (float)m_gpu.width(), (float)m_gpu.height()};
    bindTexture(m_offDescSlot[0]);
    addQuad(dest.x, dest.y, dest.right(), dest.bottom(), 0, 0, 1, 1, Color::white());
    flush();

    useShader(ShaderProgram::Basic);
}


// Geometry emission

// addQuad flushes when a quad would not fit, but this drops silently, and a
// frame that ran out mid-shape lost every remaining vertex without a trace:
// the menu rendered partially and flickered, since which shapes survived
// depended on how many the animated background had emitted first. printf goes
// nowhere on a console, so the drop is invisible; watch verts= in the perf log
// instead, which reports the peak against the 65536 cap.
void Renderer::addVertex(float x, float y, float u, float v, const Color& c) {
    if (m_vtxCount >= GpuDevice::MAX_VERTICES) {
        std::printf("[Renderer] WARN: vertex buffer full (%u)\n", m_vtxCount);
        return;
    }
    // Shadow the geometry the journal needs, in normal cached memory, at the
    // moment it is written, and check the incoming coordinate here.
    //
    // Shadowing was added to remove a suspected uncached-readback artifact.
    // It did not: 40-47 batches per frame still carry bounds near 1e19 even
    // though both the write and the read now happen in an ordinary vector, so
    // the value is already wrong when it arrives. The bit patterns decode to
    // plausible screen coordinates under a different exponent byte
    // (0xDF43EB6C -> 195.920, 0x5FC3EB6C -> 391.839) and differ from a sane
    // exponent by the same mask in both cases (0xDF^0xC3 == 0x5F^0x43 == 0x1C).
    //
    // This records the offending value at its entry point, together with the
    // shape state and emission site active at the time, so the caller that
    // produces it can be named instead of inferred. It matters because the
    // affected batches are the ones carrying alpha 0.200, and 1 - 0.200 is the
    // attenuation measured in the artifact.
    if (m_journalEnabled) {
        if (m_vtxCount < kVtxShadowCap) {
            auto& s = m_vtxShadow[m_vtxCount];
            s.x = x; s.y = y;
            s.r = c.r; s.g = c.g; s.b = c.b; s.a = c.a;
        }
        const bool sane = std::isfinite(x) && std::isfinite(y) &&
                          std::fabs(x) < 1e5f && std::fabs(y) < 1e5f;
        if (!sane) {
            ++m_journalBadVertices;
            if (m_journalBadSamples < kBadVertexSamples) {
                auto& b = m_journalBadVertex[m_journalBadSamples++];
                std::memcpy(&b.xBits, &x, 4);
                std::memcpy(&b.yBits, &y, 4);
                b.vtxIndex   = m_vtxCount;
                b.batchStart = m_vtxBatchStart;
                b.site       = (uint16_t)m_emitSite;
                b.tag        = m_drawTag;
                b.shader     = (uint16_t)m_curShader;
                b.radius     = m_shapeRadius;
                b.thickness  = m_shapeThickness;
                b.centreX    = m_shapeCentre.x;
                b.centreY    = m_shapeCentre.y;
                b.halfX      = m_shapeHalf.x;
                b.halfY      = m_shapeHalf.y;
                b.a          = c.a;
            }
        }
    }
    auto& vtx  = m_vtxBase[m_vtxCount++];
    if (m_vtxCount > m_peakVtxCount) m_peakVtxCount = m_vtxCount;
    vtx.x = x; vtx.y = y;
    vtx.u = u; vtx.v = v;
    vtx.sx = x - m_shapeCentre.x; vtx.sy = y - m_shapeCentre.y;
    vtx.hx = m_shapeHalf.x;       vtx.hy = m_shapeHalf.y;
    vtx.rad = m_shapeRadius;      vtx.thick = m_shapeThickness;
    vtx.r = c.r; vtx.g = c.g; vtx.b = c.b; vtx.a = c.a;
}

void Renderer::addQuad(float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1,
                        const Color& c)
{
    // flush() advances the batch marker but does not reclaim arena space — that
    // happens once per frame in beginFrame — so flushing here left the quad to
    // tear anyway. Drop it whole so the triangle stream stays aligned.
    if (m_vtxCount + 6 > GpuDevice::MAX_VERTICES) return;
    const EmitSiteScope site{*this, EmitSite::Quad};
    addVertex(x0, y0, u0, v0, c);
    addVertex(x1, y0, u1, v0, c);
    addVertex(x1, y1, u1, v1, c);
    addVertex(x0, y0, u0, v0, c);
    addVertex(x1, y1, u1, v1, c);
    addVertex(x0, y1, u0, v1, c);
}

void Renderer::addQuadGrad(float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1,
                            const Color& cTop, const Color& cBot)
{
    if (m_vtxCount + 6 > GpuDevice::MAX_VERTICES) return;   // see addQuad
    const EmitSiteScope site{*this, EmitSite::QuadGrad};
    addVertex(x0, y0, u0, v0, cTop);
    addVertex(x1, y0, u1, v0, cTop);
    addVertex(x1, y1, u1, v1, cBot);
    addVertex(x0, y0, u0, v0, cTop);
    addVertex(x1, y1, u1, v1, cBot);
    addVertex(x0, y1, u0, v1, cBot);
}

void Renderer::drawRect(const Rect& r, const Color& c) {
    // Record at the primitive boundary as well as at flush(). Batching can put
    // a fullscreen scrim and unrelated widgets into one GPU draw; a batch-only
    // record would then show their union and colour range but not prove that a
    // standalone dimming primitive actually existed. This entry preserves the
    // caller's exact rect and colour while Batch preserves what reached the GPU.
    if (m_journalEnabled) {
        DrawJournalEntry e;
        e.kind = JournalKind::Primitive;
        e.shader = (uint16_t)m_curShader;
        e.target = (int16_t)m_journalTarget;
        e.texSlot = WHITE_TEX_SLOT;
        e.verts = 6;
        e.x0 = r.x; e.y0 = r.y; e.x1 = r.right(); e.y1 = r.bottom();
        e.minR = e.maxR = c.r; e.minG = e.maxG = c.g;
        e.minB = e.maxB = c.b; e.minA = e.maxA = c.a;
        e.sx = m_journalScissor.x;     e.sy = m_journalScissor.y;
        e.sw = m_journalScissor.width; e.sh = m_journalScissor.height;
        journalPush(e);
    }
    bindTexture(-1);
    addQuad(r.x, r.y, r.right(), r.bottom(), 0, 0, 1, 1, c);
}

void Renderer::drawRectOutline(const Rect& r, const Color& c, float t) {
    drawRect({r.x, r.y, r.width, t}, c);
    drawRect({r.x, r.bottom() - t, r.width, t}, c);
    drawRect({r.x, r.y + t, t, r.height - 2*t}, c);
    drawRect({r.right()-t, r.y + t, t, r.height - 2*t}, c);
}

void Renderer::drawGradientRect(const Rect& r, const Color& top, const Color& bottom) {
    bindTexture(-1);
    addQuadGrad(r.x, r.y, r.right(), r.bottom(), 0, 0, 1, 1, top, bottom);
}

void Renderer::drawRoundedRect(const Rect& r, const Color& c, float radius) {
    if (radius <= 0.f) { drawRect(r, c); return; }
    bindTexture(-1);
    drawRoundedMasked(r, std::min(radius, std::min(r.width, r.height) * 0.5f),
                      c, Rect{0.f, 0.f, 1.f, 1.f});
}

// Shading the whole rect to draw a thin band is what put the frame over
// budget. The home screen emits about 65 strokes and circles a frame — the
// draw count went from 105 to 170 when they joined the mask — and at full
// rect each, together they cover more than the screen: measured 16.7ms a
// frame before, 19.1ms after, with the draw count itself proven irrelevant
// (halving it later moved nothing).
//
// Only the band is emitted now: four edge strips and four corner boxes. The
// mask parameters describe the whole shape regardless, so the distance field
// is unchanged and the sub-quads merely decide which fragments get to run it.
// One pixel of bleed outward keeps somewhere for the outer half of the
// antialiasing ramp to land.
void Renderer::drawRoundedRectOutline(const Rect& r, const Color& c, float radius, float t) {
    if (radius <= 0.f) { drawRectOutline(r, c, t); return; }
    if (t <= 0.f || r.width <= 0.f || r.height <= 0.f) return;

    const float rad = std::min(radius, std::min(r.width, r.height) * 0.5f);
    bindTexture(-1);

    // The band is the stroke plus a pixel of ramp on each side: the strips
    // start one pixel outside the edge and reach one past the stroke's inner
    // face, so neither half of the antialiasing gets clipped off.
    const float band = t + 2.f;
    const float cr   = rad + 1.f;       // corner box, same outward bleed
    const float midW = r.width  - 2.f * rad;
    const float midH = r.height - 2.f * rad;

    // Degenerate once the band is a sizeable share of the shape: the strips
    // would overlap and double-blend, so the plain quad is both simpler and no
    // more expensive at that size.
    if (band * 2.f >= std::min(r.width, r.height)) {
        drawRoundedMasked(r, rad, c, Rect{0.f, 0.f, 1.f, 1.f}, t);
        return;
    }

    beginShape(r, rad, t);
    const EmitSiteScope site{*this, EmitSite::RoundedOutline};

    const float x0 = r.x - 1.f, y0 = r.y - 1.f;
    const float x1 = r.right() + 1.f, y1 = r.bottom() + 1.f;

    auto quad = [&](float qx, float qy, float qw, float qh) {
        if (qw <= 0.f || qh <= 0.f) return;
        addQuad(qx, qy, qx + qw, qy + qh, 0.f, 0.f, 1.f, 1.f, c);
    };

    quad(x0,      y0,      cr, cr);     // corners
    quad(x1 - cr, y0,      cr, cr);
    quad(x0,      y1 - cr, cr, cr);
    quad(x1 - cr, y1 - cr, cr, cr);

    quad(r.x + rad, y0,        midW, band);   // edges
    quad(r.x + rad, y1 - band, midW, band);
    quad(x0,        r.y + rad, band, midH);
    quad(x1 - band, r.y + rad, band, midH);

    endShape();
}

void Renderer::drawFrostedInset(const Rect& r, const Color& tint,
                                const Color& border, const Color& highlight,
                                float radius, float opacity) {
    const float alpha = std::clamp(opacity, 0.f, 1.f);
    if (alpha <= 0.01f || r.width <= 0.f || r.height <= 0.f)
        return;

    drawRoundedRect({r.x, r.y + 3.f, r.width, r.height},
                    Color::black().withAlpha(0.12f * alpha), radius);
    drawRoundedRect(r, tint.withAlpha(tint.a * alpha), radius);
    drawRoundedRectOutline(r, border.withAlpha(border.a * alpha), radius, 1.f);
    drawRoundedRectOutline(r.shrunk(1.5f),
                           highlight.withAlpha(highlight.a * alpha),
                           std::max(0.f, radius - 1.5f), 1.f);
    drawRoundedRectOutline(r.shrunk(3.f),
                           Color::black().withAlpha(0.045f * alpha),
                           std::max(0.f, radius - 3.f), 1.f);
}

// A circle is the mask with the radius pinned to half the shorter side, so it
// shares the fill path rather than keeping a fan of its own. The segment count
// no longer means anything: the edge is exact at any size.
void Renderer::drawCircle(const Vec2& center, float radius, const Color& c, int segments) {
    (void)segments;
    if (radius <= 0.f) return;
    bindTexture(-1);
    const Rect box{center.x - radius, center.y - radius, radius * 2.f, radius * 2.f};
    drawRoundedMasked(box, radius, c, Rect{0.f, 0.f, 1.f, 1.f});
}

// These two emitted straight into the arena with no capacity check. addVertex
// drops silently once it is full, so a primitive that ran out part-way left a
// batch whose vertex count was no longer a multiple of three, and every
// triangle after it was assembled from a shifted triple — thin black streaks
// across the screen, which is a torn vertex stream rather than a shape bug.
//
// Flushing cannot help here: it only advances the batch marker, and the arena
// is reclaimed once per frame in beginFrame. So the primitive is dropped whole
// instead, which keeps the stream aligned and costs one shape rather than the
// rest of the frame.
void Renderer::drawTriangle(const Vec2& p1, const Vec2& p2, const Vec2& p3, const Color& c) {
    if (m_vtxCount + 3 > GpuDevice::MAX_VERTICES) return;
    bindTexture(-1);
    const EmitSiteScope site{*this, EmitSite::Triangle};
    addVertex(p1.x, p1.y, 0, 0, c);
    addVertex(p2.x, p2.y, 0, 0, c);
    addVertex(p3.x, p3.y, 0, 0, c);
}

void Renderer::drawLine(const Vec2& from, const Vec2& to, const Color& c, float thickness) {
    if (m_vtxCount + 6 > GpuDevice::MAX_VERTICES) return;
    Vec2 d = (to - from).normalized();
    Vec2 n = {-d.y, d.x};
    float ht = thickness * 0.5f;
    Vec2 a = from + n * ht, b = from - n * ht;
    Vec2 cc = to + n * ht,  dd = to - n * ht;
    bindTexture(-1);
    const EmitSiteScope site{*this, EmitSite::Line};
    addVertex(a.x, a.y, 0, 0, c);
    addVertex(b.x, b.y, 0, 0, c);
    addVertex(cc.x, cc.y, 0, 0, c);
    addVertex(b.x, b.y, 0, 0, c);
    addVertex(dd.x, dd.y, 0, 0, c);
    addVertex(cc.x, cc.y, 0, 0, c);
}

void Renderer::drawTexture(const Texture* tex, const Rect& dest, const Color& tint) {
    if (!tex) return;
    bindTexture(tex->descriptorSlot());
    addQuad(dest.x, dest.y, dest.right(), dest.bottom(), 0, 0, 1, 1, tint);
}

void Renderer::drawTextureSub(const Texture* tex, const Rect& src, const Rect& dest, const Color& tint) {
    if (!tex) return;
    float tw = (float)tex->width(), th = (float)tex->height();
    float u0 = src.x / tw, v0 = src.y / th;
    float u1 = src.right() / tw, v1 = src.bottom() / th;
    bindTexture(tex->descriptorSlot());
    addQuad(dest.x, dest.y, dest.right(), dest.bottom(), u0, v0, u1, v1, tint);
}

// A rounded fill is one quad; the fragment shader cuts the corner. The mask
// state is per shape, so the batch is closed on both sides of it and cleared
// afterwards, leaving every other draw in the unmasked state.
void Renderer::beginShape(const Rect& dest, float radius, float thickness) {
    m_shapeCentre    = {dest.x + dest.width * 0.5f, dest.y + dest.height * 0.5f};
    m_shapeHalf      = {dest.width * 0.5f, dest.height * 0.5f};
    m_shapeRadius    = radius;
    m_shapeThickness = thickness;
}

void Renderer::endShape() {
    m_shapeRadius    = 0.f;
    m_shapeThickness = 0.f;
}

void Renderer::drawRoundedMasked(const Rect& dest, float radius, const Color& c,
                                 const Rect& uv, float thickness) {
    beginShape(dest, radius, thickness);
    const EmitSiteScope site{*this, EmitSite::RoundedMasked};
    addQuad(dest.x, dest.y, dest.right(), dest.bottom(),
            uv.x, uv.y, uv.right(), uv.bottom(), c);
    endShape();
}

void Renderer::drawTextureRounded(const Texture* tex, const Rect& dest, float radius, const Color& tint) {
    if (!tex) { return; }
    if (radius <= 0) { drawTexture(tex, dest, tint); return; }
    bindTexture(tex->descriptorSlot());
    drawRoundedMasked(dest, std::min(radius, std::min(dest.width, dest.height) * 0.5f),
                      tint, Rect{0.f, 0.f, 1.f, 1.f});
}

// Um recorte da textura, com o mesmo corte de canto do desenho inteiro.
// Existe para a miniatura animada: os quadros vem numa folha unica e sao
// percorridos por UV, sem canto quadrado sobre o painel arredondado.
void Renderer::drawTextureRoundedSub(const Texture* tex, const Rect& src,
                                     const Rect& dest, float radius, const Color& tint) {
    if (!tex) return;
    const float tw = (float)tex->width(), th = (float)tex->height();
    if (tw <= 0.f || th <= 0.f) return;
    const Rect uv{src.x / tw, src.y / th, src.width / tw, src.height / th};
    bindTexture(tex->descriptorSlot());
    if (radius <= 0.f) {
        addQuad(dest.x, dest.y, dest.right(), dest.bottom(),
                uv.x, uv.y, uv.right(), uv.bottom(), tint);
        return;
    }
    drawRoundedMasked(dest, std::min(radius, std::min(dest.width, dest.height) * 0.5f),
                      tint, uv);
}

void Renderer::drawTextureSubRounded(const Texture* tex, const Rect& src,
                                     const Rect& dest, float radius,
                                     const Color& tint) {
    drawTextureRoundedSub(tex, src, dest, radius, tint);
}

void Renderer::drawText(const std::string& text, const Vec2& pos, Font* font,
                         const Color& color, float scale) {
    if (!font || text.empty()) return;
    font->draw(*this, text, pos, color, scale);
}

std::string Renderer::formatDrawJournal() const {
    static const char* kKindName[] = {
        "PRIMITIVE", "BATCH", "DIMMER", "CLEAR", "BIND-TGT", "RESTORE-TGT",
        "CLIP-PUSH", "CLIP-POP", "CAPTURE", "BLUR", "PRESENT",
    };
    static const char* kShaderName[] = {
        "Basic", "Backdrop", "BlurH", "BlurV", "Wave", "LiquidGlass", "Gradient",
    };

    std::string out;
    out.reserve(m_journal.size() * 128 + 512);

    char line[320];
    // The swapchain slot is recorded because a double-buffer hazard would bind
    // the artifact to one slot's parity. Across a 200-frame capture, glitches
    // landing on a single slot value is evidence no still image can give.
    std::snprintf(line, sizeof(line),
                  "frame_serial=%llu slot=%d entries=%zu dropped=%u fb=%dx%d "
                  "backbuffer_clear=%s rgba=(%.3f,%.3f,%.3f,%.3f)\n",
                  (unsigned long long)m_frameSerial, m_gpu.slot(), m_journal.size(),
                  m_journalDropped, m_gpu.width(), m_gpu.height(),
                  m_journalBackbufferClearSeen ? "yes" : "NO",
                  m_journalBackbufferClear.r, m_journalBackbufferClear.g,
                  m_journalBackbufferClear.b, m_journalBackbufferClear.a);
    out += line;

    // Timing of the frame this journal describes, sampled at its present.
    // present_interval is the gap since the previous present: at 60 Hz a frame
    // that met its deadline sits near 16.67ms, and the dump cadence of one
    // capture every third frame adds a known hitch on capture frames only.
    // acquire/fence separate waiting on the display from waiting on the GPU.
    std::snprintf(line, sizeof(line),
                  "timing: present_interval=%.3fms submit_to_present=%.3fms "
                  "frame_cpu=%.3fms acquire=%.3fms fence=%.3fms\n",
                  m_gpu.lastPresentIntervalNs() / 1e6,
                  m_gpu.lastSubmitToPresentNs() / 1e6,
                  m_gpu.lastFrameCpuNs() / 1e6,
                  m_gpu.lastAcquireNs() / 1e6,
                  m_gpu.lastFenceWaitNs() / 1e6);
    out += line;

    // Dual-readback self-test. The same GPU image was copied twice, back to
    // back, into separate buffers. Nothing downstream of presentation can write
    // into that image, so if these two reads disagree the capture path is the
    // unstable element and every attenuation figure measured from these dumps
    // must be re-read in that light.
    std::snprintf(line, sizeof(line),
                  "readback: mismatched_bytes=%u first_at=%d max_delta=%u %s\n",
                  m_gpu.lastDumpMismatchBytes(),
                  m_gpu.lastDumpFirstMismatch() == UINT32_MAX
                      ? -1 : (int)m_gpu.lastDumpFirstMismatch(),
                  m_gpu.lastDumpMaxDelta(),
                  m_gpu.lastDumpMismatchBytes() == 0
                      ? "(stable: captured pixels are what the GPU image holds)"
                      : "(UNSTABLE: capture path differs between two reads)");
    out += line;

    // The verdict line is computed here rather than on the PC, because the
    // question it answers is binary and the on-device record is the only place
    // where the answer is not a reconstruction. A "dimming draw" is a
    // backbuffer draw whose geometry covers essentially the whole framebuffer
    // (or an exact half of it), whose colour is dark, and whose alpha is
    // partial — the only combination that multiplies the existing image by a
    // constant instead of replacing it.
    //
    // The first version of this test scanned only Primitive entries, which
    // drawRect emits. The Plaza screen never calls drawRect: its captures
    // recorded zero Primitive entries, so the verdict read "0" for every frame
    // whether or not a dimmer existed. A zero that cannot become non-zero is
    // not evidence. Batch entries are scanned too, so any submitted geometry
    // covering the framebuffer is tested regardless of which caller emitted it.
    //
    // Batch bounds are read from the vertex arena, which lives in
    // DkMemBlockFlags_CpuUncached memory and has been observed to read back
    // with a corrupted exponent byte (values near 1e19). Those entries are
    // skipped rather than counted, since their geometry cannot be trusted; the
    // skipped count is reported so a frame full of them cannot masquerade as a
    // clean negative.
    // The per-quad scan in flush() already applied the geometry and colour
    // tests to each individual quad, so the verdict is a count of what it
    // found. Whole-batch ranges are no longer consulted for this decision:
    // batching mixes quads, and a batch summary cannot represent any single
    // one of them.
    int suspects = 0;
    int unreadable = 0;
    for (const auto& e : m_journal) {
        if (e.kind == JournalKind::Dimmer) { ++suspects; continue; }
        if (e.kind != JournalKind::Batch || e.target != -1) continue;
        const float w = e.x1 - e.x0;
        const float h = e.y1 - e.y0;
        if (!(std::fabs(e.x0) < 1e6f && std::fabs(e.y0) < 1e6f &&
              w >= 0.f && w < 1e6f && h >= 0.f && h < 1e6f))
            ++unreadable;
    }
    const bool trustworthy = (m_journalDropped == 0 && unreadable == 0 &&
                              m_journalNonQuadBatches == 0 &&
                              m_journalUnshadowed == 0);
    std::snprintf(line, sizeof(line),
                  "verdict: dimmer_quads=%d unreadable_batches=%d "
                  "non_quad_batches=%u unshadowed=%u %s\n",
                  suspects, unreadable, m_journalNonQuadBatches,
                  m_journalUnshadowed,
                  suspects > 0
                      ? "(a submitted quad covers the framebuffer and dims it)"
                      : (trustworthy
                             ? "(0 => no submitted quad could have dimmed it)"
                             : "(INCONCLUSIVE: some geometry was untestable)"));
    out += line;

    // Vertices that were already invalid when they reached addVertex, reported
    // with the raw bits and the state that produced them. The mantissa is
    // printed re-exponented to 0x43 so the plausible original coordinate is
    // visible next to the corrupted value.
    static const char* kSiteName[] = {
        "none", "quad", "quadGrad", "quad3D", "roundedMasked", "roundedOutline",
        "circle", "triangle", "line", "text", "offscreen", "glass", "blur",
    };
    // Scene population and submitted work for this frame. The artifact becomes
    // more frequent the longer Plaza runs, so something accumulates; these make
    // "something" measurable per frame instead of inferred. Draw counts come
    // from the previous frame's totals, which is what the existing counters
    // expose, and the scene counts are whatever the owning screen last
    // reported.
    std::snprintf(line, sizeof(line),
                  "scene: miis=%u pedestals=%u bubbles=%u ambient=%u shapes=%u "
                  "draws=%u binds=%u verts=%u blurs=%u captures=%u\n",
                  m_sceneMiis, m_scenePedestals, m_sceneBubbles,
                  m_sceneAmbient, m_sceneShapes,
                  m_lastFrameDrawCalls, m_lastFramePipelineBinds,
                  m_lastFrameVertices, m_lastFrameBlurPasses,
                  m_lastFrameCaptures);
    out += line;

    std::snprintf(line, sizeof(line),
                  "badverts: total=%u sampled=%u\n",
                  m_journalBadVertices, m_journalBadSamples);
    out += line;

    if (!m_probes.empty() || m_probesDropped) {
        std::snprintf(line, sizeof(line), "probes: %zu dropped=%u\n",
                      m_probes.size(), m_probesDropped);
        out += line;
        for (const auto& p : m_probes) {
            out += "  ";
            out += p;
            out += '\n';
        }
    }
    for (uint32_t i = 0; i < m_journalBadSamples; ++i) {
        const auto& b = m_journalBadVertex[i];
        const uint32_t rx = (0x43u << 24) | (b.xBits & 0x00FFFFFFu);
        const uint32_t ry = (0x43u << 24) | (b.yBits & 0x00FFFFFFu);
        float fx, fy;
        std::memcpy(&fx, &rx, 4);
        std::memcpy(&fy, &ry, 4);
        const char* site = ((size_t)b.site < std::size(kSiteName))
                         ? kSiteName[b.site] : "?";
        std::snprintf(line, sizeof(line),
            "  bad[%u] tag=%-22s site=%-14s shader=%u vtx=%u batchStart=%u alpha=%.3f\n"
            "        xbits=0x%08X ybits=0x%08X remantissa=(%.3f,%.3f)\n"
            "        shape centre=(%.1f,%.1f) half=(%.1f,%.1f) rad=%.2f thick=%.2f\n",
            i, (b.tag && *b.tag) ? b.tag : "(untagged)", site,
            b.shader, b.vtxIndex, b.batchStart, b.a,
            b.xBits, b.yBits, fx, fy,
            b.centreX, b.centreY, b.halfX, b.halfY, b.radius, b.thickness);
        out += line;
    }

    for (size_t i = 0; i < m_journal.size(); ++i) {
        const auto& e = m_journal[i];
        const char* kind = ((size_t)e.kind < std::size(kKindName))
                         ? kKindName[(size_t)e.kind] : "?";
        if (e.kind == JournalKind::Primitive || e.kind == JournalKind::Batch ||
            e.kind == JournalKind::Dimmer) {
            const char* sh = ((size_t)e.shader < std::size(kShaderName))
                           ? kShaderName[e.shader] : "?";
            // Attenuation is what this draw would multiply the destination by
            // under the frame's SrcAlpha/InvSrcAlpha blend, for a black source.
            // Printing it makes the comparison against the measured 0.8000 /
            // 0.6091 / 0.4711 ratios direct rather than mental arithmetic.
            std::snprintf(line, sizeof(line),
                "%03zu %-11s tgt=%-3d sh=%-11s v=%-4u rect=[%.1f,%.1f %.1fx%.1f] "
                "rgbaRange=(%.3f..%.3f,%.3f..%.3f,%.3f..%.3f,%.3f..%.3f) "
                "attenRange=%.4f..%.4f tex=%d uv=[%.3f..%.3f,%.3f..%.3f] "
                "scissor=[%.0f,%.0f %.0fx%.0f]\n",
                i, kind, (int)e.target, sh, e.verts,
                e.x0, e.y0, e.x1 - e.x0, e.y1 - e.y0,
                e.minR, e.maxR, e.minG, e.maxG, e.minB, e.maxB,
                e.minA, e.maxA, 1.f - e.maxA, 1.f - e.minA, (int)e.texSlot,
                e.minU, e.maxU, e.minV, e.maxV,
                e.sx, e.sy, e.sw, e.sh);
        } else {
            std::snprintf(line, sizeof(line),
                "%03zu %-11s tgt=%-3d rgba=(%.3f,%.3f,%.3f,%.3f) "
                "scissor=[%.0f,%.0f %.0fx%.0f]\n",
                i, kind, (int)e.target,
                e.minR, e.minG, e.minB, e.minA,
                e.sx, e.sy, e.sw, e.sh);
        }
        out += line;
    }
    return out;
}

void Renderer::pushClipRect(const Rect& r) {
    flush();
    Rect clip = r;
    if (!m_clipStack.empty()) {
        auto& prev = m_clipStack.back();
        float x0 = std::max(clip.x, prev.x);
        float y0 = std::max(clip.y, prev.y);
        float x1 = std::min(clip.right(), prev.right());
        float y1 = std::min(clip.bottom(), prev.bottom());
        clip = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    m_clipStack.push_back(clip);
    m_gpu.cmdBuf().setScissors(0, DkScissor{
        (uint32_t)std::max(0.f, clip.x), (uint32_t)std::max(0.f, clip.y),
        (uint32_t)std::max(0.f, clip.width), (uint32_t)std::max(0.f, clip.height)});
    if (m_journalEnabled) {
        m_journalScissor = clip;
        journalEvent(JournalKind::ClipPush);
    }
}

void Renderer::popClipRect() {
    flush();
    if (!m_clipStack.empty()) m_clipStack.pop_back();
    if (m_clipStack.empty()) {
        m_gpu.cmdBuf().setScissors(0, DkScissor{
            0, 0, (uint32_t)m_gpu.width(), (uint32_t)m_gpu.height()});
    } else {
        auto& r = m_clipStack.back();
        m_gpu.cmdBuf().setScissors(0, DkScissor{
            (uint32_t)std::max(0.f, r.x), (uint32_t)std::max(0.f, r.y),
            (uint32_t)std::max(0.f, r.width), (uint32_t)std::max(0.f, r.height)});
    }
    if (m_journalEnabled) {
        m_journalScissor = m_clipStack.empty()
            ? Rect{0.f, 0.f, (float)m_gpu.width(), (float)m_gpu.height()}
            : m_clipStack.back();
        journalEvent(JournalKind::ClipPop);
    }
}

} // namespace nxui
