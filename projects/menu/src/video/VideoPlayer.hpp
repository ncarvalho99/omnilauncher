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

private:
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

    FrameBuffer m_buf[2];
    std::atomic<int> m_readyBuf{-1};

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
