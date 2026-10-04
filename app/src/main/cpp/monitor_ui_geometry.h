#pragma once

#include "field_monitor_layout.h"
#include "monitor_ui_state.h"

#include <array>
#include <cstddef>

namespace field_monitor {

static constexpr int MONITOR_MENU_WIDTH = 56;
static constexpr int MONITOR_FUNCTION_RAIL_WIDTH = 86;
static constexpr int MONITOR_FUNCTION_ROW_HEIGHT = 56;
static constexpr int MONITOR_PRESET_MENU_WIDTH = 142;
static constexpr int MONITOR_PRESET_TITLE_HEIGHT = 34;
static constexpr int MONITOR_PRESET_ROW_HEIGHT = 32;
static constexpr int MONITOR_PRESET_RAIL_GAP = 2;

enum class MonitorPresetAction {
    None,

    ZebraOff,
    Zebra70,
    Zebra80,
    Zebra90,
    Zebra95,
    Zebra100,

    PeakingOff,
    PeakingLow,
    PeakingMid,
    PeakingHigh,
    PeakingMonoLow,
    PeakingMonoMid,
    PeakingMonoHigh,

    FalseColorOff,
    FalseColorVideo,
    FalseColorHdrNits,

    FrameOff,
    Frame16x9,
    Frame1_85,
    Frame2_00,
    Frame2_39,
    Frame4x3,
    Frame1x1,
    Frame9x16,
    ToggleCross,
    ToggleSafe,
};

struct MonitorFunctionButtonGeometry {
    RectI rect;
    FunctionKey key = FunctionKey::None;
    const char* keyText = "";
    const char* nameText = "";
    const char* valueText = "";
    bool selected = false;
    bool enabled = true;
};

struct MonitorPresetRowGeometry {
    RectI rect;
    MonitorPresetAction action = MonitorPresetAction::None;
    const char* text = "";
    bool selected = false;
    bool enabled = true;
};

struct MonitorUiGeometry {
    RectI menuTrigger;

    bool railVisible = false;
    RectI functionRail;
    std::array<MonitorFunctionButtonGeometry, 4> functionButtons{};
    RectI lockButton;

    bool presetVisible = false;
    RectI presetPanel;
    RectI presetTitleRect;
    const char* presetTitle = "";
    std::array<MonitorPresetRowGeometry, 10> presetRows{};
    std::size_t presetRowCount = 0;
};

MonitorUiGeometry calculateMonitorUiGeometry(
        const UiLayout& layout,
        const MonitorUiSnapshot& snapshot);

bool monitorRectContains(
        const RectI& rect,
        float x,
        float y);

}  // namespace field_monitor
