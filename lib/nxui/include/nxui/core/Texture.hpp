#pragma once
#include <nxui/core/Types.hpp>
#include <nxui/core/GpuDevice.hpp>
#ifdef NXUI_BACKEND_DEKO3D
#include <deko3d.hpp>
#endif
#include <string>
#include <memory>
#include <vector>

struct SDL_Texture;

namespace nxui {

class Renderer;

// An image read and decoded, but not yet on the GPU.
//
// The two halves of loading cost very different things: reading a cover off
// the card and turning JPEG into pixels is hundreds of milliseconds, and
// uploading the result is microseconds. Only the upload has to happen on the
// render thread, so the expensive half is split out to be done anywhere.
struct DecodedImage {
    std::vector<std::uint8_t> rgba;
    int width  = 0;
    int height = 0;

    bool valid() const {
        return width > 0 && height > 0
            && rgba.size() >= static_cast<std::size_t>(width) * height * 4;
    }
};

class Texture {
public:
    Texture() = default;
    ~Texture();

#ifdef NXUI_BACKEND_DEKO3D
    // deko3d move semantics
    Texture(Texture&& o) noexcept
        : m_image(o.m_image)
        , m_mem(static_cast<dk::MemBlock&&>(o.m_mem))
        , m_width(o.m_width), m_height(o.m_height)
        , m_slot(o.m_slot), m_valid(o.m_valid)
        , m_allocSize(o.m_allocSize)
        , m_gpu(o.m_gpu)
        , m_ren(o.m_ren)
    {
        o.m_width = o.m_height = 0;
        o.m_slot = -1;
        o.m_valid = false;
        o.m_allocSize = 0;
        o.m_gpu = nullptr;
        o.m_ren = nullptr;
    }
    Texture& operator=(Texture&& o) noexcept {
        if (this != &o) {
            retireGpuResources();
            m_mem   = nullptr;
            m_image = o.m_image;
            m_mem   = static_cast<dk::MemBlock&&>(o.m_mem);
            m_width = o.m_width;  m_height = o.m_height;
            m_slot  = o.m_slot;   m_valid  = o.m_valid;
            m_allocSize = o.m_allocSize;
            m_gpu = o.m_gpu;
            m_ren = o.m_ren;
            o.m_width = o.m_height = 0;
            o.m_slot = -1;
            o.m_valid = false;
            o.m_allocSize = 0;
            o.m_gpu = nullptr;
            o.m_ren = nullptr;
        }
        return *this;
    }
#else
    // SDL2 move semantics
    Texture(Texture&& o) noexcept;
    Texture& operator=(Texture&& o) noexcept;
#endif
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Load from RGBA pixel data already in memory
    bool loadFromPixels(GpuDevice& gpu, Renderer& ren,
                        const uint8_t* rgba, int w, int h);

    // Load from RGBA pixel data into a shared pool MemBlock (no per-texture allocation).
    bool loadFromPixelsPooled(GpuDevice& gpu, Renderer& ren,
                              const uint8_t* rgba, int w, int h);

    // Load from image file (PNG/JPG via stb_image).
    // maxSide <= 0 keeps the source resolution.
    bool loadFromFile(GpuDevice& gpu, Renderer& ren, const std::string& path, int maxSide = 128);

    // Read and decode without touching the GPU. Safe to call from any thread,
    // which is the point: a caller that must not stall the frame does this on a
    // worker and hands the result to loadFromDecoded when it lands.
    static DecodedImage decodeFile(const std::string& path, int maxSide = 0);

    // Upload pixels already decoded. Cheap enough to do mid-frame.
    bool loadFromDecoded(GpuDevice& gpu, Renderer& ren, const DecodedImage& image);

    // Fast in-place pixel update for streaming video frames (RGBA8)
    bool updatePixels(GpuDevice& gpu, const void* pixels, uint32_t size);

    // Load from in-memory image data (JPEG/PNG via stb_image)
    bool loadFromMemory(GpuDevice& gpu, Renderer& ren,
                        const uint8_t* data, size_t dataSize, int maxSide = 0);

#ifdef NXUI_BACKEND_DEKO3D
    // A DDS of BC1 blocks, uploaded compressed and sampled by the GPU as-is.
    // Reached automatically by loadFromFile for a ".dds" path.
    bool loadBc1File(GpuDevice& gpu, Renderer& ren, const std::string& path);

    // The same DDS with the file already read. The two halves cost very
    // different things -- reading a frame sequence off the card is tens of
    // megabytes of I/O, uploading it is microseconds -- and only the upload has
    // to happen on the render thread. Split so the reading can go elsewhere.
    bool loadBc1Memory(GpuDevice& gpu, Renderer& ren, const uint8_t* data, size_t size);
#endif

    // Load from SDL_Surface-style data (RGBA8, row pitch may differ)
    bool loadFromSurface(GpuDevice& gpu, Renderer& ren,
                         const uint8_t* data, int w, int h, int pitch);

    int  width()  const { return m_width; }
    int  height() const { return m_height; }
    bool valid()  const { return m_valid; }
    std::size_t allocationSize() const {
#ifdef NXUI_BACKEND_DEKO3D
        return (static_cast<std::size_t>(m_allocSize) + kGpuAlign - 1u) &
               ~(static_cast<std::size_t>(kGpuAlign) - 1u);
#else
        return static_cast<std::size_t>(m_width > 0 ? m_width : 0) *
               static_cast<std::size_t>(m_height > 0 ? m_height : 0) * 4u;
#endif
    }

    // What this actually occupies on the GPU. Not width*height*4: a compressed
    // texture is a fraction of that, and a caller budgeting by the arithmetic
    // would refuse frames it has room for. 0 when the memory came from a pool.
#ifdef NXUI_BACKEND_DEKO3D
    uint32_t gpuBytes() const { return m_allocSize; }
#else
    uint32_t gpuBytes() const { return 0; }
#endif

    // Descriptor slot in the renderer's image descriptor set
    int  descriptorSlot() const { return m_slot; }

#ifdef NXUI_BACKEND_DEKO3D
    dk::Image&       image()       { return m_image; }
    const dk::Image& image() const { return m_image; }
#else
    SDL_Texture* sdlTexture() const { return m_sdlTex; }
#endif

private:
    void releaseSlot();

    // Gives the descriptor slot and the image memory back, but only once the
    // GPU has finished with them. Every path that stops owning a live texture
    // goes through here.
    void retireGpuResources();

#ifdef NXUI_BACKEND_DEKO3D
    // Shared by every format: allocation, budget accounting and descriptor
    // registration live here once.
    bool loadImageData(GpuDevice& gpu, Renderer& ren,
                       const uint8_t* data, uint64_t dataSize,
                       int w, int h, uint32_t format);
#endif
#ifdef NXUI_BACKEND_DEKO3D
    dk::Image          m_image;
    dk::UniqueMemBlock m_mem;
    uint32_t m_allocSize = 0;
    GpuDevice* m_gpu = nullptr;
    // Guardado para devolver o slot de descritor ao morrer. Sem isso cada
    // textura destruida deixava o seu slot perdido para sempre.
    Renderer* m_ren = nullptr;
#else
    SDL_Texture* m_sdlTex = nullptr;
    GpuDevice*   m_gpu = nullptr;
#endif
    int m_width  = 0;
    int m_height = 0;
    int m_slot   = -1;
    bool m_valid = false;
};

} // namespace nxui
