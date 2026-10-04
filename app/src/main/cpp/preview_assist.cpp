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
