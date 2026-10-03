#include "ThemeShopScreen.hpp"

#include "ThemeHttp.hpp"

#include "core/DebugLog.hpp"

#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/ThreadPool.hpp>

#include <curlpp/Easy.hpp>
#include <curlpp/Infos.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>

namespace {

constexpr int kCommunityPreviewGridCols = 2;
constexpr int kCommunityPreviewVisibleRows = 2;
constexpr int kCommunityPreviewEntriesPerPage = kCommunityPreviewGridCols * kCommunityPreviewVisibleRows;
constexpr int kMaxPreviewUploadsPerFrame = 1;
constexpr int kMaxPreviewDownloadsInFlight = kCommunityPreviewEntriesPerPage;

// Quantas previas guardam os bytes comprimidos depois de sair da tela. Cada
// uma custa dezenas de kilobytes de RAM e evita uma requisicao de rede; a
// textura decodificada, que custa cerca de um megabyte, e liberada de todo
// jeito.
constexpr int kMaxCachedPreviewBytesEntries = 64;
// A previa era guardada com 960 de lado e desenhada num cartao de ~340: cada
// pixel na tela lia texels espalhados, sem mipmap, e a loja gastava 20 ms de
// GPU por quadro contra 10 na home. 640 continua acima do cartao e corta a
// amostragem pela metade.
constexpr int kCommunityPreviewMaxSide = 640;

// A folha e uma grade, nao uma foto: reduzi-la a 640 encolhe cada celula junto
// e a tela cheia vira mosaico. Ela ja e gerada no tamanho certo, entao vai
// inteira.
constexpr int kPreviewSheetMaxSide = 2560;   // a folha HD tem 2560x1152

// Covers come off the card, so nothing is gained by reading several at once,
// and something is lost: each decode holds the image at its source size before
// the downscale, and the menu has little heap to spare.
constexpr int kMaxInstalledPreviewDecodesInFlight = 1;

bool isPreviewSheetPath(const std::string& path) {
    return path.find("preview_sheet") != std::string::npos;
}

// A cover stored as compressed blocks goes to the DDS loader instead of the
// stb_image decoder, and only loadFromFile knows how to pick between them. Such
// a path keeps the synchronous route: the read is the whole cost there, and it
// is already a fraction of a decode.
bool isBlockCompressedPath(const std::string& path) {
    if (path.size() < 4)
        return false;
    std::string tail = path.substr(path.size() - 4);
    for (char& c : tail)
        c = (char)std::tolower((unsigned char)c);
    return tail == ".dds";
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string trimLeadingSlashes(std::string value) {
    while (!value.empty() && value.front() == '/')
        value.erase(value.begin());
    return value;
}

std::string trimWhitespace(std::string value) {
    auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    while (!value.empty() && isSpace((unsigned char)value.front()))
        value.erase(value.begin());
    while (!value.empty() && isSpace((unsigned char)value.back()))
        value.pop_back();
    return value;
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return (char)std::tolower(ch);
    });
    return value;
}

bool containsInsensitive(const std::string& haystack, const std::string& needleLower) {
    if (needleLower.empty())
        return true;
    return asciiLower(haystack).find(needleLower) != std::string::npos;
}

bool matchesSearch(const ThemeShopScreen::ThemeShopEntry& entry, const std::string& needleLower) {
    return needleLower.empty()
        || containsInsensitive(entry.name, needleLower)
        || containsInsensitive(entry.author, needleLower)
        || containsInsensitive(entry.id, needleLower)
        || containsInsensitive(entry.version, needleLower)
        || containsInsensitive(entry.source, needleLower)
        || containsInsensitive(entry.soundPreset, needleLower);
}

bool matchesSearch(const ThemeCatalogClient::Entry& entry, const std::string& needleLower) {
    return needleLower.empty()
        || containsInsensitive(entry.name, needleLower)
        || containsInsensitive(entry.id, needleLower)
        || containsInsensitive(entry.version, needleLower)
        || containsInsensitive(entry.author, needleLower);
}

// Qual imagem representa a entrada na grade: a folha de miniaturas quando o
// catalogo oferece, a capa quando nao.
//
// Uma regra so, num lugar so. Ela nasceu espalhada -- o download pedia a capa,
// o desenho aplicava recortes de folha, a poda retinha pela capa, a fase
// consultava outra ainda -- e cada divergencia dessas apareceu como um sintoma
// diferente na tela, corrigido um de cada vez ao longo de varias builds.
std::string gridPreviewPathOf(const ThemeCatalogClient::Entry& entry) {
    return entry.thumbSheet.empty() ? entry.cover : entry.thumbSheet;
}

} // namespace

ThemeShopScreen::ThemeShopScreen()
    : TabbedOverlayScreen(ScreenMode::ThemeShop) {
}

void ThemeShopScreen::setThreadPool(nxui::ThreadPool* pool) {
    m_threadPool = pool;
    m_youTubeClient.setThreadPool(pool);
}

void ThemeShopScreen::setRenderContext(nxui::GpuDevice* gpu, nxui::Renderer* renderer) {
    m_gpu = gpu;
    m_renderer = renderer;
    m_youTubeClient.setRenderContext(gpu, renderer);
}

bool ThemeShopScreen::consumeRenderDiagnosticsFrame() {
    m_renderDiagnosticsActive = false;
    if (m_renderDebugFrames <= 0)
        return false;

    --m_renderDebugFrames;
    m_renderDiagnosticsActive = true;
    return true;
}

void ThemeShopScreen::beginStateRefreshBatch() {
    ++m_stateRefreshBatchDepth;
}

void ThemeShopScreen::endStateRefreshBatch() {
    if (m_stateRefreshBatchDepth == 0)
        return;
    if (--m_stateRefreshBatchDepth != 0 || !m_stateRefreshPending)
        return;

    m_stateRefreshPending = false;
    rebuildCurrentTab();
}

void ThemeShopScreen::refreshState() {
    if (m_stateRefreshBatchDepth != 0) {
        m_stateRefreshPending = true;
        return;
    }

    rebuildCurrentTab();
}

void ThemeShopScreen::refreshCommunityCatalog() {
    if (!m_threadPool)
        return;

    m_catalogClient.refresh(*m_threadPool);
    syncCommunityCatalog(m_catalogClient.snapshot());
}

void ThemeShopScreen::setThemeShopState(const std::vector<ThemeShopEntry>& entries,
                                        const std::string& activeId) {
    m_allThemeShopEntries = entries;
    pruneInstalledPreviewCache(entries);
    applySearchFilter();
    // Um tema recém-instalado entra aqui, então é aqui que a medição do que ele
    // ocupa é enfileirada. Quem já foi medido não é medido de novo.
    measureInstalledThemes();

    auto hasId = [&](const std::string& id) {
        return std::any_of(m_themeShopEntries.begin(), m_themeShopEntries.end(),
                           [&](const ThemeShopEntry& entry) {
                               return entry.id == id;
                           });
    };

    if (m_themeShopEntries.empty()) {
        m_themeShopSelectedId.clear();
        return;
    }

    if (!hasId(m_themeShopSelectedId))
        m_themeShopSelectedId = activeId;
    if (!hasId(m_themeShopSelectedId))
        m_themeShopSelectedId = m_themeShopEntries.front().id;
}

void ThemeShopScreen::setPackageTransferState(const ThemeTransferState& state,
                                              std::string themeId,
                                              bool installMode) {
    m_packageTransferState = state;
    m_packageTransferThemeId = std::move(themeId);
    m_packageTransferInstallMode = installMode;

    if (m_packageTransferState.isRunning()) {
        clearCommunityPreviewCache();
        m_lastPreviewPrimeKey.clear();
    }
}

const ThemeShopScreen::ThemeShopEntry* ThemeShopScreen::selectedThemeShopEntry() const {
    if (m_themeShopEntries.empty())
        return nullptr;

    for (const auto& entry : m_themeShopEntries) {
        if (entry.id == m_themeShopSelectedId)
            return &entry;
    }

    return &m_themeShopEntries.front();
}

const ThemeCatalogClient::Entry* ThemeShopScreen::selectedCommunityThemeEntry() const {
    if (m_communityEntries.empty())
        return nullptr;

    for (const auto& entry : m_communityEntries) {
        if (entry.id == m_communitySelectedId)
            return &entry;
    }

    return &m_communityEntries.front();
}

const ThemeCatalogClient::Entry* ThemeShopScreen::findCommunityThemeEntry(const std::string& themeId) const {
    for (const auto& entry : m_allCommunityEntries) {
        if (entry.id == themeId)
            return &entry;
    }
    return nullptr;
}

int ThemeShopScreen::detailScreenshotCount() const {
    if (!isCommunityTab())
        return 0;

    const auto* entry = selectedCommunityThemeEntry();
    if (!entry)
        return 0;

    return (int)entry->screenshots.size();
}

std::string ThemeShopScreen::currentDetailCommunityPreviewPath() const {
    const auto* entry = selectedCommunityThemeEntry();
    if (!entry)
        return {};

    // A folha tambem em tela cheia, agora que a celula tem 256x144 e amplia 4.5x
    // em vez de 7x. Um tema animado apresentado por foto parada nao mostra o que
    // ele e, e essa era a tela onde mais se olha.
    //
    // Isto decide tanto o que se pede quanto o que se desenha: quando os dois
    // discordavam, o detalhe pedia a capa e tentava desenhar a folha, e a tela
    // dizia "captura indisponivel". E ampliar uma celula de 160x90 para a tela
    // inteira vira mosaico -- em tela cheia o que interessa e a imagem, nao o
    // movimento.
    // Em tela cheia, a folha grande. Ela tem celulas de 512x288 contra 256x144
    // da folha da grade, entao amplia 2.2x em vez de 4.5x -- e ali so um tema
    // esta na tela, o que torna os 11.8 MB dela pagaveis. Os quadros reais do
    // tema seriam 67 MB de download para uma previa.
    if (m_detailScreenshotIndex <= 0) {
        if (m_detailFullscreen && !entry->thumbSheetHd.empty())
            return entry->thumbSheetHd;
        if (!entry->thumbSheet.empty())
            return entry->thumbSheet;
    }

    if (!entry->screenshots.empty()) {
        int index = std::clamp(m_detailScreenshotIndex, 0, (int)entry->screenshots.size() - 1);
        return entry->screenshots[(size_t)index];
    }

    return entry->cover;
}

void ThemeShopScreen::clampDetailScreenshotIndex() {
    int count = detailScreenshotCount();
    if (count <= 0) {
        m_detailScreenshotIndex = 0;
        m_detailFullscreen = false;
        if (m_detailFocusArea == DetailFocusArea::Preview)
            m_detailFocusArea = DetailFocusArea::Buttons;
        return;
    }

    m_detailScreenshotIndex = std::clamp(m_detailScreenshotIndex, 0, count - 1);
}

void ThemeShopScreen::stepDetailScreenshot(int delta) {
    int count = detailScreenshotCount();
    if (count <= 0)
        return;

    int nextIndex = std::clamp(m_detailScreenshotIndex + delta, 0, count - 1);
    if (nextIndex == m_detailScreenshotIndex)
        return;

    m_detailScreenshotIndex = nextIndex;
    primeCommunityPreview(currentDetailCommunityPreviewPath());
}

void ThemeShopScreen::applySearchFilter() {
    const std::string needleLower = asciiLower(trimWhitespace(m_searchQuery));

    m_themeShopEntries.clear();
    m_themeShopEntries.reserve(m_allThemeShopEntries.size());
    for (const auto& entry : m_allThemeShopEntries) {
        if (matchesSearch(entry, needleLower))
            m_themeShopEntries.push_back(entry);
    }

    // One fetch feeds two tabs. Entries remember which index they came from,
    // so the split is a filter here rather than a second catalogue client and
    // a second download.
    const bool wantAnimated = isAnimatedTab();
    m_communityEntries.clear();
    m_communityEntries.reserve(m_allCommunityEntries.size());
    for (const auto& entry : m_allCommunityEntries) {
        const bool fromOurs =
            (entry.catalogUrl == ThemeCatalogClient::kDefaultCatalogUrl ||
             entry.catalogUrl == ThemeCatalogClient::kFallbackCatalogUrl);
        if (fromOurs != wantAnimated)
            continue;
        if (matchesSearch(entry, needleLower))
            m_communityEntries.push_back(entry);
    }

    auto syncSelection = [](const auto& entries, std::string& selectedId) {
        auto hasId = [&](const std::string& id) {
            return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.id == id;
            });
        };

        if (entries.empty()) {
            selectedId.clear();
            return;
        }

        if (!hasId(selectedId))
            selectedId = entries.front().id;
    };

    syncSelection(m_themeShopEntries, m_themeShopSelectedId);
    syncSelection(m_communityEntries, m_communitySelectedId);

    if (m_themeShopEntries.empty())
        m_installedScrollRow = 0;
    if (m_communityEntries.empty())
        m_communityScrollRow = 0;
}

bool ThemeShopScreen::promptSearchQuery() {
    auto& i18n = nxui::I18n::instance();

    if (m_requestTextEntryCb) {
        std::string title = isMusicTab()
            ? i18n.tr("themeshop.music.search_title", "Search YouTube Music")
            : i18n.tr("themeshop.search.title", "Search Themes");
        std::string guide = isMusicTab()
            ? i18n.tr("themeshop.music.search_guide", "Type a song or artist name...")
            : i18n.tr("themeshop.search.guide", "Search themes");
        std::string initial = isMusicTab() ? m_youtubeSearchQuery : m_searchQuery;

        m_requestTextEntryCb(title, guide, initial, [this](std::string query) {
            if (isMusicTab()) {
                searchYouTube(query);
            } else {
                m_searchQuery = query;
                applySearchFilter();
            }
        });
        return true;
    }

#ifdef SWITCHU_MENU
    // Same constraint as WiiUMenuApp::runSystemKeyboard: this menu is a library
    // applet and swkbdShow never returns here. Proven by menu.log on 2026-08-31,
    // where the folder keyboard logged its "show" line and never its "returned"
    // one while the console sat frozen. This path had not been exercised yet and
    // would have hung the same way.
    requestToast(i18n.tr("themeshop.search.unavailable",
                         "Search keyboard is unavailable."), 2.5f);
    return true;
#else
    SwkbdConfig swkbd;
    Result rc = swkbdCreate(&swkbd, 0);
    if (R_FAILED(rc)) {
        requestToast(i18n.tr("themeshop.search.unavailable", "Search keyboard is unavailable."), 2.5f);
        return true;
    }

    swkbdConfigMakePresetDefault(&swkbd);
    swkbdConfigSetType(&swkbd, SwkbdType_All);
    swkbdConfigSetStringLenMax(&swkbd, 64);

    const std::string guide = i18n.tr("themeshop.search.guide", "Search themes");
    swkbdConfigSetGuideText(&swkbd, guide.c_str());
    if (!m_searchQuery.empty())
        swkbdConfigSetInitialText(&swkbd, m_searchQuery.c_str());

    std::array<char, 65> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", m_searchQuery.c_str());

    appletUpdateCallerAppletCaptureImage();
    // Committed, not buffered: a keyboard that never returns leaves no other
    // trace of where the frame loop stopped. FileLog only exists in the menu
    // build; the standalone NRO writes through DebugLog to stderr anyway.
#ifdef SWITCHU_MENU
    switchu::FileLog::logCommit("[keyboard] themeshop-search show");
#else
    DebugLog::log("[keyboard] themeshop-search show");
#endif
    rc = swkbdShow(&swkbd, buffer.data(), buffer.size());
#ifdef SWITCHU_MENU
    switchu::FileLog::logCommit("[keyboard] themeshop-search returned rc=0x%08X",
                                static_cast<unsigned>(rc));
#else
    DebugLog::log("[keyboard] themeshop-search returned rc=0x%08X",
                  static_cast<unsigned>(rc));
#endif
    swkbdClose(&swkbd);

    if (R_FAILED(rc))
        return true;

    m_searchQuery = trimWhitespace(buffer.data());
    closeDetail();
    applySearchFilter();
    ensureSelectionVisible();
    return true;
#endif
}

bool ThemeShopScreen::pollCommunityCatalog() {
    if (!m_catalogClient.poll())
        return false;

    auto snapshot = m_catalogClient.snapshot();
    if (snapshot.revision == m_communityRevision)
        return false;

    syncCommunityCatalog(snapshot);
    return true;
}

std::string ThemeShopScreen::resolveCommunityPreviewUrl(const ThemeCatalogClient::Entry& entry) const {
    return resolveCommunityPreviewUrl(gridPreviewPathOf(entry));
}

std::string ThemeShopScreen::resolveCommunityPreviewUrl(const std::string& previewPath) const {
    if (previewPath.empty())
        return {};
    if (startsWith(previewPath, "https://") || startsWith(previewPath, "http://"))
        return previewPath;

    const std::string& catalogUrl = m_catalogClient.catalogUrl();
    std::size_t slash = catalogUrl.find_last_of('/');
    if (slash == std::string::npos)
        return previewPath;
    return catalogUrl.substr(0, slash + 1) + trimLeadingSlashes(previewPath);
}

std::shared_ptr<ThemeShopScreen::PreviewImageState> ThemeShopScreen::communityPreviewState(const std::string& previewPath) const {
    std::string url = resolveCommunityPreviewUrl(previewPath);
    if (url.empty())
        return nullptr;

    auto it = m_communityPreviewCache.find(url);
    if (it == m_communityPreviewCache.end())
        return nullptr;
    return it->second;
}

std::shared_ptr<ThemeShopScreen::PreviewImageState> ThemeShopScreen::communityPreviewState(const ThemeCatalogClient::Entry& entry) const {
    return communityPreviewState(gridPreviewPathOf(entry));
}

ThemeShopScreen::PreviewPhase ThemeShopScreen::communityPreviewPhase(const std::string& previewPath) const {
    auto state = communityPreviewState(previewPath);
    if (!state)
        return previewPath.empty() ? PreviewPhase::Failed : PreviewPhase::Idle;

    std::lock_guard<std::mutex> lk(state->mutex);
    return state->phase;
}

ThemeShopScreen::PreviewPhase ThemeShopScreen::communityPreviewPhase(const ThemeCatalogClient::Entry& entry) const {
    // Pela mesma imagem que se pede e se desenha. Consultando a fase da capa,
    // o cartao relatava o estado de um arquivo que ninguem baixou.
    return communityPreviewPhase(gridPreviewPathOf(entry));
}

const nxui::Texture* ThemeShopScreen::communityPreviewTexture(const std::string& previewPath) const {
    auto state = communityPreviewState(previewPath);
    if (!state)
        return nullptr;

    std::lock_guard<std::mutex> lk(state->mutex);
    if (state->phase != PreviewPhase::Ready || !state->texture.valid())
        return nullptr;
    return &state->texture;
}

const nxui::Texture* ThemeShopScreen::communityPreviewTexture(const ThemeCatalogClient::Entry& entry) const {
    return communityPreviewTexture(gridPreviewPathOf(entry));
}

ThemeShopScreen::PreviewPhase ThemeShopScreen::installedPreviewPhase(const std::string& previewPath) const {
    if (previewPath.empty())
        return PreviewPhase::Failed;

    auto it = m_installedPreviewCache.find(previewPath);
    if (it == m_installedPreviewCache.end() || !it->second)
        return PreviewPhase::Idle;

    std::lock_guard<std::mutex> lk(it->second->mutex);
    return it->second->phase;
}

const nxui::Texture* ThemeShopScreen::installedPreviewTexture(const std::string& previewPath) const {
    if (previewPath.empty())
        return nullptr;

    auto it = m_installedPreviewCache.find(previewPath);
    if (it == m_installedPreviewCache.end() || !it->second)
        return nullptr;

    std::lock_guard<std::mutex> lk(it->second->mutex);
    if (it->second->phase != PreviewPhase::Ready || !it->second->texture.valid())
        return nullptr;
    return &it->second->texture;
}

// Asked for from inside the frame, for every installed card the grid is about
// to draw. It used to answer by doing the whole job right there: read the cover
// off the card, decode it, and upload it, on the render thread, mid-draw.
//
// Measured on the console with the opening-frame trace, five installed covers
// (a 694 KB PNG, an 87 KB JPEG and three around 30 KB) cost 287.738 ms of
// content command-build on the first visible frame, against 0.31-1.35 ms on
// every frame after it. That one frame was the whole Theme Shop opening stall;
// nothing else in the frame came near a millisecond.
//
// So only the upload stays here. Reading and decoding go to the pool, exactly
// as community previews already do, and the card shows its "Loading
// screenshot..." placeholder for the frames in between -- which is the state
// the grid was already written to draw.
void ThemeShopScreen::primeInstalledPreview(const std::string& previewPath) {
    if (!m_gpu || !m_renderer || previewPath.empty())
        return;

    auto& slot = m_installedPreviewCache[previewPath];
    if (!slot)
        slot = std::make_shared<PreviewImageState>();

    const int maxSide = isPreviewSheetPath(previewPath)
        ? kPreviewSheetMaxSide : kCommunityPreviewMaxSide;

    {
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (slot->url != previewPath) {
            slot->url = previewPath;
            slot->phase = PreviewPhase::Idle;
            slot->failureCount = 0;
            slot->cancelled = false;
            slot->decoded = nxui::DecodedImage();
            slot->texture = nxui::Texture();
        }
        // Loading is a worker already on it and Downloaded is pixels waiting for
        // the next frame to upload them. Neither wants a second task.
        if (slot->phase != PreviewPhase::Idle)
            return;
        slot->cancelled = false;
        slot->phase = PreviewPhase::Loading;
    }

    // No pool, or a format the decoder does not handle: keep the original
    // synchronous path rather than leaving the card blank forever. decodeFile
    // is stb_image, and loadFromFile is the one that knows to route a DDS to
    // the compressed-block loader.
    if (!m_threadPool || isBlockCompressedPath(previewPath)) {
        nxui::Texture uploaded;
        bool ok = uploaded.loadFromFile(*m_gpu, *m_renderer, previewPath, maxSide);
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (ok) {
            slot->texture = std::move(uploaded);
            slot->phase = PreviewPhase::Ready;
            slot->failureCount = 0;
        } else {
            slot->phase = PreviewPhase::Failed;
            slot->failureCount += 1;
        }
        return;
    }

    // One decode at a time.
    //
    // Submitting all five covers at once measured worse than the synchronous
    // version it replaced: the opening frame fell from 287.738 ms to 110.553 ms
    // of content, but a 1,414.1 ms frame appeared right behind it. stbi_load
    // allocates the image at its full source size before the downscale, two
    // pool workers hold one of those each, and the decoded results then queued
    // up waiting to upload -- on a process already sitting at 429.5 MB of
    // 457.9 MB. The old path was slow but only ever held one of these buffers,
    // and freed it before reading the next file.
    //
    // Serialised, the transient cost is strictly lower than what the render
    // thread used to pay, and it is no longer paid in the frame.
    int decodesInFlight = 0;
    for (const auto& cached : m_installedPreviewCache) {
        if (!cached.second || cached.second == slot)
            continue;
        std::lock_guard<std::mutex> lk(cached.second->mutex);
        if (cached.second->phase == PreviewPhase::Loading)
            ++decodesInFlight;
    }
    if (decodesInFlight >= kMaxInstalledPreviewDecodesInFlight) {
        // Back to Idle so the next frame asks again. The grid calls this every
        // frame for every card it draws, so no queue of its own is needed.
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (slot->phase == PreviewPhase::Loading)
            slot->phase = PreviewPhase::Idle;
        return;
    }

    m_hasPendingInstalledPreviewWork = true;
    auto state = slot;
    slot->future = m_threadPool->submit([state, previewPath, maxSide]() {
        nxui::DecodedImage image = nxui::Texture::decodeFile(previewPath, maxSide);
        std::lock_guard<std::mutex> lk(state->mutex);
        // Dropped while it was decoding. The pixels go, and the phase is left
        // where the cancelling side put it.
        if (state->cancelled)
            return;
        if (!image.valid()) {
            state->phase = PreviewPhase::Failed;
            state->failureCount += 1;
            return;
        }
        state->decoded = std::move(image);
        state->phase = PreviewPhase::Downloaded;
    });
}

// The render-thread half. Uploading decoded pixels is microseconds, but it is
// still the one step that has to touch the GPU from this thread, so it is
// capped per frame the way the community uploads are.
void ThemeShopScreen::syncFinishedInstalledPreviewLoads() {
    if (!m_gpu || !m_renderer || !m_hasPendingInstalledPreviewWork)
        return;

    bool hasPendingWork = false;

    for (auto& entry : m_installedPreviewCache) {
        auto state = entry.second;
        if (!state)
            continue;

        if (state->future.valid()) {
            if (state->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    state->future.get();
                } catch (const std::exception& ex) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->phase = PreviewPhase::Failed;
                    state->failureCount += 1;
                    state->decoded = nxui::DecodedImage();
                    DebugLog::log("[themeshop] installed preview task failed: %s", ex.what());
                } catch (...) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->phase = PreviewPhase::Failed;
                    state->failureCount += 1;
                    state->decoded = nxui::DecodedImage();
                    DebugLog::log("[themeshop] installed preview task failed: unknown error");
                }
            } else {
                hasPendingWork = true;
                continue;
            }
        }

        nxui::DecodedImage image;
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            if (state->phase != PreviewPhase::Downloaded)
                continue;
            // Moved, not copied. Unlike a community preview, whose compressed
            // bytes are worth keeping so it can come back without the network,
            // this one is already on the card: re-reading it is a file open.
            image = std::move(state->decoded);
            state->decoded = nxui::DecodedImage();
        }

        // Uploaded the same frame it is ready, with no per-frame cap. The cap
        // the community uploads use is there to spread network arrivals; here
        // only one decode exists at a time, and holding its pixels for a later
        // frame is exactly the retained megabytes that made the first opening
        // stall. The upload itself is microseconds.
        nxui::Texture uploaded;
        bool ok = uploaded.loadFromDecoded(*m_gpu, *m_renderer, image);
        std::lock_guard<std::mutex> lk(state->mutex);
        if (ok) {
            state->texture = std::move(uploaded);
            state->phase = PreviewPhase::Ready;
            state->failureCount = 0;
        } else {
            state->phase = PreviewPhase::Failed;
            state->failureCount += 1;
            DebugLog::log("[themeshop] previa instalada nao subiu (%dx%d): imagem em %.1f de %.1f MB",
                          image.width, image.height,
                          m_gpu->imageMemoryUsed() / 1048576.0,
                          nxui::GpuDevice::imageBudget() / 1048576.0);
        }
    }

    m_hasPendingInstalledPreviewWork = hasPendingWork;
}

void ThemeShopScreen::clearInstalledPreviewCache() {
    if (m_installedPreviewCache.empty())
        return;

    if (m_gpu)
        m_gpu->waitIdle();

    // A worker may still hold this state through its shared_ptr. Retiring the
    // texture here, on the thread that owns the GPU, is what keeps a decode
    // finishing late from destroying an image behind the render thread's back.
    for (auto& entry : m_installedPreviewCache) {
        auto& state = entry.second;
        if (!state)
            continue;

        std::lock_guard<std::mutex> lk(state->mutex);
        state->cancelled = true;
        state->decoded = nxui::DecodedImage();
        state->texture = nxui::Texture();
    }

    m_installedPreviewCache.clear();
    m_hasPendingInstalledPreviewWork = false;
}

// Publishing the installed list used to drop every cached preview, and the list
// is republished each time the sidebar opens the Theme Shop. That made the
// decode above unavoidable on every single opening, not only the first one.
// Keep what the new list still refers to; only what left the list goes.
void ThemeShopScreen::pruneInstalledPreviewCache(const std::vector<ThemeShopEntry>& entries) {
    if (m_installedPreviewCache.empty())
        return;

    std::unordered_set<std::string> retained;
    for (const auto& entry : entries) {
        if (!entry.coverPath.empty())
            retained.insert(entry.coverPath);
    }

    bool willErase = false;
    for (const auto& entry : m_installedPreviewCache) {
        if (retained.find(entry.first) == retained.end()) {
            willErase = true;
            break;
        }
    }
    if (!willErase)
        return;

    if (m_gpu)
        m_gpu->waitIdle();

    for (auto it = m_installedPreviewCache.begin(); it != m_installedPreviewCache.end();) {
        if (retained.find(it->first) != retained.end()) {
            ++it;
            continue;
        }
        if (it->second) {
            std::lock_guard<std::mutex> lk(it->second->mutex);
            it->second->cancelled = true;
            it->second->decoded = nxui::DecodedImage();
            it->second->texture = nxui::Texture();
        }
        it = m_installedPreviewCache.erase(it);
    }
}

void ThemeShopScreen::primeCommunityPreview(const std::string& previewPath) {
    if (!m_threadPool)
        return;

    constexpr int kMaxPreviewFailureCount = 2;

    std::string url = resolveCommunityPreviewUrl(previewPath);
    if (url.empty())
        return;

    auto& slot = m_communityPreviewCache[url];
    if (!slot)
        slot = std::make_shared<PreviewImageState>();

    bool canStart = false;
    {
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (slot->url != url) {
            slot->phase = PreviewPhase::Idle;
            slot->failureCount = 0;
            slot->cancelled = false;
            slot->url = url;
            slot->bytes.clear();
            slot->future = std::future<void>();
            slot->texture = nxui::Texture();
        } else {
            slot->cancelled = false;
        }

        if (slot->phase == PreviewPhase::Idle
            || (slot->phase == PreviewPhase::Failed && slot->failureCount < kMaxPreviewFailureCount)) {
            canStart = true;
        }
    }

    if (!canStart)
        return;

    int inFlightDownloads = 0;
    for (const auto& entry : m_communityPreviewCache) {
        const auto& state = entry.second;
        if (!state)
            continue;

        // Loading only. Downloaded used to be counted too, which was harmless
        // while it meant "waiting to upload, briefly" -- but it now also means
        // "bytes kept from a page you left", and those held every download slot
        // so nothing on the next page could start. That reads as a broken
        // preview, not as a busy one.
        std::lock_guard<std::mutex> lk(state->mutex);
        if (state->phase == PreviewPhase::Loading)
            ++inFlightDownloads;
    }

    if (inFlightDownloads >= kMaxPreviewDownloadsInFlight)
        return;

    bool shouldStart = false;
    {
        std::lock_guard<std::mutex> lk(slot->mutex);
        if (slot->phase == PreviewPhase::Idle
            || (slot->phase == PreviewPhase::Failed && slot->failureCount < kMaxPreviewFailureCount)) {
            slot->phase = PreviewPhase::Loading;
            shouldStart = true;
        }
    }

    if (!shouldStart)
        return;

    m_hasPendingCommunityPreviewWork = true;
    slot->future = m_threadPool->submit([slot, url]() {
        try {
            {
                std::lock_guard<std::mutex> lk(slot->mutex);
                if (slot->cancelled)
                    return;
            }

            std::vector<std::uint8_t> bytes = themeshop::http::getBytes(url);
            std::lock_guard<std::mutex> lk(slot->mutex);
            if (slot->cancelled) {
                slot->bytes.clear();
                return;
            }
            slot->bytes = std::move(bytes);
            if (slot->bytes.empty()) {
                slot->failureCount += 1;
                slot->phase = PreviewPhase::Failed;
            } else {
                slot->failureCount = 0;
                slot->phase = PreviewPhase::Downloaded;
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(slot->mutex);
            if (slot->cancelled)
                return;
            slot->bytes.clear();
            slot->failureCount += 1;
            slot->phase = PreviewPhase::Failed;
            DebugLog::log("[themeshop] preview fetch failed: %s", ex.what());
        } catch (...) {
            std::lock_guard<std::mutex> lk(slot->mutex);
            if (slot->cancelled)
                return;
            slot->bytes.clear();
            slot->failureCount += 1;
            slot->phase = PreviewPhase::Failed;
            DebugLog::log("[themeshop] preview fetch failed: unknown error");
        }
    });
}

void ThemeShopScreen::primeCommunityPreview(const ThemeCatalogClient::Entry& entry) {
    primeCommunityPreview(gridPreviewPathOf(entry));
}

void ThemeShopScreen::primeVisibleCommunityPreviews() {
    if (!isCommunityTab() || m_communityEntries.empty())
        return;

    std::unordered_set<std::string> queuedUrls;
    auto queuePreview = [&](const std::string& previewPath) {
        std::string url = resolveCommunityPreviewUrl(previewPath);
        if (url.empty() || !queuedUrls.emplace(url).second)
            return;
        primeCommunityPreview(previewPath);
    };

    const auto* selected = selectedCommunityThemeEntry();
    if (selected) {
        if (m_detailOpen) {
            std::string current = currentDetailCommunityPreviewPath();
            if (!current.empty())
                queuePreview(current);
        }

        const std::string selectedPath = gridPreviewPathOf(*selected);
        if (!selectedPath.empty())
            queuePreview(selectedPath);
    }

    int scrollRow = m_communityScrollRow;
    int start = scrollRow * kCommunityPreviewGridCols;
    int end = std::min((int)m_communityEntries.size(), start + kCommunityPreviewEntriesPerPage);
    for (int i = start; i < end; ++i)
        // Esta e a funcao que de fato baixa para a pagina visivel, e pedia a
        // capa. Todo o resto ja usava a folha -- desenho, detalhe, poda,
        // sincronizacao -- e nada disso adiantava com o download apontando para
        // outro arquivo. A grade mostrava "indisponivel" ate alguem abrir um
        // tema, porque so o caminho do detalhe pedia a folha.
        queuePreview(gridPreviewPathOf(m_communityEntries[(size_t)i]));
}

void ThemeShopScreen::syncFinishedCommunityPreviewLoads() {
    if (!m_gpu || !m_renderer || !m_hasPendingCommunityPreviewWork)
        return;

    bool hasPendingWork = false;
    int uploadsThisFrame = 0;
    // So sobe para a GPU o que esta em uso agora. Antes este laco subia
    // qualquer entrada do cache que tivesse bytes, e o cache guarda ate 64
    // delas: a poda liberava a textura de uma previa fora da tela, marcava a
    // entrada como Downloaded para reaproveitar os bytes, e este passo a subia
    // de novo no quadro seguinte. Liberar e re-subir se alimentavam.
    //
    // Medido no console, percorrendo as abas parado: a memoria de imagem subia
    // 12.7 MB a cada 2 segundos ate 108 MB, e o menu morria. Nao era vazamento
    // -- a contabilidade devolvia certo -- era o cache inteiro querendo estar
    // na GPU ao mesmo tempo.
    const std::unordered_set<std::string> wanted = wantedCommunityPreviewUrls();
    for (auto& entry : m_communityPreviewCache) {
        auto state = entry.second;
        if (!state)
            continue;

        if (state->future.valid()) {
            if (state->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    state->future.get();
                } catch (const std::exception& ex) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->phase = PreviewPhase::Failed;
                    state->failureCount += 1;
                    state->bytes.clear();
                    DebugLog::log("[themeshop] preview task failed: %s", ex.what());
                } catch (...) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->phase = PreviewPhase::Failed;
                    state->failureCount += 1;
                    state->bytes.clear();
                    DebugLog::log("[themeshop] preview task failed: unknown error");
                }
            } else {
                hasPendingWork = true;
            }
        }

        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            if (state->phase == PreviewPhase::Downloaded) {
                if (wanted.find(entry.first) == wanted.end()) {
                    // Fora de uso: os bytes ficam, a textura nao nasce. Quando
                    // voltar a ser preciso, sobe sem ir a rede.
                } else if (uploadsThisFrame >= kMaxPreviewUploadsPerFrame) {
                    hasPendingWork = true;
                } else {
                    // Copied, not moved. The compressed bytes are what lets a
                    // preview come back without going to the network again;
                    // they are tens of kilobytes against the megabyte the
                    // decoded texture occupies, so they are the half worth
                    // keeping when the texture has to go.
                    bytes = state->bytes;
                }
            }
        }

        if (bytes.empty())
            continue;

        ++uploadsThisFrame;
        nxui::Texture uploaded;
        bool ok = uploaded.loadFromMemory(*m_gpu, *m_renderer, bytes.data(), bytes.size(),
                                          isPreviewSheetPath(state->url)
                                              ? kPreviewSheetMaxSide : kCommunityPreviewMaxSide);
        std::lock_guard<std::mutex> lk(state->mutex);
        if (ok) {
            state->texture = std::move(uploaded);
            state->failureCount = 0;
            state->phase = PreviewPhase::Ready;
        } else {
            state->failureCount += 1;
            state->phase = PreviewPhase::Failed;
            // Sem esta linha a previa some sem deixar rastro: os bytes chegaram,
            // a decodificacao passou, e o que falhou foi a alocacao de imagem --
            // que so escreve no stderr, onde nada le. O relato vira "captura
            // indisponivel" e o log nao tem uma palavra sobre o assunto.
            //
            // Com o fundo animado como padrao, 71 dos 112 MB de imagem ja estao
            // gastos antes de a loja abrir, entao os numeros vao junto.
            DebugLog::log("[themeshop] previa nao subiu (%zu KB): imagem em %.1f de %.1f MB",
                          bytes.size() / 1024,
                          m_gpu->imageMemoryUsed() / 1048576.0,
                          nxui::GpuDevice::imageBudget() / 1048576.0);
        }
    }

    m_hasPendingCommunityPreviewWork = hasPendingWork;
}

void ThemeShopScreen::clearCommunityPreviewCache() {
    if (m_communityPreviewCache.empty())
        return;

    if (m_gpu)
        m_gpu->waitIdle();

    for (auto& entry : m_communityPreviewCache) {
        auto& state = entry.second;
        if (!state)
            continue;

        std::lock_guard<std::mutex> lk(state->mutex);
        state->cancelled = true;
        state->bytes.clear();
        state->texture = nxui::Texture();
    }

    m_communityPreviewCache.clear();
    m_hasPendingCommunityPreviewWork = false;
}

// O que esta em uso neste instante: a pagina visivel, o selecionado e, com o
// detalhe aberto, a imagem que ele mostra. Uma regra so, porque ter duas foi o
// que produziu a maioria dos defeitos desta tela -- pedir por um caminho e
// reter por outro apaga exatamente o que acabou de chegar.
std::unordered_set<std::string> ThemeShopScreen::wantedCommunityPreviewUrls() const {
    std::unordered_set<std::string> urls;
    auto want = [&](const std::string& previewPath) {
        std::string url = resolveCommunityPreviewUrl(previewPath);
        if (!url.empty())
            urls.insert(std::move(url));
    };

    int start = m_communityScrollRow * kCommunityPreviewGridCols;
    int end = std::min((int)m_communityEntries.size(), start + kCommunityPreviewEntriesPerPage);
    for (int i = start; i < end; ++i)
        want(gridPreviewPathOf(m_communityEntries[(size_t)i]));

    if (const auto* selected = selectedCommunityThemeEntry()) {
        want(gridPreviewPathOf(*selected));
        if (m_detailOpen)
            want(currentDetailCommunityPreviewPath());
    }
    return urls;
}

void ThemeShopScreen::trimCommunityPreviewCache() {
    if (m_communityPreviewCache.empty())
        return;

    if (!isCommunityTab()) {
        clearCommunityPreviewCache();
        return;
    }

    std::unordered_set<std::string> retainedUrls = wantedCommunityPreviewUrls();


    bool willErase = false;
    for (auto it = m_communityPreviewCache.begin(); it != m_communityPreviewCache.end();) {
        if (retainedUrls.find(it->first) == retainedUrls.end()) {
            willErase = true;
            break;
        }
        ++it;
    }

    if (willErase && m_gpu)
        m_gpu->waitIdle();

    // Leaving a page used to erase the entry outright, which threw away the
    // downloaded bytes along with the texture -- so paging back fetched the
    // same image over the network a second time. Now the texture goes and the
    // bytes stay: the picture comes straight back, and the GPU memory it was
    // holding is still released, which is what the eviction was for.
    //
    // Bounded so a large catalogue does not accumulate indefinitely. Past the
    // cap the old behaviour applies and the entry is dropped for real.
    int erased = 0;
    int retainedBytes = 0;
    for (auto it = m_communityPreviewCache.begin(); it != m_communityPreviewCache.end();) {
        if (retainedUrls.find(it->first) != retainedUrls.end()) {
            ++it;
            continue;
        }

        bool keepBytes = false;
        if (it->second) {
            std::lock_guard<std::mutex> lk(it->second->mutex);
            const bool hasBytes = !it->second->bytes.empty();
            const bool settled = it->second->phase == PreviewPhase::Ready
                              || it->second->phase == PreviewPhase::Downloaded;
            keepBytes = hasBytes && settled && retainedBytes < kMaxCachedPreviewBytesEntries;
            if (keepBytes) {
                // Back to Downloaded: the upload step picks it up again when
                // the preview is next needed, without a request.
                //
                // And it has to be told there is work, because that step is
                // gated on a flag that only the download path used to raise.
                // Without this the revived entry sits at Downloaded forever and
                // the preview reads as loading and never arrives -- which is
                // exactly what this cache change first shipped as.
                it->second->texture = nxui::Texture();
                it->second->phase = PreviewPhase::Downloaded;
                m_hasPendingCommunityPreviewWork = true;
                ++retainedBytes;
            } else {
                it->second->cancelled = true;
                it->second->bytes.clear();
                it->second->texture = nxui::Texture();
            }
        }

        if (keepBytes) {
            ++it;
        } else {
            it = m_communityPreviewCache.erase(it);
            ++erased;
        }
    }

    if (erased > 0) {
        DebugLog::log("[themeshop] preview cache trimmed: erased=%d retained=%zu",
                      erased,
                      m_communityPreviewCache.size());
    }
}

void ThemeShopScreen::syncCommunityCatalog(const ThemeCatalogClient::Snapshot& snapshot) {
    m_communityTransferState = snapshot.loading;
    m_allCommunityEntries = snapshot.entries;
    m_communityRevision = snapshot.revision;

    std::unordered_set<std::string> validUrls;
    for (const auto& entry : m_allCommunityEntries) {
        // A folha entra na lista de validas. Sem isso, cada sincronizacao do
        // catalogo apagava do cache exatamente a imagem que a grade usa -- e o
        // catalogo sincroniza varias vezes seguidas, entao a previa era apagada
        // antes de terminar de chegar e a tela ja abria com "indisponivel".
        //
        // Quarto lugar onde pedido e retencao discordavam: desenho, detalhe,
        // poda por pagina e agora a sincronizacao. A regra de qual imagem
        // representa a entrada vive em gridPreviewPathOf; todos passam por la.
        std::string sheetUrl = resolveCommunityPreviewUrl(gridPreviewPathOf(entry));
        if (!sheetUrl.empty())
            validUrls.insert(std::move(sheetUrl));

        // A folha grande tambem: ela e usada so em tela cheia, e sem constar
        // aqui a sincronizacao do catalogo a apagaria do cache assim que
        // chegasse.
        std::string hdUrl = resolveCommunityPreviewUrl(entry.thumbSheetHd);
        if (!hdUrl.empty())
            validUrls.insert(std::move(hdUrl));

        std::string coverUrl = resolveCommunityPreviewUrl(entry.cover);
        if (!coverUrl.empty())
            validUrls.insert(std::move(coverUrl));
        for (const auto& screenshot : entry.screenshots) {
            std::string screenshotUrl = resolveCommunityPreviewUrl(screenshot);
            if (!screenshotUrl.empty())
                validUrls.insert(std::move(screenshotUrl));
        }
    }
    for (auto it = m_communityPreviewCache.begin(); it != m_communityPreviewCache.end();) {
        if (validUrls.find(it->first) == validUrls.end())
            it = m_communityPreviewCache.erase(it);
        else
            ++it;
    }

    applySearchFilter();

    auto hasId = [&](const std::string& id) {
        return std::any_of(m_communityEntries.begin(), m_communityEntries.end(),
                           [&](const ThemeCatalogClient::Entry& entry) {
                               return entry.id == id;
                           });
    };

    if (m_communityEntries.empty()) {
        m_communitySelectedId.clear();
        return;
    }

    if (!hasId(m_communitySelectedId))
        m_communitySelectedId = m_communityEntries.front().id;
}

void ThemeShopScreen::pollMusicDownloads() {
    m_youTubeClient.updateThumbnailTextures();
}

void ThemeShopScreen::searchYouTube(const std::string& query) {
    m_youtubeSearchQuery = query;
    m_musicScrollRow = 0;
    m_musicSelectedIndex = 0;
    m_youTubeClient.search(query, [this](bool ok, const std::string& err) {
        if (!ok) {
            DebugLog::log("[themeshop] YouTube search error: %s", err.c_str());
        }
    });
}

void ThemeShopScreen::resetMusicTabState() {
    m_youtubeSearchQuery.clear();
    m_musicScrollRow = 0;
    m_musicSelectedIndex = 0;
    m_youTubeClient.loadInstalledTracks();
}

const YouTubeClient::TrackItem* ThemeShopScreen::selectedMusicTrack() const {
    if (!isMusicTab())
        return nullptr;
    int idx = currentSelectedIndex();
    if (idx < 0)
        return nullptr;
    return m_youTubeClient.trackAt(static_cast<size_t>(idx));
}
