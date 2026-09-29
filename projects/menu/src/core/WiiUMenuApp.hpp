#pragma once
#include <nxui/Activity.hpp>
#include <nxui/Application.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/Theme.hpp>
#include "widgets/IconGrid.hpp"
#include "core/GridModel.hpp"
#include "widgets/SelectionCursor.hpp"
#include "widgets/WaraWaraBackground.hpp"
#include "widgets/GameArtworkBackdrop.hpp"
#include "widgets/DateTimeWidget.hpp"
#include "widgets/BatteryWidget.hpp"
#include "widgets/TitlePillWidget.hpp"
#include "core/AudioManager.hpp"
#include "core/AccessibilityManager.hpp"
#include "widgets/LaunchAnimation.hpp"
#include "widgets/OverlayDialog.hpp"
#include "widgets/QuickSettingsOverlay.hpp"
#include "widgets/ContextMenu.hpp"
#include "widgets/ProgressDialog.hpp"
#include "widgets/LockScreen.hpp"
#include "widgets/AppletButton.hpp"
#include "widgets/PageIndicator.hpp"
#include "widgets/UserAvatarButton.hpp"
#include "widgets/FolderBackdrop.hpp"
#include "widgets/SteamGridDbBackdrop.hpp"
#include "steamgriddb/SteamGridDbManager.hpp"
#include "steamgriddb/ArtworkCache.hpp"
#include "settings/SettingsScreen.hpp"
#include "settings/GameOptionsScreen.hpp"
#include "settings/SteamGridDbPickerScreen.hpp"
#include "settings/PlatformPickerScreen.hpp"
#include "settings/FolderOptionsScreen.hpp"
#include "settings/ControllerTestScreen.hpp"
#include "settings/TextEntryScreen.hpp"
#include "themeshop/ThemeShopScreen.hpp"
#include "gallery/GameGalleryScreen.hpp"
#include "details/GameDetailsScreen.hpp"
#include "update/UpdateClient.hpp"
#include "mods/GameModsScreen.hpp"
#include "cheats/GameCheatsScreen.hpp"
#include "activity/ActivityLogScreen.hpp"
#include "activity/ActivityLogManager.hpp"
#include "warawara/MiiAvatarManager.hpp"
#include "warawara/PlazaDialogueEngine.hpp"
#include "warawara/AnimalesePlayer.hpp"
#include "warawara/WaraWaraPlazaScreen.hpp"
#include "warawara/PlazaScreenSwapButton.hpp"
#include "widgets/MediaCenterButton.hpp"
#include "media/MediaCenterScreen.hpp"
#include "gallery/GameArtworkStore.hpp"
#include "core/Config.hpp"
#include "core/FolderStore.hpp"
#include "core/WidgetStore.hpp"
#include "core/ThemePreset.hpp"
#include "sidebar/SidebarManager.hpp"
#include "launcher/AppletLauncher.hpp"
#include "launcher/AppListLoader.hpp"
#include "launcher/IconStreamer.hpp"
#include "core/SystemMessages.hpp"
#include "navigation/MenuNavigator.hpp"
#include "services/ClockService.hpp"
#ifdef SWITCHU_DEBUG_UI
#include "debug/DebugImGuiOverlay.hpp"
#endif
#include <nxui/widgets/Background.hpp>
#include <nxui/widgets/Box.hpp>
#include <nxui/widgets/GlassPanel.hpp>
#include <nxui/widgets/Label.hpp>
#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <future>
#include <utility>
#include <unordered_map>
#include <switch.h>
#ifdef SWITCHU_MENU
#include <switchu/smi_protocol.hpp>
#endif


#ifdef SWITCHU_HOMEBREW
static constexpr const char* SD_ASSETS = "romfs:";
#else
static constexpr const char* SD_ASSETS = "sdmc:/switch/SwitchU";
#endif

class WiiUMenuApp : public nxui::Activity {
public:
    WiiUMenuApp();
    ~WiiUMenuApp();

    void setTutorialStartupFade(bool enabled);
    void setStartupConfig(const AppConfig& config);

#ifdef SWITCHU_MENU
    void setStartupStatus(const switchu::smi::SystemStatus& status);
    void setMenuMainTrace(uint64_t tick, uint32_t core);
#endif

    bool onCreate() override;
    void onDestroy() override;
    void onUpdate(float dt) override;
    void onRender(nxui::Renderer& ren) override;

    nxui::Widget* focusRoot() override;

private:
    struct GridLayoutMetrics {
        float cellW = 150.f;
        float cellH = 150.f;
        float padX = 20.f;
        float padY = 16.f;
    };

    void loadResources();
    void loadStaticTextures();
    void setupLockScreen();
    GridLayoutMetrics computeGridLayoutMetrics() const;
    GridLayoutMetrics computeGridLayoutMetrics(int columns, int rows) const;
    std::pair<int, int> folderGridDimensions(std::uint32_t folderId) const;
    void reflowHomeGrid();
    void buildGrid();
    void applyGlassSharpness(float sharpness);
    // The + menu on a focused icon: what the stock home menu offers, minus
    // the entries that would need a daemon round trip to answer.
    // Cycles hand-made order, A-Z, most recently opened, most played. The hint
    // bar shows which one is in force rather than just naming the button.
    void cycleSortMode();
    std::string sortModeLabel() const;
    // Play time for sort mode 4 and its badge. pdm is asked on the thread pool,
    // in one batch; the sort comparator and the icons only ever read the cache
    // in m_config.playtime, never pdm itself.
    void requestPlaytimeRefresh(const char* reason);
    void pollPlaytimeRefresh();
    std::string playtimeBadgeFor(const AppEntry& entry) const;
    void applyPlaytimeBadges();
    void showIconOptions();
    // Homebrew, forwarders and ports: the dossier has nothing to show for them,
    // so + offers the single action that applies.
    void showNonGameOptions(std::uint64_t titleId, const std::string& title);
    void showGameOptionsMenu(std::uint64_t titleId, const std::string& title);
    void showGameCustomizeMenu(std::uint64_t titleId, const std::string& title);
    void showGameArtworkStatus(std::uint64_t titleId, const std::string& title);
    void showGameArtworkRestoreMenu(std::uint64_t titleId, const std::string& title);
    void restoreGameArtworkFromOptions(std::uint64_t titleId, const std::string& title,
                                       gallery::ArtworkKind kind);
    void showSoftwareInformation(std::uint64_t titleId, const std::string& title);
    void showGameGallery(std::uint64_t titleId, const std::string& title);
    void showGameDetails(std::uint64_t titleId, const std::string& title,
                         nxui::Texture* liveCover = nullptr);
    void showGameMods(std::uint64_t titleId, const std::string& title);
    void showGameCheats(std::uint64_t titleId, const std::string& title);
    void confirmDeleteGameMod();
    void confirmGameArtwork(std::uint64_t titleId, const std::string& title,
                            GameGalleryClient::Category category,
                            GameGalleryClient::Asset asset,
                            std::shared_ptr<const std::vector<std::uint8_t>> bytes);
    void startGameArtworkSave(std::uint64_t titleId, GameGalleryClient::Category category,
                              GameGalleryClient::Asset asset,
                              std::shared_ptr<const std::vector<std::uint8_t>> bytes);
    void confirmRestoreGameArtwork(std::uint64_t titleId, const std::string& title,
                                   GameGalleryClient::Category category);
    void startGameArtworkRestore(std::uint64_t titleId, GameGalleryClient::Category category);
    void syncGameArtworkSave();
    void refreshGameArtworkBackdrop(std::uint64_t titleId);
    void confirmDeleteSoftware(std::uint64_t titleId, const std::string& title);
    void buildUserAvatarBar(bool loadImmediately = true);
    void loadNextUserAvatar();
    void appendAddUserButton();
    void wireUserAvatarNavigation();
    void composeRootPending(std::vector<PendingApp>& apps);
    GridModel buildRootFolderModel();
    GridModel buildOpenFolderModel(std::uint32_t folderId) const;
    void applyDisplayModel(GridModel model, std::uint64_t focusId, bool animate);
    void updateGridXmbContext();
    // The streamer thinks in pages. Each view has to say which "page" it is on
    // and how many icons that page holds, and the three models answer
    // differently: the paged grid reports its real page, the carousel reports
    // the focused index with one icon per page, and XMB reports the focused
    // index of its games column. Deriving that in one place is what keeps a
    // view from silently reporting the paged-grid geometry -- which is what
    // blanked the bar's icons after returning from a game.
    struct StreamPump {
        int page = 0;
        int perPage = 1;
    };
    StreamPump gridStreamPump() const;
    void pumpIconStreamer();
    void syncPageIndicator();
    void flipPageFromEdge(int dir);
    void requestOpenFolder(std::uint32_t folderId, std::uint64_t focusTitleId = 0);
    void openCapturedFolder();
    void closeFolder(bool preserveEditMode = false);
    void showFolderAssignment(std::uint64_t titleId, const std::string& title);
    void assignTitleToFolder(std::uint32_t folderId, std::uint64_t titleId);
    void removeTitleFromFolder(std::uint64_t titleId);
    void createFolder(int targetSlot = -1);
    void finishCreateFolder(int targetSlot, const std::string& typed);
    void showAddContextMenu(int targetSlot, const nxui::Rect& anchor);
    void showWidgetTypeMenu(int targetSlot, const nxui::Rect& anchor);
    void showWidgetSizeMenu(int targetSlot, const nxui::Rect& anchor,
                            switchu::widgets::WidgetType type);
    void showWidgetAssetMenu(int targetSlot, const nxui::Rect& anchor,
                             switchu::widgets::WidgetType type,
                             switchu::widgets::WidgetSize size);
    void showWidgetOptionsMenu(std::uint32_t widgetId, int slot,
                               const nxui::Rect& anchor);
    void createWidget(int targetSlot, switchu::widgets::WidgetType type,
                      switchu::widgets::WidgetSize size,
                      const std::string& assetRef = {});
    bool saveWidgetsOrReport(const char* operation);
    bool canPlaceWidget(int targetSlot, switchu::widgets::WidgetSize size,
                        std::uint32_t ignoringWidgetId = 0) const;
    void normalizeWidgetPlacements();
    // Cells a multi-cell tile spans hold 0 in the layout, exactly like free
    // space. These two tell them apart so nothing is dropped into a tile's
    // own footprint; see the comment on the definitions.
    std::vector<bool> layoutSpanCoverage(
        const std::vector<std::uint64_t>& slots) const;
    void claimFreeLayoutSlot(std::vector<std::uint64_t>& slots,
                             std::vector<bool>& covered,
                             std::uint64_t titleId) const;
    std::vector<std::pair<std::string, std::string>> listWidgetAssets(bool screenshotsOnly) const;
    std::string resolveWidgetAssetRef(const std::string& assetRef) const;
    void commitLaunchRecency(std::uint64_t titleId, const std::string& title);
    // Folder tiles show the icons of the titles inside them. The textures are
    // owned here, one set per folder, keyed by the member list so a membership
    // change is noticed; the icons only borrow raw pointers to them.
    struct FolderPreviewDecode {
        std::uint32_t folderId = 0;
        std::string signature;
        std::vector<IconStreamer::DecodedIcon> icons;
        std::atomic<bool> cancelled{false};
    };
    struct FolderPreviewAssets {
        std::string signature;
        std::vector<std::unique_ptr<nxui::Texture>> textures;
    };
    void syncFolderPreviews();
    std::string folderPreviewSignature(const switchu::folders::Folder& folder) const;
    std::unordered_map<std::uint32_t, FolderPreviewAssets> m_folderPreviews;
    std::shared_ptr<FolderPreviewDecode> m_folderPreviewDecode;
    std::future<void> m_folderPreviewFuture;
    std::size_t m_folderPreviewUploadStage = 0;
    // std::future is consumed by get(), so readiness cannot be re-tested
    // through it on the frames that follow.
    bool m_folderPreviewDecoded = false;
    void syncWidgetIconContent();
    void syncWidgetPageAssets();
    std::string randomScreenshotPath(std::uint32_t widgetId) const;
    std::string widgetTypeLabel(switchu::widgets::WidgetType type) const;
    std::string widgetDurationLabel(std::uint64_t seconds) const;
    void refreshRecentActivityDuration();
    void ensureRecentWidgetAssets(std::uint64_t titleId);
    void pollRecentWidgetAssets();
    void syncRecentWidgetTextures();
    void ensureGameArtwork(std::uint64_t titleId);
    void startNextGameArtworkDecode();
    void pollGameArtworkAssets();
    void syncGameArtworkTextures(std::uint64_t titleId);
    switchu::widgets::WidgetSize gameGridSize(std::uint64_t titleId,
                                               AppLayoutMode mode) const;
    bool canPlaceGridItem(int targetSlot, switchu::widgets::WidgetSize size,
                          std::uint64_t ignoringTitleId = 0,
                          std::uint64_t alsoIgnoringTitleId = 0) const;
#ifdef SWITCHU_MENU
    void activateApplication(GlossyIcon* source, AppEntry* entry,
                             std::uint64_t titleId,
                             const std::string& launchTitle);
#endif
    void renameFolder(std::uint32_t folderId);
    void showFolderContextMenu(std::uint32_t folderId);
    void showGamePortPlatformMenu(std::uint64_t titleId, const std::string& title);
    void removeGamePort(std::uint64_t titleId);
    bool saveFoldersOrReport(const char* operation);
    // Text entry is asynchronous: the on-screen keyboard runs for as many frames
    // as the player needs, so callers hand over what to do with the result
    // instead of waiting for a return value.
    void requestTextEntry(const std::string& title, const std::string& guide,
                          const std::string& initial, int maxLength, bool password,
                          std::function<void(const std::string&)> onAccept,
                          std::function<void()> onCancel = nullptr);
    void createTextEntry();
    std::string defaultFolderName() const;
    void editSteamGridDbApiKey();
    void startSteamGridDbScrape();
    void openSteamGridDbPicker(GameOptionsScreen::ArtworkKind kind,
                                const std::string& query = std::string());
    void openImagePinSteamGridDbPicker(int targetSlot, const nxui::Rect& anchor,
                                       switchu::widgets::WidgetSize size,
                                       const std::string& query = std::string());
    void editSteamGridDbPickerQuery();
    void applySteamGridDbCandidate(const SteamGridDbManager::BrowseResult& browse,
                                    const SteamGridDbManager::Candidate& candidate);
    void applyImagePinSteamGridDbCandidate(const SteamGridDbManager::BrowseResult& browse,
                                           const SteamGridDbManager::Candidate& candidate);
    void syncSteamGridDb();
    void showFocusedSteamGridDbArtwork(bool forceReload = false);
    void applyTheme();
    void applyThemeResources(const ThemePreset& preset);
    void retryPendingBackgroundImage();
    void applyThemeMusic(const std::vector<std::string>& tracks);
    // Faixas que o tema atual traz. O preset de som carrega em outra thread e
    // chegava depois, sobrescrevendo o que o tema tinha posto -- entao quem
    // carrega o preset precisa saber que nao deve tocar na musica.
    std::vector<std::string> m_themeMusicTracks;
    std::vector<TrackInfo> m_customBgmTracks;
    void applyUiLanguage();
    void rebuildThemeFromColors();
    ThemePreset buildEffectiveThemePreset();
    std::string resolveThemeAssetPath(const ThemePreset& preset, const std::string& rawPath) const;
    ThemePreset* findPresetPtr(const std::string& name);
    void deletePreset(const std::string& presetId);
    void startSoftwareDeletion(uint64_t titleId, const std::string& title,
                               bool closeGameOptionsOnSuccess);
    void syncSoftwareDeletion();
    void updateCursor();
    struct ActionHint {
        std::string icon;
        std::string label;
    };
    std::vector<ActionHint> buildActionHints();
    struct HintCapsule {
        std::string icon;
        std::string label;
        std::string outgoing;
        float width = 0.f;
        float widthFrom = 0.f, widthTo = 0.f;
        float widthT = 1.f;
        float swapT = 1.f;
    };
    std::vector<HintCapsule> m_hintCapsules;
    void syncHintCapsules(float dt);
    float hintCapsuleWidth(const std::string& icon, const std::string& label);
    void renderActionHintBar(nxui::Renderer& ren);
    void renderActionHintPanel(nxui::Renderer& ren);
    void renderPageArrows(nxui::Renderer& ren);
    bool pagingAvailable();
    int  dynamicLineNeighbour(int dir) const;
    int   m_lineRepeatDir = 0;
    float m_lineRepeatTimer = 0.f;
    bool stepDynamicLine(int dir);
    struct PageArrowAnim {
        float show = 0.f;
        float press = 0.f;
    };
    PageArrowAnim m_arrowAnimLeft, m_arrowAnimRight;
    bool m_touchArrowLeft = false, m_touchArrowRight = false;
    nxui::Rect pageArrowRect(bool left);
    void kickPageArrow(int dir);
    bool flipPage(int dir);
    bool addPageAvailable();
    void createFolderPage();
    void createHomePage();
    bool deletePageAvailable();
    void deleteFolderPage();
    void deleteHomePage();
    float m_addPageHold = 0.f;
    bool m_addPageMode = false;
    bool m_addPageTouchHold = false;
    bool m_addPageTriggered = false;
    float m_deletePageHold = 0.f;
    bool m_deletePageMode = false;
    bool m_deletePageTouchHold = false;
    bool m_deletePageTriggered = false;
    bool m_zlQuickFlipArmed = false;
    float m_zlQuickFlipHeld = 0.f;
    int findTitleIndex(uint64_t titleId) const;
    bool focusTitle(uint64_t titleId);
    // Devolve o seletor para a grade quando não há um título específico para
    // focar. Usado ao voltar pelo HOME.
    bool focusGridSelection();
    void markSuspendedIcon(uint64_t titleId);
    // Overlays created at startup sit below the dossier, gallery and mods
    // screens, which are built on demand and appended after them. One of those
    // early overlays then takes focus without being drawn, and the grid looks
    // frozen under a menu nobody can see. Anything shown on top must be moved
    // to the end of the layer first; this is the one place that does it.
    void raiseOverlay(const std::shared_ptr<nxui::Widget>& overlay);
    // Asks GitHub for the newest release at most once a day, then offers it.
    void startUpdateCheck(bool forced);
    void syncUpdateCheck();
    void offerUpdate(const update::UpdateClient::Release& release, bool automatic);
    void startUpdateDownload(const update::UpdateClient::Release& release);
    void syncUpdateDownload();
    void publishUpdateState();
    void showReleaseNotes();
    void closeActiveOverlays(bool preserveDialog = false);
    void handleTouch();
    std::shared_ptr<GlossyIcon> makeIcon(const AppEntry& entry);
    void wireFocusCallback();
    void wireGlobalActions();
    void toggleAccessibilitySpeech();
    bool handleAccessibilityToggleCombo();
    bool handleFrameDumpShortcut();
    void syncFrameDumpCapture();
#ifdef SWITCHU_MENU
    void checkNewGameSteamGridDbPrompt();
    void promptSteamGridDbForGame(std::uint64_t titleId, const std::string& title);
    std::string resolveAppTitle(std::uint64_t titleId, const std::string& currentTitle);
    void pollAutoNtpSync(float dt);
#endif
    void handleSortShortcutRelease(float dt);
    void handleZlShortcutRelease(float dt);
    // Longer than a deliberate tap, far shorter than the hold used to reach
    // Sphaira, so the two gestures never get confused for one another.
    static constexpr float kSortShortcutTapSeconds = 0.35f;
    bool isCurrentFocusableWidget(nxui::Widget* w) const;
    std::string accessibilityPositionFor(nxui::Widget* w) const;
    void createSettings();
    void createQuickSettings();
    void openQuickSettings();
    void closeQuickSettings();
    void createActivityLog();
    void openActivityLog(std::uint64_t initialTitleId = 0);
    void closeActivityLog();
    void createThemeShop();
    void createGameOptions();
    void createFolderOptions();
    void createControllerTest();
    void createGameGallery();
    void createGameDetails();
    void createWaraWaraPlaza();
    void openWaraWaraPlaza();
    void closeWaraWaraPlaza();
    void toggleWaraWaraPlaza();
    void toggleMediaCenter();
    void syncMediaCenterState();
    void refreshPlazaCommunities();
    void reloadThemePresets();
    void refreshThemeShopState();
    std::vector<ThemeShopScreen::ThemeShopEntry> buildThemeShopEntries();
    void startThemePackageTransfer(const ThemeCatalogClient::Entry& entry, bool installMode);
    void syncThemePackageTransfer();
    void activateThemePreset(ThemePreset* preset, bool applyBundledSound);
    std::string resolveSoundPresetId(const std::string& preset) const;
    void loadSoundPreset(const std::string& preset);
    void changeSoundPreset(const std::string& preset);
    std::vector<std::string> scanAvailablePresets();
    std::vector<TrackInfo> scanCustomBgmTracks();
    void reloadMusicTracks();
    void loadMenuLayout();
    void saveMenuLayout();
    void quiesceWritersForPowerAction();
    void applyMenuLayoutToPending(std::vector<PendingApp>& apps);
    void startEditGhost(GlossyIcon* sourceIcon);
    nxui::Texture* adoptEditGhostTexture(GlossyIcon* sourceIcon);
    void detachEditSourceIcon();
    void reattachEditSourceIcon();
    void stopEditGhost();
    void updateEditGhost(float dt);
    bool commitEditModePlacement();
    bool activateEditModeTarget();
    bool moveFocusedIcon(nxui::FocusDirection dir);
    void enterEditMode();
    void exitEditMode();
    void bindEditActions(GlossyIcon* icon);
    void unbindEditActions();
    bool isEditableIcon(nxui::Widget* w) const;
    void announceFocusedWidget(nxui::Widget* w);
    std::string accessibilityContextFor(nxui::Widget* w) const;
    std::string accessibilityActionsFor(nxui::Widget* w) const;

    void toggleAppLayoutMode();
    void setAppLayoutMode(AppLayoutMode mode);
    void configureDynamicLineNavigation();
    AppLayoutMode appLayoutMode() const { return m_appLayoutMode; }
    // DynamicLine, Flow, Shelf, Deck, and Cover are presentations of one carousel model: a
    // single wrapping row, ring icon streaming, no paging, and reordering by
    // visible row order. Behaviour common to all carousel views must test this
    // rather than DynamicLine alone.
    bool isCarouselLayout() const {
        return m_appLayoutMode == AppLayoutMode::DynamicLine
            || m_appLayoutMode == AppLayoutMode::Flow
            || m_appLayoutMode == AppLayoutMode::Shelf
            || m_appLayoutMode == AppLayoutMode::Deck
            || m_appLayoutMode == AppLayoutMode::Cover;
    }
    bool isXmbLayout() const {
        return m_appLayoutMode == AppLayoutMode::Xmb;
    }
    warawara::MiiAvatarManager& miiAvatarManager() { return m_miiAvatarManager; }
    const warawara::MiiAvatarManager& miiAvatarManager() const { return m_miiAvatarManager; }
    warawara::PlazaDialogueEngine& plazaDialogueEngine() { return m_plazaDialogueEngine; }
    const warawara::PlazaDialogueEngine& plazaDialogueEngine() const { return m_plazaDialogueEngine; }
    warawara::AnimalesePlayer& animalesePlayer() { return m_animalesePlayer; }
    const warawara::AnimalesePlayer& animalesePlayer() const { return m_animalesePlayer; }

#ifdef SWITCHU_MENU
    void refreshAppList();
    void finalizeRefresh();
    void handleSystemAction(SysAction a);
    void showGameContextMenu(GlossyIcon* icon);
#endif

    nxui::Font  m_fontNormal;
    nxui::Font  m_fontSmall;
    nxui::Font  m_fontIcons;
    // The lock screen clock, rasterised at the size it is drawn. Scaling
    // the 24pt face up to it showed every pixel of the smaller bitmap.
    nxui::Font  m_fontClock;

    GridModel    m_model;
    nxui::Theme  m_theme;
    switchu::services::ClockService m_clockService;

    // Drawn last and outside the widget tree: while it is up focusRoot()
    // hands back nothing, so no widget can be navigated or activated
    // behind it, and it reads its own presses straight from the pad.
    LockScreen   m_lockScreen;
    // One rendered black frame must precede disabled rendering; otherwise the
    // prior menu image remains scanned out while the GPU is idle.
    bool         m_lockScreenLowPowerPrimed = false;
    // Counts up to the next re-read of the console's own sleep plan.
    float        m_sleepPlanTimer = 0.f;

    std::string              m_activePresetName = "Default Dark";
    ThemeColorSet            m_activeColors;
    nxui::ThemeMode          m_activeMode = nxui::ThemeMode::Light;
    std::vector<ThemePreset> m_allPresets;
    ThemePreset              m_effectivePreset;

    std::shared_ptr<WaraWaraBackground> m_background;
    std::shared_ptr<GameArtworkBackdrop> m_gameArtworkBackdrop;
    std::shared_ptr<IconGrid>          m_grid;
    std::shared_ptr<SelectionCursor>   m_cursor;
    std::shared_ptr<SelectionCursor>   m_pointerCursor;
    std::shared_ptr<DateTimeWidget>    m_clock;
    std::shared_ptr<BatteryWidget>     m_battery;
    std::shared_ptr<TitlePillWidget>   m_titlePill;
    std::shared_ptr<PageIndicator>     m_pageIndicator;
    std::shared_ptr<LaunchAnimation>   m_launchAnim;
    std::shared_ptr<OverlayDialog>     m_userSelect;
    std::shared_ptr<OverlayDialog>     m_dialog;
    std::shared_ptr<QuickSettingsOverlay> m_quickSettings;
    std::shared_ptr<ContextMenu>       m_contextMenu;
    std::shared_ptr<ProgressDialog>    m_progressDialog;
    std::shared_ptr<SettingsScreen>    m_settings;
    std::shared_ptr<ThemeShopScreen>   m_themeShop;
    std::shared_ptr<GameGalleryScreen> m_gameGallery;
    std::shared_ptr<GameDetailsScreen> m_gameDetails;
    std::shared_ptr<GameModsScreen>    m_gameMods;
    std::shared_ptr<GameCheatsScreen>  m_gameCheats;
    std::shared_ptr<GameOptionsScreen> m_gameOptions;
    std::shared_ptr<SteamGridDbPickerScreen> m_steamGridDbPicker;
    std::shared_ptr<PlatformPickerScreen> m_platformPicker;
    std::uint64_t m_platformPickerTitleId = 0;
    std::string m_platformPickerTitle;
    std::shared_ptr<FolderOptionsScreen> m_folderOptions;
    std::shared_ptr<ControllerTestScreen> m_controllerTest;
    std::shared_ptr<TextEntryScreen>      m_textEntry;
    std::shared_ptr<ActivityLogScreen>    m_activityLog;
    switchu::activity::ActivityLogManager m_activityLogManager;
    warawara::MiiAvatarManager            m_miiAvatarManager;
    warawara::PlazaDialogueEngine         m_plazaDialogueEngine;
    warawara::AnimalesePlayer             m_animalesePlayer;
    std::shared_ptr<warawara::WaraWaraPlazaScreen>   m_plazaScreen;
    std::shared_ptr<nxui::Box>                       m_topCenterCluster;
    std::shared_ptr<warawara::PlazaScreenSwapButton> m_screenSwapButton;
    std::shared_ptr<widgets::MediaCenterButton>      m_mediaCenterButton;
    std::shared_ptr<media::MediaCenterScreen>        m_mediaCenterScreen;
    int m_plazaIconPumpIndex = 0;
    nxui::Widget* m_activityLogReturnFocus = nullptr;
    nxui::Widget* m_plazaReturnFocus = nullptr;
    nxui::Widget* m_mediaCenterReturnFocus = nullptr;

    nxui::Texture m_gameCardTex;
    nxui::Texture m_arrowTexLeft;
    nxui::Texture m_arrowTexRight;
    nxui::Texture m_batteryConsoleTex;
    nxui::Texture m_batteryJoyconLeftTex;
    nxui::Texture m_batteryJoyconRightTex;
    nxui::Texture m_batteryControllerTex;
    // XMB needs artwork the sidebar does not carry: the bar shows a Media, a
    // Network and a Homebrew category that have no sidebar button to borrow an
    // icon from. Without these every one of those rows fell back to the
    // settings gear or the album icon, so a whole category rendered as repeats
    // of one picture. Loaded once with the other static textures; any that is
    // missing from romfs stays invalid and the existing fallback still applies.
    nxui::Texture m_xmbAlbumTex;
    nxui::Texture m_xmbSettingsTex;
    nxui::Texture m_xmbControllersTex;
    nxui::Texture m_xmbPowerTex;
    nxui::Texture m_xmbThemesTex;
    nxui::Texture m_xmbMiiTex;
    nxui::Texture m_xmbMediaCenterTex;
    nxui::Texture m_xmbNetworkTex;
    nxui::Texture m_xmbBrowserTex;
    nxui::Texture m_xmbHomebrewTex;
    nxui::AnimatedFloat m_arrowCenterY;
    bool m_arrowCenterInit = false;

    std::shared_ptr<nxui::Box> m_bgLayer;
    std::shared_ptr<nxui::Box> m_contentLayer;
    std::shared_ptr<nxui::Box> m_overlayLayer;
    std::shared_ptr<nxui::Box> m_topHud;
    std::shared_ptr<nxui::Box> m_leftSidebar;
    std::shared_ptr<nxui::Box> m_rightSidebar;
    std::shared_ptr<nxui::Box> m_userAvatarBar;
    std::shared_ptr<FolderBackdrop> m_folderBackdrop;
    std::shared_ptr<SteamGridDbBackdrop> m_steamGridDbBackdrop;
    std::shared_ptr<nxui::GlassPanel> m_folderHeader;
    std::shared_ptr<nxui::Label> m_folderHeaderLabel;
    std::vector<std::shared_ptr<UserAvatarButton>> m_userAvatarButtons;

    AudioManager m_audio;
    AccessibilityManager m_accessibility;
    std::future<void>    m_audioFuture;
    // The config save runs on the thread pool. A power request has to wait on
    // it: the corruption is intermittent at roughly one reboot in three, which
    // is what losing a race with an in-flight write looks like.
    std::future<void>    m_configSaveFuture;
    std::future<void>    m_themeDeleteFuture;
    // Written by the worker, read on the main thread once m_playtimeFuture is
    // ready. The pool hands back only std::future<void>.
    struct PlaytimeRefreshState {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> playtime;
        // Raised when the console is about to be handed to a game: the batch
        // stops where it is rather than making the player wait for it.
        std::atomic<bool> cancelled{false};
    };
    std::shared_ptr<PlaytimeRefreshState> m_playtimeRefresh;
    std::future<void>    m_playtimeFuture;
    bool                 m_playtimeRefreshQueued = false;
    bool                 m_playtimeResortPending = false;
    bool                 m_audioStarted = false;
    bool                 m_musicFadeActive = false;
#ifdef SWITCHU_MENU
    uint64_t             m_transitionOriginTick = 0;
    uint64_t             m_menuMainTick = 0;
    uint32_t             m_menuMainCore = 0;
#endif
    std::vector<std::string> m_availablePresets;
    bool                 m_presetChangePending = false;
    std::string          m_loadedSoundPreset;
    std::string          m_pendingSoundPreset;

    struct ThemePackageTransferShared {
        std::mutex mutex;
        ThemeTransferState state;
        std::string themeId;
        bool installMode = false;
        std::string destinationPath;
        std::uint64_t revision = 0;
    };

    struct GameArtworkSaveShared {
        std::mutex mutex;
        gallery::ArtworkSaveResult result;
    };
    struct SteamGridDbApplyProgressShared {
        std::mutex mutex;
        std::string message;
        float progress01 = 0.f;
        std::uint64_t revision = 0;
    };

    nxui::ThreadPool m_threadPool{2};
    SidebarManager  m_sidebar;
    AppletLauncher  m_launcher;
    AppListLoader   m_appLoader;
    IconStreamer    m_iconStreamer;
    SystemMessages  m_sysMsg;
    switchu::navigation::MenuNavigator m_navigator;

    bool m_showDebugOverlay  = false;
#ifdef SWITCHU_DEBUG_UI
    std::unique_ptr<DebugImGuiOverlay> m_debugOverlay;
#endif
    bool m_showWireframe     = false;
    bool m_editMode          = false;
    int  m_editSourceIndex   = -1;
    int  m_editTargetIndex   = -1;
    int  m_editOriginRootSlot = -1;
    int  m_editOriginFolderIndex = -1;
    std::uint32_t m_editOriginFolderId = 0;
    std::uint64_t m_editHeldTitleId = 0;
    std::string m_editHeldTitle;
    GlossyIcon* m_editBoundIcon = nullptr;
    GlossyIcon* m_editSourceIcon = nullptr;
    std::shared_ptr<GlossyIcon> m_editGhostIcon;
    std::unique_ptr<nxui::Texture> m_editGhostTexture;
    nxui::Rect m_editGhostTargetRect {0.f, 0.f, 0.f, 0.f};
    float m_editGhostPulse = 0.f;
    std::vector<uint64_t> m_layoutSlots;
    std::unordered_map<std::uint64_t, switchu::widgets::WidgetSize> m_gameSizes;
    bool m_layoutDirty = false;
    switchu::folders::FolderStore m_folderStore;
    switchu::widgets::WidgetStore m_widgetStore;
    std::uint64_t m_recentWidgetAssetTitleId = 0;
    std::uint64_t m_recentWidgetLoadedTitleId = 0;
    std::unique_ptr<nxui::Texture> m_recentWidgetHero;
    std::unique_ptr<nxui::Texture> m_recentWidgetLogo;
    std::unique_ptr<nxui::Texture> m_recentWidgetIcon;
    struct RecentWidgetAssetDecodeState {
        std::uint64_t titleId = 0;
        steamgriddb::artwork::DecodedImage hero;
        steamgriddb::artwork::DecodedImage logo;
        IconStreamer::DecodedIcon icon;
        std::atomic<bool> cancelled{false};
        std::int64_t elapsedMs = 0;
    };
    std::shared_ptr<RecentWidgetAssetDecodeState> m_recentWidgetAssetDecode;
    std::shared_ptr<RecentWidgetAssetDecodeState> m_recentWidgetAssetReady;
    std::future<void> m_recentWidgetAssetFuture;
    int m_recentWidgetAssetUploadStage = 0;
    struct GameArtworkTextures {
        std::unique_ptr<nxui::Texture> hero;
        std::unique_ptr<nxui::Texture> logo;
    };
    std::unordered_map<std::uint64_t, GameArtworkTextures> m_gameArtwork;
    std::unordered_map<std::uint64_t, nxui::Texture> m_plazaCommunityTextures;
    struct GameArtworkDecodeState {
        std::uint64_t titleId = 0;
        steamgriddb::artwork::DecodedImage hero;
        steamgriddb::artwork::DecodedImage logo;
        std::atomic<bool> cancelled{false};
        std::int64_t elapsedMs = 0;
    };
    std::vector<std::uint64_t> m_gameArtworkDecodeQueue;
    std::shared_ptr<GameArtworkDecodeState> m_gameArtworkDecode;
    std::shared_ptr<GameArtworkDecodeState> m_gameArtworkReady;
    std::future<void> m_gameArtworkFuture;
    GameArtworkTextures m_gameArtworkUploadTextures;
    int m_gameArtworkUploadStage = 0;
    struct RetainedImagePin {
        std::string assetRef;
        std::string assetPath;
        std::shared_ptr<GlossyIcon> icon;
    };
    std::unordered_map<std::uint64_t, RetainedImagePin> m_retainedImagePins;
    int m_widgetAssetPage = -1;
    bool m_widgetAssetsWereSliding = false;
    std::vector<std::uint8_t> m_widgetAssetCurrentScratch;
    std::vector<std::uint8_t> m_widgetAssetKeepScratch;
    int m_consoleBatteryPercent = 0;
    bool m_consoleBatteryCharging = false;
    std::vector<AppEntry> m_allApps;
    std::uint32_t m_openFolderId = 0;
    std::uint32_t m_requestedFolderId = 0;
    std::uint64_t m_folderOpenFocusTitleId = 0;
    bool m_folderCaptureRequested = false;
    bool m_folderCaptureReady = false;
    bool m_gridSliding = false;
    // Frames the log stays unbuffered for after the run loop starts, so a hang
    // in early-frame work (deferred asset uploads) still reaches the SD card.
    int m_logImmediateFrames = 300;
    float m_perfAccumDt = 0.f;
    int   m_perfFrames = 0;
    float m_perfWorstDt = 0.f;
    bool  m_probeSceneHidden = false;

    // Vale so para a amostragem de memoria: a loja de temas e onde o menu saiu
    // duas vezes sem deixar rastro, e medir o tempo todo encheria o log.
    bool  inThemeShopForMemorySampling() const;
    float m_memSampleTimer = 0.f;

    int  m_touchHitIndex     = -1;
    bool m_touchOnFocused    = false;
    bool m_touchEditDragActive = false;
    UserAvatarButton* m_touchAvatarTarget = nullptr;
    bool m_touchAvatarWasFocused = false;
    AppletButton* m_touchSidebarTarget = nullptr;
    bool m_touchSidebarWasFocused = false;
    int  m_deferredRefreshFrames = 0;
    bool m_refreshQueued         = false;
    int  m_refreshCooldownFrames = 0;
    bool m_asyncRefreshPending   = false;
    int  m_refreshPrevPage       = 0;

    AppConfig m_config;
    SteamGridDbManager m_steamGridDb;
    std::future<SteamGridDbManager::BrowseResult> m_steamGridDbBrowseFuture;
    std::future<SteamGridDbManager::ApplyResult> m_steamGridDbApplyFuture;
    std::shared_ptr<SteamGridDbApplyProgressShared> m_steamGridDbApplyProgress;
    std::uint64_t m_steamGridDbApplyProgressUiRevision = 0;
    std::uint64_t m_steamGridDbUiRevision = 0;
    std::uint64_t m_steamGridDbLastCompletedTitleId = 0;
    bool m_steamGridDbWasRunning = false;
    bool m_newGamePromptChecked = false;
    bool m_autoNtpSynced = false;
    float m_ntpCheckTimer = 0.f;
    bool m_startupConfigProvided = false;
    bool m_settingsNeedRefresh        = false;
    std::string m_loadedRegularFontPath;
    std::string m_loadedSmallFontPath;
    std::string m_loadedGameCardPath;
    std::string m_loadedBackgroundImagePath;
    bool m_backgroundImageLoaded      = false;
    std::string m_pendingBackgroundImagePath;
    int m_backgroundImageRetryFrames = 0;
    int m_backgroundImageRetryAttempts = 0;
    bool m_forceThemeResourceReload   = false;
    std::uint64_t m_gameOptionsTitleId = 0;
    int m_imagePinTargetSlot = -1;
    bool m_imagePinApplyPending = false;
    switchu::widgets::WidgetSize m_imagePinSize;
    nxui::Rect m_imagePinAnchor{0.f, 0.f, 0.f, 0.f};
    std::uint32_t m_folderOptionsId = 0;
    nxui::Widget* m_contextMenuReturnFocus = nullptr;
    nxui::Widget* m_dialogReturnFocus = nullptr;
    // The Gallery is launched from a close-on-press dialog. Its return focus
    // must outlive that dialog's own return-focus handoff.
    nxui::Widget* m_gameGalleryReturnFocus = nullptr;
    nxui::Widget* m_gameDetailsReturnFocus = nullptr;
    bool m_dialogWasActive            = false;
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
    // Diagnostic-only state. Production lock behavior still clears every
    // overlay; the edge test keeps its single prepared-launch dialog alive.
    bool m_preflightEdgePending       = false;
    bool m_preflightEdgeLockObserved  = false;
#endif
    bool m_suppressNextNavigateSfx    = false;
    bool m_pendingNetConnect          = false;
    int  m_deferredBluetoothInitFrames = 0;
    int  m_deferredInitialAssetFrames = 0;
    bool m_deferredStaticTextures = false;
    int m_deferredProfileFrames = 0;
    struct DeferredProfileList {
        std::vector<AccountUid> uids;
        Result result = 0;
    };
    std::shared_ptr<DeferredProfileList> m_deferredProfileList;
    std::future<void> m_deferredProfileListFuture;
    std::vector<AccountUid> m_pendingProfileUids;
    std::size_t m_pendingProfileIndex = 0;
    std::future<void> m_accessibilityFuture;
    bool m_accessibilityReady = false;
    std::uint64_t m_fastReturnStartupTick = 0;
    bool m_audioInitPending = false;
    bool m_audioHeldLogged = false;
    bool m_fastReturnRequested = false;
    std::future<void> m_themePackageTransferFuture;
    std::future<void> m_softwareDeleteFuture;
    Result m_softwareDeleteResult = 0;
    std::string m_softwareDeleteTitle;
    std::uint64_t m_softwareDeleteTitleId = 0;
    bool m_softwareDeleteClosesGameOptions = false;
    // What the SD sweep did, alongside the ns result: a port that was never
    // registered fails the ns call and still has content to remove, and a
    // registered title can have both.
    int m_softwareDeleteSdRemoved = 0;
    std::string m_softwareDeleteSdFailure;
    // Written by the worker, read by the frame that draws the bar.
    std::atomic<std::uint64_t> m_softwareDeleteDone{0};
    std::atomic<std::uint64_t> m_softwareDeleteTotal{0};
    std::future<void> m_gameArtworkSaveFuture;
    std::shared_ptr<GameArtworkSaveShared> m_gameArtworkSave;
    std::shared_ptr<ThemePackageTransferShared> m_themePackageTransfer;
    std::uint64_t m_themePackageTransferUiRevision = 0;
    std::uint64_t m_themePackageTransferHandledRevision = 0;
    int m_themeRenderDebugFrames = 0;

    float m_returnFadeTimer = 0.f;
    float m_tutorialStartupFadeTimer = 0.f;
    std::uint64_t m_tutorialStartupFadeDeadlineTick = 0;
    bool  m_tutorialStartupFade = false;
    bool m_hintPanelInitialized = false;
    bool m_hintCapsulesInitialized = false;
    bool m_accessibilityToggleComboHeld = false;
    bool m_frameDumpShortcutHeld = false;
    bool m_frameDumpActive = false;
    int  m_frameDumpRemaining = 0;
    int  m_frameDumpIndex = 0;
    int  m_frameDumpCounter = 0;
    std::string m_frameDumpBatchDir;
    // Set while R is held so a release only sorts when the press began here,
    // and not when R was already down on the way back from another screen.
    update::UpdateClient m_updateClient;
    std::uint64_t m_updateSeenRevision = 0;
    bool m_updateOffered = false;
    bool m_updateCheckForced = false;
    update::UpdateClient::Release m_pendingUpdate;
    // The newest published release, whether or not it is newer than this
    // build, so its notes can be read at any time.
    update::UpdateClient::Release m_latestRelease;
    std::string m_updateStatus;
    struct UpdateDownload;
    std::shared_ptr<UpdateDownload> m_updateDownload;
    std::future<void> m_updateDownloadFuture;
    bool m_sortShortcutArmed = false;
    float m_sortShortcutHeld = 0.f;
    bool m_plusExitPending = false;
    AppLayoutMode m_appLayoutMode = AppLayoutMode::Grid;
    float m_plusExitPendingTimer = 0.f;
    nxui::AnimatedFloat m_hintPanelW{0.f};
    nxui::AnimatedFloat m_hintPanelH{0.f};
    nxui::AnimatedFloat m_hintContentReveal{1.f};
    std::string m_hintSignature;
    static constexpr float kReturnFadeInDur = 0.22f;
    static constexpr float kTutorialStartupFadeDur = 0.34f;
};
