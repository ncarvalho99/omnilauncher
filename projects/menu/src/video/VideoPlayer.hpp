#pragma once

#include <nxui/core/Texture.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <thread>
#endif

struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;
struct SwsContext;

namespace switchu::video {

bool isVideoPath(const std::string& path);

// Cheap header-only check (no decoder, no GPU) of whether open() would accept
// this wallpaper. On false, `reason` holds a short human-readable cause.
bool probeWallpaper(const std::string& path, std::string& reason);

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // path: path to an .mp4 or other supported container on the filesystem (e.g. sdmc:/...)
    // loop: whether to loop playback continuously (theme wallpapers)
    // audio: whether to decode audio and mix into SDL_mixer (wallpapers stay silent)
    bool open(const std::string& path, bool loop = true, bool audio = false);
    void close();

    // Advance video state and upload the most recent decoded frame to GPU
    void tick(nxui::GpuDevice& gpu, nxui::Renderer& ren);

    void setPaused(bool paused);
    bool isPaused() const;

    void seek(double seconds);
    double position() const;
    double duration() const;
    bool hasEnded() const;

    const nxui::Texture* texture() const;
    int width() const { return m_width; }
    int height() const { return m_height; }
    bool isOpened() const { return m_opened; }
    const std::string& path() const { return m_path; }

    // Path of the last video that open() refused or that the decoder gave up
    // on. Survives close() so a caller can avoid re-opening the same broken
    // file on every theme apply; cleared by the next successful open().
    const std::string& failedPath() const { return m_failedPath; }
    void clearFailedPath() { m_failedPath.clear(); }

private:
    bool openInternal(const std::string& path, bool loop, bool audio);
    void decodeLoop();
    void doSeek(double seconds);
    void clockSet(double seconds);
    double clockNow() const;

#if defined(__SWITCH__)
    static void decodeTrampoline(void* self);
#endif

    struct FrameBuffer {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
    };

    std::string m_path;
    bool m_loop = true;
    bool m_audio = false;
    bool m_opened = false;

    int m_width = 0;
    int m_height = 0;
    double m_duration = 0.0;

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_dec = nullptr;
    AVBufferRef* m_hwDeviceCtx = nullptr;
    int m_streamIdx = -1;

    SwsContext* m_sws = nullptr;

    // Two buffers, not three: the process has ~13 MB of headroom and a third
    // 1280x720 RGBA buffer is 3.6 MB. The handoff is made safe by m_frameMutex:
    // m_readyBuf is the last published buffer, m_readingBuf the one tick() is
    // copying to the GPU right now. The worker only ever writes the buffer that
    // is neither (the one it did not just publish) and, before writing, waits
    // for tick() to finish with it.
    FrameBuffer m_buf[2];
    std::atomic<int> m_readyBuf{-1};
    std::mutex m_frameMutex;
    int m_readingBuf = -1;

    // Set by the worker when it gives up (too many consecutive decode errors);
    // tick() then closes the player on the render thread.
    std::atomic<bool> m_failed{false};
    std::string m_failedPath;

    // Thread control
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_seekReq{false};
    std::atomic<double> m_seekTo{0.0};
    std::atomic<bool> m_ended{false};

    double m_skipUntil = 0.0;

    // Clock
    mutable std::mutex m_clockMutex;
    double m_clockBase = 0.0;
    double m_pauseAt = 0.0;
    uint64_t m_clockTick = 0;

#if defined(__SWITCH__)
    Thread m_thread{};
    bool m_threadCreated = false;
    alignas(0x1000) uint8_t m_threadStack[0x40000]; // 256 KB stack
#else
    std::thread m_thread;
#endif

    nxui::Texture m_texture;
};

} // namespace switchu::video
