# Android UVC Field Monitor
## UI v5 実装仕様書 + Preview Assist Shader 疑似コード

Version: UI v5
Basis:
- `field_monitor_ui_mock_menu_header_v5.html`（UI v5確定モック）
- `field_monitor_layout.h`
- `scope_ui.cpp`
- `scope_ui.h`

---

# 1. 目的

本仕様は、既存の Android UVC Field Monitor に以下のフィールドモニター機能を追加するための実装仕様である。

- MENU / F1-F4 / LOCK 操作UI
- F1 Zebra
- F2 Peaking
  - LOW / MID / HIGH
  - MONO LOW / MONO MID / MONO HIGH
- F3 False Color
  - OFF / VIDEO / HDR NITS
- F4 Frame
  - OFF
  - 16:9
  - 1.85
  - 2.00
  - 2.39
  - 4:3
  - 1:1
  - 9:16
  - CENTER CROSS ON/OFF
  - SAFE AREA ON/OFF
- Frame OFF時の完全CLEAN Preview

Waveform / RGB Parade / Histogram / Vectorscopeの集計構造とgeometryは維持する。表示密度のみ、Phase UI-6の確定gainとVectorscope exact 1-bin表示を適用する。

---

# 2. 実装原則

## 2.1 既存Scope描画を壊さない

既存 `ScopeUi` は以下を保持する。

- Waveform
- RGB Parade
- Histogram
- Vectorscope
- CAL header
- runtime status
- scope grid / labels / border

Scope用SSBO、maxima SSBO、scope fragment shaderのデータ構造は変更しない。

表示設定はWaveform gain `1.5`、RGB Parade gain `1.5`、Vectorscope gain `1.75`とする。gainは平方根密度変換後に適用して0〜1へclampする。Vectorscopeは3×3 expansionを行わずexact 1-binで描画し、Histogramは変更しない。

---

## 2.2 Preview Assistは追加FBOを基本的に作らない

Zebra / Peaking / Mono / False Color は Preview fragment shader の1パス内で処理する。

基本構造:

```text
Capture texture(s)
    ↓
既存 Capture Calibration / Signal Processing
    ↓
既存 Preview RGB生成
    ↓
Preview Assist
    ├─ False Color
    ├─ Mono
    ├─ Zebra
    └─ Peaking
    ↓
Preview output
```

Frame Guide / Center Cross / Safe Area / MENU UI は画素処理ではないため、既存 `ScopeUi::drawOverlay()` 系の矩形・文字描画で行う。

---

## 2.3 UIは不透明

MENU系UIでは透過処理を使用しない。

```cpp
BLACK = {0.0f, 0.0f, 0.0f, 1.0f};
WHITE = {1.0f, 1.0f, 1.0f, 1.0f};
```

通常状態:

```text
背景  BLACK
文字  WHITE
枠    WHITE
```

選択状態:

```text
背景  WHITE
文字  BLACK
枠    WHITE
```

プリセット選択状態は白黒反転のみで示す。

`SELECTED` 文字は表示しない。

LOCK中のFキーをdim表示する場合のみ補助色を使用してよい。

---

# 3. 現行レイアウト

`field_monitor_layout(1).h` をSource of Truthとする。

## 3.1 Landscape

Logical canvas:

```text
1280 x 454
```

```text
inputStatus    = {   0,   0, 711,  28 }
preview        = {   0,  28, 711, 400 }
runtimeStatus  = {   0, 428, 711,  26 }

waveform       = { 711,  28, 330, 200 }
parade         = { 711, 228, 330, 200 }
histogram      = {1041, 228, 239, 200 }
vectorscope    = {1041,  28, 239, 200 }
```

配置:

```text
┌─────────────────────────────┬──────────────────┬─────────────┐
│ CAL / STATUS / MENU         │                  │             │
├─────────────────────────────┤ WAVEFORM         │ VECTOR      │
│                             │                  │             │
│          PREVIEW            ├──────────────────┼─────────────┤
│                             │ RGB PARADE       │ HISTOGRAM   │
│                             │                  │             │
├─────────────────────────────┤                  │             │
│ ASSIST STATUS / UI FPS      │                  │             │
└─────────────────────────────┴──────────────────┴─────────────┘
```

---

## 3.2 Portrait

Logical canvas:

```text
710 x 796
```

```text
inputStatus    = {  0,   0, 710,  28 }
preview        = { 36,  28, 638, 359 }
runtimeStatus  = {  0, 387, 710,  22 }

waveform       = {  0, 409, 413, 193 }
parade         = {  0, 602, 413, 194 }
histogram      = {413, 602, 297, 194 }
vectorscope    = {413, 409, 297, 193 }
```

配置:

```text
┌──────────────────────────────────────┐
│ CAL / STATUS                    MENU │
├──────────────────────────────────────┤
│               PREVIEW                │
├──────────────────────────────────────┤
│ ASSIST STATUS / UI FPS               │
├───────────────────────┬──────────────┤
│ WAVEFORM              │ VECTOR       │
├───────────────────────┼──────────────┤
│ RGB PARADE            │ HISTOGRAM    │
└───────────────────────┴──────────────┘
```

---

# 4. MENU UI仕様

## 4.1 MENUトリガー

MENUは `inputStatus` の右上に配置する。

logical geometry:

```cpp
constexpr int MENU_W = 56;

menuRect.x      = inputStatus.x + inputStatus.width - MENU_W;
menuRect.y      = inputStatus.y;
menuRect.width  = MENU_W;
menuRect.height = inputStatus.height;
```

表示:

```text
MENU
```

展開中:

```text
CLOSE
```

背景:

```text
BLACK
```

文字・枠:

```text
WHITE
```

---

## 4.2 Function Rail

MENUタップ時のみ表示する。

構成:

```text
┌──────────┐
│ F1       │
│ ZEBRA    │
│ 70       │
├──────────┤
│ F2       │
│ PEAK     │
│ MID      │
├──────────┤
│ F3       │
│ FALSE    │
│ OFF      │
├──────────┤
│ F4       │
│ FRAME    │
│ 16:9     │
├──────────┤
│ LOCK     │
│ ON       │
└──────────┘
```

推奨logical width:

```cpp
FUNCTION_RAIL_W = 86;
```

位置:

```cpp
rail.right = menuRect.right;
rail.top   = menuRect.bottom;
```

---

# 5. LOCK仕様

起動時デフォルト:

```cpp
locked = true;
```

LOCK ON:

- F1-F4タップは設定変更しない
- preset menuを開かない
- MENU自体は開閉可能
- LOCKボタンのみ操作可能

LOCK OFF:

- F1-F4操作可能

LOCKをONにした瞬間:

```cpp
selectedFunction = NONE;
presetMenuOpen = false;
```

---

# 6. Fキー共通操作

## 6.1 初回タップ

例:

```text
F2 tap
```

動作:

```text
selectedFunction = F2
preset menuをF2の左側へ表示
現在の設定値は変更しない
```

---

## 6.2 同じFキーを再タップ

現在のFキーが選択中の場合:

```text
次の主プリセットへ進む
```

例:

```text
F2:
OFF
 ↓
LOW
 ↓
MID
 ↓
HIGH
 ↓
MONO LOW
 ↓
MONO MID
 ↓
MONO HIGH
 ↓
OFF
```

F4では `CROSS` / `SAFE` は再タップcycleに含めない。

```text
OFF
 ↓
16:9
 ↓
1.85
 ↓
2.00
 ↓
2.39
 ↓
4:3
 ↓
1:1
 ↓
9:16
 ↓
OFF
```

---

## 6.3 Preset直接タップ

Preset行をタップすると即時適用する。

Preset menuは閉じない。

選択状態:

```text
WHITE background
BLACK text
```

`SELECTED` 表記は使用しない。

---

## 6.4 MENU外タップ

MENU展開中にUI外をタップした場合:

```cpp
menuOpen = false;
selectedFunction = NONE;
```

Assist設定自体は維持する。

---

## 6.5 Runtime Assist Status

`runtimeStatus` 左側は固定文字列 `CLEAN` ではなく、**現在Previewに実際に掛かっているAssist状態**を簡易表示する。

この表示は「保存されている設定値一覧」ではない。

### 基本原則

```text
表示対象 = 現在のPreview出力に視覚的に反映されている機能
```

固定順序:

```text
F1 Zebra
F2 Peaking
F3 False Color
F4 Frame
```

OFF項目は表示しない。

MENU / LOCK状態は表示しない。

状態変更時は即時更新する。

### 短縮表記

```text
Zebra
Z70
Z80
Z90
Z95
Z100

Peaking
P LOW
P MID
P HIGH
P MONO LOW
P MONO MID
P MONO HIGH

False Color
FC

Frame
F 16:9
F 1.85
F 2.00
F 2.39
F 4:3
F 1:1
F 9:16
```

FrameでCenter CrossがON:

```text
C
```

Safe AreaがON:

```text
S
```

をFrame表記の後ろへ追加する。

例:

```text
F 16:9 C S
F 2.39 C
F 4:3 S
```

### CLEAN条件

現在Previewに表示されるAssistが1つもない場合のみ:

```text
CLEAN
```

を表示する。

例:

```text
Zebra       OFF
Peaking     OFF
False Color OFF
Frame       OFF
```

なら:

```text
CLEAN
```

### False Color優先時

False Color ON時は、Zebra / Peaking の保存状態は維持するが、Preview上ではそれらを表示しない。

したがってRuntime Assist StatusからもZebra / Peakingを一時的に除外する。

例:

保存状態:

```text
Zebra       90
Peaking     MID
False Color ON
Frame       16:9
Cross       ON
Safe        ON
```

実際のPreview表示:

```text
False Color
Frame 16:9
Cross
Safe
```

Runtime Assist Status:

```text
FC  F 16:9 C S
```

False ColorをOFFに戻すと:

```text
Z90  P MID  F 16:9 C S
```

へ復帰する。

### 実装上の意味

表示文字列は raw state から直接生成せず、まず **Effective Preview State** を解決してから生成する。

```cpp
struct EffectivePreviewState {
    bool zebraVisible;
    ZebraPreset zebra;

    bool peakingVisible;
    PeakingPreset peaking;

    bool falseColorVisible;

    bool frameVisible;
    FrameAspect frameAspect;
    bool centerCrossVisible;
    bool safeAreaVisible;
};
```

概念:

```cpp
EffectivePreviewState resolveEffectivePreviewState(
    const PreviewAssistState& state)
{
    EffectivePreviewState out{};

    out.falseColorVisible =
        state.falseColor;

    // False ColorがPreview画素表示を置換するため、
    // Zebra / Peakingは表示上抑制される。
    out.zebraVisible =
        !state.falseColor &&
        state.zebra != ZebraPreset::Off;

    out.peakingVisible =
        !state.falseColor &&
        state.peaking != PeakingPreset::Off;

    out.frameVisible =
        state.frameEnabled;

    out.frameAspect =
        state.frameAspect;

    out.centerCrossVisible =
        state.frameEnabled &&
        state.centerCross;

    out.safeAreaVisible =
        state.frameEnabled &&
        state.safeArea;

    return out;
}
```

その後:

```cpp
std::string buildRuntimeAssistText(
    const EffectivePreviewState& effective);
```

で表示文字列を生成する。

---

# 7. Preset Menu寸法

v5基準:

```cpp
PRESET_MENU_W = 142;
PRESET_TITLE_H = 34;
PRESET_ROW_H = 32;
```

位置:

```cpp
menu.right = functionRail.left - gap;
```

縦位置:

- 原則として選択Fキー中央へ合わせる
- logical canvas上端・下端からはみ出さないようclampする

---

# 8. 状態モデル

UIのbool引数を `drawOverlay()` に増殖させない。

新規状態型を推奨する。

```cpp
enum class ZebraPreset {
    Off,
    Ire70,
    Ire80,
    Ire90,
    Ire95,
    Ire100
};

enum class PeakingPreset {
    Off,
    Low,
    Mid,
    High,
    MonoLow,
    MonoMid,
    MonoHigh
};

enum class FrameAspect {
    Ratio16x9,
    Ratio1_85,
    Ratio2_00,
    Ratio2_39,
    Ratio4x3,
    Ratio1x1,
    Ratio9x16
};

enum class FunctionKey {
    None,
    F1Zebra,
    F2Peaking,
    F3FalseColor,
    F4Frame
};

struct PreviewAssistState {
    ZebraPreset zebra = ZebraPreset::Off;
    PeakingPreset peaking = PeakingPreset::Off;
    bool falseColor = false;

    bool frameEnabled = false;
    FrameAspect frameAspect = FrameAspect::Ratio2_39;

    // OFF時も値を保持する。
    bool centerCross = true;
    bool safeArea = true;
};

struct MonitorMenuState {
    bool menuOpen = false;
    bool locked = true;
    FunctionKey selectedFunction = FunctionKey::None;
};
```

Frame OFFでは:

```cpp
frameEnabled == false
```

であり、内部の `frameAspect / centerCross / safeArea` は保持してよい。

OFFは描画マスクであり、保存済み設定を破棄しない。

---

# 9. F1 Zebra仕様

Preset:

```text
OFF
70
80
90
95
100
```

UI表示値:

```text
F1
ZEBRA
90
```

OFF:

```text
Zebra処理なし
```

ON:

```text
対象画素に斜線patternを表示
```

## 9.1 判定パラメータ

70 / 80 / 90 / 95は中心値±3 IRE、100は100 IRE以上を判定する。Stage 3 source video levelをFULLまたはLIMITEDから0〜100 IREへ正規化し、SDR / PQで共通tableを使用する。

```cpp
struct ZebraShaderParams {
    bool enabled;
    float thresholdLow;
    float thresholdHigh;
};
```

PQ EOTFとTone MappingはZebra判定へ適用しない。

---

# 10. F2 Peaking仕様

Preset:

```text
OFF
LOW
MID
HIGH
MONO LOW
MONO MID
MONO HIGH
```

通常:

```text
元Preview RGB + edge highlight
```

MONO:

```text
Preview background = grayscale
edge highlight = color
```

MONOはPeaking ONの派生モードであり、独立機能にはしない。

状態の分解:

```cpp
bool peakingEnabled;
bool peakingMono;
PeakingSensitivity sensitivity;
```

例:

```text
MONO MID

peakingEnabled = true
peakingMono = true
sensitivity = MID
```

## 10.1 判定パラメータ

4-neighbor gradientへLOW `0.20`、MID `0.12`、HIGH `0.06`を適用し、SDR / PQで共通tableを使用する。edge highlight色は赤とする。

---

# 11. F3 False Color仕様

Preset:

```text
OFF
VIDEO
HDR NITS
```

VIDEOまたはHDR NITS選択時はPreviewをFalse Color表示へ置換する。HDR NITSはPQ Profile時のみ選択可能とする。

v5ではFalse Color ON時、Zebra / Peakingの画素overlayは表示しない。

状態値は保持する。

表示優先順位:

```text
False Color
    >
normal / mono preview + Zebra + Peaking
```

## 11.1 判定パラメータ

VIDEOはRange正規化したStage 3 source video levelを10 bandへ、HDR NITSはStage 3 PQ codeへPQ EOTFを適用したabsolute nitsを11 bandへ割り当てる。境界とpaletteは実装計画書D-08〜D-11を正とする。

---

# 12. F4 Frame仕様

Preset Menu:

```text
OFF
16:9
1.85
2.00
2.39
4:3
1:1
9:16
CROSS ON/OFF
SAFE  ON/OFF
```

---

## 12.1 OFF

Frame OFF時はPreviewを完全CLEANにする。

描画しない:

- Aspect frame
- Center Cross
- Safe Area

つまり:

```cpp
if (!frameEnabled) {
    drawNoFrameOverlay();
}
```

現行 `scope_ui(1).cpp` のCenter Cross / 90% Safeの無条件描画は削除し、`frameEnabled` の条件下へ移動する。

---

## 12.2 16:9

16:9はPreviewと同一aspectでも有効presetとして保持する。

目的:

```text
16:9のまま
Center Cross
Safe Area
を使用可能にする
```

16:9 Frame自体はPreview外周と一致してよい。

---

## 12.3 Center Cross

Frame ON中のみ描画する。

v5基準:

```text
横 25 logical px
縦 25 logical px
中心1px
WHITE
alpha = 1.0
```

現行コードの `mx ± 12`, `my ± 12` を利用可能。

---

## 12.4 Safe Area

Frame ON中かつ `safeArea == true` のとき描画する。

現行コードとの互換を維持し:

```text
90% width
90% height
中央配置
```

とする。

```cpp
safeX = preview.x + round(preview.width  * 0.05);
safeY = preview.y + round(preview.height * 0.05);
safeW = round(preview.width  * 0.90);
safeH = round(preview.height * 0.90);
```

WHITE / alpha 1.0。

---

# 13. Frame Aspect geometry

Preview rect:

```cpp
RectI preview;
```

Target aspect:

```cpp
targetAspect = frameWidth / frameHeight;
previewAspect = preview.width / preview.height;
```

Pseudo:

```cpp
RectF calculateFrameRect(RectF preview, float targetAspect)
{
    float previewAspect = preview.width / preview.height;

    RectF frame = preview;

    if (targetAspect < previewAspect) {
        // pillar-box guide
        frame.height = preview.height;
        frame.width = preview.height * targetAspect;
        frame.x = preview.x + (preview.width - frame.width) * 0.5;
    }
    else {
        // letter-box guide
        frame.width = preview.width;
        frame.height = preview.width / targetAspect;
        frame.y = preview.y + (preview.height - frame.height) * 0.5;
    }

    return frame;
}
```

Aspect table:

```cpp
16:9  = 16.0 / 9.0
1.85  = 1.85
2.00  = 2.00
2.39  = 2.39
4:3   = 4.0 / 3.0
1:1   = 1.0
9:16  = 9.0 / 16.0
```

---

# 14. drawOverlay()変更案

`runtimeStatus` 左側は固定 `CLEAN` を廃止し、`EffectivePreviewState` から生成したAssist summaryを描画する。

概念:

```cpp
const EffectivePreviewState effective =
    resolveEffectivePreviewState(assist);

const std::string runtimeAssistText =
    buildRuntimeAssistText(effective);

addText(
    runtimeStatus.x + 12,
    runtimeStatus.y + 7,
    runtimeAssistText,
    previewText,
    1
);
```

右側の `UI xx.x FPS` は現状維持する。

現行:

```cpp
void ScopeUi::drawOverlay(
    double currentUiFps,
    EGLint surfaceWidth,
    EGLint surfaceHeight,
    const UiLayout& layout,
    const UiCanvasViewport& canvas,
    bool calibrationEnabled,
    int calibrationWidth,
    int calibrationHeight,
    bool calibrationLimited,
    int vectorColorimetry,
    GLuint vao);
```

推奨:

```cpp
void ScopeUi::drawOverlay(
    double currentUiFps,
    EGLint surfaceWidth,
    EGLint surfaceHeight,
    const UiLayout& layout,
    const UiCanvasViewport& canvas,
    bool calibrationEnabled,
    int calibrationWidth,
    int calibrationHeight,
    bool calibrationLimited,
    int vectorColorimetry,
    const PreviewAssistState& assist,
    const MonitorMenuState& menu,
    GLuint vao);
```

または引数増殖を避けるため:

```cpp
struct MonitorUiRenderState {
    PreviewAssistState assist;
    MonitorMenuState menu;
};

void ScopeUi::drawOverlay(
    ...
    const MonitorUiRenderState& uiState,
    GLuint vao);
```

を推奨する。

---

# 15. MENU描画疑似コード

```cpp
void drawMenuTrigger(
    const RectI& inputStatus,
    const MonitorMenuState& state)
{
    RectI menuRect;

    menuRect.width  = 56;
    menuRect.height = inputStatus.height;
    menuRect.x =
        inputStatus.x +
        inputStatus.width -
        menuRect.width;
    menuRect.y = inputStatus.y;

    addFilledRect(menuRect, BLACK);
    addBorder(menuRect, WHITE);

    addCenteredText(
        menuRect,
        state.menuOpen ? "CLOSE" : "MENU",
        WHITE
    );
}
```

---

# 16. Function Rail描画疑似コード

```cpp
if (!menu.menuOpen) {
    return;
}

RectI rail;

rail.width = 86;
rail.x = menuRect.right - rail.width;
rail.y = menuRect.bottom;

drawOpaqueBlackPanel(rail);

drawFunctionButton(F1, "ZEBRA", zebraDisplayValue);
drawFunctionButton(F2, "PEAK",  peakingDisplayValue);
drawFunctionButton(F3, "FALSE", falseColorDisplayValue);
drawFunctionButton(F4, "FRAME", frameDisplayValue);
drawLockButton();
```

Function selected:

```cpp
background = WHITE;
text = BLACK;
```

Normal:

```cpp
background = BLACK;
text = WHITE;
```

---

# 17. Preset Menu描画疑似コード

```cpp
if (menu.locked) {
    return;
}

if (menu.selectedFunction == NONE) {
    return;
}

RectI presetMenu;

presetMenu.width = 142;
presetMenu.right =
    functionRail.left - GAP;

presetMenu.y =
    clamp(
        selectedButton.centerY -
        presetMenu.height / 2,
        canvas.top,
        canvas.bottom - presetMenu.height
    );

drawBlackPanel(presetMenu);

for (Preset preset : currentPresets) {

    bool selected =
        isPresetSelected(preset);

    if (selected) {
        drawWhiteRow();
        drawBlackText();
    }
    else {
        drawBlackRow();
        drawWhiteText();
    }
}
```

`SELECTED`文字は追加しない。

---

# 18. Touch座標変換

Android Surface物理座標をlogical UI座標へ変換する。

```cpp
bool surfaceToLogical(
    float surfaceX,
    float surfaceY,
    const UiCanvasViewport& canvas,
    float& logicalX,
    float& logicalY)
{
    if (surfaceX < canvas.x ||
        surfaceY < canvasSurfaceTop ||
        surfaceX >= canvas.x + canvas.width ||
        surfaceY >= canvasSurfaceTop + canvas.height) {
        return false;
    }

    logicalX =
        (surfaceX - canvas.x) /
        canvas.scale;

    logicalY =
        (surfaceY - canvasSurfaceTop) /
        canvas.scale;

    return true;
}
```

Y座標についてはAndroid inputがtop-origin、OpenGL viewportがbottom-originであるため、既存座標系との混同を避ける。

UI hit test自体は `UiLayout` と同じtop-origin logical座標で統一する。

---

# 19. UIイベント状態遷移

## MENU

```cpp
onMenuTap()
{
    menuOpen = !menuOpen;

    if (!menuOpen) {
        selectedFunction = NONE;
    }
}
```

## LOCK

```cpp
onLockTap()
{
    locked = !locked;

    if (locked) {
        selectedFunction = NONE;
    }
}
```

## F1-F4

```cpp
onFunctionTap(key)
{
    if (locked) {
        return;
    }

    if (selectedFunction != key) {
        selectedFunction = key;
        return;
    }

    advanceMainPreset(key);
}
```

## Preset

```cpp
onPresetTap(key, preset)
{
    if (locked) {
        return;
    }

    applyPreset(key, preset);
}
```

---

# 20. FrameのCROSS / SAFE操作

`CROSS` と `SAFE` はAspect Presetではなくsub-toggleとして扱う。

推奨実装意味論:

```cpp
CROSS:
    centerCross = !centerCross

SAFE:
    safeArea = !safeArea
```

Frame OFF時はこれらの保存値に関係なく描画しない。

Aspect presetを選択した場合:

```cpp
frameEnabled = true;
frameAspect = selectedAspect;
```

OFF:

```cpp
frameEnabled = false;
```

Frame OFF→ON時には以前の `centerCross / safeArea` 状態を復帰する。

---

# 21. Preview Assist Shader interface

UI enumをfragment shaderへ直接渡さず、render時に解決したパラメータを渡す。

例:

```cpp
enum class FalseColorDomain {
    VideoLevel,
    HdrNits
};

struct PreviewAssistGpuParams {
    int zebraEnabled;
    float zebraLow;
    float zebraHigh;

    int peakingEnabled;
    int peakingMono;
    float peakingThreshold;
    vec3 peakingColor;

    int falseColorEnabled;
    FalseColorDomain falseColorDomain;
};
```

GL uniform例:

```glsl
uniform int   uZebraEnabled;
uniform float uZebraLow;
uniform float uZebraHigh;

uniform int   uPeakingEnabled;
uniform int   uPeakingMono;
uniform float uPeakingThreshold;
uniform vec3  uPeakingColor;

uniform int   uFalseColorEnabled;

// 0 = VideoLevel
// 1 = HdrNits
uniform int   uFalseColorDomain;
```

初期状態では:

```cpp
falseColorEnabled = false;
```

F3からVIDEO / HDR NITSを手動選択する。非PQ入力ではHDR NITSを選択不可とし、使用中にPQ Profileを解除した場合はVIDEOへ復帰する。

---

# 22. Preview Assist Analysis Tap / Domain

Assist解析の共通tap pointは **Capture Calibration直後のStage 3** に固定する。

```text
Stage 3
Calibrated Encoded R'G'B'
```

を以下の共通analysis sourceとする。

```text
Stage 3
   │
   ├─ Zebra
   ├─ False Color
   └─ Peaking edge analysis
```

Tone Mapping、Gamut Conversion、Output Transfer、Optional User 3D LUTの結果をAssist判定へ使用しない。

これにより、

```text
User LUT変更
Tone Mapping変更
Preview look変更
```

によってZebra / False Color / Peaking判定そのものが変化しないようにする。

shader設計上は:

```glsl
vec3 analysisRgb;  // Stage 3
vec3 displayRgb;   // 最終Preview表示用
```

を明確に分離する。

```text
analysisRgb
    ├─ Zebra
    ├─ False Color
    └─ Peaking edge detection

displayRgb
    ├─ Normal Preview
    └─ Peaking MONO background
```

---

## 22.1 Zebra Domain

ZebraはStage 3のsource video levelを評価する。

```text
Stage 3 Encoded R'G'B'
        ↓
source luma / video level
        ↓
Zebra threshold test
```

初期実装ではHDR/SDRともtap pointを変更しない。

threshold semantics:

```text
>= threshold
or
window / band
```

は定数・モード設定として別途確定する。

---

## 22.2 Peaking Domain

Peaking edge detectionもStage 3を使用する。

```text
Stage 3
   ↓
source luma
   ↓
spatial gradient
   ↓
edge strength
```

User LUT / Tone Mapping後のdisplay contrastはedge判定へ使用しない。

ただし `MONO LOW / MID / HIGH` の背景生成だけは最終 `displayRgb` を使用する。

```text
edge analysis
    = Stage 3

MONO background
    = final displayRgb → grayscale
```

したがってMONOは、

```text
Stage 3でフォーカス解析
+
普段のPreview表示を白黒化
```

という意味になる。

---

## 22.3 False Color Domain

False Colorは2種類のdomainを設計上サポート可能にする。

```cpp
enum class FalseColorDomain {
    VideoLevel,
    HdrNits
};
```

### VideoLevel

SDR / PQ共通のsource-level domain。

```text
Stage 3
Calibrated Encoded R'G'B'
        ↓
source luma / video level
        ↓
False Color table
```

SDRおよび通常のsource-level False Colorはこのdomainを使用する。

### HdrNits

HDR PQ入力用domain。

tap point自体はStage 3のままとし、False Color専用analysis branchだけでPQ EOTFを適用する。

```text
Stage 3
Calibrated PQ Encoded R'G'B'
        ↓
analysis-only PQ EOTF
        ↓
absolute linear luminance
[nits]
        ↓
HDR NITS False Color table
```

このbranchには以下を入れない。

```text
Tone Mapping
Gamut Conversion for Preview
Output Transfer Encode
Optional User 3D LUT
```

つまり:

```text
NG
Stage 3
 ↓
PQ EOTF
 ↓
Tone Mapping
 ↓
False Color
```

ではなく、

```text
OK
Stage 3
 ↓
PQ EOTF
 ↓
absolute nits
 ↓
False Color
```

とする。

---

## 22.4 HDR NITS False Colorの設計境界

HDR NITS False Colorは **Preview変換機能ではなくanalysis機能** とする。

目的:

```text
PQ code value
        ↓
absolute scene/display luminance representation
        ↓
100 nit
203 nit
400 nit
1000 nit
4000 nit
...
```

等の絶対輝度bandへ割り当てること。

False Color ON時の表示は最終Previewを置換するが、判定値はPreview display pipelineから取得しない。

概念:

```glsl
float falseColorMetric;

if (uFalseColorDomain == FALSE_COLOR_VIDEO_LEVEL) {
    falseColorMetric =
        sourceVideoLevel(analysisRgb);
}
else {
    falseColorMetric =
        pqToAbsoluteNits(
            sourcePqLevel(analysisRgb)
        );
}

vec3 fc =
    falseColorMap(
        falseColorMetric,
        uFalseColorDomain
    );
```

---

## 22.5 Domain Contract

最終契約:

| Assist | Tap Point | Analysis Domain | Display Base |
|---|---|---|---|
| Zebra | Stage 3 | Source video level | final Preview |
| Peaking | Stage 3 | Source luma spatial gradient | final Preview |
| Peaking MONO | Stage 3 | Source luma spatial gradient | final Preview grayscale |
| False Color / VideoLevel | Stage 3 | Source video level | False Color replacement |
| False Color / HdrNits | Stage 3 | PQ EOTF → absolute nits | False Color replacement |
| Frame/Cross/Safe | none | none | UI overlay |

Stage 3以外をAssist tapとして選択する設定は初期実装では持たない。


---

# 23. Preview Fragment Shader 疑似コード

以下は構造疑似コードであり、確定GLSLではない。

```glsl
fragment()
{
    uv = getPreviewUv();

    // Existing capture/preview path.
    SourceSample src = sampleCapture(uv);

    // Existing calibration / HDR-SDR / color pipeline.
    vec3 analysisRgb = buildAnalysisRgb(src);
    vec3 displayRgb  = buildDisplayRgb(src);

    float analysisY = luma(analysisRgb);

    // -------------------------------------------------
    // 1. False Color has highest visual priority.
    // -------------------------------------------------

    if (uFalseColorEnabled != 0) {

        float falseColorMetric;

        if (uFalseColorDomain ==
            FALSE_COLOR_HDR_NITS) {

            // Stage 3 PQ code-domain source only.
            // Do NOT pass Tone Mapping / User LUT.
            falseColorMetric =
                pqToAbsoluteNits(
                    analysisY
                );
        }
        else {

            falseColorMetric =
                analysisY;
        }

        vec3 falseColor =
            falseColorMap(
                falseColorMetric,
                uFalseColorDomain
            );

        outputColor =
            vec4(falseColor, 1.0);

        return;
    }

    // -------------------------------------------------
    // 2. Base preview / MONO
    // -------------------------------------------------

    vec3 baseRgb = displayRgb;

    if (uPeakingEnabled != 0 &&
        uPeakingMono != 0) {

        float displayY =
            luma(displayRgb);

        baseRgb =
            vec3(displayY);
    }

    // -------------------------------------------------
    // 3. Zebra
    // -------------------------------------------------

    if (uZebraEnabled != 0) {

        bool zebraHit =
            analysisY >= uZebraLow &&
            analysisY <= uZebraHigh;

        if (zebraHit) {

            int stripeCoord =
                int(gl_FragCoord.x) +
                int(gl_FragCoord.y);

            bool stripe =
                (stripeCoord % ZEBRA_PERIOD)
                < ZEBRA_WIDTH;

            if (stripe) {
                baseRgb = vec3(1.0);
            }
        }
    }

    // -------------------------------------------------
    // 4. Peaking
    // -------------------------------------------------

    if (uPeakingEnabled != 0) {

        float edge =
            computePeakingEdge(uv);

        if (edge >= uPeakingThreshold) {
            baseRgb =
                uPeakingColor;
        }
    }

    outputColor =
        vec4(baseRgb, 1.0);
}
```

---

# 24. Peaking edge detector 疑似コード

低遅延優先で、最初は4-neighbor gradientを推奨する。

```glsl
float sampleAnalysisLuma(vec2 uv)
{
    vec3 rgb =
        buildAnalysisRgb(
            sampleCapture(uv)
        );

    return luma(rgb);
}


float computePeakingEdge(vec2 uv)
{
    vec2 dx =
        vec2(texelSize.x, 0.0);

    vec2 dy =
        vec2(0.0, texelSize.y);

    float left  =
        sampleAnalysisLuma(uv - dx);

    float right =
        sampleAnalysisLuma(uv + dx);

    float up =
        sampleAnalysisLuma(uv - dy);

    float down =
        sampleAnalysisLuma(uv + dy);

    float gx =
        abs(right - left);

    float gy =
        abs(down - up);

    return gx + gy;
}
```

メリット:

- Sobel 3x3よりsample数を抑えやすい
- threshold調整が単純
- LOW/MID/HIGHをuniformだけで切替可能
- 追加FBO不要

Y planeを直接利用できるPreview pathでは、RGB再構築せずYからedgeを取る最適化を検討可能。

---

# 25. Peaking preset resolution

CPU側でpresetをGPU paramへ変換する。

```cpp
switch (state.peaking) {

case Off:
    enabled = false;
    break;

case Low:
    enabled = true;
    mono = false;
    threshold = 0.20;
    break;

case Mid:
    enabled = true;
    mono = false;
    threshold = 0.12;
    break;

case High:
    enabled = true;
    mono = false;
    threshold = 0.06;
    break;

case MonoLow:
    enabled = true;
    mono = true;
    threshold = 0.20;
    break;

case MonoMid:
    enabled = true;
    mono = true;
    threshold = 0.12;
    break;

case MonoHigh:
    enabled = true;
    mono = true;
    threshold = 0.06;
    break;
}
```

---

# 26. Zebra shader helper 疑似コード

判定windowはUI stateから分離してshaderへ渡す。

```glsl
bool zebraTest(float y)
{
    return
        y >= uZebraLow &&
        y <= uZebraHigh;
}
```

CPU側:

```cpp
ZebraShaderParams resolveZebraPreset(
    ZebraPreset preset,
    SignalRange range,
    SignalDomain domain);
```

70 / 80 / 90 / 95は±3 IRE、100は100 IRE以上へ解決する。FULL / LIMITEDは共通IRE domainへ正規化する。

---

# 27. False Color shader helper 疑似コード

False Colorはdomain別テーブルを使用する。

```glsl
vec3 falseColorMapVideoLevel(float level)
{
    if (level < band0.max) return band0.color;
    if (level < band1.max) return band1.color;
    if (level < band2.max) return band2.color;
    ...
    return lastBand.color;
}
```

HDR NITS:

```glsl
vec3 falseColorMapHdrNits(float nits)
{
    if (nits < nitBand0.max) return nitBand0.color;
    if (nits < nitBand1.max) return nitBand1.color;
    if (nits < nitBand2.max) return nitBand2.color;
    ...
    return lastNitBand.color;
}
```

dispatcher:

```glsl
vec3 falseColorMap(
    float metric,
    int domain)
{
    if (domain == FALSE_COLOR_HDR_NITS) {
        return falseColorMapHdrNits(metric);
    }

    return falseColorMapVideoLevel(metric);
}
```

PQ EOTFはFalse Color専用analysis helperとして分離する。

```glsl
float pqToAbsoluteNits(float pqCode)
{
    // ST2084 EOTF
    // normalized PQ code → 0 ... 10000 nit
    //
    // exact constants / precision implementation
    // は既存HDR signal processing実装と共通化する。
}
```

重要:

```text
HDR NITS False Color
= Stage 3 PQ code
  → PQ EOTF
  → absolute nits
  → False Color table
```

であり、

```text
Preview Tone Mapping後RGB
```

からnitsを逆算しない。

VideoLevel / HDR NITSともshader内の固定tableを使用し、境界とpaletteは実装計画書D-08〜D-11へ合わせる。


---

# 28. Frame Guideはshaderへ入れない

Frame Guide / Cross / SafeはPreview textureの画素加工ではなく、既存UI geometryへ追加する。

理由:

- 現行 `ScopeUi` に `addRect()` が存在する
- Center Cross / Safe Areaは既に同方式で実装済み
- aspect lineは4本の矩形だけで描ける
- shader uniform / branchを増やさない
- Preview assist shaderからUI geometryを分離できる

Pseudo:

```cpp
if (assist.frameEnabled) {

    RectF frame =
        calculateFrameRect(
            preview,
            aspectValue(
                assist.frameAspect
            )
        );

    drawRectOutline(frame, WHITE);

    if (assist.centerCross) {
        drawCenterCross(preview);
    }

    if (assist.safeArea) {
        drawSafeArea90(preview);
    }
}
```

---

# 29. CAL Header + MENU

現行CAL表示は維持する。

左側:

```text
[CAL] 1280x720 BT.709 LIMITED
```

右側:

```text
MENU
```

MENUは `inputStatus` 内に収める。

CALテキストがMENU領域へ侵入しないよう、CAL文字列の最大描画Xを:

```cpp
menuRect.x - TEXT_GAP
```

でclipまたは省略判定する。

現行想定の文字列ではLandscape 711幅内に十分収まるが、Portraitも同じルールを使う。

---

# 30. Render order

推奨最終描画順:

```text
1. Capture frame acquisition
2. Capture Calibration / signal processing
3. Preview shader
   ├ Zebra
   ├ Peaking / Mono
   └ False Color
4. Scope data/render
   ├ Waveform
   ├ Parade
   ├ Histogram
   └ Vectorscope
5. ScopeUi overlay
   ├ CAL header
   ├ runtime Assist status + UI FPS
   ├ scope shells/grid/labels
   ├ Frame guide / Cross / Safe
   └ MENU / F buttons / Preset menu
6. Present
```

MENUは最前面。

---

# 31. Blend state

現行 `ScopeUi::drawOverlay()` は:

```cpp
glEnable(GL_BLEND);
glBlendFunc(
    GL_SRC_ALPHA,
    GL_ONE_MINUS_SRC_ALPHA
);
```

を使用している。

v5 MENU UIは全色 `alpha = 1.0` とするため、既存blend stateを維持しても結果は完全不透明になる。

したがって初期実装ではMENUだけのためにblend stateを分割する必要はない。

Frame Guideも `alpha = 1.0` とする。

既存scope grid等のalphaは別仕様として維持可能。

---

# 32. ScopeUi font

現行UIは独自5x7 bitmap fontを使用している。

Character advance:

```text
6 * scale
```

v5 native実装ではブラウザfontを再現せず、既存 `addText()` をそのまま利用する。

MENU / F1-F4 / LOCK / preset labelに必要な文字は現行fontでほぼカバーされる。

追加確認対象:

```text
必要な文字:
MENU
CLOSE
LOCK
ON
OFF
ZEBRA
PEAK
FALSE
FRAME
MONO
LOW
MID
HIGH
CROSS
SAFE
16:9
1.85
2.00
2.39
4:3
1:1
9:16
```

現行fontには大文字・数字・`.`・`:`・`/`・`-` があるため対応可能。

---

# 33. 実装ファイル分割案

既存責務を肥大化させないため以下を推奨する。

```text
field_monitor_layout.h
    既存logical layout

scope_ui.h / scope_ui.cpp
    Scope描画
    CAL/status
    Frame Guide
    MENU描画

monitor_ui_state.h
    Assist / Menu state enum & structs

monitor_ui_controller.h/.cpp
    touch hit-test
    state transition
    preset resolve

preview_assist.h/.cpp
    CPU preset -> GPU uniform解決

preview shader
    Zebra
    Peaking / Mono
    False Color
```

`ScopeUi` に操作ロジックを持たせず、描画専用に維持する。

---

# 34. 実装Phase

一気に追加しない。

## Phase UI-0: State型のみ

追加:

- `PreviewAssistState`
- `MonitorMenuState`
- enum群

描画変更なし。

Test:

- build
- 初期値ログ確認

---

## Phase UI-1: Frame CLEAN化

現行の無条件Center Cross / Safe Areaを条件描画へ変更。

実装:

```text
Frame OFF
    → Crossなし
    → Safeなし

Frame ON
    → aspect
    → optional Cross
    → optional Safe
```

Test:

- 全Assist OFF時 `CLEAN`
- Frame OFFかつ他Assist ON時は該当Assist短縮表示
- False Color ON時はZebra/PeakingをRuntime表示から抑制
- False Color OFF復帰時は保存済みZebra/Peaking表示を復帰
- OFF完全CLEAN
- 16:9 + Cross
- 16:9 + Safe
- 2.39
- 4:3
- 1:1
- 9:16
- landscape
- portrait

---

## Phase UI-2: MENU / LOCK

実装:

- CAL header右上 MENU
- MENU/CLOSE
- F1-F4
- LOCK
- preset menu
- 142px幅
- selected反転
- SELECTED文字なし

まだAssist shaderは変更しない。

Test:

- LOCK初期ON
- FキーLOCK中無効
- LOCK OFF
- Fキー初回tap
- Fキー再tap
- preset直接tap
- 外側tap閉じる
- orientation変更

---

## Phase UI-3: Zebra

Preview shaderへZebraだけ追加。

Test:

- OFF時shader出力完全一致
- preset切替
- stripe
- performance
- HDR/SDR path regressなし

判定は70 / 80 / 90 / 95を±3 IRE、100を100 IRE以上とする。

---

## Phase UI-4: Peaking

実装:

- LOW/MID/HIGH
- edge overlay
- MONO LOW/MID/HIGH

Test:

- OFF output一致
- sensitivity順序
- Mono grayscale
- edge highlight維持
- 60fps維持
- GPU time比較

---

## Phase UI-5: False Color

実装:

- OFF / VIDEO / HDR NITS
- `FalseColorDomain::VideoLevel / HdrNits`
- VideoLevel palette mapping
- PQ EOTF / absolute nits palette mapping
- False Color ON時Zebra/Peaking visual suppress
- 非PQ入力でHDR NITSを選択不可

```text
Stage 3 PQ
 ↓
PQ EOTF
 ↓
absolute nits
 ↓
HDR NITS palette
```

Test:

- palette境界
- IRE domain
- SDR/HDR
- range
- performance

False Colorの境界とpaletteは実装計画書D-08〜D-11を使用する。

---

## Phase UI-6: Integration

全機能組み合わせ。

Scope表示はWaveform gain `1.5`、RGB Parade gain `1.5`、Vectorscope gain `1.75`、Vectorscope exact 1-binとする。

Test matrix:

```text
False OFF / Zebra OFF / Peak OFF
False OFF / Zebra ON  / Peak OFF
False OFF / Zebra OFF / Peak ON
False OFF / Zebra ON  / Peak ON
False ON  / Zebra ON  / Peak ON
Peak MONO
Frame OFF
Frame 16:9 + Cross + Safe
Frame 2.39
```

---

# 35. Performance acceptance

本UI追加によってlatest-frame思想を壊さないこと。

最低確認項目:

```text
USB fps
decodeCallMs
decodeTotalMs
frameSlotDrop
queueDrop
render/present latency
CPU usage
GPU/frame time
```

特にPeakingは追加texture sampleが発生するため単独で計測する。

False Color / Zebraは基本的に1pixel内branchのみで完結させる。

MENU描画は既存 `uiVbo_ + GL_STREAM_DRAW` の範囲で追加し、別SurfaceやAndroid Viewを作らない。

---

# 36. Regression条件

以下を満たすまで次Phaseへ進まない。

```text
1. Scope集計値とgeometryが変わらず、確定済みtrace gainが適用される
2. Waveform位置が変わらない
3. Parade位置が変わらない
4. Histogram位置が変わらない
5. Vectorscope円形が崩れない
6. CAL表示が崩れない
7. Portrait配置が崩れない
8. 全有効Preview Assist OFF時のみRuntime表示がCLEAN
9. Assist OFF時Preview pixel pathに不要な見た目の差がない
10. new UIによるqueue/drop増加なし
```

---

# 37. 確定仕様 / 未確定仕様

## 確定

- v5 UI layout
- MENUはCAL header右上
- BLACK + WHITE
- selectedはWHITE + BLACK
- opacity 1.0
- SELECTED文字なし
- Preset menu幅142
- LOCK
- F1-F4
- F1 preset一覧
- F2 preset一覧
- F3 OFF / VIDEO / HDR NITS
- F4 preset一覧
- Frame OFF時はCross / Safe / Aspectを描画しない
- Runtime Statusは「現在Previewに実際に掛かっているAssist」を表示
- 全ての有効表示AssistがOFFの場合のみ `CLEAN`
- False Color ON時はZebra / PeakingをRuntime Statusからも抑制
- Runtime Statusの表示順はF1 → F2 → F3 → F4
- 16:9あり
- Center Cross
- 90% Safe Area
- False Color表示中はv5上Zebra/Peaking画素overlayを表示しない
- Assist共通tap pointはCapture Calibration直後のStage 3
- ZebraはStage 3 source video level
- Peaking解析はStage 3 source luma gradient
- Peaking MONO背景だけはfinal displayRgbをgrayscale化
- False Color標準domainはStage 3 VideoLevel
- HDR NITS False ColorはStage 3 → PQ EOTF → absolute nitsの専用analysis branch
- HDR NITS False ColorへTone Mapping / User LUTを入れない
- Landscape / Portrait layout

## 未確定

なし。最終値は実装計画書の決定一覧D-01〜D-22を正とする。

---

# 38. 最終構成

```text
UVC / Decode
     │
     ▼
Capture Calibration
     │
     ▼
===== Stage 3 : Assist / Scope Tap =====
Calibrated Encoded R'G'B'
     │
     ├──────────────→ Scope
     │                Wave / Parade / Hist / Vector
     │
     ├──────────────→ Zebra
     │                source video level
     │
     ├──────────────→ Peaking
     │                source luma gradient
     │
     ├──────────────→ False Color / VideoLevel
     │                source video level
     │
     └──────────────→ False Color / HDR NITS
                      PQ EOTF
                         ↓
                      absolute nits
                         ↓
                      HDR NITS palette

Stage 3
     │
     ▼
Signal Interpretation
SDR inverse / PQ EOTF
     │
     ▼
Tone Mapping / Gamut Conversion
     │
     ▼
Output Transfer
     │
     ▼
Optional User 3D LUT
     │
     ▼
displayRgb
     │
     ├──────────────→ Normal Preview
     │
     └──────────────→ Peaking MONO background
                      grayscale(displayRgb)

Preview + Assist Result
     │
     ▼
ScopeUi Overlay
     ├─ CAL / Runtime Assist Status
     ├─ Frame / Cross / Safe
     └─ MENU / F1-F4 / LOCK
     │
     ▼
Present
```

HDR NITS False ColorだけはStage 3から専用PQ EOTF branchを持つ。

このbranchはanalysis専用であり、通常PreviewのTone Mapping / Gamut / User LUTとは独立する。

UI操作、source analysis、display processingを分離し、既存Scope処理への影響を最小化する。


---

# 39. 実装開始時の最初の変更

最初のコード変更はPreview shaderではなく、以下のみとする。

```text
1. monitor_ui_state.h追加
2. Frame stateをdrawOverlayへ渡す
3. 現在の無条件Center Cross / SafeをFrame state条件下へ移動
4. Frame OFFでCLEANになることだけ確認
```

ここが通ってからMENUへ進む。

その後もPhase単位で実装・実機確認する。
