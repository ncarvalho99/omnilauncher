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
#include <cstdio>
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

namespace {

// A decoder that fails this many times in a row without producing a frame is
// treated as dead. One count per failed receive or rejected packet; a corrupt
// region that recovers at the next keyframe resets the count as soon as a frame
// comes out. With nothing left to feed, the worker re-polls every 2 ms, so a
// stream that is dead at its end is given up after about a quarter of a second.
constexpr int kMaxConsecutiveDecodeErrors = 120;

// A frame later than this behind the playback clock is dropped without being
// converted (software decode falling behind would otherwise never catch up).
// At most kMaxConsecutiveLateDrops in a row, so the picture still advances.
constexpr double kLateDropSeconds = 0.25;
constexpr int kMaxConsecutiveLateDrops = 3;

// First real video stream. av_find_best_stream may pick cover art stored as an
// attached picture (mjpeg/png), which the codec gate then rejects even though
// the file has a perfectly good H.264 track.
int pickVideoStream(AVFormatContext* fmt) {
    int first = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* s = fmt->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
        if (s->disposition & AV_DISPOSITION_DEFAULT) return static_cast<int>(i);
        if (first < 0) first = static_cast<int>(i);
    }
    if (first >= 0) return first;
    return av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
}

// Single definition of "a wallpaper this console can play", shared by open()
// and by probeWallpaper() so the Theme Shop can refuse a clip before applying.
// Returns nullptr when acceptable, otherwise a short log-safe reason.
const char* wallpaperStreamRejection(const AVStream* stream) {
    // Fail closed before allocating decoder/GPU buffers: a 4K or >60 fps
    // wallpaper stalls the UI even when the output is downscaled to 720p.
    // 60000/1001 is below 60; inspect both advertised rates because either
    // container field can understate the source rate. Unknown rates are unsafe.
    static thread_local char buf[160];
    const int srcW = stream->codecpar->width;
    const int srcH = stream->codecpar->height;
    const auto rate = [](AVRational r) -> double {
        return (r.num > 0 && r.den > 0) ? av_q2d(r) : 0.0;
    };
    const double avgFps = rate(stream->avg_frame_rate);
    const double nominalFps = rate(stream->r_frame_rate);

    // Only H.264 is validated on the console. A VP9 .webm wallpaper (stored
    // under an .mp4 name by the store installer) aborted the menu inside
    // deko3d a few seconds after boot, every boot, until the theme was reset.
    if (stream->codecpar->codec_id != AV_CODEC_ID_H264) {
        std::snprintf(buf, sizeof(buf), "codec id=%d (only H.264 supported)",
                      static_cast<int>(stream->codecpar->codec_id));
        return buf;
    }
    // The codec id alone is not enough: High 10 and 4:2:2 / 4:4:4 H.264 pass it
    // but the hardware decoder only handles 8-bit 4:2:0. The pixel format is
    // known after find_stream_info; if it is not, fall back to the profile.
    const int fmt = stream->codecpar->format;
    const int profile = stream->codecpar->profile & 0xFF;   // drop CONSTRAINED/INTRA flags
    const bool known = fmt != AV_PIX_FMT_NONE;
    const bool is420x8 = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
    const bool highProfile = profile == 110 || profile == 122 || profile == 244 || profile == 44;
    if ((known && !is420x8) || (!known && highProfile)) {
        std::snprintf(buf, sizeof(buf), "pixel format %d profile %d (need 8-bit 4:2:0)",
                      fmt, stream->codecpar->profile);
        return buf;
    }
    if (srcW <= 0 || srcH <= 0 || srcW > 1920 || srcH > 1080 ||
        (avgFps <= 0.0 && nominalFps <= 0.0) ||
        avgFps > 60.01 || nominalFps > 60.01) {
        std::snprintf(buf, sizeof(buf),
                      "source %dx%d avg=%.2f nominal=%.2f (max 1920x1080 at 60 fps)",
                      srcW, srcH, avgFps, nominalFps);
        return buf;
    }
    return nullptr;
}

} // namespace

namespace {

// Rejection grounds that do not depend on the analysed window: codec id, a known
// pixel format that is not 8-bit 4:2:0, or known dimensions over the limit.
bool cheapRejection(const AVStream* st) {
    const AVCodecParameters* cp = st->codecpar;
    if (cp->codec_id != AV_CODEC_ID_H264) return true;
    if (cp->format != AV_PIX_FMT_NONE &&
        cp->format != AV_PIX_FMT_YUV420P && cp->format != AV_PIX_FMT_YUVJ420P) return true;
    return cp->width > 1920 || cp->height > 1080;
}

enum class ProbeVerdict { Accepted, Rejected, Inconclusive };

// One probe pass. `capped` bounds the container read window so a refusal does
// not stall the UI thread reading megabytes from the SD (a 132 MB 4K clip took
// 1.67 s with the default 5 MB window). A capped pass can leave pix_fmt or the
// frame rates unset, so it never turns a missing field into a verdict: it
// reports Inconclusive and the caller repeats the pass uncapped.
ProbeVerdict probeOnce(const std::string& url, bool capped, std::string& reason) {
    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    if (capped) {
        av_dict_set(&opts, "probesize", "262144", 0);
        av_dict_set(&opts, "analyzeduration", "0", 0);
    }
    const int openRc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (openRc < 0) {
        reason = "the file could not be opened";
        return ProbeVerdict::Rejected;
    }
    ProbeVerdict v = ProbeVerdict::Rejected;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        reason = capped ? std::string() : "the file is not a readable video";
        if (capped) v = ProbeVerdict::Inconclusive;
    } else {
        const int idx = pickVideoStream(fmt);
        if (idx < 0) {
            reason = "the file has no video stream";
            if (capped) v = ProbeVerdict::Inconclusive;
        } else {
            const AVStream* st = fmt->streams[idx];
            const char* why = wallpaperStreamRejection(st);
            if (!why) {
                // Frame rates from a capped window can be guessed, so an
                // acceptance is only trusted from the full pass.
                v = capped ? ProbeVerdict::Inconclusive : ProbeVerdict::Accepted;
            } else if (capped && !cheapRejection(st)) {
                // Rejected only on frame rate or an unset field: not reliable
                // with a truncated window.
                v = ProbeVerdict::Inconclusive;
            } else {
                reason = why;
            }
        }
    }
    avformat_close_input(&fmt);
    return v;
}

} // namespace

bool probeWallpaper(const std::string& path, std::string& reason) {
    reason.clear();
    const std::string url = "file:" + path;
    ProbeVerdict v = probeOnce(url, true, reason);
    if (v == ProbeVerdict::Inconclusive) {
        reason.clear();
        v = probeOnce(url, false, reason);
    }
    const bool ok = v == ProbeVerdict::Accepted;
    if (!ok)
        DebugLog::log("[video] probe rejected %s: %s", path.c_str(), reason.c_str());
    return ok;
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
    // A file refused here is remembered so the app does not re-probe it on
    // every theme apply; a successful open of anything clears the memory.
    if (openInternal(path, loop, audio)) {
        m_failedPath.clear();
        return true;
    }
    if (!path.empty())
        m_failedPath = path;
    return false;
}

bool VideoPlayer::openInternal(const std::string& path, bool loop, bool audio) {
    close();
    m_failed.store(false);
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

    m_streamIdx = pickVideoStream(m_fmt);
    if (m_streamIdx < 0) {
        DebugLog::log("[video] no video stream found");
        close();
        return false;
    }

    const AVStream* stream = m_fmt->streams[m_streamIdx];

    if (const char* why = wallpaperStreamRejection(stream)) {
        DebugLog::log("[video] rejected theme %s", why);
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
    m_readingBuf = -1;

    // The worker has joined, so nothing refills the buffers. Release the GPU
    // texture too: it used to stay allocated (3.6 MB of the image budget) after
    // switching to a still theme, and a later video of the same size would show
    // this stale last frame until its first decoded frame arrived. Assigning an
    // empty Texture goes through retireGpuResources(), which waits for the GPU
    // before freeing the image and its descriptor slot.
    m_texture = nxui::Texture{};

    m_opened = false;
    m_width = m_height = 0;
    m_duration = 0.0;
    m_path.clear();
}

void VideoPlayer::tick(nxui::GpuDevice& gpu, nxui::Renderer& ren) {
    if (!m_opened) return;

    // The worker gave up (see decodeLoop). Stop here, on the render thread, so
    // the texture is released under the GPU-idle wait, and remember the file.
    if (m_failed.load(std::memory_order_acquire)) {
        const std::string failed = m_path;
        close();
        m_failedPath = failed;
        return;
    }

    // Claim the ready buffer. m_readingBuf tells the worker not to overwrite it
    // while the upload below (which can wait on the GPU) is still copying it.
    int b;
    {
        std::lock_guard<std::mutex> lk(m_frameMutex);
        b = m_readyBuf.exchange(-1, std::memory_order_acq_rel);
        if (b < 0) return;
        m_readingBuf = b;
    }

    const auto& frame = m_buf[b];
    if (!frame.rgba.empty() && frame.width > 0 && frame.height > 0) {
        if (!m_texture.valid() || m_texture.width() != frame.width || m_texture.height() != frame.height) {
            m_texture.loadFromPixels(gpu, ren, frame.rgba.data(), frame.width, frame.height);
        } else {
            m_texture.updatePixels(gpu, frame.rgba.data(), static_cast<uint32_t>(frame.rgba.size()));
        }
    }

    std::lock_guard<std::mutex> lk(m_frameMutex);
    m_readingBuf = -1;
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
    bool gaveUp = false;
    int decodeErrors = 0;
    int transferErrors = 0;
    int convertErrors = 0;
    int lateDrops = 0;
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
                decodeErrors = 0;
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
                // A real decode error. It used to `break` here and leave decEof
                // unset, so a corrupt stream froze the wallpaper on its last
                // frame with the worker polling forever. Count it, keep feeding
                // packets (the decoder usually recovers at the next keyframe),
                // and give up when it never does.
                if (++decodeErrors >= kMaxConsecutiveDecodeErrors) {
                    gaveUp = true;
                    break;
                }
            }

            // Decoder needs more input packets
            if (!vq.empty()) {
                AVPacket* front = vq.front();
                vq.pop_front();
                const int sr = avcodec_send_packet(m_dec, front);
                if (sr == AVERROR(EAGAIN)) {
                    // Not consumed: the decoder wants its output read first.
                    // Keep the packet; dropping it here lost a frame of data.
                    vq.push_front(front);
                    break;
                }
                av_packet_free(&front);
                if (sr < 0 && sr != AVERROR_EOF && ++decodeErrors >= kMaxConsecutiveDecodeErrors) {
                    gaveUp = true;
                    break;
                }
            } else if (readEof && !drainSent) {
                avcodec_send_packet(m_dec, nullptr);
                drainSent = true;
            } else {
                break;
            }
        }

        if (gaveUp) {
            DebugLog::log("[video] decoder gave up after %d consecutive errors, closing wallpaper",
                          decodeErrors);
            m_failed.store(true, std::memory_order_release);
            break;
        }

        // Frame processing and timing
        if (haveFrame) {
            // Transfer from hardware if needed
            AVFrame* f = decFrame;
            bool convertible = true;
#if defined(SWITCHU_HAS_NVTEGRA)
            if (decFrame->format == AV_PIX_FMT_NVTEGRA) {
                swFrame->format = AV_PIX_FMT_NV12;
                if (av_hwframe_transfer_data(swFrame, decFrame, 0) == 0) {
                    f = swFrame;
                    transferErrors = 0;
                } else {
                    // The hardware frame is opaque to swscale: converting it
                    // reads garbage. Skip this frame instead.
                    convertible = false;
                    // Its own counter: every successful receive resets
                    // decodeErrors, so a transfer that fails every time would
                    // otherwise stay at 1 and never give up.
                    if (++transferErrors >= kMaxConsecutiveDecodeErrors) {
                        DebugLog::log("[video] hardware frame transfer kept failing, closing wallpaper");
                        m_failed.store(true, std::memory_order_release);
                        av_frame_unref(swFrame);
                        av_frame_unref(decFrame);
                        break;
                    }
                }
            }
#endif

            // A frame already far behind the playback clock is not worth
            // converting: dropping it lets a slow decoder catch up instead of
            // playing everything late forever. Bounded, so it never starves.
            if (convertible && clockNow() - framePts > kLateDropSeconds &&
                lateDrops < kMaxConsecutiveLateDrops) {
                ++lateDrops;
                convertible = false;
            } else {
                lateDrops = 0;
            }

            // Colorspace convert into RGBA buffer ahead of presentation time.
            // tick() may still be uploading this very buffer from two frames
            // ago (the upload can wait on the GPU); wait until it is done.
            auto& b = m_buf[writeBuf];
            while (!m_stop.load(std::memory_order_relaxed)) {
                {
                    std::lock_guard<std::mutex> lk(m_frameMutex);
                    if (m_readingBuf != writeBuf) break;
                }
#if defined(__SWITCH__)
                svcSleepThread(500'000);
#else
                std::this_thread::sleep_for(std::chrono::microseconds(500));
#endif
            }
            if (m_stop.load(std::memory_order_relaxed)) {
                av_frame_unref(swFrame);
                av_frame_unref(decFrame);
                break;
            }
            if (convertible)
                m_sws = sws_getCachedContext(m_sws,
                                             f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                             m_width, m_height, AV_PIX_FMT_RGBA,
                                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

            bool converted = false;
            if (convertible && m_sws) {
                uint8_t* dstData[4] = { b.rgba.data(), nullptr, nullptr, nullptr };
                int dstLinesize[4] = { m_width * 4, 0, 0, 0 };
                // sws_scale returns the number of output rows written; anything
                // short of the full picture is a failed conversion, not a frame.
                const int rows = sws_scale(m_sws, f->data, f->linesize, 0, f->height,
                                           dstData, dstLinesize);
                if (rows == m_height) {
                    b.width = m_width;
                    b.height = m_height;
                    converted = true;
                    convertErrors = 0;
                }
            }
            if (convertible && !converted && ++convertErrors >= kMaxConsecutiveDecodeErrors) {
                DebugLog::log("[video] colour conversion kept failing, closing wallpaper");
                m_failed.store(true, std::memory_order_release);
                av_frame_unref(swFrame);
                av_frame_unref(decFrame);
                break;
            }

            av_frame_unref(swFrame);
            av_frame_unref(decFrame);
            haveFrame = false;

            // Nothing new to show (dropped late, hardware transfer failed or
            // swscale could not be created): do not publish the stale buffer.
            if (!converted)
                continue;

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
