#include "VideoPlayer.hpp"
#include <core/DebugLog.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>

namespace switchu::video {

bool isVideoPath(const std::string& path) {
    if (path.size() < 4) return false;
    const std::string ext4 = path.substr(path.size() - 4);
    const std::string ext5 = path.size() >= 5 ? path.substr(path.size() - 5) : "";
    auto toLower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string l4 = toLower(ext4);
    const std::string l5 = toLower(ext5);
    return (l4 == ".mp4" || l4 == ".mkv" || l4 == ".mov" || l4 == ".avi" || l5 == ".webm");
}

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() {
    close();
}

void VideoPlayer::clockSet(double seconds) {
    std::lock_guard<std::mutex> lk(m_clockMutex);
    m_clockBase = m_pauseAt = seconds;
#if defined(__SWITCH__)
    m_clockTick = armGetSystemTick();
#else
    m_clockTick = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

double VideoPlayer::clockNow() const {
    std::lock_guard<std::mutex> lk(m_clockMutex);
    if (m_paused.load(std::memory_order_relaxed)) {
        return m_pauseAt;
    }
#if defined(__SWITCH__)
    const uint64_t now = armGetSystemTick();
    const double elapsed = static_cast<double>(now - m_clockTick) / static_cast<double>(armGetSystemTickFreq());
#else
    const uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
    const double elapsed = static_cast<double>(now - m_clockTick) / 1000000.0;
#endif
    return m_clockBase + elapsed;
}

void VideoPlayer::setPaused(bool paused) {
    std::lock_guard<std::mutex> lk(m_clockMutex);
    if (paused == m_paused.load(std::memory_order_relaxed)) return;
#if defined(__SWITCH__)
    const uint64_t now = armGetSystemTick();
    const double elapsed = static_cast<double>(now - m_clockTick) / static_cast<double>(armGetSystemTickFreq());
#else
    const uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
    const double elapsed = static_cast<double>(now - m_clockTick) / 1000000.0;
#endif
    if (paused) {
        m_pauseAt = m_clockBase + elapsed;
    } else {
        m_clockBase = m_pauseAt;
        m_clockTick = now;
    }
    m_paused.store(paused, std::memory_order_release);
}

bool VideoPlayer::isPaused() const {
    return m_paused.load(std::memory_order_relaxed);
}

double VideoPlayer::position() const {
    if (!m_fmt) return 0.0;
    const double p = clockNow();
    return m_duration > 0.0 ? std::clamp(p, 0.0, m_duration) : std::max(0.0, p);
}

double VideoPlayer::duration() const {
    return m_duration;
}

bool VideoPlayer::hasEnded() const {
    return m_ended.load(std::memory_order_relaxed);
}

const nxui::Texture* VideoPlayer::texture() const {
    return m_texture.valid() ? &m_texture : nullptr;
}

void VideoPlayer::seek(double seconds) {
    if (!m_fmt) return;
    if (m_duration > 0.0) seconds = std::min(seconds, m_duration - 0.1);
    m_seekTo.store(std::max(0.0, seconds), std::memory_order_relaxed);
    m_seekReq.store(true, std::memory_order_release);
}

void VideoPlayer::doSeek(double seconds) {
    if (!m_fmt || m_streamIdx < 0) return;
    const AVStream* vs = m_fmt->streams[m_streamIdx];
    const int64_t start = (vs->start_time != AV_NOPTS_VALUE) ? vs->start_time : 0;
    const int64_t ts = start + static_cast<int64_t>(seconds / av_q2d(vs->time_base));
    av_seek_frame(m_fmt, m_streamIdx, ts, AVSEEK_FLAG_BACKWARD);
    if (m_dec) avcodec_flush_buffers(m_dec);
    m_skipUntil = seconds;
    clockSet(seconds);
    m_ended.store(false, std::memory_order_relaxed);
}

#if defined(__SWITCH__)
void VideoPlayer::decodeTrampoline(void* self) {
    static_cast<VideoPlayer*>(self)->decodeLoop();
}
#endif

bool VideoPlayer::open(const std::string& path, bool loop, bool audio) {
    close();
    if (path.empty()) return false;

    m_path = path;
    m_loop = loop;
    m_audio = audio;
    m_opened = false;
    m_ended.store(false);

    // Force "file:" protocol so FFmpeg does not misinterpret "sdmc:/" as a protocol name
    const std::string url = "file:" + m_path;

    if (avformat_open_input(&m_fmt, url.c_str(), nullptr, nullptr) < 0) {
        DebugLog::log("[video] failed to open input: %s", url.c_str());
        close();
        return false;
    }

    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        DebugLog::log("[video] failed to find stream info");
        close();
        return false;
    }

    m_streamIdx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_streamIdx < 0) {
        DebugLog::log("[video] no video stream found");
        close();
        return false;
    }

    const AVStream* stream = m_fmt->streams[m_streamIdx];

    // Fail closed before allocating decoder/GPU buffers: a 4K or >60 fps
    // wallpaper stalls the UI even when the output is downscaled to 720p.
    // 60000/1001 is below 60; inspect both advertised rates because either
    // container field can understate the source rate. Unknown rates are unsafe.
    const int srcW = stream->codecpar->width;
    const int srcH = stream->codecpar->height;
    const auto rate = [](AVRational r) -> double {
        return (r.num > 0 && r.den > 0) ? av_q2d(r) : 0.0;
    };
    const double avgFps = rate(stream->avg_frame_rate);
    const double nominalFps = rate(stream->r_frame_rate);
    if (srcW <= 0 || srcH <= 0 || srcW > 1920 || srcH > 1080 ||
        (avgFps <= 0.0 && nominalFps <= 0.0) ||
        avgFps > 60.01 || nominalFps > 60.01) {
        DebugLog::log("[video] rejected theme source %dx%d avg=%.2f nominal=%.2f (max 1920x1080 at 60 fps)",
                      srcW, srcH, avgFps, nominalFps);
        close();
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        DebugLog::log("[video] codec decoder not found");
        close();
        return false;
    }

    m_dec = avcodec_alloc_context3(codec);
    if (!m_dec) {
        close();
        return false;
    }

    if (avcodec_parameters_to_context(m_dec, stream->codecpar) < 0) {
        close();
        return false;
    }

#if defined(SWITCHU_HAS_NVTEGRA)
    // Hardware NVDEC accelerator via averne/FFmpeg nvtegra backend
    int hwErr = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
    if (hwErr >= 0 && m_hwDeviceCtx) {
        m_dec->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        DebugLog::log("[video] NVDEC hardware acceleration initialized");
    } else {
        DebugLog::log("[video] NVDEC hwaccel unavailable (code %d), falling back to software decode", hwErr);
    }
#endif

    if (avcodec_open2(m_dec, codec, nullptr) < 0) {
        DebugLog::log("[video] failed to open codec");
        close();
        return false;
    }

    int targetW = m_dec->width;
    int targetH = m_dec->height;
    if (targetW <= 0 || targetH <= 0) {
        close();
        return false;
    }

    // Downscale 4K / 1440p / 1080p videos to 1280x720 to match native Switch resolution and fit GPU staging budget
    if (targetW > 1280 || targetH > 720) {
        float scale = std::min(1280.0f / targetW, 720.0f / targetH);
        targetW = (static_cast<int>(targetW * scale) / 2) * 2;
        targetH = (static_cast<int>(targetH * scale) / 2) * 2;
    }

    m_width = targetW;
    m_height = targetH;
    m_duration = (m_fmt->duration > 0) ? static_cast<double>(m_fmt->duration) / AV_TIME_BASE : 0.0;

    for (auto& b : m_buf) {
        b.rgba.resize(static_cast<size_t>(m_width) * m_height * 4);
        b.width = m_width;
        b.height = m_height;
    }
    m_readyBuf.store(-1);

    m_stop.store(false);
    m_paused.store(false);
    m_seekReq.store(false);
    clockSet(0.0);

#if defined(__SWITCH__)
    // Priority 0x3B (background priority, matching sLaunch) pinned to Core 2 so the
    // video worker never interrupts or preempts the main UI render loop on Core 0.
    Result rc = threadCreate(&m_thread, &VideoPlayer::decodeTrampoline, this,
                             m_threadStack, sizeof(m_threadStack), 0x3B, 2);
    if (R_FAILED(rc)) {
        rc = threadCreate(&m_thread, &VideoPlayer::decodeTrampoline, this,
                          m_threadStack, sizeof(m_threadStack), 0x3B, -2);
    }
    if (R_FAILED(rc)) {
        DebugLog::log("[video] threadCreate failed: 0x%x", rc);
        close();
        return false;
    }
    m_threadCreated = true;
    threadStart(&m_thread);
#else
    m_thread = std::thread(&VideoPlayer::decodeLoop, this);
#endif

    m_opened = true;
    DebugLog::log("[video] opened %s (%dx%d, %.2fs, hw=%d)",
                  m_path.c_str(), m_width, m_height, m_duration, m_hwDeviceCtx ? 1 : 0);
    return true;
}

void VideoPlayer::close() {
    m_stop.store(true, std::memory_order_release);

#if defined(__SWITCH__)
    if (m_threadCreated) {
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_threadCreated = false;
    }
#else
    if (m_thread.joinable()) {
        m_thread.join();
    }
#endif

    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }

    if (m_dec) {
        avcodec_free_context(&m_dec);
        m_dec = nullptr;
    }

    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }

    if (m_fmt) {
        avformat_close_input(&m_fmt);
        m_fmt = nullptr;
    }

    for (auto& b : m_buf) {
        b.rgba.clear();
        b.width = b.height = 0;
    }
    m_readyBuf.store(-1);

    m_opened = false;
    m_width = m_height = 0;
    m_duration = 0.0;
    m_path.clear();
}

void VideoPlayer::tick(nxui::GpuDevice& gpu, nxui::Renderer& ren) {
    if (!m_opened) return;

    int b = m_readyBuf.exchange(-1, std::memory_order_acq_rel);
    if (b < 0) return;

    const auto& frame = m_buf[b];
    if (frame.rgba.empty() || frame.width <= 0 || frame.height <= 0) return;

    if (!m_texture.valid() || m_texture.width() != frame.width || m_texture.height() != frame.height) {
        m_texture.loadFromPixels(gpu, ren, frame.rgba.data(), frame.width, frame.height);
    } else {
        m_texture.updatePixels(gpu, frame.rgba.data(), static_cast<uint32_t>(frame.rgba.size()));
    }
}

void VideoPlayer::decodeLoop() {
    const AVStream* stream = m_fmt->streams[m_streamIdx];
    const int64_t vstart = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
    const double vtb = av_q2d(stream->time_base);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();
    AVFrame* swFrame = av_frame_alloc();
    std::deque<AVPacket*> vq;

    bool readEof = false;
    bool decEof = false;
    bool drainSent = false;
    bool haveFrame = false;
    double framePts = 0.0;
    int writeBuf = 0;

    auto clearQueue = [&]() {
        for (AVPacket* q : vq) av_packet_free(&q);
        vq.clear();
        if (haveFrame) {
            av_frame_unref(decFrame);
            haveFrame = false;
        }
        readEof = decEof = drainSent = false;
    };

    while (pkt && decFrame && swFrame && !m_stop.load(std::memory_order_acquire)) {
        if (m_seekReq.exchange(false)) {
            clearQueue();
            doSeek(m_seekTo.load());
        }

        // Demux packets ahead
        while (!readEof && !m_stop.load() && !m_seekReq.load() && vq.size() < 120) {
            if (av_read_frame(m_fmt, pkt) < 0) {
                readEof = true;
                break;
            }
            if (pkt->stream_index == m_streamIdx) {
                AVPacket* q = av_packet_alloc();
                if (q) {
                    av_packet_move_ref(q, pkt);
                    vq.push_back(q);
                } else {
                    av_packet_unref(pkt);
                }
            } else {
                av_packet_unref(pkt);
            }
        }

        // Decode next video frame
        while (!haveFrame && !decEof && !m_stop.load() && !m_seekReq.load()) {
            const int r = avcodec_receive_frame(m_dec, decFrame);
            if (r == 0) {
                framePts = (decFrame->best_effort_timestamp != AV_NOPTS_VALUE)
                               ? static_cast<double>(decFrame->best_effort_timestamp - vstart) * vtb
                               : clockNow();
                if (framePts < m_skipUntil - 0.001) {
                    av_frame_unref(decFrame);
                    continue;
                }
                haveFrame = true;
                break;
            }
            if (r == AVERROR_EOF) {
                decEof = true;
                break;
            }
            if (r != AVERROR(EAGAIN)) {
                break;
            }

            // Decoder needs more input packets
            if (!vq.empty()) {
                AVPacket* front = vq.front();
                vq.pop_front();
                avcodec_send_packet(m_dec, front);
                av_packet_free(&front);
            } else if (readEof && !drainSent) {
                avcodec_send_packet(m_dec, nullptr);
                drainSent = true;
            } else {
                break;
            }
        }

        // Frame processing and timing
        if (haveFrame) {
            // Transfer from hardware if needed
            AVFrame* f = decFrame;
#if defined(SWITCHU_HAS_NVTEGRA)
            if (decFrame->format == AV_PIX_FMT_NVTEGRA) {
                swFrame->format = AV_PIX_FMT_NV12;
                if (av_hwframe_transfer_data(swFrame, decFrame, 0) == 0) {
                    f = swFrame;
                }
            }
#endif

            // Colorspace convert into RGBA buffer ahead of presentation time
            auto& b = m_buf[writeBuf];
            m_sws = sws_getCachedContext(m_sws,
                                         f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                         m_width, m_height, AV_PIX_FMT_RGBA,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

            if (m_sws) {
                uint8_t* dstData[4] = { b.rgba.data(), nullptr, nullptr, nullptr };
                int dstLinesize[4] = { m_width * 4, 0, 0, 0 };
                sws_scale(m_sws, f->data, f->linesize, 0, f->height, dstData, dstLinesize);
                b.width = m_width;
                b.height = m_height;
            }

            av_frame_unref(decFrame);
            haveFrame = false;

            // Precision sleep until presentation timestamp
            while (clockNow() < framePts - 0.003 && !m_stop.load(std::memory_order_relaxed)) {
#if defined(__SWITCH__)
                svcSleepThread(500'000); // 0.5ms
#else
                std::this_thread::sleep_for(std::chrono::microseconds(500));
#endif
            }

            // Publish pre-converted ready frame instantly
            m_readyBuf.store(writeBuf, std::memory_order_release);
            writeBuf ^= 1;
            continue;
        }

        // Loop on end-of-stream
        if (readEof && vq.empty() && (decEof || drainSent)) {
            if (decEof && m_loop) {
                clearQueue();
                doSeek(0.0);
                continue;
            }
            if (clockNow() >= m_duration - 0.05) {
                m_ended.store(true, std::memory_order_relaxed);
            }
        }

#if defined(__SWITCH__)
        svcSleepThread(2'000'000); // 2ms
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
    }

    for (AVPacket* q : vq) av_packet_free(&q);
    av_frame_free(&swFrame);
    av_frame_free(&decFrame);
    av_packet_free(&pkt);
}

} // namespace switchu::video
