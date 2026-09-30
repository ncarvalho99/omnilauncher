#include <nxui/core/GpuDevice.hpp>
#include <cstdio>
#include <cstring>
#include <array>

namespace nxui {

namespace {

// Sem este retorno o deko3d nao tem para onde falar, e uma alocacao recusada
// derruba o processo sem uma linha em lugar nenhum -- que e exatamente a forma
// das mortes que investigamos: o log do menu termina no meio e o daemon
// relanca. Com ele, o motivo fica escrito.
void deviceDebug(void* userData, const char* context, DkResult result, const char* message) {
    (void)userData;
    char buf[320];
    std::snprintf(buf, sizeof(buf), "[deko3d] %s: resultado=%d %s",
                  context ? context : "?", (int)result, message ? message : "");
    svcOutputDebugString(buf, std::strlen(buf));
    if (GpuDevice::debugSink())
        GpuDevice::debugSink()(buf);
}

}  // namespace

GpuDevice::DebugSink GpuDevice::s_debugSink = nullptr;

bool GpuDevice::initialize() {
    m_bulkTeardown = false;
    m_uploadSlot = 0;
    m_uploadBatchOpen = false;
    m_frameUploads = 0;
    m_lastFrameUploads = 0;
    m_frameUploadBatches = 0;
    m_lastFrameUploadBatches = 0;
    m_frameUploadWaitNs = 0;
    m_lastFrameUploadWaitNs = 0;
    m_frameDumpArmed = false;
    m_frameDumpPending = false;
    m_dev   = dk::DeviceMaker{}.setCbDebug(deviceDebug).create();
    m_queue = dk::QueueMaker{m_dev}.setFlags(DkQueueFlags_Graphics).create();

    // Two dump buffers, not one. The second is a self-test: endFrame copies the
    // same framebuffer image into both, back to back in one command list, and
    // takeFrameDump compares them.
    //
    // This separates the only two possibilities left for the Plaza artifact.
    // The dump reads the rendered image on the GPU before presentImage, so
    // nothing downstream of presentation — backlight, panel, scanout, dock —
    // can write back into it. Either the image genuinely contains attenuated
    // pixels, or the readback path returned something the image does not hold.
    // Identical copies mean the former and exonerate the capture; differing
    // copies mean the measurement itself is unstable and every attenuation
    // figure recorded so far has to be re-read in that light.
    constexpr uint32_t frameDumpSize = FB_WIDTH * FB_HEIGHT * 4u;
    for (int i = 0; i < 2; ++i) {
        m_frameDumpBuffers[i] = dk::MemBlockMaker{m_dev, frameDumpSize}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
        if (!m_frameDumpBuffers[i]) {
            std::fprintf(stderr,
                         "[GpuDevice] frame-dump buffer %d allocation failed (%u bytes)\n",
                         i, frameDumpSize);
        }
    }

    for (int i = 0; i < NUM_FB; ++i) {
        m_cmdPool[i].create(m_dev, CMD_BUF_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        m_cmdbuf[i] = dk::CmdBufMaker{m_dev}.create();
        m_cmdbuf[i].addMemory(m_cmdPool[i].block, 0, CMD_BUF_SIZE);
    }

    for (int i = 0; i < UPLOAD_SLOT_COUNT; ++i) {
        m_uploadInFlight[i] = false;
        m_uploadCopyCount[i] = 0;
        if (!m_uploadCmdPool[i].create(
                m_dev, UPLOAD_CMD_BUF_SIZE,
                DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached) ||
            !m_uploadStagingPool[i].create(
                m_dev, UPLOAD_STAGING_SIZE,
                DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
            std::fprintf(stderr,
                         "[GpuDevice] upload slot %d allocation failed\n", i);
            return false;
        }
        m_uploadCmdbuf[i] = dk::CmdBufMaker{m_dev}.create();
        m_uploadCmdbuf[i].addMemory(m_uploadCmdPool[i].block, 0, UPLOAD_CMD_BUF_SIZE);
    }

    m_codePool.create(m_dev, CODE_POOL_SIZE,
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);

    uint32_t dataSize = 0;
    for (int i = 0; i < NUM_FB; ++i) {
        dataSize += VTX_BUF_SIZE + 256;
        dataSize += IDX_BUF_SIZE + 256;
        dataSize += VS_UBO_SIZE * VS_UBO_RING + 256;
        dataSize += FS_UBO_SIZE * FS_UBO_RING + 256;
    }
    dataSize += MAX_TEXTURES * sizeof(DkImageDescriptor) + DK_IMAGE_DESCRIPTOR_ALIGNMENT;
    dataSize += MAX_SAMPLERS * sizeof(DkSamplerDescriptor) + DK_SAMPLER_DESCRIPTOR_ALIGNMENT;
    dataSize = (dataSize + kGpuAlign - 1) & ~(kGpuAlign - 1);
    m_dataPool.create(m_dev, dataSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);

    for (int i = 0; i < NUM_FB; ++i) {
        m_vtxOff[i]   = m_dataPool.alloc(VTX_BUF_SIZE, 256);
        m_idxOff[i]   = m_dataPool.alloc(IDX_BUF_SIZE, 256);
        m_vsUboOff[i] = m_dataPool.alloc(VS_UBO_SIZE * VS_UBO_RING, DK_UNIFORM_BUF_ALIGNMENT);
        m_fsUboOff[i] = m_dataPool.alloc(FS_UBO_SIZE * FS_UBO_RING, DK_UNIFORM_BUF_ALIGNMENT);
    }
    m_imgDescOff = m_dataPool.alloc(MAX_TEXTURES * sizeof(DkImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    m_samDescOff = m_dataPool.alloc(MAX_SAMPLERS * sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);

    createFramebuffers();
    createDepthStencil();
    createOffscreenTargets();

    std::array<DkImage const*, NUM_FB> fbArray;
    for (int i = 0; i < NUM_FB; ++i) fbArray[i] = &m_fbImages[i];
    m_swapchain = dk::SwapchainMaker{m_dev, nwindowGetDefault(), fbArray}.create();

    std::printf("[GpuDevice] upload ring: %d slots, %u KiB staging each\n",
                UPLOAD_SLOT_COUNT, UPLOAD_STAGING_SIZE / 1024u);

    return true;
}

void GpuDevice::createFramebuffers() {
    dk::ImageLayout fbLayout;
    dk::ImageLayoutMaker{m_dev}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                  DkImageFlags_Usage2DEngine)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(FB_WIDTH, FB_HEIGHT)
        .initialize(fbLayout);

    uint64_t fbSize  = fbLayout.getSize();
    uint64_t fbAlign = fbLayout.getAlignment();
    uint32_t totalFb = 0;
    for (int i = 0; i < NUM_FB; ++i) {
        totalFb = (totalFb + fbAlign - 1) & ~(fbAlign - 1);
        totalFb += fbSize;
    }
    m_fbPool.create(m_dev, totalFb, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);

    uint32_t off = 0;
    for (int i = 0; i < NUM_FB; ++i) {
        off = (off + fbAlign - 1) & ~(fbAlign - 1);
        m_fbImages[i].initialize(fbLayout, m_fbPool.block, off);
        off += fbSize;
    }
}

void GpuDevice::createDepthStencil() {
    dk::ImageLayout dsLayout;
    dk::ImageLayoutMaker{m_dev}
        .setFlags(DkImageFlags_UsageRender)
        .setFormat(DkImageFormat_S8)
        .setDimensions(FB_WIDTH, FB_HEIGHT)
        .initialize(dsLayout);

    uint32_t dsSize = dsLayout.getSize();
    dsSize = (dsSize + kGpuAlign - 1) & ~(kGpuAlign - 1);
    m_dsPool.create(m_dev, dsSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    m_dsImage.initialize(dsLayout, m_dsPool.block, 0);
}

void GpuDevice::createOffscreenTargets() {
    dk::ImageLayout layouts[NUM_OFFSCREEN];
    for (int i = 0; i < NUM_OFFSCREEN; ++i) {
        dk::ImageLayoutMaker{m_dev}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(offscreenWidth(i), offscreenHeight(i))
            .initialize(layouts[i]);
    }

    uint32_t totalOff = 0;
    for (int i = 0; i < NUM_OFFSCREEN; ++i) {
        const uint64_t a = layouts[i].getAlignment();
        totalOff = (totalOff + a - 1) & ~(a - 1);
        totalOff += layouts[i].getSize();
    }
    m_offPool.create(m_dev, totalOff, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);

    uint32_t off = 0;
    for (int i = 0; i < NUM_OFFSCREEN; ++i) {
        const uint64_t a = layouts[i].getAlignment();
        off = (off + a - 1) & ~(a - 1);
        m_offImages[i].initialize(layouts[i], m_offPool.block, off);
        off += layouts[i].getSize();
    }

    // Freshly mapped VRAM holds whatever the previous process left in it. These
    // targets are sampled by descriptor slot, and not every consumer writes one
    // before reading it in the same frame: FolderBackdrop draws OFF_SETTINGS
    // gated only on its own fade alpha, so before the first folder capture ever
    // runs it samples uninitialised memory. That is the boot-time-only garbage —
    // once a capture has written a target the stale contents are gone, which is
    // why the artifacts were never reproducible after the first interaction and
    // why two previous fixes aimed at shape geometry could not land.
    //
    // Zero them once, here, before m_offscreenReady lets anything sample them.
    // This is init-time and runs exactly once, so it costs nothing per frame.
    auto& cmd = m_cmdbuf[0];
    cmd.clear();
    cmd.addMemory(m_cmdPool[0].block, 0, CMD_BUF_SIZE);
    for (int i = 0; i < NUM_OFFSCREEN; ++i) {
        const uint32_t w = (uint32_t)offscreenWidth(i);
        const uint32_t h = (uint32_t)offscreenHeight(i);
        dk::ImageView target{m_offImages[i]};
        cmd.bindRenderTargets(&target);
        cmd.setViewports(0, DkViewport{0.f, 0.f, (float)w, (float)h, 0.f, 1.f});
        cmd.setScissors(0, DkScissor{0, 0, w, h});
        cmd.clearColor(0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 0.f);
    }
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    m_queue.submitCommands(cmd.finishList());
    m_queue.waitIdle();

    m_offscreenReady = true;
}

int GpuDevice::beginFrame() {
    // Most texture work happens in Activity::onUpdate, before beginFrame. Send
    // that batch now so it can execute while acquireImage waits for display.
    // A second batch submission in endFrame covers glyphs created while rendering.
    const bool submittedUploads = m_uploadBatchOpen &&
        m_uploadCopyCount[m_uploadSlot] > 0;
    submitUploadBatch();
    if (submittedUploads)
        m_queue.flush();

    // These two waits mean different things and have to be told apart.
    //
    // acquireImage blocks until the display releases a buffer, so it absorbs
    // vsync: a frame with time to spare waits here. The fence blocks until the
    // GPU has finished the previous frame in this slot, so it only grows when
    // the GPU is genuinely behind.
    //
    // Wall-clock frame time cannot distinguish them. The settings overlay
    // reports exactly 33.3ms, which is two refresh periods, and says only that
    // something exceeded 16.67ms — not what, and not by how much. Four fixes
    // guessed from reading the render path all missed.
    const uint64_t tAcquire = armGetSystemTick();
    m_slot = m_queue.acquireImage(m_swapchain);
    const uint64_t tFence = armGetSystemTick();
    m_frameFences[m_slot].wait();
    const uint64_t tDone = armGetSystemTick();

    m_lastAcquireNs = armTicksToNs(tFence - tAcquire);
    m_lastFenceWaitNs = armTicksToNs(tDone - tFence);
    m_lastFrameUploads = m_frameUploads;
    m_lastFrameUploadBatches = m_frameUploadBatches;
    m_lastFrameUploadWaitNs = m_frameUploadWaitNs;
    m_frameUploads = 0;
    m_frameUploadBatches = 0;
    m_frameUploadWaitNs = 0;

    // Reset command buffer and re-feed its memory (clear invalidates memory
    // tracking — following the deko3d sample framework pattern).
    m_cmdbuf[m_slot].clear();
    m_cmdbuf[m_slot].addMemory(m_cmdPool[m_slot].block, 0, CMD_BUF_SIZE);
    m_frameBeginTick = tDone;
    return m_slot;
}

void GpuDevice::endFrame() {
    // Upload lists must enter the queue before the frame that can sample their
    // destination images. Queue submission order supplies the GPU dependency;
    // no device-wide idle is needed here.
    submitUploadBatch();

    if (m_frameDumpArmed && m_frameDumpBuffers[0] && m_frameDumpBuffers[1]) {
        auto& cmd = m_cmdbuf[m_slot];
        const DkImageRect rect{0, 0, 0, (uint32_t)FB_WIDTH, (uint32_t)FB_HEIGHT, 1};
        cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        // Both copies read the same image with the same barriers around them,
        // so any difference between them is produced by the readback path
        // rather than by the scene.
        for (int i = 0; i < 2; ++i) {
            cmd.copyImageToBuffer(
                dk::ImageView{m_fbImages[m_slot]}, rect,
                DkCopyBuf{m_frameDumpBuffers[i].getGpuAddr(),
                          (uint32_t)(FB_WIDTH * 4u),
                          (uint32_t)(FB_WIDTH * FB_HEIGHT * 4u)});
            cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        }
        cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        m_frameDumpArmed = false;
        m_frameDumpPending = true;
    }

    // Signal the fence for this slot so the NEXT time beginFrame()
    // acquires the same slot, it can wait for completion.
    m_cmdbuf[m_slot].signalFence(m_frameFences[m_slot]);

    auto cmdList = m_cmdbuf[m_slot].finishList();
    const uint64_t tSubmit = armGetSystemTick();
    m_queue.submitCommands(cmdList);
    m_queue.presentImage(m_swapchain, m_slot);
    const uint64_t tPresent = armGetSystemTick();

    m_lastSubmitToPresentNs = armTicksToNs(tPresent - tSubmit);
    m_lastFrameCpuNs = m_frameBeginTick ? armTicksToNs(tPresent - m_frameBeginTick) : 0;
    m_lastPresentIntervalNs =
        m_prevPresentTick ? armTicksToNs(tPresent - m_prevPresentTick) : 0;
    m_prevPresentTick = tPresent;
}

bool GpuDevice::requestFrameDump() {
    if (!m_frameDumpBuffers[0] || !m_frameDumpBuffers[1] || frameDumpBusy())
        return false;
    m_frameDumpArmed = true;
    return true;
}

bool GpuDevice::takeFrameDump(std::vector<std::uint8_t>& rgba) {
    if (!m_frameDumpPending || !m_frameDumpBuffers[0] || !m_frameDumpBuffers[1])
        return false;

    // endFrame submitted the copy before present. A dump is diagnostic and
    // deliberately trades one visible hitch for an exact, fully-composed frame.
    m_queue.waitIdle();
    constexpr uint32_t kBytes = FB_WIDTH * FB_HEIGHT * 4u;
    const auto* a = static_cast<const std::uint8_t*>(m_frameDumpBuffers[0].getCpuAddr());
    const auto* b = static_cast<const std::uint8_t*>(m_frameDumpBuffers[1].getCpuAddr());

    // Compare the two independent reads of the same image. Record where they
    // first diverge and how far apart they are, so a disagreement is diagnosed
    // rather than merely counted: a readback that is unstable in the same
    // diagonal pattern as the artifact would be the artifact's explanation.
    m_lastDumpMismatchBytes = 0;
    m_lastDumpFirstMismatch = UINT32_MAX;
    m_lastDumpMaxDelta = 0;
    for (uint32_t i = 0; i < kBytes; ++i) {
        if (a[i] == b[i]) continue;
        ++m_lastDumpMismatchBytes;
        if (m_lastDumpFirstMismatch == UINT32_MAX) m_lastDumpFirstMismatch = i;
        const int d = (int)a[i] - (int)b[i];
        const uint32_t ad = (uint32_t)(d < 0 ? -d : d);
        if (ad > m_lastDumpMaxDelta) m_lastDumpMaxDelta = ad;
    }

    rgba.assign(a, a + kBytes);
    m_frameDumpPending = false;
    return true;
}

void GpuDevice::beginUploadBatch() {
    if (m_uploadBatchOpen)
        return;

    const int slot = m_uploadSlot;
    if (m_uploadInFlight[slot]) {
        // submitCommands may keep small lists in deko3d's queue buffer until
        // the flush threshold or present. A same-frame upload burst can wrap
        // all slots before either occurs, so kick their fence signals before
        // waiting. Without this, Theme Shop's first glyph burst deadlocks.
        m_queue.flush();
        const uint64_t waitStart = armGetSystemTick();
        m_uploadFences[slot].wait();
        const uint64_t waitEnd = armGetSystemTick();
        m_frameUploadWaitNs += armTicksToNs(waitEnd - waitStart);
        m_uploadInFlight[slot] = false;
    }

    // The fence above is the lifetime boundary for both command memory and any
    // oversized source block owned by this slot.
    m_uploadTempStaging[slot] = {};
    m_uploadStagingPool[slot].used = 0;
    m_uploadCopyCount[slot] = 0;
    m_uploadCmdbuf[slot].clear();
    m_uploadCmdbuf[slot].addMemory(
        m_uploadCmdPool[slot].block, 0, UPLOAD_CMD_BUF_SIZE);
    m_uploadBatchOpen = true;
}

void GpuDevice::submitUploadBatch() {
    if (!m_uploadBatchOpen)
        return;

    const int slot = m_uploadSlot;
    if (m_uploadCopyCount[slot] == 0) {
        m_uploadBatchOpen = false;
        return;
    }

    // copyBufferToImage uses the 2D engine. Make every image write visible to
    // later texture sampling before signalling that this slot may be reused.
    m_uploadCmdbuf[slot].barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    m_uploadCmdbuf[slot].signalFence(m_uploadFences[slot]);
    m_queue.submitCommands(m_uploadCmdbuf[slot].finishList());
    m_uploadInFlight[slot] = true;
    ++m_frameUploadBatches;
    m_uploadBatchOpen = false;
    m_uploadSlot = (slot + 1) % UPLOAD_SLOT_COUNT;
    m_queue.flush();
}

void GpuDevice::retireSubmittedUploads() {
    for (int i = 0; i < UPLOAD_SLOT_COUNT; ++i) {
        m_uploadInFlight[i] = false;
        m_uploadCopyCount[i] = 0;
        m_uploadTempStaging[i] = {};
        m_uploadStagingPool[i].used = 0;
    }
    m_uploadBatchOpen = false;
    m_uploadSlot = 0;
}

void GpuDevice::waitIdle() {
    if (!m_queue)
        return;
    submitUploadBatch();
    m_queue.waitIdle();
    retireSubmittedUploads();
}

void GpuDevice::beginBulkTeardown() {
    if (m_bulkTeardown)
        return;
    waitIdle();
    m_bulkTeardown = true;
}

uint64_t GpuDevice::s_imageBudget = GpuDevice::kDefaultImageBudget;

dk::UniqueMemBlock GpuDevice::allocImageMemory(uint32_t size) {
    size = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
    if (m_imageMemUsed + size > s_imageBudget) {
        std::fprintf(stderr, "[GpuDevice] image budget exceeded (%llu + %u > %llu), skipping\n",
                     (unsigned long long)m_imageMemUsed, size,
                     (unsigned long long)s_imageBudget);
        return {};  // return empty MemBlock — caller should check validity
    }
    // Do not treat TotalMemorySize - UsedMemorySize as malloc headroom here.
    // svcSetHeapSize reserves the process heap and counts that reservation as
    // used, so the difference can be small while the heap still has ample free
    // space. The image budget bounds our allocations; the checked deko3d
    // allocation below is the authoritative availability test.

    auto blk = dk::MemBlockMaker{m_dev, size}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();

    // A failed allocation returns an unusable block. Keep the counter unchanged
    // and let callers stop loading optional images instead of initializing a
    // deko3d image with an invalid block.
    if (!blk) {
        std::fprintf(stderr, "[GpuDevice] image allocation failed (%u bytes)\n", size);
        return {};
    }

    m_imageMemUsed += size;
    return blk;
}

void GpuDevice::freeImageMemory(uint32_t size) {
    size = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
    if (size <= m_imageMemUsed)
        m_imageMemUsed -= size;
    else
        m_imageMemUsed = 0;
}

GpuDevice::ImageAlloc GpuDevice::allocImageFromPool(uint32_t size, uint32_t alignment) {
    if (alignment < kGpuAlign) alignment = kGpuAlign;
    size = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
    if (m_imageMemUsed + size > s_imageBudget) {
        std::fprintf(stderr, "[GpuDevice] pool budget exceeded (%llu + %u > %llu)\n",
                     (unsigned long long)m_imageMemUsed, size,
                     (unsigned long long)s_imageBudget);
        return {};
    }
    // Try to fit in an existing chunk
    for (auto& chunk : m_imageChunks) {
        uint32_t aligned = (chunk.used + alignment - 1) & ~(alignment - 1);
        if (aligned + size <= chunk.size) {
            chunk.used = aligned + size;
            m_imageMemUsed += size;
            m_poolMemUsed  += size;
            return {chunk.block, aligned};
        }
    }
    // Allocate a new chunk
    uint32_t chunkSize = std::max(kImageChunkSize, size);
    chunkSize = (chunkSize + kGpuAlign - 1) & ~(kGpuAlign - 1);
    auto blk = dk::MemBlockMaker{m_dev, chunkSize}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    m_imageChunks.push_back({std::move(blk), chunkSize, size});
    m_imageMemUsed += size;
    m_poolMemUsed  += size;
    return {m_imageChunks.back().block, 0};
}

void GpuDevice::resetImagePool() {
    m_imageChunks.clear();
    // Only subtract pool memory; individual allocations remain tracked.
    if (m_poolMemUsed <= m_imageMemUsed)
        m_imageMemUsed -= m_poolMemUsed;
    else
        m_imageMemUsed = 0;
    m_poolMemUsed = 0;
}

bool GpuDevice::uploadTexture(dk::Image& dst, const void* pixels, uint32_t size,
                              uint32_t w, uint32_t h, uint64_t expectedBytes)
{
    // deko3d does not report a bad upload, it calls svcBreak: a crash report
    // resolving to dk::detail::RaiseError under ImageLayout::calcLevelOffset is
    // what that looks like, and one arrived from a 1TB card where the menu had
    // uploaded a great many icons. Nothing here checked the arguments first, so
    // a zero-sized or mismatched upload took the process with it instead of
    // failing. These are the cases that can be caught without asking deko3d.
    if (!pixels || size == 0 || w == 0 || h == 0 || !dst.getGpuAddr()) {
        std::fprintf(stderr,
                     "[GpuDevice] refusing texture upload %ux%u size=%u pixels=%p gpuAddr=%llx\n",
                     w, h, size, pixels, (unsigned long long)dst.getGpuAddr());
        return false;
    }
    const uint64_t wanted = expectedBytes ? expectedBytes : (uint64_t)w * h * 4;
    if (size < wanted) {
        std::fprintf(stderr,
                     "[GpuDevice] refusing texture upload %ux%u: %u bytes is short of %llu\n",
                     w, h, size, (unsigned long long)wanted);
        return false;
    }

    beginUploadBatch();

    const bool oversized = size > UPLOAD_STAGING_SIZE;
    const auto& currentStaging = m_uploadStagingPool[m_uploadSlot];
    const uint32_t alignedUsed =
        (currentStaging.used + 255u) & ~255u;
    const bool stagingHasRoom = alignedUsed <= currentStaging.size &&
        size <= currentStaging.size - alignedUsed;
    if (!oversized &&
        (m_uploadCopyCount[m_uploadSlot] >= UPLOAD_COPIES_PER_BATCH ||
         !stagingHasRoom)) {
        submitUploadBatch();
        beginUploadBatch();
    } else if (oversized && m_uploadCopyCount[m_uploadSlot] > 0) {
        // Keep a large source alone: its slot owns one temporary block until
        // the fence signals, while ordinary uploads keep using fixed arenas.
        submitUploadBatch();
        beginUploadBatch();
    }

    const int slot = m_uploadSlot;
    void* srcCpu = nullptr;
    DkGpuAddr srcGpu = 0;

    if (!oversized) {
        const uint32_t offset = m_uploadStagingPool[slot].alloc(size, 256);
        if (offset == UINT32_MAX) {
            std::fprintf(stderr,
                         "[GpuDevice] upload staging allocation failed for texture (%u bytes)\n",
                         size);
            return false;
        }
        srcCpu = m_uploadStagingPool[slot].cpuAddr(offset);
        srcGpu = m_uploadStagingPool[slot].gpuAddr(offset);
    } else {
        const uint32_t allocSize = (size + kGpuAlign - 1) & ~(kGpuAlign - 1);
        m_uploadTempStaging[slot] = dk::MemBlockMaker{m_dev, allocSize}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
        srcCpu = m_uploadTempStaging[slot].getCpuAddr();
        srcGpu = m_uploadTempStaging[slot].getGpuAddr();
        if (!srcCpu || !srcGpu) {
            m_uploadTempStaging[slot] = {};
            std::fprintf(stderr,
                         "[GpuDevice] failed temp staging allocation for texture (%u bytes)\n",
                         size);
            return false;
        }
    }

    std::memcpy(srcCpu, pixels, size);

    dk::ImageView view{dst};
    m_uploadCmdbuf[slot].copyBufferToImage(
        {srcGpu, 0, 0},
        view,
        {0, 0, 0, w, h, 1}
    );

    ++m_uploadCopyCount[slot];
    ++m_frameUploads;

    // A large upload owns the slot's only temporary source block. Submit it
    // now; normal batches flush at capacity or immediately before endFrame.
    if (oversized || m_uploadCopyCount[slot] >= UPLOAD_COPIES_PER_BATCH)
        submitUploadBatch();
    return true;
}

void GpuDevice::shutdown() {
    bool uploadWorkOutstanding = m_uploadBatchOpen;
    for (int i = 0; i < UPLOAD_SLOT_COUNT && !uploadWorkOutstanding; ++i)
        uploadWorkOutstanding = m_uploadInFlight[i];

    // beginBulkTeardown already drained the queue, but preserve the lifetime
    // boundary if anything managed to enqueue after that one-time drain.
    if (m_queue && (!m_bulkTeardown || uploadWorkOutstanding))
        waitIdle();
    m_swapchain    = {};
    for (int i = 0; i < NUM_FB; ++i)
        m_cmdbuf[i] = {};
    for (int i = 0; i < UPLOAD_SLOT_COUNT; ++i)
        m_uploadCmdbuf[i] = {};
    m_queue        = {};
    for (int i = 0; i < 2; ++i)
        m_frameDumpBuffers[i] = {};
    m_frameDumpArmed = false;
    m_frameDumpPending = false;
    m_imageChunks.clear();
    m_imageMemUsed = 0;
    m_poolMemUsed  = 0;
    m_fbPool      = {};
    m_dsPool      = {};
    m_offPool     = {};
    m_offscreenReady = false;
    for (int i = 0; i < NUM_FB; ++i)
        m_cmdPool[i] = {};
    for (int i = 0; i < UPLOAD_SLOT_COUNT; ++i) {
        m_uploadTempStaging[i] = {};
        m_uploadCmdPool[i] = {};
        m_uploadStagingPool[i] = {};
        m_uploadInFlight[i] = false;
        m_uploadCopyCount[i] = 0;
    }
    m_codePool    = {};
    m_dataPool  = {};
    m_imagePool = {};
    m_dev       = {};
    m_bulkTeardown = false;
    m_uploadBatchOpen = false;
    m_uploadSlot = 0;
}

} // namespace nxui
