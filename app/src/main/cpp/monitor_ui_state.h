#pragma once

#include <cstdint>

namespace field_monitor {

enum class ZebraPreset {
    Off,
    Ire70,
    Ire80,
    Ire90,
    Ire95,
    Ire100,
};

enum class PeakingPreset {
    Off,
    Low,
    Mid,
    High,
    MonoLow,
    MonoMid,
    MonoHigh,
};

enum class FalseColorMode {
    Off,
    VideoLevel,
    HdrNits,
};

enum class FrameAspect {
    Ratio16x9,
    Ratio1_85,
    Ratio2_00,
    Ratio2_39,
    Ratio4x3,
    Ratio1x1,
    Ratio9x16,
};

enum class FunctionKey {
    None,
    F1Zebra,
    F2Peaking,
    F3FalseColor,
    F4Frame,
};

struct PreviewAssistState {
    ZebraPreset zebra = ZebraPreset::Off;
    PeakingPreset peaking = PeakingPreset::Off;
    FalseColorMode falseColor = FalseColorMode::Off;

    bool frameEnabled = false;
    FrameAspect frameAspect = FrameAspect::Ratio2_39;

    // These values remain stored while Frame is OFF.
    bool centerCross = true;
    bool safeArea = true;
};

struct MonitorMenuState {
    bool menuOpen = false;
    bool locked = true;
    FunctionKey selectedFunction = FunctionKey::None;
};

struct MonitorUiRenderState {
    PreviewAssistState assist;
    MonitorMenuState menu;
};

struct EffectivePreviewState {
    bool zebraVisible = false;
    ZebraPreset zebra = ZebraPreset::Off;

    bool peakingVisible = false;
    PeakingPreset peaking = PeakingPreset::Off;

    bool falseColorVisible = false;
    FalseColorMode falseColor = FalseColorMode::Off;

    bool frameVisible = false;
    FrameAspect frameAspect = FrameAspect::Ratio2_39;
    bool centerCrossVisible = false;
    bool safeAreaVisible = false;
};

struct MonitorRectF {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct MonitorUiSnapshot {
    MonitorUiRenderState state;
    bool hdrNitsAvailable = false;
    std::uint64_t revision = 0;
};

}  // namespace field_monitor
