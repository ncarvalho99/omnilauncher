#pragma once

#include "core/AppLayoutMode.hpp"

#include <nxui/core/Animation.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/widgets/Widget.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <vector>

class SteamGridDbBackdrop final : public nxui::Widget {
public:
    SteamGridDbBackdrop(nxui::GpuDevice& gpu, nxui::Renderer& renderer,
                        nxui::ThreadPool* threadPool);

    void setEnabled(bool enabled);
    void setLayoutMode(AppLayoutMode mode) { m_layoutMode = mode; }
    void setArtworkOpacityScale(float opacityScale) { m_artworkOpacityScale = std::clamp(opacityScale, 0.f, 1.f); }
    float artworkOpacityScale() const { return m_artworkOpacityScale; }
    void setPreloadTitles(std::vector<std::uint64_t> titleIds);
    void showTitle(std::uint64_t titleId, bool forceReload = false);
    // Call after a scan writes new files: everything cached in here was decided
    // from what was on disk at the time.
    void invalidateArtworkCaches();

protected:
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& renderer) override;

private:
    struct ArtworkSet {
        nxui::Texture hero;
        nxui::Texture logo;
        std::uint64_t titleId = 0;
        bool hasHero = false;
        bool hasLogo = false;
    };

    struct DecodedImage {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;
    };

    struct DecodedArtwork {
        DecodedImage hero;
        DecodedImage logo;
        std::uint64_t titleId = 0;
        std::uint64_t generation = 0;
        long decodeMilliseconds = 0;
    };

    struct DecodeState {
        DecodedArtwork artwork;
        std::atomic<bool> cancelled{false};
    };

    struct PendingDecode {
        std::shared_ptr<DecodeState> state;
        std::future<void> future;
    };

    static DecodedImage decodeImage(const std::string& path, int outputWidth,
                                    int outputHeight, bool fill);
    static nxui::Rect fillRect(const nxui::Texture& texture, const nxui::Rect& area);
    static nxui::Rect containRect(const nxui::Texture& texture, const nxui::Rect& area);
    void drawSet(nxui::Renderer& renderer, const ArtworkSet& set, float alpha) const;
    void startPendingDecode(std::uint64_t titleId);
    void beginCrossfade(int nextSet, std::uint64_t titleId);
    bool hasGpuArtwork(std::uint64_t titleId) const;

    nxui::GpuDevice& m_gpu;
    nxui::Renderer& m_renderer;
    nxui::ThreadPool* m_threadPool = nullptr;
    std::array<ArtworkSet, 2> m_sets;
    int m_current = 0;
    bool m_enabled = true;
    std::uint64_t m_requestedTitleId = 0;
    std::uint64_t m_requestGeneration = 0;
    std::uint64_t m_appliedGeneration = 0;
    float m_decodeDebounce = 0.f;
    std::optional<PendingDecode> m_pendingDecode;
    std::optional<DecodedArtwork> m_readyArtwork;
    std::vector<std::uint64_t> m_preloadTitleIds;
    std::vector<std::uint64_t> m_missingArtworkTitleIds;
    std::vector<DecodedArtwork> m_decodedCache;
    int m_uploadStage = 0;
    bool m_waitForGpuBeforeUpload = false;
    AppLayoutMode m_layoutMode = AppLayoutMode::Grid;
    nxui::AnimatedFloat m_fade{1.f};
    nxui::AnimatedFloat m_artworkOpacity{1.f};
    float m_artworkOpacityScale = 0.50f;
    static constexpr std::size_t kDecodedCacheLimit = 2;
    static constexpr std::size_t kMissingCacheLimit = 64;
};
