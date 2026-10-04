#pragma once

#include "monitor_ui_state.h"

#include <array>

namespace field_monitor {

struct ZebraShaderParams {
    bool enabled = false;
    float low = 0.0f;
    float high = 0.0f;
};

// Zebra thresholds are expressed in normalized source video level, where
// 0.0 is 0 IRE and 1.0 is 100 IRE. The capture shader performs the input
// range normalization before the calibrated Stage 3 RGB tap.
ZebraShaderParams resolveZebraShaderParams(ZebraPreset preset);

// 0=BT.601, 1=BT.709, 2=BT.2020. A disabled profile always resolves to
// BT.601, matching the planar-MJPEG source reconstruction fallback.
std::array<float, 3> resolveSourceLumaCoefficients(
        bool calibrationEnabled,
        int colorimetry);

}  // namespace field_monitor
