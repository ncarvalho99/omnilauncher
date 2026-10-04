#include "WaraWaraBackground.hpp"
#include "core/DebugLog.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifdef __SWITCH__
#include <switch.h>
#endif

#if 0 // Superseded by the fork's stricter handheld render budgets below.
namespace {

// Vertices held back from the background so the foreground UI always fits in
// the 65536 vertex buffer. Back to the original value now that the
// antialiasing skirt is gone and a rounded rect costs ~108 vertices again.
constexpr int kBackgroundRenderReserveVertices = 13312;
constexpr int kWorstCaseShapeVertices = 36;
constexpr int kGlassShapeLayers = 3;
constexpr int kGridRenderedShapeBudget = 448;
constexpr int kFloatingRenderedShapeBudget = 160;
constexpr int kDenseRenderCopyThreshold = 96;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;
constexpr float kShapeBodyAlphaMin = 0.84f;
constexpr float kShapeBodyAlphaMax = 0.92f;
constexpr float kShapeHighlightAlpha = 0.18f;
constexpr float kFloatingShapeEdgeAlpha = 0.11f;
constexpr float kGridShapeEdgeAlpha = 0.08f;

float degreesToRadians(float degrees) {
    return degrees * kPi / 180.f;
}

void appendArcPoints(std::vector<nxui::Vec2>& points,
                     float centerX,
                     float centerY,
                     float radius,
                     float startAngle,
                     float endAngle,
                     int segments,
                     bool includeStart)
{
    int startIndex = includeStart ? 0 : 1;
    for (int i = startIndex; i <= segments; ++i) {
        float t = segments > 0 ? (float)i / (float)segments : 0.f;
        float angle = startAngle + (endAngle - startAngle) * t;
        points.push_back({centerX + std::cos(angle) * radius,
                          centerY + std::sin(angle) * radius});
    }
}

float random01() {
    return (std::rand() % 1000) / 1000.f;
}

float randomRange(float minValue, float maxValue) {
    return minValue + (maxValue - minValue) * random01();
}

float wrapValue(float value, float minValue, float maxValue) {
    float span = maxValue - minValue;
    if (span <= 0.f)
        return minValue;
    while (value < minValue)
        value += span;
    while (value > maxValue)
        value -= span;
    return value;
}

int symmetryMultiplier(WaraWaraBackground::Symmetry symmetry) {
    switch (symmetry) {
        case WaraWaraBackground::Symmetry::MirrorHorizontal:
        case WaraWaraBackground::Symmetry::MirrorVertical:
            return 2;
        case WaraWaraBackground::Symmetry::Quad:
            return 4;
        case WaraWaraBackground::Symmetry::None:
        default:
            return 1;
    }
}

int maxSafeBaseShapeCount(const WaraWaraBackground::Config& config) {
    const int multiplier = std::max(1, symmetryMultiplier(config.symmetry));
    const int maxBudget = std::max(1, nxui::GpuDevice::MAX_VERTICES - kBackgroundRenderReserveVertices);
    const int perShapeBudget = std::max(1, kWorstCaseShapeVertices * kGlassShapeLayers * multiplier);
    return std::max(1, maxBudget / perShapeBudget);
}

int maxVisualBaseShapeCount(const WaraWaraBackground::Config& config) {
    const int multiplier = std::max(1, symmetryMultiplier(config.symmetry));
    const int renderedBudget = (config.layout == WaraWaraBackground::Layout::Grid)
        ? kGridRenderedShapeBudget
        : kFloatingRenderedShapeBudget;
    return std::max(1, renderedBudget / multiplier);
}

int renderedShapeCopies(const WaraWaraBackground::Config& config, int baseShapeCount) {
    return std::max(0, baseShapeCount) * std::max(1, symmetryMultiplier(config.symmetry));
}

bool useDenseRenderMode(const WaraWaraBackground::Config& config, int baseShapeCount) {
    return config.layout == WaraWaraBackground::Layout::Grid
        && renderedShapeCopies(config, baseShapeCount) >= kDenseRenderCopyThreshold;
}

std::uint64_t estimateVertexCount(const WaraWaraBackground::Config& config, int baseShapeCount) {
    return (std::uint64_t)std::max(0, baseShapeCount)
        * (std::uint64_t)std::max(1, symmetryMultiplier(config.symmetry))
        * (std::uint64_t)kWorstCaseShapeVertices
        * (std::uint64_t)kGlassShapeLayers;
}

} // namespace
#endif

namespace {

constexpr int kBackgroundRenderReserveVertices = 13312;
constexpr int kWorstCaseShapeVertices = 36;
constexpr int kGlassShapeLayers = 3;
// Theme manifests are untrusted configuration. A 14x8 grid combined with
// quad symmetry previously expanded to 448 shapes per frame. Preserve the
// composition while bounding fill/geometry cost on the handheld GPU.
constexpr int kGridRenderedShapeBudget = 160;
constexpr int kFloatingRenderedShapeBudget = 80;
constexpr int kDenseRenderCopyThreshold = 64;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;
constexpr float kShapeBodyAlphaMin = 0.84f;
constexpr float kShapeBodyAlphaMax = 0.92f;
constexpr float kShapeHighlightAlpha = 0.18f;
constexpr float kFloatingShapeEdgeAlpha = 0.11f;
constexpr float kGridShapeEdgeAlpha = 0.08f;

float degreesToRadians(float degrees) {
    return degrees * kPi / 180.f;
}

void appendArcPoints(std::vector<nxui::Vec2>& points,
                     float centerX,
                     float centerY,
                     float radius,
                     float startAngle,
                     float endAngle,
                     int segments,
                     bool includeStart)
{
    int startIndex = includeStart ? 0 : 1;
    for (int i = startIndex; i <= segments; ++i) {
        float t = segments > 0 ? (float)i / (float)segments : 0.f;
        float angle = startAngle + (endAngle - startAngle) * t;
        points.push_back({centerX + std::cos(angle) * radius,
                          centerY + std::sin(angle) * radius});
    }
}

float random01() {
    return (std::rand() % 1000) / 1000.f;
}

float randomRange(float minValue, float maxValue) {
    return minValue + (maxValue - minValue) * random01();
}

float wrapValue(float value, float minValue, float maxValue) {
    float span = maxValue - minValue;
    if (span <= 0.f)
        return minValue;
    while (value < minValue)
        value += span;
    while (value > maxValue)
        value -= span;
    return value;
}

int symmetryMultiplier(WaraWaraBackground::Symmetry symmetry) {
    switch (symmetry) {
        case WaraWaraBackground::Symmetry::MirrorHorizontal:
        case WaraWaraBackground::Symmetry::MirrorVertical:
            return 2;
        case WaraWaraBackground::Symmetry::Quad:
            return 4;
        case WaraWaraBackground::Symmetry::None:
        default:
            return 1;
    }
}

int maxSafeBaseShapeCount(const WaraWaraBackground::Config& config) {
    const int multiplier = std::max(1, symmetryMultiplier(config.symmetry));
    const int maxBudget = std::max(1, nxui::GpuDevice::MAX_VERTICES - kBackgroundRenderReserveVertices);
    const int perShapeBudget = std::max(1, kWorstCaseShapeVertices * kGlassShapeLayers * multiplier);
    return std::max(1, maxBudget / perShapeBudget);
}

int maxVisualBaseShapeCount(const WaraWaraBackground::Config& config) {
    const int multiplier = std::max(1, symmetryMultiplier(config.symmetry));
    const int renderedBudget = (config.layout == WaraWaraBackground::Layout::Grid)
        ? kGridRenderedShapeBudget
        : kFloatingRenderedShapeBudget;
    return std::max(1, renderedBudget / multiplier);
}

int renderedShapeCopies(const WaraWaraBackground::Config& config, int baseShapeCount) {
    return std::max(0, baseShapeCount) * std::max(1, symmetryMultiplier(config.symmetry));
}

bool useDenseRenderMode(const WaraWaraBackground::Config& config, int baseShapeCount) {
    return config.layout == WaraWaraBackground::Layout::Grid
        && renderedShapeCopies(config, baseShapeCount) >= kDenseRenderCopyThreshold;
}

std::uint64_t estimateVertexCount(const WaraWaraBackground::Config& config, int baseShapeCount) {
    return (std::uint64_t)std::max(0, baseShapeCount)
        * (std::uint64_t)std::max(1, symmetryMultiplier(config.symmetry))
        * (std::uint64_t)kWorstCaseShapeVertices
        * (std::uint64_t)kGlassShapeLayers;
}

} // namespace

WaraWaraBackground::WaraWaraBackground() { regenerate(30); }

void WaraWaraBackground::setConfig(const Config& config) {
    m_config = config;
    m_config.shapeCount = std::max(1, m_config.shapeCount);
    m_config.gridColumns = std::max(1, m_config.gridColumns);
    m_config.gridRows = std::max(1, m_config.gridRows);
    m_config.spacingX = std::max(1.f, m_config.spacingX);
    m_config.spacingY = std::max(1.f, m_config.spacingY);
    m_config.sizeMin = std::max(1.f, m_config.sizeMin);
    m_config.sizeMax = std::max(m_config.sizeMin, m_config.sizeMax);
    m_config.speedMin = std::max(0.f, m_config.speedMin);
    m_config.speedMax = std::max(m_config.speedMin, m_config.speedMax);
    m_config.wobble = std::max(0.f, m_config.wobble);
    m_config.opacity = std::clamp(m_config.opacity, 0.f, 1.f);
    m_config.cornerRoundness = std::clamp(m_config.cornerRoundness, 0.f, 1.f);
    m_config.imageOpacity = std::clamp(m_config.imageOpacity, 0.f, 1.f);
    regenerate(0);
}

bool WaraWaraBackground::loadImage(nxui::GpuDevice& gpu, nxui::Renderer& ren, const std::string& path) {
    if (path.empty()) {
        clearImage();
        return false;
    }

    nxui::Texture texture;
    if (!texture.loadFromFile(gpu, ren, path, 0))
        return false;

    // Keep the current wallpaper alive until the replacement has loaded, but
    // retire every part of it before publishing the still image. Otherwise an
    // animated theme leaves m_frames populated and currentBackground() keeps
    // selecting those stale frames ahead of the newly loaded image.
    const size_t replacedFrames = m_frames.size();
    clearImage();
    m_backgroundImage = std::move(texture);
    if (replacedFrames > 0)
        DebugLog::log("[background] still image replaced %zu animated frames", replacedFrames);
    return true;
}

bool WaraWaraBackground::loadImageSequence(nxui::GpuDevice& gpu, nxui::Renderer& ren,
                                           const std::vector<std::string>& paths,
                                           float fps) {
    clearImage();
    if (paths.empty())
        return false;

    // The wallpaper's share of the image budget. The rest belongs to game icons,
    // and exhausting that budget is what produced the crash reports this project
    // spent days on -- a wallpaper does not get to reopen it.
    //
    // The console reports 458 MB to this process with 220 MB free, so this is
    // nowhere near what the hardware allows; it is what a background is worth.
    //
    // 80 MB fits 30 fps over ten seconds, which is 300 frames and 68 MB. At the
    // previous 64 MB that sequence would have been sampled in half and played
    // at 15 fps -- worse than the 24 it was meant to improve on.
    // 80 MB, que e o maior valor comprovado. Nao e uma escolha de gosto: acima
    // de ~100 MB de imagem o processo morre carregando os quadros, sem erro,
    // sem estouro de orcamento, sem relatorio de falha.
    //
    // Medido no console:
    //   315 quadros x 232 KB =  71 MB  funciona
    //   300 quadros x 232 KB =  68 MB  funciona
    //   600 quadros x 232 KB = 136 MB  trava carregando
    //   240 quadros x 640 KB = 150 MB  trava carregando
    //
    // Repare que 240 blocos falharam onde 315 funcionaram: nao e a quantidade
    // de alocacoes, e o total de bytes. Foi o que essa comparacao desfez -- a
    // conclusao anterior culpava a contagem de blocos e estava errada.
    // Derivado do orcamento de imagem, e nao mais fixo em 80 MB. O orcamento
    // agora depende de quanto heap o console concedeu no arranque, e isso pode
    // variar: o menu e um applet, e voltar de um jogo e quando o bolso de
    // memoria esta mais apertado. Se a escada de heap recuar, o papel de parede
    // tem de encolher junto -- com um teto fixo ele tentaria carregar o que nao
    // cabe mais, e um quadro que nao aloca derruba o processo.
    //
    // Oitenta e dois por cento ainda deixa 42 MB dos 232 MB medidos para as
    // previas da loja, icones e fontes (pico observado: 30 MB), mas comporta
    // os 300 quadros BC1 de 1280x720: 187.5 MB. A regra anterior de 80% dava
    // 185.6 MB e, por faltar so 1.9 MB, reduzia o video inteiro para 15 fps.
    const uint64_t kFrameMemoryBudget = nxui::GpuDevice::imageBudget() * 82 / 100;

    // Teto por contagem, alem do teto por bytes, porque nem tudo que uma
    // sequencia consome e memoria: cada quadro e um MemBlock de GPU proprio, e
    // o driver nao aguenta uma quantidade arbitraria deles.
    //
    // Medido no console: 126 quadros carregam sempre; 600 travaram o menu e
    // derrubaram o sistema durante o carregamento, cerca de 540 quadros
    // adentro, antes de qualquer teto de bytes ser alcancado. 320 fica com
    // folga larga do que falhou e ainda cobre 13 segundos a 24 fps.
    //
    // O limite exato entre 126 e 600 nao foi medido -- este numero e
    // conservador de proposito, e o custo de erra-lo para baixo e um laco mais
    // espacado, contra um crash se errar para cima.
    constexpr size_t kMaxFrames = 400;

    auto frameCost = [](const nxui::Texture& tex) -> uint64_t {
        // What the GPU actually holds. Not width*height*4: a compressed frame
        // is a fraction of that, and a tiled one is more than its pixels.
        const uint64_t reported = tex.gpuBytes();
        return reported ? reported : (uint64_t)tex.width() * tex.height() * 4;
    };

    // The first frame is loaded to be measured, because only the device knows
    // what a frame really costs once tiled -- 640x360 of BC1 is 115200 bytes of
    // data and 163840 bytes of image.
    nxui::Texture first;
    if (!first.loadFromFile(gpu, ren, paths[0], 0)) {
        DebugLog::log("[background] first frame failed to load (%s)", paths[0].c_str());
        return false;
    }

    const uint64_t cost = frameCost(first);
    size_t affordable = (cost > 0) ? (size_t)(kFrameMemoryBudget / cost) : paths.size();
    if (affordable > kMaxFrames)
        affordable = kMaxFrames;
    if (affordable < 1)
        affordable = 1;

    // When the whole sequence will not fit, take every Nth frame rather than the
    // first N. Both drop frames; only one keeps the loop.
    //
    // Truncating leaves the animation ending wherever the budget ran out and
    // snapping back to the start, mid-motion, every time round. That shipped
    // once: a 120 frame loop silently became 89, and it read as a glitch in the
    // video rather than as a theme that did not fit. Sampling instead covers the
    // same span more coarsely, which looks like a lower frame rate -- honest
    // about what was lost.
    size_t stride = 1;
    if (affordable < paths.size()) {
        stride = (paths.size() + affordable - 1) / affordable;
        DebugLog::log("[background] %zu frames at %.2f MB each exceed the %.0f MB share; "
                      "taking every %zu to keep the loop whole",
                      paths.size(), cost / 1048576.0,
                      kFrameMemoryBudget / 1048576.0, stride);
    }

    // Everything past here used to happen right now, on this thread: 314 more
    // files, 71 MB off the card, 3.4 seconds of a menu that is supposed to come
    // back instantly. The first frame is already loaded and is a perfectly good
    // still wallpaper, so the menu can go live on it while the rest arrives.
    std::vector<std::string> rest;
    rest.reserve(paths.size() / stride + 1);
    for (size_t i = stride; i < paths.size(); i += stride)
        rest.push_back(paths[i]);

    m_frames.reserve(rest.size() + 1);
    m_frames.push_back(std::move(first));

    m_backgroundImage = nxui::Texture{};
    m_frameIndex = 0;
    m_frameTimer = 0.f;
    m_frameInterval = 0.f;          // parado ate a sequencia estar inteira

    m_pendingFrameTotal = rest.size() + 1;
    m_pendingFrameCost = cost;
    m_pendingFrameSourceFps = fps;
    m_pendingFrameStride = stride;
    m_pendingFrameSourceCount = paths.size();
    // Scaled by the stride, so a sampled sequence still plays at the speed the
    // clip was filmed instead of racing through it.
    m_pendingFrameInterval = (fps > 0.f && m_pendingFrameTotal > 1) ? ((float)stride / fps) : 0.f;

    if (rest.empty()) {
        m_pendingFrameTotal = 1;
        DebugLog::log("[background] quadro unico (%dx%d)",
                      m_frames[0].width(), m_frames[0].height());
        return true;
    }

    m_frameSequencePending = true;
    m_frameReaderStop = false;
    m_frameReaderDone = false;
    m_frameReader = std::thread([this, rest = std::move(rest)]() {
#ifdef __SWITCH__
        // Keep sequential SD reads and frame decompression away from the
        // core-0 render/input loop. Core 2 is inside the menu's 0-2 mask.
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, UINT64_C(1) << 2);
#endif
        for (const std::string& path : rest) {
            if (m_frameReaderStop)
                break;

            // Espera enquanto a fila esta cheia, em vez de ler tudo de uma vez:
            // o teto e o que impede o leitor de segurar a sequencia inteira em
            // memoria comum antes de ela chegar na GPU.
            for (;;) {
                if (m_frameReaderStop)
                    return;
                {
                    std::lock_guard<std::mutex> lk(m_frameQueueMutex);
                    if (m_frameQueue.size() < kFrameQueueLimit)
                        break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f)
                break;
            std::fseek(f, 0, SEEK_END);
            const long end = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<std::uint8_t> bytes;
            bool ok = end > 128;
            if (ok) {
                bytes.resize((size_t)end);
                ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
            }
            std::fclose(f);
            if (!ok)
                break;

            std::lock_guard<std::mutex> lk(m_frameQueueMutex);
            m_frameQueue.push_back(std::move(bytes));
        }
        m_frameReaderDone = true;
    });

    return true;
}

void WaraWaraBackground::pumpImageSequence(nxui::GpuDevice& gpu, nxui::Renderer& ren) {
    if (m_videoPlayer.isOpened()) {
        m_videoPlayer.tick(gpu, ren);
    }

    if (!m_frameSequencePending)
        return;

    for (int uploaded = 0; uploaded < kFrameUploadsPerFrame; ++uploaded) {
        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lk(m_frameQueueMutex);
            if (m_frameQueue.empty())
                break;
            bytes = std::move(m_frameQueue.front());
            m_frameQueue.pop_front();
        }

        nxui::Texture tex;
        if (!tex.loadBc1Memory(gpu, ren, bytes.data(), bytes.size())) {
            // Stopping here rather than carrying on: a sequence missing a frame
            // in the middle stutters at that point every loop, which looks like
            // a fault rather than a shorter animation.
            DebugLog::log("[background] quadro %zu nao subiu, ficando com %zu",
                          m_frames.size(), m_frames.size());
            m_frameReaderStop = true;
            break;
        }
        m_frames.push_back(std::move(tex));
    }

    bool queueEmpty;
    {
        std::lock_guard<std::mutex> lk(m_frameQueueMutex);
        queueEmpty = m_frameQueue.empty();
    }
    if (!queueEmpty && !m_frameReaderStop)
        return;
    if (!m_frameReaderDone && !m_frameReaderStop)
        return;

    stopFrameReader();
    m_frameSequencePending = false;

    if (m_frames.size() > 1)
        m_frameInterval = m_pendingFrameInterval;
    m_frameIndex = 0;
    m_frameTimer = 0.f;

    // The rate this actually plays at, not the one the theme asked for. A
    // sampled sequence kept reporting the manifest's figure, and a 600 frame
    // theme sampled down to 200 logged itself as 60 fps while playing at 20 --
    // which is exactly the kind of number somebody then makes a decision on.
    const float effectiveFps = (m_frameInterval > 0.f) ? (1.f / m_frameInterval) : 0.f;
    // O custo real por quadro, que nao e largura*altura/2: a imagem e
    // ladrilhada e o padding depende da altura de um jeito que so o driver
    // sabe. Sem este numero no log, dimensionar uma sequencia nova e chute.
    DebugLog::log("[background] %.0f KB por quadro, %.1f MB no total",
                  m_pendingFrameCost / 1024.0,
                  (double)(m_pendingFrameCost * m_frames.size()) / 1048576.0);

    if (m_pendingFrameStride > 1) {
        DebugLog::log("[background] %zu frames at %.1f fps (%dx%d) -- sampled 1 in %zu "
                      "from %zu, the theme asked for %.1f",
                      m_frames.size(), effectiveFps,
                      m_frames[0].width(), m_frames[0].height(),
                      m_pendingFrameStride, m_pendingFrameSourceCount, m_pendingFrameSourceFps);
    } else {
        DebugLog::log("[background] %zu frames at %.1f fps (%dx%d)",
                      m_frames.size(), effectiveFps,
                      m_frames[0].width(), m_frames[0].height());
    }
}

void WaraWaraBackground::stopFrameReader() {
    m_frameReaderStop = true;
    if (m_frameReader.joinable())
        m_frameReader.join();
    std::lock_guard<std::mutex> lk(m_frameQueueMutex);
    m_frameQueue.clear();
}

void WaraWaraBackground::clearImage() {
    // Primeiro o leitor: ele escreve numa fila deste objeto, e trocar de tema
    // enquanto ele corre deixaria quadros do tema anterior chegando no novo.
    stopFrameReader();
    m_videoPlayer.close();
    m_frameSequencePending = false;
    m_backgroundImage = nxui::Texture{};
    m_frames.clear();
    m_frameInterval = 0.f;
    m_frameTimer = 0.f;
    m_frameIndex = 0;
}

bool WaraWaraBackground::loadVideo(const std::string& path, bool loop) {
    clearImage();
    if (path.empty())
        return false;
    return m_videoPlayer.open(path, loop, false);
}

bool WaraWaraBackground::isVideoActive() const {
    return m_videoPlayer.isOpened();
}

// The texture to draw this frame: the sequence when there is one, the still
// wallpaper otherwise. Everything downstream asks through here so neither path
// has to know about the other.
const nxui::Texture* WaraWaraBackground::currentBackground() const {
    if (m_videoPlayer.isOpened()) {
        const nxui::Texture* vt = m_videoPlayer.texture();
        if (vt && vt->valid())
            return vt;
    }
    if (!m_frames.empty())
        return &m_frames[(size_t)m_frameIndex];
    return m_backgroundImage.valid() ? &m_backgroundImage : nullptr;
}

// Six frames a second is a step every 166ms, and no amount of image quality
// hides that -- it reads as a stutter. More frames is not available: twelve a
// second across this clip would be 122 textures and 27 MB, most of the image
// budget, for a wallpaper.
//
// So the frames are dissolved into one another instead. Motion becomes
// continuous at whatever rate the display runs, the memory cost is unchanged,
// and it works here because the content is a soft gradient: blending two of
// them looks like the real intermediate frame. Sharp footage would smear, and
// this would be the wrong trick for it.
const nxui::Texture* WaraWaraBackground::nextBackground() const {
    if (m_videoPlayer.isOpened())
        return nullptr;
    if (m_frames.size() < 2)
        return nullptr;
    return &m_frames[((size_t)m_frameIndex + 1) % m_frames.size()];
}

float WaraWaraBackground::frameBlend() const {
    if (m_frameInterval <= 0.f || m_frames.size() < 2)
        return 0.f;

    // Above a certain rate the dissolve stops earning its cost. It exists to
    // hide the step between frames, and at 12 fps that step is 83ms and very
    // visible; at 30 fps it is 33ms and the blend is guessing at almost
    // nothing. What it charges either way is a second full-screen draw on
    // every rendered frame.
    //
    // Measured: the menu runs at 57 fps with no animated wallpaper and about
    // 48 with one, at any frame rate -- because the per-frame work is the same
    // two quads regardless of how many frames are stored. This is where those
    // nine frames per second go.
    if (m_frameInterval < kCrossFadeMaxInterval)
        return 0.f;

    const float t = m_frameTimer / m_frameInterval;
    return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
}

WaraWaraBackground::ShapeType WaraWaraBackground::pickShapeType() const {
    switch (m_config.shapeSet) {
        case ShapeSet::Circle:
            return Circle;
        case ShapeSet::Triangle:
            return Triangle;
        case ShapeSet::Square:
            return Square;
        case ShapeSet::Diamond:
            return Diamond;
        case ShapeSet::Hexagon:
            return Hexagon;
        case ShapeSet::Mixed:
        default:
            return static_cast<ShapeType>(std::rand() % ShapeCount);
    }
}

void WaraWaraBackground::regenerate(int count) {
    float areaX = m_rect.x;
    float areaY = m_rect.y;
    float areaW = (m_rect.width > 1.f) ? m_rect.width : 1280.f;
    float areaH = (m_rect.height > 1.f) ? m_rect.height : 720.f;

    int requestedShapeCount = count > 0 ? count : m_config.shapeCount;
    const int requestedGridCells = std::max(1, m_config.gridColumns * m_config.gridRows);
    if (m_config.layout == Layout::Grid)
        requestedShapeCount = requestedGridCells;

    const int safeShapeCount = maxSafeBaseShapeCount(m_config);
    const int visualShapeCount = maxVisualBaseShapeCount(m_config);
    const int shapeCount = std::min(requestedShapeCount, std::min(safeShapeCount, visualShapeCount));
    if (shapeCount != requestedShapeCount) {
        DebugLog::log("[background] shape request clamped: layout=%s requested=%d effective=%d symmetry=%d estVerts=%llu safeVerts=%d visualCap=%d renderedCopies=%d",
                      m_config.layout == Layout::Grid ? "grid" : "floating",
                      requestedShapeCount,
                      shapeCount,
                      symmetryMultiplier(m_config.symmetry),
                      (unsigned long long)estimateVertexCount(m_config, requestedShapeCount),
                      nxui::GpuDevice::MAX_VERTICES - kBackgroundRenderReserveVertices,
                      visualShapeCount,
                      renderedShapeCopies(m_config, shapeCount));
    }

    m_shapes.resize(shapeCount);
    for (int index = 0; index < shapeCount; ++index) {
        Shape& s = m_shapes[index];
        s.type = pickShapeType();
        if (m_config.layout == Layout::Grid) {
            const int cellIndex = std::min(requestedGridCells - 1,
                                           (shapeCount >= requestedGridCells)
                                               ? index
                                               : (int)(((long long)index * requestedGridCells) / shapeCount));
            int col = cellIndex % m_config.gridColumns;
            int row = cellIndex / m_config.gridColumns;
            float gridW = (m_config.gridColumns - 1) * m_config.spacingX;
            float gridH = (m_config.gridRows - 1) * m_config.spacingY;
            float startX = areaX + (areaW - gridW) * 0.5f;
            float startY = areaY + (areaH - gridH) * 0.5f;
            s.pos = {startX + col * m_config.spacingX, startY + row * m_config.spacingY};
        } else {
            s.pos = {areaX + random01() * areaW, areaY + random01() * areaH};
        }
        s.size = randomRange(m_config.sizeMin, m_config.sizeMax);
        s.speed = randomRange(m_config.speedMin, m_config.speedMax);
        s.phase = random01() * 6.28f;
        s.wobble = m_config.layout == Layout::Grid ? 0.f : randomRange(m_config.wobble * 0.4f, m_config.wobble);
        if (m_config.fixedOrientation) {
            s.rotation = degreesToRadians(m_config.orientationDegrees);
            s.rotSpeed = m_config.rotationSpeed;
        } else {
            s.rotation = random01() * 6.28f;
            s.rotSpeed = randomRange(m_config.rotationSpeed * 0.35f, m_config.rotationSpeed);
            if (std::rand() % 2) s.rotSpeed = -s.rotSpeed;
        }
        s.glassAlpha = randomRange(kShapeBodyAlphaMin, kShapeBodyAlphaMax) * m_config.opacity;
        s.color = nxui::Color::white().withAlpha(s.glassAlpha);
    }
}

void WaraWaraBackground::onUpdate(float dt) {
    // Scaling time rather than each speed keeps drift, wobble and spin in step
    // with one another, so the layer slows down as a whole instead of the
    // shapes sliding while still spinning at the authored rate.
    const float wallpaperDt = dt * m_frameSpeedScale;
    dt *= m_speedScale;
    m_time += dt;

    // Deliberately not the shape-scaled dt. The slider still moves the
    // wallpaper, but through its own factor: the shapes carry a x2 their
    // authored speeds were tuned around, and applying that here played a ten
    // second clip in five.
    if (m_frameInterval > 0.f && m_frames.size() > 1) {
        m_frameTimer += wallpaperDt;
        while (m_frameTimer >= m_frameInterval) {
            m_frameTimer -= m_frameInterval;
            // Wrap to the start. This used to ping-pong, back when the sequence
            // was one second of a ten second clip and wrapping jumped visibly at
            // the seam. Covering the whole clip removed the reason: a background
            // video is authored to loop, so its last frame already matches its
            // first, and reversing direction is what made a slow drift read as
            // hurried -- the gradient shuttled instead of flowing.
            //
            // A sequence that does not loop cleanly will show a seam here. That
            // is a property of the clip, and the fix belongs to whoever chooses
            // it, not to the player.
            if (++m_frameIndex >= (int)m_frames.size()) m_frameIndex = 0;
        }
    }
    for (auto& s : m_shapes) {
        if (m_config.layout == Layout::Floating) {
            float top = m_rect.y - s.size - 20.f;
            float bottom = m_rect.y + ((m_rect.height > 1.f) ? m_rect.height : 720.f) + s.size + 20.f;
            float left = m_rect.x;
            float width = (m_rect.width > 1.f) ? m_rect.width : 1280.f;
            s.pos.y -= s.speed * dt;
            s.pos.x += std::sin(m_time * 0.7f + s.phase) * s.wobble * dt;
            s.pos.x = wrapValue(s.pos.x, left - s.size, left + width + s.size);
            if (s.pos.y + s.size < top) {
                s.pos.y = bottom;
                s.pos.x = left + random01() * width;
            }
        }
        s.rotation += s.rotSpeed * dt;
    }

    if (m_ambientMiisEnabled && !m_ambientMiis.empty()) {
        for (auto& mii : m_ambientMiis) {
            mii->update(dt);
        }
    }
}

nxui::Rect WaraWaraBackground::backgroundImageRect() const {
    const nxui::Texture* bg = currentBackground();
    if (!bg || !bg->valid() || bg->width() <= 0 || bg->height() <= 0)
        return m_rect;

    float areaX = m_rect.x;
    float areaY = m_rect.y;
    float areaW = (m_rect.width > 1.f) ? m_rect.width : 1280.f;
    float areaH = (m_rect.height > 1.f) ? m_rect.height : 720.f;
    float texW = static_cast<float>(bg->width());
    float texH = static_cast<float>(bg->height());
    float sx = areaW / texW;
    float sy = areaH / texH;
    float scale = m_config.imageCover ? std::max(sx, sy) : std::min(sx, sy);
    float drawW = texW * scale;
    float drawH = texH * scale;
    return {areaX + (areaW - drawW) * 0.5f, areaY + (areaH - drawH) * 0.5f, drawW, drawH};
}

// Someone found the wallpaper too sharp and the drifting shapes distracting.
// Both come from the same layer, so both are softened by the same control: the
// layer is drawn into the half-resolution scene target, blurred there, and
// drawn back. Blurring only the image would have left the shapes crisp, which
// is the half of it that was actually in the way.
//
// The scene target is free at this point — the icon glass captures the finished
// framebuffer into it later in the frame, so this is done with it by then.
void WaraWaraBackground::onRender(nxui::Renderer& ren) {
    // O limiar nao e sobre gosto, e sobre o que o desfoque custa: OFF_SCENE e
    // OFF_BG_BLUR sao alvos de meia resolucao, entao ligar o desfoque desenha o
    // papel de parede em 640x360 e o estica de volta para 1280x720.
    //
    // Com 0.01 de limiar e 0.05 de padrao, todo mundo pagava isso sem pedir. No
    // valor padrao o raio e 1.25 com um passe -- desfoque invisivel -- e o que
    // se via era um fundo de video detalhado reduzido a 640x360. Relatado como
    // "aplicou a miniatura em vez do tema", que descreve com precisao o que
    // estava acontecendo.
    //
    // The slider must respond from its first visible increment without
    // collapsing the wallpaper at 5%.  A 0.75 power curve gives the first
    // increments a visible but restrained response; zero remains the only
    // value that skips the blur pass.
    // Video wallpapers are blurred by the same path as stills; zero remains the
    // only value that skips it.
    const bool blurred = m_blurStrength > 0.001f && ren.gpu().offscreenReady();
    if (blurred)
        ren.beginScreenSpaceTarget(nxui::GpuDevice::OFF_SCENE);

    renderLayer(ren, blurred);

    if (blurred) {
        // Linear slider -> linear blur width. The blur shader's 9-tap kernel
        // has sigma ~= 1.8 x the tap spacing (`radius`), and applyBlurBetween()
        // keeps each pass at spacing <= 2.5 and buys the rest with extra
        // passes, so total sigma stays ~1.8 x radius. In half-resolution texels
        // (2 screen px each): 5% -> sigma ~1.1 (a faint softening), 10% -> ~1.8,
        // 50% -> ~7 (3 passes), 100% -> ~13.5 (9 passes, 18 draws -- under the
        // 36 the previous curve spent at its maximum).
        //
        // Passes and per-pass spacing are chosen here (spacing <= 2.5, which
        // applyBlurBetween leaves untouched) so the total width stays
        // continuous: total = spacing * sqrt(passes). Handing it one wide pass
        // instead would let it re-quantize the pass count and flatten whole
        // stretches of the slider into identical blur.
        const float total = 0.25f + 7.25f * m_blurStrength;
        constexpr float kMaxSpacing = 2.5f;
        const int passes = std::max(1, static_cast<int>(std::ceil((total / kMaxSpacing) * (total / kMaxSpacing))));
        const float spacing = total / std::sqrt(static_cast<float>(passes));
        ren.applyBlurBetween(nxui::GpuDevice::OFF_SCENE, nxui::GpuDevice::OFF_BG_BLUR,
                             spacing, passes);
        ren.drawOffscreen(nxui::GpuDevice::OFF_SCENE, m_rect,
                          nxui::Color::white().withAlpha(m_opacity));
        ren.flush();
    }
}

void WaraWaraBackground::renderLayer(nxui::Renderer& ren, bool intoOffscreen) {
    (void)intoOffscreen;

    // Skip procedural gradient pass when an opaque video wallpaper is active
    if (!isVideoActive() || m_config.imageOpacity < 0.99f) {
        ren.useShader(nxui::ShaderProgram::Gradient);
        nxui::FsUniforms fs = {};
        fs.useTexture = 0;
        fs.param1 = m_time;
        fs.extra[0] = m_accent.r;  fs.extra[1] = m_accent.g;
        fs.extra[2] = m_accent.b;  fs.extra[3] = m_accent.a;
        fs.extra[4] = m_secondary.r;  fs.extra[5] = m_secondary.g;
        fs.extra[6] = m_secondary.b;  fs.extra[7] = m_secondary.a;
        fs.extra[8]  = m_shapeColor.r * 2.f;  fs.extra[9]  = m_shapeColor.g * 2.f;
        fs.extra[10] = m_shapeColor.b * 2.f;  fs.extra[11] = m_shapeColor.a;
        ren.pushFsUniforms(fs);
        ren.drawRect(m_rect, nxui::Color::white());
        ren.flush();
    }
    ren.useShader(nxui::ShaderProgram::Basic);

    const nxui::Texture* bg = currentBackground();
    if (bg && bg->valid() && m_config.imageOpacity > 0.f) {
        const float alpha = m_config.imageOpacity * m_opacity;
        const nxui::Rect rect = backgroundImageRect();
        ren.drawTexture(bg, rect, nxui::Color::white().withAlpha(alpha));

        // Then the frame it is turning into, faded in on top. At full opacity
        // the two draws compose to exactly a linear cross-fade; below that the
        // blend drifts slightly, which is invisible on a wallpaper already
        // being faded into whatever is behind it.
        const float blend = frameBlend();
        if (blend > 0.f) {
            if (const nxui::Texture* nextFrame = nextBackground()) {
                ren.drawTexture(nextFrame, rect,
                                nxui::Color::white().withAlpha(alpha * blend));
            }
        }
    }

    if (!isVideoActive()) {
        const nxui::Renderer::DrawTagScope tag{ren, "bg.shapes"};
        for (const auto& s : m_shapes)
            drawShapeWithSymmetry(ren, s);
    }

    if (m_ambientMiisEnabled && !m_ambientMiis.empty()) {
        const nxui::Renderer::DrawTagScope tag{ren, "bg.ambientMii"};
        for (const auto& mii : m_ambientMiis) {
            mii->render(ren, m_ambientFont, m_ambientSmallFont);
        }
    }

    // The background owns the shape field and the ambient Miis, so it reports
    // those two counts; the Plaza screen reports its own population and does
    // not overwrite them.
    if (ren.drawJournalEnabled())
        ren.setBackgroundCounts((uint32_t)m_ambientMiis.size(),
                                (uint32_t)m_shapes.size());

    ren.flush();
}

void WaraWaraBackground::drawShapeWithSymmetry(nxui::Renderer& ren, const Shape& s) const {
    drawGlassShape(ren, s);

    float left = m_rect.x;
    float top = m_rect.y;
    float width = (m_rect.width > 1.f) ? m_rect.width : 1280.f;
    float height = (m_rect.height > 1.f) ? m_rect.height : 720.f;

    auto mirrorHorizontal = [&](const Shape& source) {
        Shape mirrored = source;
        mirrored.pos.x = left + width - (source.pos.x - left);
        return mirrored;
    };

    auto mirrorVertical = [&](const Shape& source) {
        Shape mirrored = source;
        mirrored.pos.y = top + height - (source.pos.y - top);
        return mirrored;
    };

    if (m_config.symmetry == Symmetry::MirrorHorizontal || m_config.symmetry == Symmetry::Quad)
        drawGlassShape(ren, mirrorHorizontal(s));

    if (m_config.symmetry == Symmetry::MirrorVertical || m_config.symmetry == Symmetry::Quad)
        drawGlassShape(ren, mirrorVertical(s));

    if (m_config.symmetry == Symmetry::Quad)
        drawGlassShape(ren, mirrorVertical(mirrorHorizontal(s)));
}

void WaraWaraBackground::drawGlassShape(nxui::Renderer& ren, const Shape& s) const {
    float a = s.color.a * m_opacity;
    if (a < 0.003f) return;

    nxui::Color body = m_shapeColor.withAlpha(a);
    drawRoundedShape(ren, s, body);

    if (useDenseRenderMode(m_config, (int)m_shapes.size()))
        return;

    Shape highlight = s;
    highlight.pos.y -= s.size * 0.08f;
    highlight.size   = s.size * 0.85f;
    const float effectAlpha = m_config.opacity * m_opacity;
    nxui::Color hi = nxui::Color(1.f, 1.f, 1.f, kShapeHighlightAlpha * effectAlpha);
    drawRoundedShape(ren, highlight, hi);

    Shape edge = s;
    edge.size = s.size * 1.06f;
    const float edgeAlpha = (m_config.layout == Layout::Grid) ? kGridShapeEdgeAlpha : kFloatingShapeEdgeAlpha;
    nxui::Color edgeC = nxui::Color(1.f, 1.f, 1.f, edgeAlpha * effectAlpha);
    drawRoundedShape(ren, edge, edgeC);
}

void WaraWaraBackground::drawRoundedShape(nxui::Renderer& ren, const Shape& s, const nxui::Color& c) const {
    float sz = s.size;
    float h = sz * 0.707f;

    switch (s.type) {
    case Circle:
        ren.drawCircle(s.pos, sz, c, 12);
        break;
    case Square:
    case Triangle:
    case Diamond:
    case Hexagon:
    default: {
        float roundness = std::clamp(m_config.cornerRoundness, 0.f, 1.f);
        float radius = h * (roundness > 0.001f ? roundness : 0.32f);
        ren.drawRoundedRect(nxui::Rect{s.pos.x - h, s.pos.y - h, h * 2.f, h * 2.f}, c, radius);
        break;
    }
    }
}

void WaraWaraBackground::setAmbientAvatars(const std::vector<warawara::MiiAvatarData>& avatars,
                                          nxui::Font* font,
                                          nxui::Font* smallFont) {
    m_ambientFont = font;
    m_ambientSmallFont = smallFont;
    m_ambientMiis.clear();

    if (avatars.empty()) {
        return;
    }

    // Spawn 6 roaming ambient Miis behind the main grid
    const size_t count = std::min<size_t>(8, std::max<size_t>(4, avatars.size()));
    m_ambientMiis.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const auto& av = avatars[i % avatars.size()];
        auto mii = std::make_unique<warawara::MiiFigure>(av);

        // Place along the lower half of the screen
        float x = 80.0f + static_cast<float>(i) * (1120.0f / static_cast<float>(count));
        float y = 520.0f + static_cast<float>(i % 3) * 35.0f;
        mii->setPosition({x, y});

        // Ambient scale: slightly smaller (0.84f) so they sit gently in the background
        mii->setScale(0.84f);

        // Gentle ambient shadow and subtle nickname pill
        auto cfg = mii->config();
        cfg.wanderBounds = nxui::Rect{60.0f, 490.0f, 1160.0f, 140.0f};
        cfg.walkSpeed = 38.0f; // Slower, calmer stroll
        cfg.showNamePill = true;
        mii->setConfig(cfg);

        mii->setAutonomous(true);
        mii->idle(1.0f + static_cast<float>(i) * 0.7f);

        m_ambientMiis.push_back(std::move(mii));
    }
}
