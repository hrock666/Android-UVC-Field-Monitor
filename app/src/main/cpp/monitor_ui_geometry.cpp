#include "monitor_ui_geometry.h"

#include "monitor_ui_controller.h"

#include <algorithm>

namespace field_monitor {
namespace {

const char* zebraValueText(ZebraPreset preset)
{
    switch (preset) {
    case ZebraPreset::Ire70:
        return "70";
    case ZebraPreset::Ire80:
        return "80";
    case ZebraPreset::Ire90:
        return "90";
    case ZebraPreset::Ire95:
        return "95";
    case ZebraPreset::Ire100:
        return "100";
    case ZebraPreset::Off:
    default:
        return "OFF";
    }
}

const char* peakingValueText(PeakingPreset preset)
{
    switch (preset) {
    case PeakingPreset::Low:
        return "LOW";
    case PeakingPreset::Mid:
        return "MID";
    case PeakingPreset::High:
        return "HIGH";
    case PeakingPreset::MonoLow:
        return "MONO LOW";
    case PeakingPreset::MonoMid:
        return "MONO MID";
    case PeakingPreset::MonoHigh:
        return "MONO HIGH";
    case PeakingPreset::Off:
    default:
        return "OFF";
    }
}

const char* falseColorValueText(FalseColorMode mode)
{
    switch (mode) {
    case FalseColorMode::VideoLevel:
        return "VIDEO";
    case FalseColorMode::HdrNits:
        return "HDR NITS";
    case FalseColorMode::Off:
    default:
        return "OFF";
    }
}

const char* frameValueText(const PreviewAssistState& assist)
{
    if (!assist.frameEnabled) {
        return "OFF";
    }

    switch (assist.frameAspect) {
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
        return "OFF";
    }
}

void addPresetRow(
        MonitorUiGeometry& geometry,
        MonitorPresetAction action,
        const char* text,
        bool selected,
        bool enabled = true)
{
    if (geometry.presetRowCount >= geometry.presetRows.size()) {
        return;
    }

    MonitorPresetRowGeometry& row =
            geometry.presetRows[geometry.presetRowCount++];
    row.action = action;
    row.text = text;
    row.selected = selected;
    row.enabled = enabled;
}

void populatePresetRows(
        MonitorUiGeometry& geometry,
        const MonitorUiSnapshot& snapshot)
{
    const PreviewAssistState& assist = snapshot.state.assist;

    switch (snapshot.state.menu.selectedFunction) {
    case FunctionKey::F1Zebra:
        geometry.presetTitle = "ZEBRA";
        addPresetRow(geometry, MonitorPresetAction::ZebraOff, "OFF",
                     assist.zebra == ZebraPreset::Off);
        addPresetRow(geometry, MonitorPresetAction::Zebra70, "70",
                     assist.zebra == ZebraPreset::Ire70);
        addPresetRow(geometry, MonitorPresetAction::Zebra80, "80",
                     assist.zebra == ZebraPreset::Ire80);
        addPresetRow(geometry, MonitorPresetAction::Zebra90, "90",
                     assist.zebra == ZebraPreset::Ire90);
        addPresetRow(geometry, MonitorPresetAction::Zebra95, "95",
                     assist.zebra == ZebraPreset::Ire95);
        addPresetRow(geometry, MonitorPresetAction::Zebra100, "100",
                     assist.zebra == ZebraPreset::Ire100);
        break;

    case FunctionKey::F2Peaking:
        geometry.presetTitle = "PEAKING";
        addPresetRow(geometry, MonitorPresetAction::PeakingOff, "OFF",
                     assist.peaking == PeakingPreset::Off);
        addPresetRow(geometry, MonitorPresetAction::PeakingLow, "LOW",
                     assist.peaking == PeakingPreset::Low);
        addPresetRow(geometry, MonitorPresetAction::PeakingMid, "MID",
                     assist.peaking == PeakingPreset::Mid);
        addPresetRow(geometry, MonitorPresetAction::PeakingHigh, "HIGH",
                     assist.peaking == PeakingPreset::High);
        addPresetRow(geometry, MonitorPresetAction::PeakingMonoLow, "MONO LOW",
                     assist.peaking == PeakingPreset::MonoLow);
        addPresetRow(geometry, MonitorPresetAction::PeakingMonoMid, "MONO MID",
                     assist.peaking == PeakingPreset::MonoMid);
        addPresetRow(geometry, MonitorPresetAction::PeakingMonoHigh, "MONO HIGH",
                     assist.peaking == PeakingPreset::MonoHigh);
        break;

    case FunctionKey::F3FalseColor:
        geometry.presetTitle = "FALSE COLOR";
        addPresetRow(geometry, MonitorPresetAction::FalseColorOff, "OFF",
                     assist.falseColor == FalseColorMode::Off);
        addPresetRow(geometry, MonitorPresetAction::FalseColorVideo, "VIDEO",
                     assist.falseColor == FalseColorMode::VideoLevel);
        addPresetRow(
                geometry,
                MonitorPresetAction::FalseColorHdrNits,
                "HDR NITS",
                assist.falseColor == FalseColorMode::HdrNits,
                snapshot.hdrNitsAvailable);
        break;

    case FunctionKey::F4Frame:
        geometry.presetTitle = "FRAME";
        addPresetRow(geometry, MonitorPresetAction::FrameOff, "OFF",
                     !assist.frameEnabled);
        addPresetRow(geometry, MonitorPresetAction::Frame16x9, "16:9",
                     assist.frameEnabled &&
                     assist.frameAspect == FrameAspect::Ratio16x9);
        addPresetRow(geometry, MonitorPresetAction::Frame1_85, "1.85",
                     assist.frameEnabled &&
                     assist.frameAspect == FrameAspect::Ratio1_85);
        addPresetRow(geometry, MonitorPresetAction::Frame2_00, "2.00",
                     assist.frameEnabled &&
                     assist.frameAspect == FrameAspect::Ratio2_00);
        addPresetRow(geometry, MonitorPresetAction::Frame2_39, "2.39",
                     assist.frameEnabled &&
                     assist.frameAspect == FrameAspect::Ratio2_39);
        addPresetRow(geometry, MonitorPresetAction::Frame4x3, "4:3",
                     assist.frameEnabled &&
                     assist.frameAspect == FrameAspect::Ratio4x3);
        addPresetRow(geometry, MonitorPresetAction::Frame1x1, "1:1",
                     assist.frameEnabled &&
                     assist.frameAspect == FrameAspect::Ratio1x1);
        addPresetRow(geometry, MonitorPresetAction::Frame9x16, "9:16",
                     assist.frameEnabled &&
                     assist.frameAspect == FrameAspect::Ratio9x16);
        addPresetRow(geometry, MonitorPresetAction::ToggleCross,
                     assist.centerCross ? "CROSS ON" : "CROSS OFF",
                     assist.centerCross);
        addPresetRow(geometry, MonitorPresetAction::ToggleSafe,
                     assist.safeArea ? "SAFE ON" : "SAFE OFF",
                     assist.safeArea);
        break;

    case FunctionKey::None:
    default:
        break;
    }
}

}  // namespace

MonitorUiGeometry calculateMonitorUiGeometry(
        const UiLayout& layout,
        const MonitorUiSnapshot& snapshot)
{
    MonitorUiGeometry geometry{};

    geometry.menuTrigger = {
            layout.inputStatus.x +
                    layout.inputStatus.width - MONITOR_MENU_WIDTH,
            layout.inputStatus.y,
            MONITOR_MENU_WIDTH,
            layout.inputStatus.height,
    };

    geometry.railVisible = snapshot.state.menu.menuOpen;
    geometry.functionRail = {
            geometry.menuTrigger.x + geometry.menuTrigger.width -
                    MONITOR_FUNCTION_RAIL_WIDTH,
            geometry.menuTrigger.y + geometry.menuTrigger.height,
            MONITOR_FUNCTION_RAIL_WIDTH,
            MONITOR_FUNCTION_ROW_HEIGHT * 5,
    };

    const PreviewAssistState& assist = snapshot.state.assist;
    const MonitorMenuState& menu = snapshot.state.menu;
    const bool functionEnabled = !menu.locked;

    geometry.functionButtons[0] = {
            {geometry.functionRail.x,
             geometry.functionRail.y,
             geometry.functionRail.width,
             MONITOR_FUNCTION_ROW_HEIGHT},
            FunctionKey::F1Zebra,
            "F1", "ZEBRA", zebraValueText(assist.zebra),
            menu.selectedFunction == FunctionKey::F1Zebra,
            functionEnabled,
    };
    geometry.functionButtons[1] = {
            {geometry.functionRail.x,
             geometry.functionRail.y + MONITOR_FUNCTION_ROW_HEIGHT,
             geometry.functionRail.width,
             MONITOR_FUNCTION_ROW_HEIGHT},
            FunctionKey::F2Peaking,
            "F2", "PEAK", peakingValueText(assist.peaking),
            menu.selectedFunction == FunctionKey::F2Peaking,
            functionEnabled,
    };
    geometry.functionButtons[2] = {
            {geometry.functionRail.x,
             geometry.functionRail.y + MONITOR_FUNCTION_ROW_HEIGHT * 2,
             geometry.functionRail.width,
             MONITOR_FUNCTION_ROW_HEIGHT},
            FunctionKey::F3FalseColor,
            "F3", "FALSE", falseColorValueText(assist.falseColor),
            menu.selectedFunction == FunctionKey::F3FalseColor,
            functionEnabled,
    };
    geometry.functionButtons[3] = {
            {geometry.functionRail.x,
             geometry.functionRail.y + MONITOR_FUNCTION_ROW_HEIGHT * 3,
             geometry.functionRail.width,
             MONITOR_FUNCTION_ROW_HEIGHT},
            FunctionKey::F4Frame,
            "F4", "FRAME", frameValueText(assist),
            menu.selectedFunction == FunctionKey::F4Frame,
            functionEnabled,
    };
    geometry.lockButton = {
            geometry.functionRail.x,
            geometry.functionRail.y + MONITOR_FUNCTION_ROW_HEIGHT * 4,
            geometry.functionRail.width,
            MONITOR_FUNCTION_ROW_HEIGHT,
    };

    geometry.presetVisible = isPresetMenuOpen(menu);
    if (!geometry.presetVisible) {
        return geometry;
    }

    populatePresetRows(geometry, snapshot);
    if (geometry.presetRowCount == 0) {
        geometry.presetVisible = false;
        return geometry;
    }

    const std::size_t selectedIndex =
            static_cast<std::size_t>(menu.selectedFunction) - 1u;
    const RectI& selectedButton = geometry.functionButtons[selectedIndex].rect;
    const int panelHeight =
            MONITOR_PRESET_TITLE_HEIGHT +
            static_cast<int>(geometry.presetRowCount) *
                    MONITOR_PRESET_ROW_HEIGHT;
    const int desiredY =
            selectedButton.y + selectedButton.height / 2 - panelHeight / 2;
    const int panelY = std::clamp(
            desiredY,
            0,
            std::max(0, layout.canvasHeight - panelHeight));

    geometry.presetPanel = {
            geometry.functionRail.x - MONITOR_PRESET_RAIL_GAP -
                    MONITOR_PRESET_MENU_WIDTH,
            panelY,
            MONITOR_PRESET_MENU_WIDTH,
            panelHeight,
    };
    geometry.presetTitleRect = {
            geometry.presetPanel.x,
            geometry.presetPanel.y,
            geometry.presetPanel.width,
            MONITOR_PRESET_TITLE_HEIGHT,
    };

    for (std::size_t index = 0;
         index < geometry.presetRowCount;
         ++index) {
        geometry.presetRows[index].rect = {
                geometry.presetPanel.x,
                geometry.presetPanel.y + MONITOR_PRESET_TITLE_HEIGHT +
                        static_cast<int>(index) * MONITOR_PRESET_ROW_HEIGHT,
                geometry.presetPanel.width,
                MONITOR_PRESET_ROW_HEIGHT,
        };
    }

    return geometry;
}

bool monitorRectContains(
        const RectI& rect,
        float x,
        float y)
{
    return
            x >= static_cast<float>(rect.x) &&
            y >= static_cast<float>(rect.y) &&
            x < static_cast<float>(rect.x + rect.width) &&
            y < static_cast<float>(rect.y + rect.height);
}

}  // namespace field_monitor
