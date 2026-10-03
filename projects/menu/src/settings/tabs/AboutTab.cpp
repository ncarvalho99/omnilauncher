#include "TabBuilders.hpp"
#include <nxui/core/I18n.hpp>

#ifndef SWITCHU_VERSION
#define SWITCHU_VERSION "unknown"
#endif

#ifndef SWITCHU_UPSTREAM_VERSION
#define SWITCHU_UPSTREAM_VERSION "1.2.0"
#endif

// This build is a fork, and the two extra rows exist for reasons beyond
// vanity: the source link pointed at the upstream repository, so a bug
// introduced here would have been reported to someone who never wrote it, and
// GPL-2.0 section 2(a) asks a modified version to say that it was modified.
// PoloNX stays named above the fork, which is both correct and the point.

SettingsScreen::Tab settings::tabs::AboutTab::build(SettingsScreen& /* screen */) {
    using Tab = SettingsScreen::Tab;
    using SettingItem = SettingsScreen::SettingItem;
    using ItemType = SettingsScreen::ItemType;
    auto& i18n = nxui::I18n::instance();
    Tab t;
    t.name = i18n.tr("settings.tabs.about", "About");

    auto info = [&](const char* key, const char* fallback, std::string value) {
        SettingItem it;
        it.label    = i18n.tr(key, fallback);
        it.type     = ItemType::Info;
        it.infoText = std::move(value);
        t.items.push_back(std::move(it));
    };

    info("settings.about.version", "Version", "OmniLaunch " SWITCHU_VERSION);
    info("settings.about.based_on", "Based on", "SwitchU + sLaunch");
    info("settings.about.credits", "Original authors", "PoloNX & etonedemid");
    info("settings.about.author", "Maintained by", "ncarvalho99");
    info("settings.about.license", "License", "GPL-3.0");
    info("settings.about.source_code", "Source code", "github.com/ncarvalho99/omnilauncher");

    // Acknowledgements
    {
        SettingItem it;
        it.label = i18n.tr("settings.about.acknowledgements", "Acknowledgements");
        it.type  = ItemType::Section;
        t.items.push_back(std::move(it));
    }

    {
        SettingItem it;
        it.label = i18n.tr("settings.about.acknowledgements_desc",
                           "Special thanks to PoloNX (SwitchU), etonedemid (sLaunch), "
                           "and xortroll (uLaunch) for their groundbreaking work.");
        it.type  = ItemType::Info;
        it.infoText = "";
        it.wrapLabel = true;
        t.items.push_back(std::move(it));
    }

    return t;
}
