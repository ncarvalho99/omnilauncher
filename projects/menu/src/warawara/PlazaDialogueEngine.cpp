#include "PlazaDialogueEngine.hpp"
#include "core/DebugLog.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace warawara {

PlazaDialogueEngine::PlazaDialogueEngine() {
    std::random_device rd;
    m_rng.seed(rd());
}

bool PlazaDialogueEngine::initialize(const std::string& assetBase) {
    m_gameTips.clear();
    m_genericTips.clear();
    m_miiversePosts.clear();

    std::string path = assetBase + "/data/plaza_dialogues.json";
    bool success = parseJsonFile(path);

    if (!success) {
        DebugLog::log("[warawara] Failed to load %s, using offline built-in fallbacks", path.c_str());
        loadBuiltinFallbacks();
    } else {
        DebugLog::log("[warawara] Loaded %zu game tips, %zu generic tips, %zu Miiverse posts",
                      m_gameTips.size(), m_genericTips.size(), m_miiversePosts.size());
    }

    m_loaded = true;
    return true;
}

bool PlazaDialogueEngine::parseJsonFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        nlohmann::json j;
        file >> j;

        if (j.contains("game_tips") && j["game_tips"].is_array()) {
            for (const auto& item : j["game_tips"]) {
                GameTipRecord rec;
                rec.game = item.value("game", "");
                if (item.contains("keywords") && item["keywords"].is_array()) {
                    for (const auto& kw : item["keywords"]) {
                        rec.keywords.push_back(kw.get<std::string>());
                    }
                }
                if (item.contains("tips") && item["tips"].is_array()) {
                    for (const auto& tip : item["tips"]) {
                        rec.tips.push_back(tip.get<std::string>());
                    }
                }
                if (!rec.tips.empty()) {
                    m_gameTips.push_back(rec);
                }
            }
        }

        if (j.contains("generic_tips") && j["generic_tips"].is_array()) {
            for (const auto& tip : j["generic_tips"]) {
                m_genericTips.push_back(tip.get<std::string>());
            }
        }

        if (j.contains("miiverse_posts") && j["miiverse_posts"].is_array()) {
            for (const auto& post : j["miiverse_posts"]) {
                m_miiversePosts.push_back(post.get<std::string>());
            }
        }

        return (!m_gameTips.empty() || !m_miiversePosts.empty());
    } catch (const std::exception& e) {
        DebugLog::log("[warawara] JSON parsing error: %s", e.what());
        return false;
    }
}

void PlazaDialogueEngine::loadBuiltinFallbacks() {
    m_gameTips = {
        {
            "The Legend of Zelda: Breath of the Wild",
            {"zelda", "breath", "botw"},
            {
                "Cooking a Hearty Durian gives bonus yellow hearts!",
                "You can parry Guardian lasers with a simple Pot Lid!",
                "Shield surfing down snowy slopes is super fast and fun!"
            }
        },
        {
            "The Legend of Zelda: Tears of the Kingdom",
            {"tears", "totk"},
            {
                "Attach Keese Eyeballs to arrows for homing tracking!",
                "Two Zonai Fans and a Steering Stick create a great Hoverbike!",
                "Always check ceilings for Ascend shortcuts!"
            }
        },
        {
            "Super Mario Odyssey",
            {"mario", "odyssey"},
            {
                "Throw Cappy and hold Y, then dive into him to extend your jump!",
                "Ground pound glowing spots to uncover hidden Power Moons!",
                "Capture Goombas to walk safely on slippery ice!"
            }
        },
        {
            "Mario Kart 8 Deluxe",
            {"mario kart", "mk8"},
            {
                "Hold your drift longer to unleash Purple Ultra Mini-Turbos!",
                "In 200cc, tap the B brake button while drifting around corners!",
                "Hold ZL to drag bananas behind you for protection!"
            }
        },
        {
            "Super Smash Bros. Ultimate",
            {"smash", "ssbu"},
            {
                "Press Jump and Attack together for short-hop aerial attacks!",
                "Release your shield at the exact moment an attack lands to parry!",
                "Direct your launch angle with DI to survive high percentages!"
            }
        }
    };

    m_genericTips = {
        "Remember to stretch and hydrate during long gaming sessions!",
        "Organize your favorite titles into custom folders in OmniLaunch!",
        "Dark Mode themes in OmniLaunch help preserve battery life!",
        "Calibrate your Joy-Cons in System Settings if you notice drift!"
    };

    m_miiversePosts = {
        "Why can't Metroid crawl?",
        "Y can't Metroid crawl?",
        "Water in video games always looks so refreshing.",
        "The music in this game is so catchy!",
        "Hello from Brazil! Enjoying the game!",
        "Wii U was truly ahead of its time. Long live WaraWara Plaza!",
        "Give this post a Yeah if you love platformers!",
        "I finally beat the final boss after 20 tries! Yeah!",
        "Remember to take a break every hour, fellow gamers!"
    };
}

std::string PlazaDialogueEngine::findMatchingTip(const std::string& titleName) {
    std::string lowerTitle = titleName;
    std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& entry : m_gameTips) {
        for (const auto& kw : entry.keywords) {
            if (lowerTitle.find(kw) != std::string::npos && !entry.tips.empty()) {
                int idx = randomInt(0, static_cast<int>(entry.tips.size()) - 1);
                return entry.tips[idx];
            }
        }
    }

    return "";
}

float PlazaDialogueEngine::randomFloat(float min, float max) {
    if (min >= max) return min;
    std::uniform_real_distribution<float> dist(min, max);
    return dist(m_rng);
}

int PlazaDialogueEngine::randomInt(int min, int max) {
    if (min >= max) return min;
    std::uniform_int_distribution<int> dist(min, max);
    return dist(m_rng);
}

SpeechBubbleData PlazaDialogueEngine::getDialogueForMii(
    const MiiAvatarData& mii,
    const std::vector<std::pair<uint64_t, std::string>>& installedTitles,
    const switchu::activity::ActivityLogManager* activityLog,
    int batteryPercent,
    bool isCharging) {

    float roll = randomFloat(0.0f, 1.0f);

    // 1. Playtime Milestone (25% chance if Activity Log is available)
    if (roll < 0.25f && activityLog && activityLog->hasData()) {
        const auto& rankings = activityLog->allTimeRankings();
        if (!rankings.empty()) {
            int idx = randomInt(0, std::min(4, static_cast<int>(rankings.size()) - 1));
            return getPlaytimeMilestoneDialogue(rankings[idx], mii.nickname);
        }
    }

    // 2. Installed Game Tip (35% chance if matching titles exist)
    if (roll < 0.60f && !installedTitles.empty()) {
        int startIdx = randomInt(0, static_cast<int>(installedTitles.size()) - 1);
        for (size_t i = 0; i < installedTitles.size(); ++i) {
            size_t idx = (startIdx + i) % installedTitles.size();
            const auto& [titleId, name] = installedTitles[idx];
            std::string tip = findMatchingTip(name);
            if (!tip.empty()) {
                SpeechBubbleData data;
                data.author = mii.nickname;
                data.topic = name;
                data.text = tip;
                data.yeahCount = randomInt(4, 38);
                return data;
            }
        }
    }

    // 3. System Status / Battery (12% chance)
    if (roll < 0.72f) {
        return getSystemStatusDialogue(batteryPercent, isCharging, mii.nickname);
    }

    // 4. Classic Miiverse Post (20% chance)
    if (roll < 0.92f && !m_miiversePosts.empty()) {
        return getMiiverseDialogue(mii.nickname);
    }

    // 5. Generic Gaming Tip
    SpeechBubbleData data;
    data.author = mii.nickname;
    data.topic = "Tips";
    if (!m_genericTips.empty()) {
        int idx = randomInt(0, static_cast<int>(m_genericTips.size()) - 1);
        data.text = m_genericTips[idx];
    } else {
        data.text = "Enjoy your gaming session on OmniLaunch!";
    }
    data.yeahCount = randomInt(5, 45);
    return data;
}

SpeechBubbleData PlazaDialogueEngine::getGameTipDialogue(uint64_t /*titleId*/,
                                                        const std::string& titleName,
                                                        const std::string& author) {
    SpeechBubbleData data;
    data.author = author;
    data.topic = titleName;
    data.yeahCount = randomInt(4, 36);

    std::string tip = findMatchingTip(titleName);
    if (!tip.empty()) {
        data.text = tip;
    } else if (!m_genericTips.empty()) {
        int idx = randomInt(0, static_cast<int>(m_genericTips.size()) - 1);
        data.text = m_genericTips[idx];
    } else {
        data.text = "Having a blast playing " + titleName + "!";
    }

    return data;
}

SpeechBubbleData PlazaDialogueEngine::getPlaytimeMilestoneDialogue(
    const switchu::activity::TitlePlayStats& stats,
    const std::string& author) {

    SpeechBubbleData data;
    data.author = author;
    data.topic = stats.titleName;
    data.yeahCount = randomInt(8, 64);

    uint64_t hours = stats.totalPlaytimeSeconds / 3600ULL;

    if (hours >= 80) {
        data.text = "Over " + std::to_string(hours) + " hours in " + stats.titleName + "! That is legendary dedication!";
    } else if (hours >= 30) {
        data.text = "You've spent " + std::to_string(hours) + " hours exploring " + stats.titleName + "! Still finding secrets?";
    } else if (hours >= 10) {
        data.text = std::to_string(hours) + " hours in " + stats.titleName + "! Such an awesome adventure!";
    } else if (stats.totalLaunches >= 20) {
        data.text = "Launched " + stats.titleName + " " + std::to_string(stats.totalLaunches) + " times! Clearly a favorite!";
    } else {
        data.text = "Been playing " + stats.titleName + " lately! It looks stunning on Switch!";
    }

    return data;
}

SpeechBubbleData PlazaDialogueEngine::getMiiverseDialogue(const std::string& author) {
    SpeechBubbleData data;
    data.author = author;
    data.topic = "Miiverse";
    data.yeahCount = randomInt(6, 52);

    if (!m_miiversePosts.empty()) {
        int idx = randomInt(0, static_cast<int>(m_miiversePosts.size()) - 1);
        data.text = m_miiversePosts[idx];
    } else {
        data.text = "Long live WaraWara Plaza!";
    }

    return data;
}

SpeechBubbleData PlazaDialogueEngine::getSystemStatusDialogue(int batteryPercent,
                                                             bool isCharging,
                                                             const std::string& author) {
    SpeechBubbleData data;
    data.author = author;
    data.topic = "Console";
    data.yeahCount = randomInt(2, 24);

    if (isCharging) {
        data.text = "Charging up at " + std::to_string(batteryPercent) + "%! Perfect time for a long session!";
    } else if (batteryPercent >= 80) {
        data.text = "Battery is at " + std::to_string(batteryPercent) + "%! Plenty of power for gaming on the go!";
    } else if (batteryPercent <= 25) {
        data.text = "Battery is at " + std::to_string(batteryPercent) + "%! Might want to plug in the charger soon!";
    } else {
        data.text = "System running smooth at " + std::to_string(batteryPercent) + "% battery. Have fun!";
    }

    return data;
}

} // namespace warawara
