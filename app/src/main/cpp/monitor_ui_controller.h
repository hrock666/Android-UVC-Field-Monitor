#pragma once

#include "monitor_ui_state.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace field_monitor {

EffectivePreviewState resolveEffectivePreviewState(
        const PreviewAssistState& state);

std::string buildRuntimeAssistText(
        const EffectivePreviewState& effective);

float frameAspectValue(FrameAspect aspect);

MonitorRectF calculateFrameRect(
        const MonitorRectF& preview,
        FrameAspect aspect);

bool isPresetMenuOpen(const MonitorMenuState& menu);

class MonitorUiController {
public:
    MonitorUiController() = default;

    MonitorUiSnapshot snapshot() const;

    bool setHdrNitsAvailable(bool available);

    bool toggleMenu();
    bool closeMenu();
    bool toggleLock();
    bool tapOutsideMenuUi();
    bool tapFunction(FunctionKey key);

    bool selectZebraPreset(ZebraPreset preset);
    bool selectPeakingPreset(PeakingPreset preset);
    bool selectFalseColorMode(FalseColorMode mode);
    bool selectFrameOff();
    bool selectFrameAspect(FrameAspect aspect);
    bool toggleCenterCross();
    bool toggleSafeArea();

private:
    bool advanceMainPresetLocked(FunctionKey key);
    void markChangedLocked();

    mutable std::mutex mutex_;
    MonitorUiRenderState state_{};
    bool hdrNitsAvailable_ = false;
    std::uint64_t revision_ = 0;
};

// Process-lifetime UI state. JNI input and the render thread exchange only
// controller mutations and immutable snapshots; GL calls stay on the render
// thread.
MonitorUiController& monitorUiController();

}  // namespace field_monitor
