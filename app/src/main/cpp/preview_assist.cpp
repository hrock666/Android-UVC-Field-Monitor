#include "preview_assist.h"

#include <limits>

namespace field_monitor {
namespace {

constexpr float ireToNormalizedLevel(float ire)
{
    return ire / 100.0f;
}

ZebraShaderParams band(float centerIre)
{
    return {
            true,
            ireToNormalizedLevel(centerIre - 3.0f),
            ireToNormalizedLevel(centerIre + 3.0f),
    };
}

}  // namespace

ZebraShaderParams resolveZebraShaderParams(ZebraPreset preset)
{
    switch (preset) {
    case ZebraPreset::Ire70:
        return band(70.0f);
    case ZebraPreset::Ire80:
        return band(80.0f);
    case ZebraPreset::Ire90:
        return band(90.0f);
    case ZebraPreset::Ire95:
        return band(95.0f);
    case ZebraPreset::Ire100:
        return {
                true,
                ireToNormalizedLevel(100.0f),
                std::numeric_limits<float>::max(),
        };
    case ZebraPreset::Off:
    default:
        return {};
    }
}

PeakingShaderParams resolvePeakingShaderParams(PeakingPreset preset)
{
    switch (preset) {
    case PeakingPreset::Low:
        return {true, false, 0.20f, {1.0f, 0.0f, 0.0f}};
    case PeakingPreset::Mid:
        return {true, false, 0.12f, {1.0f, 0.0f, 0.0f}};
    case PeakingPreset::High:
        return {true, false, 0.06f, {1.0f, 0.0f, 0.0f}};
    case PeakingPreset::MonoLow:
        return {true, true, 0.20f, {1.0f, 0.0f, 0.0f}};
    case PeakingPreset::MonoMid:
        return {true, true, 0.12f, {1.0f, 0.0f, 0.0f}};
    case PeakingPreset::MonoHigh:
        return {true, true, 0.06f, {1.0f, 0.0f, 0.0f}};
    case PeakingPreset::Off:
    default:
        return {};
    }
}

FalseColorShaderParams resolveFalseColorShaderParams(FalseColorMode mode)
{
    FalseColorShaderParams params{};
    params.enabled = mode != FalseColorMode::Off;
    params.domain =
            mode == FalseColorMode::HdrNits
            ? FalseColorDomain::HdrNits
            : FalseColorDomain::VideoLevel;

    params.videoBoundaries = {
            0.00f,
            0.05f,
            0.20f,
            0.40f,
            0.55f,
            0.70f,
            0.85f,
            0.95f,
            1.00f,
    };
    params.videoPalette = {
            0.35f, 0.00f, 0.50f,  // < 0 IRE: Dark Purple
            0.55f, 0.00f, 0.80f,  // 0..5 IRE: Purple
            0.00f, 0.15f, 1.00f,  // 5..20 IRE: Blue
            0.00f, 0.80f, 1.00f,  // 20..40 IRE: Cyan
            0.45f, 0.45f, 0.45f,  // 40..55 IRE: Gray
            0.00f, 1.00f, 0.00f,  // 55..70 IRE: Green
            1.00f, 1.00f, 0.00f,  // 70..85 IRE: Yellow
            1.00f, 0.50f, 0.00f,  // 85..95 IRE: Orange
            1.00f, 0.00f, 0.00f,  // 95..100 IRE: Red
            1.00f, 1.00f, 1.00f,  // >= 100 IRE: White
    };
    return params;
}

std::array<float, 3> resolveSourceLumaCoefficients(
        bool calibrationEnabled,
        int colorimetry)
{
    if (!calibrationEnabled || colorimetry == 0) {
        return {0.2990f, 0.5870f, 0.1140f};
    }

    if (colorimetry == 2) {
        return {0.2627f, 0.6780f, 0.0593f};
    }

    return {0.2126f, 0.7152f, 0.0722f};
}

}  // namespace field_monitor
