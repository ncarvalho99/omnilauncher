#pragma once
#include <nxui/widgets/Background.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/Types.hpp>
#include <nxui/core/Font.hpp>
#include "warawara/MiiFigure.hpp"
#include "video/VideoPlayer.hpp"
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


class WaraWaraBackground : public nxui::Background {
public:
    enum class Layout {
        Floating,
        Grid,
    };

    enum class ShapeSet {
        Mixed,
        Circle,
        Triangle,
        Square,
        Diamond,
        Hexagon,
    };

    enum class Symmetry {
        None,
        MirrorHorizontal,
        MirrorVertical,
        Quad,
    };

    struct Config {
        Layout layout = Layout::Floating;
        ShapeSet shapeSet = ShapeSet::Mixed;
        Symmetry symmetry = Symmetry::None;
        int shapeCount = 30;
        int gridColumns = 14;
        int gridRows = 8;
        float spacingX = 88.f;
        float spacingY = 88.f;
        float sizeMin = 14.f;
        float sizeMax = 54.f;
        float speedMin = 6.f;
        float speedMax = 28.f;
        float wobble = 16.f;
        float opacity = 1.f;
        float rotationSpeed = 0.5f;
        bool fixedOrientation = false;
        float orientationDegrees = 0.f;
        float cornerRoundness = 0.f;
        float imageOpacity = 0.f;
        bool imageCover = true;
    };

    WaraWaraBackground();

    // O leitor de quadros aponta para esta instancia. Se ela sumir antes
    // dele, ele escreve numa fila que nao existe mais.
    ~WaraWaraBackground() { stopFrameReader(); }

    void setConfig(const Config& config);
    const Config& config() const { return m_config; }

    bool loadImage(nxui::GpuDevice& gpu, nxui::Renderer& ren, const std::string& path);
    // A wallpaper that moves, without decoding anything while it runs.
    //
    // Real video was measured and ruled out: a 1080p still costs 365ms to
    // decode on this hardware, and even 640x360 projects to about 40ms -- more
    // than twice a whole frame, for a background. Decoding every frame is not
    // available at any resolution, and ffmpeg would not help because it decodes
    // in software here too.
    //
    // So the frames are decoded once, at load, and the loop only swaps which
    // texture is drawn. That costs nothing per frame beyond the draw already
    // being made.
    //
    // Loading them is another matter. 315 frames are 71 MB off the card, and
    // reading that where the menu is being built cost 3.4 of the 4.1 seconds it
    // took to come back from a game -- the very delay this menu exists to
    // avoid, reintroduced by making the default wallpaper animated. So only the
    // first frame is loaded here, as a still; the rest are read on a worker
    // thread and handed over a few per rendered frame by pumpImageSequence.
    bool loadImageSequence(nxui::GpuDevice& gpu, nxui::Renderer& ren,
                           const std::vector<std::string>& paths, float fps);

    // Uploads whatever the reader has ready. Cheap and safe to call every frame;
    // does nothing once the sequence is complete. Must be called from the
    // render thread -- that is the whole reason the work is split.
    void pumpImageSequence(nxui::GpuDevice& gpu, nxui::Renderer& ren);

    void clearImage();
    bool loadVideo(const std::string& path, bool loop = true);
    bool isVideoActive() const;
    bool hasAnimatedBackground() const { return m_frames.size() > 1 || isVideoActive(); }
    const nxui::Texture* currentBackground() const;
    // The frame after the current one, and how far along we are between the two.
    // Together they let the renderer dissolve from one to the next instead of
    // stepping, which is what buys smooth motion out of few frames.
    const nxui::Texture* nextBackground() const;
    float frameBlend() const;

    // Acima de 20 quadros por segundo a dissolucao e desligada: o intervalo
    // fica curto demais para ela compensar o desenho extra em tela cheia que
    // custa. 1/20 de segundo e o ponto de corte.
    static constexpr float kCrossFadeMaxInterval = 1.f / 20.f;

    void regenerate(int count = 50) override;

    // Softens the whole wallpaper layer — the image and the drifting shapes
    // alike. 0 draws it straight to the screen and costs nothing.
    void setBlurStrength(float v) { m_blurStrength = v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    float blurStrength() const { return m_blurStrength; }

    // Multiplies the pace of the whole layer. 0 stops it, 1 is what the theme
    // asked for, above that it hurries.
    void setSpeedScale(float v) { m_speedScale = v < 0.f ? 0.f : (v > 3.f ? 3.f : v); }
    float speedScale() const { return m_speedScale; }

    // The same slider, but without the factor the shapes carry. Callers double
    // the value above because the authored shape speeds were tuned expecting
    // it; a frame sequence has no such calibration -- it has a real duration,
    // and doubling it plays the clip at twice the speed it was filmed. So the
    // wallpaper is given the slider as the user set it.
    void setWallpaperSpeedScale(float v) {
        m_frameSpeedScale = v < 0.f ? 0.f : (v > 3.f ? 3.f : v);
    }

    // Ambient Mii Support
    void setAmbientAvatars(const std::vector<warawara::MiiAvatarData>& avatars,
                           nxui::Font* font = nullptr,
                           nxui::Font* smallFont = nullptr);
    void setAmbientMiisEnabled(bool enabled) { m_ambientMiisEnabled = enabled; }
    bool ambientMiisEnabled() const { return m_ambientMiisEnabled; }

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

private:
    void renderLayer(nxui::Renderer& ren, bool intoOffscreen);

    enum ShapeType { Circle, Triangle, Square, Diamond, Hexagon, ShapeCount };

    struct Shape {
        ShapeType type;
        nxui::Vec2  pos;
        float size;
        float speed;
        float phase;
        float wobble;
        float rotation;
        float rotSpeed;
        nxui::Color color;
        float glassAlpha;
    };

    ShapeType pickShapeType() const;
    nxui::Rect backgroundImageRect() const;
    void drawShapeWithSymmetry(nxui::Renderer& ren, const Shape& s) const;
    void drawGlassShape(nxui::Renderer& ren, const Shape& s) const;
    void drawRoundedShape(nxui::Renderer& ren, const Shape& s, const nxui::Color& c) const;

    Config m_config;
    std::vector<Shape> m_shapes;
    nxui::Texture m_backgroundImage;
    switchu::video::VideoPlayer m_videoPlayer;
    // Extra frames beyond the first. A still wallpaper leaves this empty and
    // costs exactly what it did before.
    std::vector<nxui::Texture> m_frames;
    float m_frameInterval = 0.f;   // seconds; 0 disables cycling
    float m_frameTimer    = 0.f;
    int   m_frameIndex    = 0;
    float m_time = 0.f;
    float m_blurStrength = 0.f;
    float m_speedScale = 1.f;
    float m_frameSpeedScale = 1.f;

    // --- leitura dos quadros restantes, fora da thread de desenho ---
    //
    // A fila e limitada porque ela e memoria comum: sem teto o leitor corre ate
    // o fim da sequencia e chega a segurar os 71 MB inteiros de uma vez, que e
    // o que se estava tentando evitar. Com o teto ele espera o desenho consumir.
    static constexpr size_t kFrameQueueLimit = 24;
    // Quantos entram na GPU por quadro desenhado. O upload em si e barato; o
    // limite existe para nao gastar o orcamento do quadro num pico.
    static constexpr int kFrameUploadsPerFrame = 2;

    void stopFrameReader();

    std::thread m_frameReader;
    std::mutex  m_frameQueueMutex;
    std::deque<std::vector<std::uint8_t>> m_frameQueue;
    std::atomic<bool> m_frameReaderStop{false};
    std::atomic<bool> m_frameReaderDone{false};
    // Enquanto a sequencia nao esta inteira, o fundo fica parado no primeiro
    // quadro: animar sobre o que ja chegou faria a velocidade mudar sozinha
    // enquanto o resto carrega, que le como defeito.
    bool  m_frameSequencePending = false;
    float m_pendingFrameInterval = 0.f;
    size_t m_pendingFrameTotal = 0;
    std::uint64_t m_pendingFrameCost = 0;
    float m_pendingFrameSourceFps = 0.f;
    size_t m_pendingFrameStride = 1;
    size_t m_pendingFrameSourceCount = 0;

    bool m_ambientMiisEnabled = false;
    std::vector<std::unique_ptr<warawara::MiiFigure>> m_ambientMiis;
    nxui::Font* m_ambientFont = nullptr;
    nxui::Font* m_ambientSmallFont = nullptr;
};

