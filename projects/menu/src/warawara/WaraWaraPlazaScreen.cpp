#include "WaraWaraPlazaScreen.hpp"
#include "WiiUPointerHandBytes.hpp"
#include <cmath>
#include <algorithm>
#include <chrono>

namespace warawara {

WaraWaraPlazaScreen::WaraWaraPlazaScreen() {
    setRect({0.0f, 0.0f, 1280.0f, 720.0f});
    setVisible(false);
    setFocusable(false);
    setTag("warawara_plaza_screen");

    // Application::dispatchInput() sends actions to the active FocusManager
    // before WiiUMenuApp::onUpdate() runs. Binding A here is therefore the
    // reliable controller path: once openWaraWaraPlaza() explicitly focuses
    // this screen, the press is consumed here and cannot depend on a parallel
    // manual poll running later in the frame.
    addAction(static_cast<std::uint64_t>(nxui::Button::A), [this]() {
        if (m_active)
            activateHandTarget();
    });

    std::random_device rd;
    m_rng.seed(rd());
}

void WaraWaraPlazaScreen::initGpuAssets(nxui::GpuDevice& gpu, nxui::Renderer& ren) {
    if (!m_handCursorTex.valid()) {
        static constexpr const char* kSdIcon = "sdmc:/switch/OmniLaunch/icons/pointer_hand.png";
        static constexpr const char* kRomfsIcon = "romfs:/icons/pointer_hand.png";
        if (!m_handCursorTex.loadFromFile(gpu, ren, kSdIcon, 128)) {
            if (!m_handCursorTex.loadFromFile(gpu, ren, kRomfsIcon, 128)) {
                m_handCursorTex.loadFromMemory(gpu, ren, kWiiUPointerHandPng, sizeof(kWiiUPointerHandPng), 128);
            }
        }
    }
}

void WaraWaraPlazaScreen::setupCommunities(const std::vector<GameCommunityEntry>& entries) {
    struct FallbackCommunity {
        uint64_t titleId;
        const char* title;
        const char* subtitle;
    };

    static const FallbackCommunity kFallbacks[] = {
        {0x0100000000001000ULL, "OmniLaunch Community", "Latest News & Tips"},
        {0x0100000000001001ULL, "Miiverse Plaza", "Drawings & Reactions"},
        {0x0100000000001002ULL, "Nintendo eShop", "Featured Titles"},
        {0x0100000000001003ULL, "Mii Maker", "Create & Customize"},
        {0x0100000000001004ULL, "Activity Log", "Play Records & Stats"},
        {0x0100000000001005ULL, "Theme Shop", "Wii U Style Themes"},
        {0x0100000000001006ULL, "Super Mario", "Mushroom Kingdom Hub"},
        {0x0100000000001007ULL, "The Legend of Zelda", "Hyrule Gathering"},
        {0x0100000000001008ULL, "Mario Kart", "Grand Prix Community"},
        {0x0100000000001009ULL, "Super Smash Bros.", "Fighter Battle Arena"}
    };

    std::vector<PlazaCommunityData> communityList;
    communityList.reserve(10);

    // 1. Add installed game communities
    for (const auto& entry : entries) {
        if (communityList.size() >= 10) break;
        PlazaCommunityData data;
        data.titleId = entry.titleId;
        data.titleName = entry.title;
        data.subtitle = entry.subtitle.empty() ? "Community Gathering" : entry.subtitle;
        data.iconTexture = entry.iconTexture;
        communityList.push_back(data);
    }

    // 2. Gracefully pad up to 10 communities using authentic Nintendo Wii U fallbacks
    size_t fallbackIdx = 0;
    while (communityList.size() < 10 && fallbackIdx < 10) {
        const auto& fb = kFallbacks[fallbackIdx++];
        bool alreadyExists = false;
        for (const auto& c : communityList) {
            if (c.titleName == fb.title) {
                alreadyExists = true;
                break;
            }
        }
        if (!alreadyExists) {
            PlazaCommunityData data;
            // Fallback communities are presentation-only; zero prevents the
            // hand's A action from pretending they are installed titles.
            data.titleId = 0;
            data.titleName = fb.title;
            data.subtitle = fb.subtitle;
            data.iconTexture = nullptr;
            communityList.push_back(data);
        }
    }

    // If pedestals are already built with matching communities, update data (textures/playtime)
    // without reallocating pedestals or rebuilding Miis for instantaneous opening
    if (m_pedestals.size() == communityList.size() && !m_miis.empty()) {
        bool matches = true;
        for (size_t i = 0; i < communityList.size(); ++i) {
            if (m_pedestals[i]->data().titleId != communityList[i].titleId) {
                matches = false;
                break;
            }
        }
        if (matches) {
            for (size_t i = 0; i < communityList.size(); ++i) {
                m_pedestals[i]->setData(communityList[i]);
            }
            return;
        }
    }

    m_pedestals.clear();
    const size_t count = std::min<size_t>(communityList.size(), 10);

    for (size_t i = 0; i < count; ++i) {
        m_pedestals.push_back(std::make_unique<PlazaPedestal>(
            communityList[i], nxui::Vec2{640.0f, 430.0f}, 68.0f, 1.0f));
    }

    m_carouselAngle = 0.0f;
    m_carouselTargetAngle = 0.0f;
    updateCarouselPositions();
    populateMiis();
}

void WaraWaraPlazaScreen::updateCarouselPositions() {
    const nxui::Vec2 kPlazaCenter{640.0f, 430.0f};
    static constexpr float kRadiusX = 475.0f;
    static constexpr float kRadiusY = 185.0f;
    static constexpr float kPi = 3.14159265358979323846f;
    const size_t count = m_pedestals.size();
    if (count == 0) return;

    for (size_t i = 0; i < count; ++i) {
        const float phase = m_carouselAngle - kPi * 0.5f +
            (2.0f * kPi * (static_cast<float>(i) + 0.5f)) / static_cast<float>(count);
        const float depth = 0.5f + 0.5f * std::sin(phase);
        const nxui::Vec2 pos{
            kPlazaCenter.x + std::cos(phase) * kRadiusX,
            kPlazaCenter.y + std::sin(phase) * kRadiusY
        };
        const float perspectiveScale = 0.76f + depth * 0.30f;
        m_pedestals[i]->setPosition(pos);
        m_pedestals[i]->setScale(perspectiveScale);
    }

    // Position tethered Miis based on their pedestal or central placement
    for (size_t m = 0; m < m_miis.size() && m < m_miiBindings.size(); ++m) {
        auto& binding = m_miiBindings[m];
        auto& mii = m_miis[m];
        if (binding.pedestalIndex >= 0 && binding.pedestalIndex < static_cast<int>(count)) {
            auto& ped = m_pedestals[binding.pedestalIndex];
            nxui::Vec2 slot = ped->getGatheringSlot(binding.slotIndex);
            nxui::Vec2 finalPos = slot + binding.localJitter * ped->scale();
            mii->setPosition(finalPos);
            mii->setScale(ped->scale() * (0.94f + ((binding.slotIndex % 3) - 1) * 0.05f));
            mii->setFacingLeft(finalPos.x > ped->position().x);
        } else if (binding.pedestalIndex == -1) {
            // Central welcoming Miis
            mii->setPosition(kPlazaCenter + binding.localJitter);
            mii->setScale(0.92f);
            mii->setFacingLeft(binding.localJitter.x > 0.0f);
        }
    }
}

void WaraWaraPlazaScreen::populateMiis() {
    m_miis.clear();
    m_miiBindings.clear();

    if (!m_avatarManager) {
        return;
    }

    const auto& allAvatars = m_avatarManager->avatars();
    if (allAvatars.empty()) {
        return;
    }

    const size_t targetMiiCount = std::min<size_t>(36, std::max<size_t>(20, allAvatars.size() * 2));
    m_miis.reserve(targetMiiCount);
    m_miiBindings.reserve(targetMiiCount);

    size_t avatarIdx = 0;

    // 1. Assign 3 Miis to gather around each community pedestal
    for (size_t p = 0; p < m_pedestals.size(); ++p) {
        for (size_t s = 0; s < 3; ++s) {
            const auto& av = allAvatars[avatarIdx % allAvatars.size()];
            avatarIdx++;

            auto mii = std::make_unique<MiiFigure>(av);
            mii->setAutonomous(false);

            MiiBinding binding;
            binding.pedestalIndex = static_cast<int>(p);
            binding.slotIndex = static_cast<int>(s);
            binding.localJitter = {
                (s == 0) ? -10.0f : ((s == 1) ? 10.0f : 0.0f),
                (s == 2) ? 6.0f : -3.0f
            };
            binding.stateTimer = 1.5f + static_cast<float>(s) * 0.9f;

            m_miis.push_back(std::move(mii));
            m_miiBindings.push_back(binding);
        }
    }

    // 2. Add 3 central welcoming Miis in the center of the plaza
    const nxui::Vec2 centerOffsets[] = {
        {-28.0f, 0.0f},
        {0.0f, -8.0f},
        {28.0f, 0.0f}
    };
    for (size_t c = 0; c < 3; ++c) {
        const auto& av = allAvatars[avatarIdx % allAvatars.size()];
        avatarIdx++;

        auto mii = std::make_unique<MiiFigure>(av);
        mii->setAutonomous(false);

        MiiBinding binding;
        binding.pedestalIndex = -1;
        binding.slotIndex = static_cast<int>(c);
        binding.localJitter = centerOffsets[c];
        binding.stateTimer = 1.2f + static_cast<float>(c) * 0.7f;

        m_miis.push_back(std::move(mii));
        m_miiBindings.push_back(binding);
    }

    updateCarouselPositions();
}

void WaraWaraPlazaScreen::open() {
    m_active = true;
    setVisible(true);
    m_cameraTargetX = 0.0f;
    m_cameraTargetY = 0.0f;
    m_cameraX = 0.0f;
    m_cameraY = 0.0f;
    // Begin close on the welcoming Miis, then ease out to the full radial overview
    m_zoomTarget = 1.0f;
    m_zoom = 2.10f;
    m_focusedPedestalIndex = -1;
    m_focusedMii = nullptr;
    m_speechBubbleTimer = 2.5f;

    updateCarouselPositions();

    if (!m_pedestals.empty()) {
        selectPedestalStep(0);
    }

    // Welcoming cheers
    for (size_t i = 0; i < m_miis.size(); ++i) {
        if (i % 3 == 0) {
            m_miis[i]->cheer(2.4f);
        }
    }
}

void WaraWaraPlazaScreen::close() {
    m_active = false;
    for (auto& mii : m_miis) {
        mii->dismissSpeechBubble(true);
    }
    if (m_animalesePlayer) {
        m_animalesePlayer->stop();
    }
}

void WaraWaraPlazaScreen::triggerRandomSpeechBubble() {
    if (!m_dialogueEngine || m_miis.empty()) return;

    // Find candidate Miis visible on screen that don't already have an active bubble
    std::vector<MiiFigure*> candidates;
    for (auto& mii : m_miis) {
        const float sx = worldToScreen(mii->position()).x;
        if (sx >= 60.0f && sx <= 1220.0f && !mii->hasSpeechBubble()) {
            candidates.push_back(mii.get());
        }
    }

    if (candidates.empty()) return;

    std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
    MiiFigure* chosenMii = candidates[dist(m_rng)];

    // Check if chosen Mii is near a community pedestal
    const PlazaPedestal* nearbyPed = nullptr;
    for (const auto& ped : m_pedestals) {
        float dx = ped->position().x - chosenMii->position().x;
        float dy = ped->position().y - chosenMii->position().y;
        if (std::sqrt(dx * dx + dy * dy) < 140.0f) {
            nearbyPed = ped.get();
            break;
        }
    }

    SpeechBubbleData bubbleData;
    if (nearbyPed && nearbyPed->data().titleId != 0) {
        bubbleData = m_dialogueEngine->getGameTipDialogue(
            nearbyPed->data().titleId,
            nearbyPed->data().titleName,
            chosenMii->data().nickname
        );
    } else {
        bubbleData = m_dialogueEngine->getDialogueForMii(
            chosenMii->data(),
            {},
            m_activityLog
        );
    }

    chosenMii->showSpeechBubble(bubbleData, 6.0f);

    if (m_animalesePlayer) {
        m_animalesePlayer->speak(bubbleData.text, chosenMii->data().gender, 1.0f);
    }
}

MiiFigure* WaraWaraPlazaScreen::nearestVisibleMii() const {
    const float viewCenterX = m_cameraX + 640.0f;
    MiiFigure* bestMii = nullptr;
    float bestDist = 1e9f;

    for (const auto& mii : m_miis) {
        const float screenX = mii->position().x - m_cameraX;
        if (screenX < 40.0f || screenX > 1240.0f)
            continue;

        const float dx = mii->position().x - viewCenterX;
        const float dy = mii->position().y - 480.0f;
        float dist = dx * dx + dy * dy;
        // Prefer a Mii without an existing bubble without making an already
        // speaking Mii impossible to select when it is the only visible one.
        if (mii->hasSpeechBubble())
            dist += 400.0f * 400.0f;
        if (dist < bestDist) {
            bestDist = dist;
            bestMii = mii.get();
        }
    }
    return bestMii;
}

bool WaraWaraPlazaScreen::interactWithNearestVisibleMii() {
    if (auto* mii = nearestVisibleMii()) {
        interactWithMii(mii);
        return true;
    }
    triggerRandomSpeechBubble();
    return !m_miis.empty();
}

void WaraWaraPlazaScreen::interactWithMii(MiiFigure* mii) {
    if (!mii) return;

    mii->cheer(3.0f);

    SpeechBubbleData bubble;
    if (m_dialogueEngine) {
        bubble = m_dialogueEngine->getDialogueForMii(mii->data(), {}, m_activityLog);
    } else {
        bubble.text = "Hello! Having fun in WaraWara Plaza?";
        bubble.author = mii->data().nickname;
        bubble.yeahCount = 12;
    }

    mii->showSpeechBubble(bubble, 6.5f);

    if (m_animalesePlayer) {
        m_animalesePlayer->speak(bubble.text, mii->data().gender, 1.05f);
    }
}

void WaraWaraPlazaScreen::update(float dt) {
    m_time += dt;

    // Fade transition
    const float fadeSpeed = 5.0f;
    if (m_active) {
        m_fadeAlpha = std::min(1.0f, m_fadeAlpha + dt * fadeSpeed);
    } else {
        m_fadeAlpha = std::max(0.0f, m_fadeAlpha - dt * fadeSpeed);
        if (m_fadeAlpha <= 0.001f) {
            setVisible(false);
            return;
        }
    }

    // Smooth zoom between 0.60x (wide overview) and 2.30x (close-up)
    m_zoomTarget = std::clamp(m_zoomTarget, 0.60f, 2.30f);
    m_zoom += (m_zoomTarget - m_zoom) * std::min(1.0f, dt * 7.0f);

    // Smooth carousel rotation around the plaza ring
    m_carouselAngle += (m_carouselTargetAngle - m_carouselAngle) * std::min(1.0f, dt * 8.5f);
    updateCarouselPositions();

    // Camera panning limits when zoomed in
    const float maxPanX = (m_zoom > 1.0f) ? (540.0f * (1.0f - 1.0f / m_zoom)) : 0.0f;
    const float maxPanY = (m_zoom > 1.0f) ? (320.0f * (1.0f - 1.0f / m_zoom)) : 0.0f;
    m_cameraTargetX = std::clamp(m_cameraTargetX, -maxPanX, maxPanX);
    m_cameraTargetY = std::clamp(m_cameraTargetY, -maxPanY, maxPanY);
    m_cameraX += (m_cameraTargetX - m_cameraX) * std::min(1.0f, dt * 7.5f);
    m_cameraY += (m_cameraTargetY - m_cameraY) * std::min(1.0f, dt * 7.5f);
    m_handPulse += dt;

    // Update pedestals
    for (auto& ped : m_pedestals) {
        ped->update(dt);
    }

    // Update Miis and their cheering/idling behavior while tethered to their pedestal
    for (size_t i = 0; i < m_miis.size(); ++i) {
        m_miis[i]->update(dt);
        if (i < m_miiBindings.size()) {
            auto& b = m_miiBindings[i];
            b.stateTimer -= dt;
            if (b.stateTimer <= 0.0f) {
                b.stateTimer = 3.0f + (static_cast<float>(i % 5) * 0.8f);
                int roll = (i + static_cast<int>(m_time * 2.0f)) % 4;
                if (roll == 0) m_miis[i]->cheer(2.5f);
                else if (roll == 1) m_miis[i]->speak(3.0f);
                else m_miis[i]->idle(3.5f);
            }
        }
    }

    // Periodic speech bubble chatter
    m_speechBubbleTimer -= dt;
    if (m_speechBubbleTimer <= 0.0f) {
        m_speechBubbleTimer = m_speechBubbleInterval;
        triggerRandomSpeechBubble();
    }
}

bool WaraWaraPlazaScreen::handleInput(const nxui::Input& input, float dt) {
    if (!m_active || m_fadeAlpha < 0.2f) return false;

    if (input.isDown(nxui::Button::A)) {
        activateHandTarget();
        return true;
    }

    if (input.isDown(nxui::Button::B)) {
        if (m_closeCb) {
            m_closeCb();
        } else {
            close();
        }
        return true;
    }

    // 1. Left stick moves the hand cursor freely across the screen
    const float lx = input.leftStickX();
    const float ly = input.leftStickY();
    const float stickMagnitude = std::sqrt(lx * lx + ly * ly);
    if (stickMagnitude > 0.16f) {
        const float pointerSpeed = 640.0f;
        m_handPos.x += lx * pointerSpeed * dt;
        m_handPos.y -= ly * pointerSpeed * dt;
        m_handPos.x = std::clamp(m_handPos.x, 26.0f, 1254.0f);
        m_handPos.y = std::clamp(m_handPos.y, 92.0f, 684.0f);
        updateHandSelection();

        // Edge scrolling when zoomed in
        if (m_zoom > 1.05f && stickMagnitude > 0.40f) {
            if (m_handPos.x < 130.0f) m_cameraTargetX -= 380.0f * dt / m_zoom;
            else if (m_handPos.x > 1150.0f) m_cameraTargetX += 380.0f * dt / m_zoom;
            if (m_handPos.y < 120.0f) m_cameraTargetY -= 320.0f * dt / m_zoom;
            else if (m_handPos.y > 610.0f) m_cameraTargetY += 320.0f * dt / m_zoom;
        }
    }

    // 2. D-Pad steps through the community pedestals along the circle and brings the selected one forward
    if (input.isDown(nxui::Button::DLeft)) {
        selectPedestalStep(-1);
    } else if (input.isDown(nxui::Button::DRight)) {
        selectPedestalStep(1);
    } else if (input.isDown(nxui::Button::DUp)) {
        selectPedestalStep(static_cast<int>(m_pedestals.size()) / 2);
    } else if (input.isDown(nxui::Button::DDown)) {
        selectPedestalStep(0);
    }

    // 3. L and R shoulder buttons rotate the carousel clockwise / counter-clockwise
    if (input.isDown(nxui::Button::L)) {
        m_carouselTargetAngle -= (2.0f * 3.14159265f / std::max<size_t>(1, m_pedestals.size()));
        if (m_navigateSfxCb) m_navigateSfxCb();
    } else if (input.isDown(nxui::Button::R)) {
        m_carouselTargetAngle += (2.0f * 3.14159265f / std::max<size_t>(1, m_pedestals.size()));
        if (m_navigateSfxCb) m_navigateSfxCb();
    }
    if (input.isHeld(nxui::Button::L)) {
        m_carouselTargetAngle -= 1.8f * dt;
    } else if (input.isHeld(nxui::Button::R)) {
        m_carouselTargetAngle += 1.8f * dt;
    }

    // 4. Right stick: camera panning when zoomed, or smooth carousel rotation when in overview
    const float rx = input.rightStickX();
    const float ry = input.rightStickY();
    if (m_zoom > 1.05f) {
        const float panSpeed = 620.0f / m_zoom;
        if (std::abs(rx) > 0.16f) m_cameraTargetX += rx * panSpeed * dt;
        if (std::abs(ry) > 0.16f) m_cameraTargetY -= ry * panSpeed * dt;
    } else {
        if (std::abs(rx) > 0.18f) {
            m_carouselTargetAngle += rx * 2.2f * dt;
        }
    }

    // 5. ZL and ZR provide zoom (0.60x to 2.30x)
    if (input.isDown(nxui::Button::ZR)) m_zoomTarget = std::min(2.30f, m_zoomTarget + 0.25f);
    if (input.isDown(nxui::Button::ZL)) m_zoomTarget = std::max(0.60f, m_zoomTarget - 0.25f);
    if (input.isHeld(nxui::Button::ZR)) m_zoomTarget = std::min(2.30f, m_zoomTarget + 1.2f * dt);
    if (input.isHeld(nxui::Button::ZL)) m_zoomTarget = std::max(0.60f, m_zoomTarget - 1.2f * dt);

    // 6. X and Y talk shortcuts
    if (input.isDown(nxui::Button::X) || input.isDown(nxui::Button::Y)) {
        interactWithNearestVisibleMii();
        return true;
    }

    return true;
}

nxui::Vec2 WaraWaraPlazaScreen::worldToScreen(const nxui::Vec2& world) const {
    const nxui::Vec2 kViewCenter{640.0f, 400.0f};
    return {
        kViewCenter.x + (world.x - m_cameraX - kViewCenter.x) * m_zoom,
        kViewCenter.y + (world.y - m_cameraY - kViewCenter.y) * m_zoom
    };
}

void WaraWaraPlazaScreen::selectPedestalStep(int step) {
    if (m_pedestals.empty()) return;
    const int count = static_cast<int>(m_pedestals.size());
    static constexpr float kPi = 3.14159265358979323846f;

    int newIdx = (m_focusedPedestalIndex < 0) ? 0 : (m_focusedPedestalIndex + step + count) % count;
    m_focusedPedestalIndex = newIdx;
    for (size_t i = 0; i < m_pedestals.size(); ++i) {
        m_pedestals[i]->setFocused(static_cast<int>(i) == newIdx);
    }

    // Rotate carousel so selected pedestal comes smoothly to the front
    const float basePhase = -kPi * 0.5f + (2.0f * kPi * (static_cast<float>(newIdx) + 0.5f)) / static_cast<float>(count);
    m_carouselTargetAngle = (kPi * 0.5f) - basePhase;

    // Place hand cursor on the selected pedestal
    const nxui::Vec2 centre = worldToScreen(m_pedestals[newIdx]->position());
    m_handPos = {centre.x + 14.0f, centre.y - 22.0f};

    if (m_navigateSfxCb) m_navigateSfxCb();
}

void WaraWaraPlazaScreen::moveHandToPedestal(int index, bool playSfx) {
    if (index < 0 || index >= static_cast<int>(m_pedestals.size())) return;

    const bool changed = m_focusedPedestalIndex != index;
    m_focusedPedestalIndex = index;
    for (size_t i = 0; i < m_pedestals.size(); ++i)
        m_pedestals[i]->setFocused(static_cast<int>(i) == index);

    const nxui::Vec2 centre = worldToScreen(m_pedestals[index]->position());
    m_handPos = {centre.x + 14.0f, centre.y - 22.0f};
    if (changed && playSfx && m_navigateSfxCb) m_navigateSfxCb();
}

void WaraWaraPlazaScreen::updateHandSelection() {
    int best = -1;
    float bestDistSq = 86.0f * 86.0f;
    for (size_t i = 0; i < m_pedestals.size(); ++i) {
        const nxui::Vec2 p = worldToScreen(m_pedestals[i]->position());
        const float dx = p.x - m_handPos.x;
        const float dy = (p.y - 24.0f) - m_handPos.y;
        const float distSq = dx * dx + dy * dy;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = static_cast<int>(i);
        }
    }

    if (best != m_focusedPedestalIndex) {
        m_focusedPedestalIndex = best;
        for (size_t i = 0; i < m_pedestals.size(); ++i)
            m_pedestals[i]->setFocused(static_cast<int>(i) == best);
        if (best >= 0 && m_navigateSfxCb) m_navigateSfxCb();
    }
}

int WaraWaraPlazaScreen::findDirectionalPedestal(int fromIndex, const nxui::Vec2& direction) const {
    if (m_pedestals.empty()) return -1;
    if (fromIndex < 0 || fromIndex >= static_cast<int>(m_pedestals.size())) {
        float best = 1e30f;
        int index = 0;
        for (size_t i = 0; i < m_pedestals.size(); ++i) {
            const nxui::Vec2 p = worldToScreen(m_pedestals[i]->position());
            const float dx = p.x - m_handPos.x;
            const float dy = p.y - m_handPos.y;
            const float d = dx * dx + dy * dy;
            if (d < best) { best = d; index = static_cast<int>(i); }
        }
        return index;
    }

    const nxui::Vec2 origin = worldToScreen(m_pedestals[fromIndex]->position());
    float bestScore = 1e30f;
    int best = -1;
    for (size_t i = 0; i < m_pedestals.size(); ++i) {
        if (static_cast<int>(i) == fromIndex) continue;
        const nxui::Vec2 p = worldToScreen(m_pedestals[i]->position());
        const float dx = p.x - origin.x;
        const float dy = p.y - origin.y;
        const float forward = dx * direction.x + dy * direction.y;
        if (forward <= 8.0f) continue;
        const float side = std::abs(dx * direction.y - dy * direction.x);
        const float score = forward + side * 2.8f;
        if (score < bestScore) { bestScore = score; best = static_cast<int>(i); }
    }
    return best;
}

void WaraWaraPlazaScreen::activateHandTarget() {
    if (m_focusedPedestalIndex >= 0 &&
        m_focusedPedestalIndex < static_cast<int>(m_pedestals.size())) {
        const uint64_t titleId = m_pedestals[m_focusedPedestalIndex]->data().titleId;
        if (titleId != 0 && m_launchGameCb) {
            if (m_activateSfxCb) m_activateSfxCb();
            m_launchGameCb(titleId);
            return;
        }
        // Presentation-only fallback communities cannot launch an application;
        // let A talk to their gathering instead of silently doing nothing.
        const nxui::Vec2 centre = m_pedestals[m_focusedPedestalIndex]->position();
        MiiFigure* nearest = nullptr;
        float nearestSq = 1e30f;
        for (const auto& mii : m_miis) {
            const float dx = mii->position().x - centre.x;
            const float dy = mii->position().y - centre.y;
            const float d = dx * dx + dy * dy;
            if (d < nearestSq) { nearestSq = d; nearest = mii.get(); }
        }
        if (nearest) {
            if (m_activateSfxCb) m_activateSfxCb();
            interactWithMii(nearest);
            return;
        }
    }
    interactWithNearestVisibleMii();
}

bool WaraWaraPlazaScreen::handleTouch(const nxui::Input& input) {
    if (!m_active || m_fadeAlpha < 0.2f) return false;

    float tx = input.touchX();
    float ty = input.touchY();

    if (input.touchDown()) {
        // 1. Check top right close area
        if (tx >= 1180.0f && ty <= 70.0f) {
            if (m_closeCb) {
                m_closeCb();
            } else {
                close();
            }
            return true;
        }

        // 2. Check if tapping an active speech bubble (e.g. "Yeah!" reaction)
        for (auto& mii : m_miis) {
            if (mii && mii->hasSpeechBubble() && mii->speechBubble().hitTest({tx, ty})) {
                mii->speechBubble().giveYeah();
                if (m_activateSfxCb) m_activateSfxCb();
                return true;
            }
        }

        // Check if tapping a community through the transformed view
        for (size_t i = 0; i < m_pedestals.size(); ++i) {
            const nxui::Vec2 p = worldToScreen(m_pedestals[i]->position());
            const float radius = 82.0f * m_zoom * m_pedestals[i]->scale();
            const float dx = tx - p.x;
            const float dy = ty - (p.y - 28.0f * m_zoom);
            if (dx * dx + dy * dy <= radius * radius) {
                const bool alreadySelected = m_focusedPedestalIndex == static_cast<int>(i);
                moveHandToPedestal(static_cast<int>(i), true);
                if (alreadySelected) activateHandTarget();
                return true;
            }
        }

        // Check if tapping a Mii
        for (auto& mii : m_miis) {
            nxui::Vec2 sPos = worldToScreen(mii->position());
            float mdx = tx - sPos.x;
            float mdy = ty - sPos.y;
            if (mdx * mdx + mdy * mdy <= 48.0f * 48.0f * m_zoom * m_zoom) {
                interactWithMii(mii.get());
                return true;
            }
        }

        m_lastTouchPos = {tx, ty};
        m_isDraggingTouch = true;
        return true;
    }

    if (input.isTouching() && m_isDraggingTouch) {
        float dx = tx - m_lastTouchPos.x;
        float dy = ty - m_lastTouchPos.y;
        if (m_zoom > 1.05f) {
            m_cameraTargetX -= dx / m_zoom;
            m_cameraTargetY -= dy / m_zoom;
        } else {
            m_carouselTargetAngle += (dx / 320.0f);
            m_carouselAngle = m_carouselTargetAngle;
        }
        m_lastTouchPos = {tx, ty};
        return true;
    }

    if (input.touchUp()) {
        m_isDraggingTouch = false;
        return true;
    }

    return false;
}

void WaraWaraPlazaScreen::drawPlazaFloor(nxui::Renderer& ren) const {
    // 1. Authentic Wii U Clean Sky / Horizon Gradient
    const float horizonY = 210.0f - m_cameraY * 0.3f;
    nxui::Rect skyRect{0.0f, 0.0f, 1280.0f, std::max(0.0f, horizonY)};
    ren.drawGradientRect(skyRect, nxui::Color(0.89f, 0.93f, 0.97f, m_fadeAlpha),
                                  nxui::Color(0.80f, 0.86f, 0.93f, m_fadeAlpha));

    // 2. Horizon divider line
    ren.drawLine({0.0f, horizonY}, {1280.0f, horizonY}, nxui::Color(0.74f, 0.80f, 0.88f, 0.65f * m_fadeAlpha), 1.5f);

    // 3. Ground Floor Base
    nxui::Rect groundRect{0.0f, horizonY, 1280.0f, 720.0f - horizonY};
    ren.drawGradientRect(groundRect, nxui::Color(0.86f, 0.90f, 0.95f, m_fadeAlpha),
                                     nxui::Color(0.76f, 0.82f, 0.89f, m_fadeAlpha));

    // 4. Concentric Circular Plaza Floor Arcs & Radial Tiles (Wii U plaza disc geometry)
    const float plazaCenterX = 640.0f - m_cameraX * m_zoom;
    const float floorCenterY = 400.0f + (420.0f - m_cameraY) * m_zoom;

    const float ringRadii[] = {340.0f, 480.0f, 620.0f, 760.0f, 900.0f};
    for (float r : ringRadii) {
        float effR = r * m_zoom;
        nxui::Rect ringRect{plazaCenterX - effR * 1.55f, floorCenterY - effR * 0.72f, effR * 3.10f, effR * 1.44f};
        ren.drawRoundedRectOutline(ringRect, nxui::Color(1.0f, 1.0f, 1.0f, 0.28f * m_fadeAlpha), ringRect.height * 0.5f, 1.6f);
    }

    // Radial perspective lines
    const float angles[] = {-0.65f, -0.45f, -0.25f, -0.08f, 0.08f, 0.25f, 0.45f, 0.65f};
    for (float a : angles) {
        float x1 = plazaCenterX + std::sin(a) * 320.0f * m_zoom;
        float y1 = horizonY;
        float x2 = plazaCenterX + std::sin(a * 1.6f) * 980.0f * m_zoom;
        float y2 = 720.0f;
        ren.drawLine({x1, y1}, {x2, y2}, nxui::Color(1.0f, 1.0f, 1.0f, 0.20f * m_fadeAlpha), 1.2f);
    }
}

void WaraWaraPlazaScreen::drawHeader(nxui::Renderer& ren) const {
    // 1. "WaraWara Plaza" Title Pill in top center
    const float titleW = 260.0f;
    const float titleH = 44.0f;
    const float titleX = (1280.0f - titleW) * 0.5f;
    const float titleY = 18.0f;
    nxui::Rect titleRect{titleX, titleY, titleW, titleH};

    // Frosted glass background
    ren.drawRoundedRect(titleRect, nxui::Color(0.08f, 0.14f, 0.24f, 0.62f * m_fadeAlpha), titleH * 0.5f);
    ren.drawRoundedRectOutline(titleRect, nxui::Color(0.35f, 0.75f, 1.0f, 0.70f * m_fadeAlpha), titleH * 0.5f, 1.5f);

    if (m_fontNormal) {
        std::string titleText = "WaraWara Plaza";
        nxui::Vec2 tSize = m_fontNormal->measure(titleText);
        float tScale = 0.68f;
        nxui::Vec2 tPos{
            titleX + (titleW - tSize.x * tScale) * 0.5f,
            titleY + (titleH - tSize.y * tScale) * 0.5f - 1.0f
        };
        ren.drawText(titleText, tPos, m_fontNormal, nxui::Color(0.97f, 0.98f, 1.0f, m_fadeAlpha), tScale);
    }
}

void WaraWaraPlazaScreen::drawHandCursor(nxui::Renderer& ren) const {
    const float pulse = 1.0f + std::sin(m_handPulse * 4.2f) * 0.035f;
    const nxui::Vec2 p = m_handPos;
    // Authentic Wii U pointer hand:
    // Aspect ratio: 104x122 (w:h ≈ 0.852). Display size: 46x54
    const float w = 46.0f * pulse;
    const float h = 54.0f * pulse;
    // Hotspot is at the tip of the pointing index finger (15/104 of w, 4/122 of h)
    const float hotX = w * (15.0f / 104.0f);
    const float hotY = h * (4.0f / 122.0f);
    const nxui::Rect dest{p.x - hotX, p.y - hotY, w, h};

    if (m_handCursorTex.valid()) {
        const nxui::Color tint(1.0f, 1.0f, 1.0f, m_fadeAlpha);
        ren.drawTexture(&m_handCursorTex, dest, tint);
    } else {
        // Fallback if texture not yet loaded
        const nxui::Color outline(0.05f, 0.50f, 0.88f, 0.98f * m_fadeAlpha);
        const nxui::Color white(0.98f, 0.99f, 1.0f, m_fadeAlpha);
        ren.drawRoundedRect({p.x - 6.0f * pulse, p.y - 30.0f * pulse, 12.0f * pulse, 34.0f * pulse}, outline, 6.0f * pulse);
        ren.drawRoundedRect({p.x - 10.0f * pulse, p.y - 2.0f * pulse, 28.0f * pulse, 24.0f * pulse}, outline, 10.0f * pulse);
        ren.drawRoundedRect({p.x - 4.0f * pulse, p.y - 28.0f * pulse, 8.0f * pulse, 30.0f * pulse}, white, 4.0f * pulse);
        ren.drawRoundedRect({p.x - 8.0f * pulse, p.y, 24.0f * pulse, 20.0f * pulse}, white, 8.0f * pulse);
    }
}

void WaraWaraPlazaScreen::render(nxui::Renderer& ren) {
    if (m_fadeAlpha <= 0.001f) return;

    // Report what the scene currently holds. The dimming artifact grows more
    // frequent the longer Plaza stays open, so the population has to be visible
    // per frame for "something accumulates" to be checked rather than assumed.
    // Speech bubbles are counted from the Miis that currently own one.
    if (ren.drawJournalEnabled()) {
        uint32_t bubbles = 0;
        for (const auto& mii : m_miis)
            if (mii && mii->hasSpeechBubble()) ++bubbles;
        ren.setPlazaCounts((uint32_t)m_miis.size(),
                           (uint32_t)m_pedestals.size(), bubbles);
    }

    // 1. Draw Plaza Sky & Floor
    {
        const nxui::Renderer::DrawTagScope tag{ren, "plaza.floor"};
        drawPlazaFloor(ren);
    }

    // 2. Render a unified depth-sorted radial scene. Pedestals and Miis share
    // one Y order so a foreground community correctly occludes rear figures.
    struct SceneItem {
        float y;
        bool pedestal;
        size_t index;
    };
    std::vector<SceneItem> scene;
    scene.reserve(m_pedestals.size() + m_miis.size());
    for (size_t i = 0; i < m_pedestals.size(); ++i)
        scene.push_back({m_pedestals[i]->position().y, true, i});
    for (size_t i = 0; i < m_miis.size(); ++i)
        scene.push_back({m_miis[i]->position().y, false, i});
    std::stable_sort(scene.begin(), scene.end(), [](const SceneItem& a, const SceneItem& b) {
        return a.y < b.y;
    });

    for (const auto& item : scene) {
        if (item.pedestal) {
            const nxui::Renderer::DrawTagScope tag{ren, "plaza.pedestal.radial"};
            m_pedestals[item.index]->render(
                ren, m_fontNormal, m_fontSmall, m_cameraX, m_cameraY, m_zoom, {640.0f, 400.0f});
        } else {
            const bool far = m_miis[item.index]->position().y < 420.0f;
            const nxui::Renderer::DrawTagScope tag{
                ren, far ? "plaza.mii.far" : "plaza.mii.near"};
            m_miis[item.index]->render(
                ren, m_fontNormal, m_fontSmall, m_cameraX, m_cameraY, m_zoom, {640.0f, 400.0f});
        }
    }

    // 3. Render active speech bubbles in foreground overlay pass
    // Rendering here ensures speech bubbles are ALWAYS on top of all Miis and pedestals.
    {
        const nxui::Renderer::DrawTagScope tag{ren, "plaza.speech_bubbles"};
        for (const auto& mii : m_miis) {
            if (mii && mii->hasSpeechBubble()) {
                mii->renderSpeechBubble(ren, m_fontNormal, m_fontSmall);
            }
        }
    }

    // 4. Header & Navigation UI
    {
        const nxui::Renderer::DrawTagScope tag{ren, "plaza.header"};
        drawHeader(ren);
    }
    drawHandCursor(ren);
}

} // namespace warawara
