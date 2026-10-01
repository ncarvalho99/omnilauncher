#pragma once

#include <cstdint>

// Persisted as a stable string, never as an ordinal: see Config.cpp. Adding a
// value here must not change what an existing installation reads back.
enum class AppLayoutMode : std::uint8_t {
    Grid,
    DynamicLine,
    Flow,
    Shelf,
    Deck,
    Cover,
    Xmb,
    List,
    Metro,
};
