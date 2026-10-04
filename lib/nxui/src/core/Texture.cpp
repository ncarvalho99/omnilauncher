#include <nxui/core/Texture.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

namespace nxui {

void Texture::releaseSlot() {
    if (m_ren && m_slot >= 0)
        m_ren->releaseTextureSlot(m_slot);
    m_slot = -1;
}

// Giving a descriptor slot and an image back is not free at any moment: the
// frame already handed to the GPU still samples that image through that slot.
// releaseSlot puts the index straight back on the renderer's free list, so the
// next texture loaded claims it and rewrites the descriptor, and destroying the
// UniqueMemBlock returns the pixels themselves. Do that mid-frame and the GPU
// reads a descriptor pointing at memory that is no longer there: the frame
// comes out as noise and deko3d ends the process with svcBreak.
//
// That is the theme-switch crash. Changing theme re-primes the installed
// previews, each one move-assigned over the texture the current frame is
// drawing. The user's file was never corrupt -- both default covers are the
// same 1280x720 JPEG, and his copy is byte-for-byte the right size.
//
// The wait only happens when there is something live to retire, which is a
// theme change or a cache eviction, not every frame.
void Texture::retireGpuResources() {
    const bool holdsMemory = m_gpu && m_mem && m_allocSize > 0;
    if (m_slot < 0 && !holdsMemory)
        return;
    if (m_gpu && !m_gpu->bulkTeardownActive())
        m_gpu->waitIdle();
    releaseSlot();
    if (holdsMemory)
        m_gpu->freeImageMemory(m_allocSize);
}

Texture::~Texture() {
    retireGpuResources();
}

bool Texture::loadFromPixels(GpuDevice& gpu, Renderer& ren,
                             const uint8_t* rgba, int w, int h)
{
    return loadImageData(gpu, ren, rgba, (uint64_t)w * h * 4, w, h,
                         DkImageFormat_RGBA8_Unorm);
}

// One body for every format, because this is the allocation path that answered
// failure with svcBreak until it was fixed. A second copy of it for compressed
// images would be a second copy of that bug waiting to be reintroduced.
bool Texture::loadImageData(GpuDevice& gpu, Renderer& ren,
                            const uint8_t* data, uint64_t dataSize,
                            int w, int h, uint32_t format)
{
    m_gpu = &gpu;
    m_ren = &ren;
    int oldSlot = m_slot;
    uint32_t oldAllocSize = m_allocSize;

    // Reloading in place is the same hazard as destruction: below, the old
    // MemBlock is dropped when a bigger one is needed, and the descriptor at
    // oldSlot is rewritten to point at the new image. Both belong to the frame
    // in flight until the GPU says otherwise.
    // Not gated on m_valid: a load that failed at registerTexture leaves
    // m_valid false but m_mem populated, with its upload possibly still queued.
    // Dropping or overwriting that block without waiting is the same hazard.
    if (oldSlot >= 0 || m_mem)
        gpu.waitIdle();

    m_valid = false;
    m_slot  = -1;
    m_allocSize = 0;

    // deko3d does not return an error for dimensions it cannot lay out: it
    // calls svcBreak, which kills the process mid-frame. A corrupt or truncated
    // image reaching this point must therefore be refused here, while refusing
    // is still possible. This is a guard, not the fix for the theme-switch
    // crash -- that one arrived with perfectly valid dimensions; see
    // retireGpuResources above.
    constexpr int kMaxSide = 16384;
    if (w <= 0 || h <= 0 || w > kMaxSide || h > kMaxSide) {
        std::printf("[Texture] refusing %dx%d image\n", w, h);
        m_width = m_height = 0;
        return false;
    }

    m_width  = w;
    m_height = h;

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{gpu.device()}
        .setFlags(0)
        .setFormat((DkImageFormat)format)
        .setDimensions(w, h)
        .initialize(layout);

    uint32_t needed = layout.getSize();
    uint32_t alignedNeeded = (needed + kGpuAlign - 1) & ~(kGpuAlign - 1);
    uint32_t alignedOld    = (oldAllocSize + kGpuAlign - 1) & ~(kGpuAlign - 1);

    if (m_mem && alignedOld >= alignedNeeded) {
        // Reuse existing MemBlock — avoids kernel free+alloc round-trip.
        // Keep the original allocSize so the budget stays accurate.
        m_allocSize = oldAllocSize;
    } else {
        // Need a bigger block — free the old one first.
        if (m_mem && oldAllocSize > 0)
            gpu.freeImageMemory(oldAllocSize);

        m_mem = gpu.allocImageMemory(needed);
        if (!m_mem) {
            if (oldSlot >= 0)
                ren.releaseTextureSlot(oldSlot);
            std::printf("[Texture] allocImageMemory FAILED (%dx%d) — GPU budget exhausted\n", w, h);
            // Two things had to be put right here, and both of them showed up
            // as a crash rather than as a missing texture.
            //
            // The old block is gone: it was destroyed by the assignment above,
            // and freeImageMemory already gave its bytes back. m_image still
            // described it, and m_valid still said true from the load that
            // succeeded before, so the next frame drew a texture whose memory
            // no longer existed -- deko3d answers that with svcBreak, which is
            // the User Break in the crash reports.
            //
            // m_allocSize still held the old size too, so the next attempt
            // would hand those same bytes back a second time. The budget walks
            // downwards from there and stops bounding anything, which is how a
            // console with enough icons reaches real exhaustion.
            m_valid = false;
            m_slot = -1;
            m_allocSize = 0;
            return false;
        }
        m_allocSize = needed;
    }

    m_image.initialize(layout, m_mem, 0);

    if (!gpu.uploadTexture(m_image, data, (uint32_t)dataSize, w, h, dataSize)) {
        std::printf("[Texture] uploadTexture FAILED (%dx%d)\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        // The descriptor slot taken from the previous image was cleared above
        // (m_slot = -1); give it back, or it leaks and still points at m_mem.
        if (oldSlot >= 0) ren.releaseTextureSlot(oldSlot);
        m_valid = false;
        return false;
    }

    dk::ImageView view{m_image};
    if (oldSlot >= 0) {
        // Reuse the same descriptor slot — avoids slot exhaustion
        ren.updateTexture(oldSlot, view);
        m_slot = oldSlot;
    } else {
        m_slot = ren.registerTexture(view);
        if (m_slot < 0) {
            std::printf("[Texture] registerTexture FAILED (%dx%d) — descriptor pool full\n", w, h);
            // Half a texture is worse than none: it can still be bound and drawn.
            m_valid = false;
            return false;
        }
    }
    m_valid = true;
    return true;
}

bool Texture::loadFromPixelsPooled(GpuDevice& gpu, Renderer& ren,
                                    const uint8_t* rgba, int w, int h)
{
    m_gpu = &gpu;
    m_ren = &ren;
    m_valid = false;
    m_slot  = -1;
    m_width  = w;
    m_height = h;

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{gpu.device()}
        .setFlags(0)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(w, h)
        .initialize(layout);

    auto alloc = gpu.allocImageFromPool(layout.getSize(), layout.getAlignment());
    if (!alloc.valid()) {
        std::printf("[Texture] pool alloc FAILED (%dx%d) — budget exhausted\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }
    // m_mem stays empty — pool owns the memory.
    m_image.initialize(layout, alloc.block, alloc.offset);

    if (!gpu.uploadTexture(m_image, rgba, w * h * 4, w, h)) {
        std::printf("[Texture] uploadTexture FAILED (%dx%d)\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }

    dk::ImageView view{m_image};
    m_slot = ren.registerTexture(view);
    if (m_slot < 0) {
        std::printf("[Texture] registerTexture FAILED (%dx%d) — descriptor pool full\n", w, h);
        // Half a texture is worse than none: it can still be bound and drawn.
        m_valid = false;
        return false;
    }
    m_valid = true;
    return true;
}

// A DDS holding BC1 blocks, which the GPU samples without unpacking. Four bytes
// a pixel is what forced theme animations down to 224x126, where a frame
// stretched to the screen looks like blocks; BC1 is half a byte a pixel, so the
// same memory holds 640x360 and the stretch drops from 5.7x to 2x.
//
// Only the layout ffmpeg and the theme tooling produce is accepted: no mipmaps,
// no cubemaps, no other fourCC. Anything else is a file this was not asked to
// read, and guessing at it is how a loader ends up handing deko3d something it
// answers with svcBreak.
bool Texture::loadBc1File(GpuDevice& gpu, Renderer& ren, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long end = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (end <= 128) { std::fclose(f); return false; }

    std::vector<uint8_t> file((size_t)end);
    const size_t read = std::fread(file.data(), 1, file.size(), f);
    std::fclose(f);
    if (read != file.size()) {
        std::printf("[Texture] dds truncated: %zu of %zu bytes in %s\n", read, file.size(), path.c_str());
        return false;
    }
    return loadBc1Memory(gpu, ren, file.data(), file.size());
}

bool Texture::loadBc1Memory(GpuDevice& gpu, Renderer& ren, const uint8_t* head, size_t size) {
    if (!head || size <= 128) return false;

    auto u32 = [&](int off) {
        return (uint32_t)head[off] | ((uint32_t)head[off+1] << 8)
             | ((uint32_t)head[off+2] << 16) | ((uint32_t)head[off+3] << 24);
    };
    const uint32_t fourCC = u32(84);
    const int      hh     = (int)u32(12);
    const int      ww     = (int)u32(16);
    if (u32(0) != 0x20534444u /* "DDS " */) {
        std::printf("[Texture] not a dds\n");
        return false;
    }

    // Dois formatos, e a diferenca esta em quanto cada bloco de 4x4 ocupa.
    //
    // O DXT1 tem fourCC proprio e cabe no cabecalho antigo. O BC7 nao tem, e se
    // declara pelo bloco DX10 que vem depois dos 128 bytes, com o numero do
    // formato DXGI -- por isso os dados dele comecam 20 bytes mais adiante.
    DkImageFormat format;
    size_t blockBytes;
    size_t offset;
    if (fourCC == 0x31545844u /* "DXT1" */) {
        format = DkImageFormat_RGB_BC1;
        blockBytes = 8;
        offset = 128;
    } else if (fourCC == 0x30315844u /* "DX10" */) {
        if (size <= 148) return false;
        const uint32_t dxgi = u32(128);
        if (dxgi != 98 /* BC7_UNORM */ && dxgi != 99 /* BC7_UNORM_SRGB */) {
            std::printf("[Texture] dds DX10 com formato %u, nao BC7\n", dxgi);
            return false;
        }
        format = DkImageFormat_RGBA_BC7_Unorm;
        blockBytes = 16;
        offset = 148;
    } else {
        std::printf("[Texture] dds nem DXT1 nem DX10\n");
        return false;
    }

    // Os dois guardam blocos de 4x4, entao as dimensoes tem de ser blocos
    // inteiros -- um bloco parcial deixaria a GPU lendo depois dos dados.
    if (ww <= 0 || hh <= 0 || (ww & 3) || (hh & 3)) {
        std::printf("[Texture] dds %dx%d is not a multiple of 4\n", ww, hh);
        return false;
    }

    const size_t blocks = (size_t)(ww / 4) * (hh / 4) * blockBytes;
    if (size < offset + blocks) {
        std::printf("[Texture] dds short: %zu bytes for %zu of pixel data\n",
                    size - offset, blocks);
        return false;
    }

    return loadImageData(gpu, ren, head + offset, blocks, ww, hh, format);
}

// Reading and decoding, with no GPU object touched anywhere in here. That is
// what makes it callable from a worker thread.
DecodedImage Texture::decodeFile(const std::string& path, int maxSide) {
    DecodedImage out;

    int w, h, ch;
    uint8_t* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) {
        std::printf("[Texture] stbi_load FAILED: %s\n", path.c_str());
        return out;
    }
    // A file can decode without failing outright and still describe nothing
    // usable. Caught here so the path that scales and uploads never sees it.
    if (w <= 0 || h <= 0) {
        std::printf("[Texture] decoded %dx%d, refusing: %s\n", w, h, path.c_str());
        stbi_image_free(data);
        return out;
    }

    int dw = w, dh = h;
    if (maxSide > 0 && (w > maxSide || h > maxSide)) {
        float scale = std::min((float)maxSide / w, (float)maxSide / h);
        dw = std::max(1, (int)(w * scale));
        dh = std::max(1, (int)(h * scale));
    }

    out.width  = dw;
    out.height = dh;
    out.rgba.resize((size_t)dw * dh * 4);
    if (dw == w && dh == h) {
        std::memcpy(out.rgba.data(), data, out.rgba.size());
    } else {
        for (int y = 0; y < dh; ++y) {
            int sy = y * h / dh;
            for (int x = 0; x < dw; ++x) {
                int sx = x * w / dw;
                std::memcpy(out.rgba.data() + ((size_t)y * dw + x) * 4,
                            data            + ((size_t)sy * w + sx) * 4, 4);
            }
        }
    }
    stbi_image_free(data);
    return out;
}

bool Texture::loadFromDecoded(GpuDevice& gpu, Renderer& ren, const DecodedImage& image) {
    if (!image.valid()) {
        m_valid = false;
        return false;
    }
    return loadFromPixels(gpu, ren, image.rgba.data(), image.width, image.height);
}

bool Texture::loadFromFile(GpuDevice& gpu, Renderer& ren, const std::string& path, int maxSide) {
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".dds") == 0)
        return loadBc1File(gpu, ren, path);

    return loadFromDecoded(gpu, ren, decodeFile(path, maxSide));
}

bool Texture::loadFromMemory(GpuDevice& gpu, Renderer& ren,
                             const uint8_t* data, size_t dataSize, int maxSide)
{
    int w, h, ch;
    uint8_t* pixels = stbi_load_from_memory(data, (int)dataSize, &w, &h, &ch, 4);
    if (!pixels) return false;

    if (maxSide > 0 && (w > maxSide || h > maxSide)) {
        float scale = std::min((float)maxSide / w, (float)maxSide / h);
        int dw = std::max(1, (int)(w * scale));
        int dh = std::max(1, (int)(h * scale));
        uint8_t* scaled = (uint8_t*)std::malloc((size_t)dw * dh * 4);
        if (scaled) {
            for (int y = 0; y < dh; ++y) {
                int sy = y * h / dh;
                for (int x = 0; x < dw; ++x) {
                    int sx = x * w / dw;
                    std::memcpy(scaled + ((size_t)y * dw + x) * 4,
                                pixels + ((size_t)sy * w + sx) * 4, 4);
                }
            }
            stbi_image_free(pixels);
            pixels = scaled;
            w = dw;
            h = dh;
        }
    }

    bool ok = loadFromPixels(gpu, ren, pixels, w, h);
    std::free(pixels);
    return ok;
}

bool Texture::loadFromSurface(GpuDevice& gpu, Renderer& ren,
                              const uint8_t* data, int w, int h, int pitch)
{
    // Convert to tightly packed RGBA if pitch != w*4
    if (pitch == w * 4) {
        return loadFromPixels(gpu, ren, data, w, h);
    }
    std::vector<uint8_t> tight(w * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(tight.data() + y * w * 4, data + y * pitch, w * 4);
    return loadFromPixels(gpu, ren, tight.data(), w, h);
}

bool Texture::updatePixels(GpuDevice& gpu, const void* pixels, uint32_t size) {
    if (!m_valid || !pixels || size == 0 || !m_image.getGpuAddr())
        return false;
    // expectedBytes is the size this image occupies, NOT the caller's `size`:
    // passing `size` made the short-buffer guard compare a number with itself
    // and always pass, so a short buffer reached the copy. updatePixels is for
    // uncompressed RGBA (video frames); this guard rejects a SHORT buffer only,
    // it does not detect a compressed destination format.
    // reuploadsLiveImage: the previous frame may still be sampling this image.
    const uint64_t expected = static_cast<uint64_t>(m_width) * static_cast<uint64_t>(m_height) * 4u;
    return gpu.uploadTexture(m_image, pixels, size, m_width, m_height, expected, true);
}

} // namespace nxui
