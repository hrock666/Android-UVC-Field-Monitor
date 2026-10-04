#include "monitor_ui_controller.h"

#include <sstream>
#include <utility>
#include <vector>

namespace field_monitor {
namespace {

const char* zebraRuntimeText(ZebraPreset preset)
{
    switch (preset) {
    case ZebraPreset::Ire70:
        return "Z70";
    case ZebraPreset::Ire80:
        return "Z80";
    case ZebraPreset::Ire90:
        return "Z90";
    case ZebraPreset::Ire95:
        return "Z95";
    case ZebraPreset::Ire100:
        return "Z100";
    case ZebraPreset::Off:
    default:
        return "";
    }
}

const char* peakingRuntimeText(PeakingPreset preset)
{
    switch (preset) {
    case PeakingPreset::Low:
        return "P LOW";
    case PeakingPreset::Mid:
        return "P MID";
    case PeakingPreset::High:
        return "P HIGH";
    case PeakingPreset::MonoLow:
        return "P MONO LOW";
    case PeakingPreset::MonoMid:
        return "P MONO MID";
    case PeakingPreset::MonoHigh:
        return "P MONO HIGH";
    case PeakingPreset::Off:
    default:
        return "";
    }
}

const char* falseColorRuntimeText(FalseColorMode mode)
{
    switch (mode) {
    case FalseColorMode::VideoLevel:
        return "FC VIDEO";
    case FalseColorMode::HdrNits:
        return "FC NITS";
    case FalseColorMode::Off:
    default:
        return "";
    }
}

const char* frameAspectText(FrameAspect aspect)
{
    switch (aspect) {
    case FrameAspect::Ratio16x9:
        return "16:9";
    case FrameAspect::Ratio1_85:
        return "1.85";
    case FrameAspect::Ratio2_00:
        return "2.00";
    case FrameAspect::Ratio2_39:
        return "2.39";
    case FrameAspect::Ratio4x3:
        return "4:3";
    case FrameAspect::Ratio1x1:
        return "1:1";
    case FrameAspect::Ratio9x16:
        return "9:16";
    default:
        return "16:9";
    }
}

ZebraPreset nextZebraPreset(ZebraPreset preset)
{
    switch (preset) {
    case ZebraPreset::Off:
        return ZebraPreset::Ire70;
    case ZebraPreset::Ire70:
        return ZebraPreset::Ire80;
    case ZebraPreset::Ire80:
        return ZebraPreset::Ire90;
    case ZebraPreset::Ire90:
        return ZebraPreset::Ire95;
    case ZebraPreset::Ire95:
        return ZebraPreset::Ire100;
    case ZebraPreset::Ire100:
    default:
        return ZebraPreset::Off;
    }
}

PeakingPreset nextPeakingPreset(PeakingPreset preset)
{
    switch (preset) {
    case PeakingPreset::Off:
        return PeakingPreset::Low;
    case PeakingPreset::Low:
        return PeakingPreset::Mid;
    case PeakingPreset::Mid:
        return PeakingPreset::High;
    case PeakingPreset::High:
        return PeakingPreset::MonoLow;
    case PeakingPreset::MonoLow:
        return PeakingPreset::MonoMid;
    case PeakingPreset::MonoMid:
        return PeakingPreset::MonoHigh;
    case PeakingPreset::MonoHigh:
    default:
        return PeakingPreset::Off;
    }
}

FrameAspect nextFrameAspect(FrameAspect aspect)
{
    switch (aspect) {
    case FrameAspect::Ratio16x9:
        return FrameAspect::Ratio1_85;
    case FrameAspect::Ratio1_85:
        return FrameAspect::Ratio2_00;
    case FrameAspect::Ratio2_00:
        return FrameAspect::Ratio2_39;
    case FrameAspect::Ratio2_39:
        return FrameAspect::Ratio4x3;
    case FrameAspect::Ratio4x3:
        return FrameAspect::Ratio1x1;
    case FrameAspect::Ratio1x1:
        return FrameAspect::Ratio9x16;
    case FrameAspect::Ratio9x16:
    default:
        return FrameAspect::Ratio16x9;
    }
}

}  // namespace

EffectivePreviewState resolveEffectivePreviewState(
        const PreviewAssistState& state)
{
    EffectivePreviewState out{};

    out.falseColorVisible =
            state.falseColor != FalseColorMode::Off;
    out.falseColor = state.falseColor;

    out.zebraVisible =
            !out.falseColorVisible &&
            state.zebra != ZebraPreset::Off;
    out.zebra = state.zebra;

    out.peakingVisible =
            !out.falseColorVisible &&
            state.peaking != PeakingPreset::Off;
    out.peaking = state.peaking;

    out.frameVisible = state.frameEnabled;
    out.frameAspect = state.frameAspect;
    out.centerCrossVisible =
            state.frameEnabled && state.centerCross;
    out.safeAreaVisible =
            state.frameEnabled && state.safeArea;

    return out;
}

std::string buildRuntimeAssistText(
        const EffectivePreviewState& effective)
{
    std::vector<std::string> parts;
    parts.reserve(4);

    if (effective.zebraVisible) {
        parts.emplace_back(zebraRuntimeText(effective.zebra));
    }

    if (effective.peakingVisible) {
        parts.emplace_back(peakingRuntimeText(effective.peaking));
    }

    if (effective.falseColorVisible) {
        parts.emplace_back(falseColorRuntimeText(effective.falseColor));
    }

    if (effective.frameVisible) {
        std::string frameText = "F ";
        frameText += frameAspectText(effective.frameAspect);
        if (effective.centerCrossVisible) {
            frameText += " C";
        }
        if (effective.safeAreaVisible) {
            frameText += " S";
        }
        parts.push_back(std::move(frameText));
    }

    if (parts.empty()) {
        return "CLEAN";
    }

    std::ostringstream text;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            text << "  ";
        }
        text << parts[index];
    }
    return text.str();
}

float frameAspectValue(FrameAspect aspect)
{
    switch (aspect) {
    case FrameAspect::Ratio16x9:
        return 16.0f / 9.0f;
    case FrameAspect::Ratio1_85:
        return 1.85f;
    case FrameAspect::Ratio2_00:
        return 2.00f;
    case FrameAspect::Ratio2_39:
        return 2.39f;
    case FrameAspect::Ratio4x3:
        return 4.0f / 3.0f;
    case FrameAspect::Ratio1x1:
        return 1.0f;
    case FrameAspect::Ratio9x16:
        return 9.0f / 16.0f;
    default:
        return 16.0f / 9.0f;
    }
}

MonitorRectF calculateFrameRect(
        const MonitorRectF& preview,
        FrameAspect aspect)
{
    if (preview.width <= 0.0f || preview.height <= 0.0f) {
        return preview;
    }

    const float targetAspect = frameAspectValue(aspect);
    const float previewAspect = preview.width / preview.height;
    MonitorRectF frame = preview;

    if (targetAspect < previewAspect) {
        frame.width = preview.height * targetAspect;
        frame.x = preview.x + (preview.width - frame.width) * 0.5f;
    }
    else {
        frame.height = preview.width / targetAspect;
        frame.y = preview.y + (preview.height - frame.height) * 0.5f;
    }

    return frame;
}

bool isPresetMenuOpen(const MonitorMenuState& menu)
{
    return
            menu.menuOpen &&
            !menu.locked &&
            menu.selectedFunction != FunctionKey::None;
}

MonitorUiSnapshot MonitorUiController::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {state_, hdrNitsAvailable_, revision_};
}

bool MonitorUiController::setHdrNitsAvailable(bool available)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (hdrNitsAvailable_ == available) {
        return false;
    }

    hdrNitsAvailable_ = available;
    if (!available &&
        state_.assist.falseColor == FalseColorMode::HdrNits) {
        state_.assist.falseColor = FalseColorMode::VideoLevel;
    }
    markChangedLocked();
    return true;
}

bool MonitorUiController::toggleMenu()
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.menu.menuOpen = !state_.menu.menuOpen;
    if (!state_.menu.menuOpen) {
        state_.menu.selectedFunction = FunctionKey::None;
    }
    markChangedLocked();
    return true;
}

bool MonitorUiController::closeMenu()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.menu.menuOpen &&
        state_.menu.selectedFunction == FunctionKey::None) {
        return false;
    }

    state_.menu.menuOpen = false;
    state_.menu.selectedFunction = FunctionKey::None;
    markChangedLocked();
    return true;
}

bool MonitorUiController::toggleLock()
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.menu.locked = !state_.menu.locked;
    if (state_.menu.locked) {
        state_.menu.selectedFunction = FunctionKey::None;
    }
    markChangedLocked();
    return true;
}

bool MonitorUiController::tapOutsideMenuUi()
{
    return closeMenu();
}

bool MonitorUiController::tapFunction(FunctionKey key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked || key == FunctionKey::None) {
        return false;
    }

    if (state_.menu.selectedFunction != key) {
        state_.menu.selectedFunction = key;
        markChangedLocked();
        return true;
    }

    if (!advanceMainPresetLocked(key)) {
        return false;
    }
    markChangedLocked();
    return true;
}

bool MonitorUiController::selectZebraPreset(ZebraPreset preset)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked || state_.assist.zebra == preset) {
        return false;
    }
    state_.assist.zebra = preset;
    markChangedLocked();
    return true;
}

bool MonitorUiController::selectPeakingPreset(PeakingPreset preset)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked || state_.assist.peaking == preset) {
        return false;
    }
    state_.assist.peaking = preset;
    markChangedLocked();
    return true;
}

bool MonitorUiController::selectFalseColorMode(FalseColorMode mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked ||
        (mode == FalseColorMode::HdrNits && !hdrNitsAvailable_) ||
        state_.assist.falseColor == mode) {
        return false;
    }
    state_.assist.falseColor = mode;
    markChangedLocked();
    return true;
}

bool MonitorUiController::selectFrameOff()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked || !state_.assist.frameEnabled) {
        return false;
    }
    state_.assist.frameEnabled = false;
    markChangedLocked();
    return true;
}

bool MonitorUiController::selectFrameAspect(FrameAspect aspect)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked ||
        (state_.assist.frameEnabled &&
         state_.assist.frameAspect == aspect)) {
        return false;
    }
    state_.assist.frameEnabled = true;
    state_.assist.frameAspect = aspect;
    markChangedLocked();
    return true;
}

bool MonitorUiController::toggleCenterCross()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked) {
        return false;
    }
    state_.assist.centerCross = !state_.assist.centerCross;
    markChangedLocked();
    return true;
}

bool MonitorUiController::toggleSafeArea()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.menu.locked) {
        return false;
    }
    state_.assist.safeArea = !state_.assist.safeArea;
    markChangedLocked();
    return true;
}

bool MonitorUiController::advanceMainPresetLocked(FunctionKey key)
{
    switch (key) {
    case FunctionKey::F1Zebra:
        state_.assist.zebra = nextZebraPreset(state_.assist.zebra);
        return true;

    case FunctionKey::F2Peaking:
        state_.assist.peaking = nextPeakingPreset(state_.assist.peaking);
        return true;

    case FunctionKey::F3FalseColor:
        switch (state_.assist.falseColor) {
        case FalseColorMode::Off:
            state_.assist.falseColor = FalseColorMode::VideoLevel;
            break;
        case FalseColorMode::VideoLevel:
            state_.assist.falseColor =
                    hdrNitsAvailable_
                    ? FalseColorMode::HdrNits
                    : FalseColorMode::Off;
            break;
        case FalseColorMode::HdrNits:
        default:
            state_.assist.falseColor = FalseColorMode::Off;
            break;
        }
        return true;

    case FunctionKey::F4Frame:
        if (!state_.assist.frameEnabled) {
            state_.assist.frameEnabled = true;
            state_.assist.frameAspect = FrameAspect::Ratio16x9;
        }
        else if (state_.assist.frameAspect == FrameAspect::Ratio9x16) {
            state_.assist.frameEnabled = false;
        }
        else {
            state_.assist.frameAspect =
                    nextFrameAspect(state_.assist.frameAspect);
        }
        return true;

    case FunctionKey::None:
    default:
        return false;
    }
}

void MonitorUiController::markChangedLocked()
{
    ++revision_;
}

MonitorUiController& monitorUiController()
{
    static MonitorUiController controller;
    return controller;
}

}  // namespace field_monitor
