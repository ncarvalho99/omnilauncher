#include "ThemeShopScreen.hpp"
#include "ThemeShopTabBuilders.hpp"
#include "core/DebugLog.hpp"

void ThemeShopScreen::buildTabs() {
    DebugLog::log("[themeshop] buildTabs() start");
    m_tabs.clear();
    DebugLog::log("[themeshop]   InstalledTab...");
    m_tabs.push_back(themeshop::tabs::InstalledTab::build(*this));
    // Animados primeiro: sao os deste projeto e os que a maioria vem buscar.
    // Os estaticos do PoloNX ficam logo depois.
    DebugLog::log("[themeshop]   AnimatedTab...");
    m_tabs.push_back(themeshop::tabs::AnimatedTab::build(*this));
    DebugLog::log("[themeshop]   CommunityTab...");
    m_tabs.push_back(themeshop::tabs::CommunityTab::build(*this));
    DebugLog::log("[themeshop]   MusicTab...");
    m_tabs.push_back(themeshop::tabs::MusicTab::build(*this));
    DebugLog::log("[themeshop]   SteamGridDbTab...");
    m_tabs.push_back(themeshop::tabs::SteamGridDbTab::build(*this));
    DebugLog::log("[themeshop]   OptionsTab...");
    m_tabs.push_back(themeshop::tabs::OptionsTab::build(*this));
    DebugLog::log("[themeshop]   UpdateTab...");
    m_tabs.push_back(themeshop::tabs::UpdateTab::build(*this));
    DebugLog::log("[themeshop]   UninstallTab...");
    m_tabs.push_back(themeshop::tabs::UninstallTab::build(*this));
    DebugLog::log("[themeshop] buildTabs() done (%d tabs)", (int)m_tabs.size());
    // Quanto do orcamento de imagem sobra para as previas. Sem este numero,
    // uma previa que nao aparece nao se distingue de uma previa que nao coube.
    if (m_gpu)
        DebugLog::log("[themeshop] imagem em %.1f de %.1f MB ao abrir",
                      m_gpu->imageMemoryUsed() / 1048576.0,
                      nxui::GpuDevice::imageBudget() / 1048576.0);

    m_cachedTabContentWidgets.clear();
    m_cachedTabContentWidgets.resize(m_tabs.size());

    // TabbedOverlayScreen owns the presentation refresh after every build:
    // warmup(), refreshTranslations(), and rebuildCurrentTab() all restore the
    // selected tab before rebuilding its widgets.  Rebuilding here used the
    // old Update-tab selection and then rebuildCurrentTab() immediately did
    // the same work again.  Keep this method to data/cache construction only.
}
