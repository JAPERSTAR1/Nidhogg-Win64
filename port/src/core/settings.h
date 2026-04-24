#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ps4::core {

enum class DisplayMode : int {
    Windowed = 0,
    Borderless = 1,
    ExclusiveFullscreen = 2,
};

enum class AspectMode : int {
    Preserve16x9 = 0,
    IntegerScale = 1,
};

enum class QualityPreset : int {
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
    Custom = 4,
};

enum class TextureFilterMode : int {
    PixelPoint = 0,
    SharpBilinear = 1,
    Bilinear = 2,
    Anisotropic = 3,
};

enum class VSyncMode : int {
    Off = 0,
    On = 1,
    Adaptive = 2,
};

struct PlayerKeyBinds {
    int up = 0;
    int down = 0;
    int left = 0;
    int right = 0;
    int jump = 0;
    int attack = 0;
