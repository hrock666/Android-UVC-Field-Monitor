# Android UVC Field Monitor
## UI v5 実装計画書

Version: implementation plan v1.1  
作成日: 2026-10-04  
基準仕様: `Android_UVC_Field_Monitor_UI_v5_Implementation_Spec.md` v1.2

仕様決定状態: D-01〜D-20 決定済み

---

# 1. 目的

本書は、UI v5実装仕様を現行リポジトリへ段階的に実装するための作業計画、Phase間の依存関係、検証項目、未確定事項の決定順序を定義する。

実装対象:

- MENU / F1-F4 / LOCK UI
- F1 Zebra
- F2 Peaking / MONO Peaking
- F3 False Color / VideoLevel / HDR NITS
- F4 Frame / Center Cross / Safe Area
- Runtime Assist Status
- Frame OFF時の完全CLEAN Preview

既存のWaveform / RGB Parade / Histogram / Vectorscopeのデータ構造と解析・描画ロジックは変更しない。

---

# 2. 作業ルール

## 2.1 Phase単位で実装する

実装順序は次のとおりとする。

```text
UI-0 State / Controller
  ↓
UI-1 Frame CLEAN化
  ↓
UI-2 MENU / LOCK / Touch
  ↓
UI-3 Zebra
  ↓
UI-4 Peaking / MONO
  ↓
UI-5 False Color
  ↓
UI-5H HDR NITS False Color
  ↓
UI-6 Integration
```

各Phaseは、そのPhaseの完了条件を満たしてから次へ進む。

## 2.2 ビルドと実機確認

- Codexはビルドを実行しない。
- ビルドと端末へのインストールはユーザーが手動で行う。
- 各Phase完了時、Codexは変更ファイル、確認項目、推奨ビルドコマンド、実機確認手順を提示する。
- ユーザーからビルド結果または実機結果を受け取った後、必要な修正または次Phaseへ進む。

## 2.3 未確定仕様

- 未確定値をUI enum、状態遷移、shader本体へ固定値として埋め込まない。
- 本書の「未確定事項決定フロー」に従って1項目ずつ決定する。
- 決定結果は本書の決定ログへ追記してから実装へ反映する。
- 未決定項目に依存するPhaseは、依存項目が決まるまで着手しない。

---

# 3. 現行実装との差分

## 3.1 ScopeUi

現状:

- Runtime Status左側は固定 `CLEAN`。
- Center Crossと90% Safe Areaは無条件描画。
- Guide色は半透明。
- status、Frame guide、scope shell、grid、labelが1つの `drawOverlay()` に含まれる。

必要な変更:

- `MonitorUiRenderState` を描画へ渡す。
- Runtime StatusをEffective Preview Stateから生成する。
- Frame OFF時はAspect / Cross / Safeを描画しない。
- v5 Frame GuideとMENUはWHITE、alpha 1.0とする。
- scope chromeと前面UIを分離し、MENUを最前面へ描画する。

## 3.2 Preview shader

現状:

- Planar Y/Cb/CrからR'G'B'を再構築する。
- Capture Calibrationを適用する。
- PQ入力時はPreview用PQ EOTF、Tone Mapping、BT.2020→BT.709、sRGB変換を適用する。
- Assist用uniformと処理は存在しない。

必要な変更:

```glsl
vec3 analysisRgb; // Calibration直後のStage 3
vec3 displayRgb;  // 現行Preview表示処理後
```

を分離し、Assist判定がTone Mappingや将来のUser LUTに影響されない構造にする。

## 3.3 Touchと状態保持

現状:

- `SurfaceView` にタッチ処理がない。
- UI状態モデルがない。
- rendererは原則として新しいUVC frame到着時だけ描画する。

必要な変更:

- Android `ACTION_UP` をJNIへ渡す。
- Surface座標をlogical top-origin座標へ変換する。
- 描画とhit-testで同じgeometryを使用する。
- JNIスレッドは状態更新だけを行い、GL操作はrender threadに限定する。
- UI変更時はcamera frame uploadやscope computeを実行せず、最後のtextureを使って1回だけ再描画する。
- UI状態はrender loopの外に保持し、Surface再生成や画面回転で失わない。

---

# 4. ファイル構成計画

## 4.1 新規ファイル

```text
app/src/main/cpp/monitor_ui_state.h
    enum、PreviewAssistState、MonitorMenuState、EffectivePreviewState

app/src/main/cpp/monitor_ui_controller.h
app/src/main/cpp/monitor_ui_controller.cpp
    state transition、cycle、preset適用、effective state、status文字列

app/src/main/cpp/monitor_ui_geometry.h
app/src/main/cpp/monitor_ui_geometry.cpp
    MENU / Rail / Presetの共通logical geometry、表示情報、hit-test

app/src/main/cpp/preview_assist.h
app/src/main/cpp/preview_assist.cpp
    UI presetからGPU parameterへの解決
    Zebra / Peaking / False Colorの定数・テーブル境界
```

## 4.2 変更ファイル

```text
app/src/main/cpp/CMakeLists.txt
    新規.cppの登録

app/src/main/cpp/scope_ui.h
app/src/main/cpp/scope_ui.cpp
    Runtime Status、Frame、MENU描画、描画pass分離

app/src/main/cpp/native-lib.cpp
    状態snapshot、JNI touch bridge、UI dirty redraw、shader uniform

app/src/main/java/com/hev/uvcfieldmonitor/MainActivity.kt
    SurfaceView touch event転送
```

`field_monitor_layout.h` のLandscape / Portrait logical layout値は変更しない。

---

# 5. Phase詳細

## Phase UI-0: State / Controller

### 実装

1. `monitor_ui_state.h` を追加する。
2. enumと初期値を定義する。F3はHDR NITS追加決定に従い `FalseColorMode::Off / VideoLevel / HdrNits` の3状態とする。
3. `MonitorUiRenderState` にAssistとMenu状態をまとめる。
4. `resolveEffectivePreviewState()` を実装する。
5. `buildRuntimeAssistText()` を実装する。
6. F1-F4 cycleとpreset直接選択を実装する。
7. CROSS / SAFEをF4 main cycleと分離する。
8. Frame Aspect計算を純粋関数として実装する。
9. process lifetimeの状態ストア、mutex、revisionを追加する。

### 必須状態遷移

- MENUを閉じると `selectedFunction = None`。
- LOCK ON時は `selectedFunction = None`。
- LOCK中のF1-F4およびpreset操作は無効。
- 異なるFキーの初回tapは選択だけを変更し、preset値を変えない。
- 同じFキーの再tapはmain presetを1段進める。
- preset直接tap後もpreset menuを閉じない。
- F3のmain cycleは `OFF → VIDEO → HDR NITS → OFF` とする。
- Frame OFFはAspect / Cross / Safe保存値を破棄しない。
- False ColorがVideoLevelまたはHdrNitsのときもZebra / Peaking保存値を破棄しない。

### 完了条件

- 全初期値が仕様書と一致する。
- Runtime Statusの固定順がF1→F2→F3→F4になる。
- False Color ON中はZebra / Peakingがstatusから消える。
- False Color OFF後にZebra / Peakingが復帰する。
- 有効表示Assistがない場合だけ `CLEAN` になる。
- 描画結果はまだ変更しない。

---

## Phase UI-1: Frame CLEAN化

### 実装

1. `ScopeUi` に `MonitorUiRenderState` snapshotを渡す。
2. 固定 `CLEAN` をEffective State由来の文字列へ置換する。
3. 無条件Cross / Safe描画を削除する。
4. `frameEnabled` の条件下でAspect枠を描画する。
5. `centerCross` と `safeArea` を独立条件で描画する。
6. Frame GuideをWHITE、alpha 1.0にする。

### Geometry

- Cross: Preview中心、横25 logical px、縦25 logical px、太さ1px。
- Safe: Preview幅・高さの90%、中央配置、太さ1px。
- Aspect: Preview内にaspect-fitし、四辺を1pxで描画する。
- 16:9: Preview外周と一致してよい。

### 完了条件

- 初期状態のPreviewにCross / Safe / Aspectがない。
- Frame OFFでは保存値にかかわらずFrame系を一切描画しない。
- 16:9 / 1.85 / 2.00 / 2.39 / 4:3 / 1:1 / 9:16が正しい。
- LandscapeとPortraitの両方で確認できる。
- 既存scopeの位置、値、grid、label、CAL表示に変化がない。

---

## Phase UI-2: MENU / LOCK / Touch

### UI Geometry

- MENU width: 56
- Function Rail width: 86
- Preset Menu width: 142
- Preset Title height: 34
- Preset Row height: 32
- MENUは `inputStatus` 右上。
- RailはMENU直下、右端をMENUと一致させる。
- Preset MenuはRail左側へ配置する。
- Preset Menuの縦位置は選択Fキー中央基準でcanvas内へclampする。

### 描画

1. MENU / CLOSE triggerを追加する。
2. F1-F4とLOCK railを追加する。
3. 選択Functionを白背景・黒文字にする。
4. preset選択行を白背景・黒文字にする。
5. `SELECTED` 文字は描画しない。
6. MENU全要素をalpha 1.0にする。
7. CAL文字列をMENU開始Xより手前に収める。
8. LOCK中のFキーを操作不能にする。
9. F3 preset menuを `OFF / VIDEO / HDR NITS` の3行とする。
10. 非PQ入力ではF3の `HDR NITS` 行をdim表示し、選択不能にする。

### Touch

1. `SurfaceView` の `ACTION_UP` を受け取る。
2. `nativeOnSurfaceTap(x, y)` を追加する。
3. `canvasSurfaceTop = surfaceHeight - canvas.y - canvas.height` でtop-originへ変換する。
4. canvas外を含むMENU UI外tapでMENUを閉じる。
5. drawとhit-testに共通geometryを使用する。
6. state revisionを更新し、render threadへUI dirtyを通知する。

### Render order

```text
Preview
Histogram background/data as existing
Scope chrome / grid / labels
Vectorscope / Parade / Waveform data
Runtime Status / Frame Guide
MENU / Rail / Preset Menu
Present
```

既存scope sampleがgridより前へ隠れないよう、現在の意図を維持したまま前面UIだけを最後に分離する。

### 完了条件

- LOCK初期ON。
- LOCK中のF1-F4は状態を変えない。
- MENUはLOCK中も開閉できる。
- LOCK OFF後に初回tap、再tap、直接preset選択が仕様どおり動く。
- 非PQ入力ではF3 cycleが `OFF → VIDEO → OFF` となり、HDR NITS直接tapが無効になる。
- MENU外tapで閉じるがAssist状態は維持する。
- 画面回転後もAssist / MENU / LOCK状態が維持される。
- UVC未接続時もMENU操作が反映される。
- UI操作による連続render、camera upload、scope computeを発生させない。

---

## Phase UI-3: Zebra

### 着手条件

- D-01からD-04、D-18、D-19が決定済みであること。

### 実装

1. Preview shader内で `analysisRgb` と `displayRgb` を分離する。
2. `ZebraShaderParams` resolverを追加する。
3. Zebra enable / low / high uniformを追加する。
4. Stage 3 source lumaでhit判定する。
5. `gl_FragCoord` ベースの斜線を表示する。
6. Zebra OFF時は既存出力経路を維持する。
7. uniform locationをprogram生成時に取得して保持する。

### 完了条件

- OFF時のPreviewが変更前と視覚的に一致する。
- 70 / 80 / 90 / 95 / 100が決定済みtableどおり動く。
- SDR / PQともAssist tap pointがStage 3である。
- Tone Mapping結果によってZebra判定が変わらない。
- Scope SSBOとscope shaderを変更していない。

---

## Phase UI-4: Peaking / MONO

### 着手条件

- D-05からD-07、D-19が決定済みであること。

### 実装

1. `PeakingShaderParams` resolverを追加する。
2. Stage 3 lumaを使った4-neighbor gradientを追加する。
3. LOW / MID / HIGH thresholdをuniformで切り替える。
4. edge highlightを最終base RGBへ適用する。
5. MONO時は `displayRgb` だけをgrayscale化する。
6. False Color ON時はPeaking処理へ入らない。

### 完了条件

- OFF時出力が変更前と一致する。
- LOW / MID / HIGHの感度関係が決定内容と一致する。
- MONO背景は最終Previewのgrayscaleである。
- edge判定はTone Mapping / User LUT後のcontrastを使用しない。
- Zebraと同時使用できる。
- Peaking単独のGPU/frame timeを確認できる。

---

## Phase UI-5: False Color

### 着手条件

- D-08、D-09、D-19が決定済みであること。

### 実装

1. `FalseColorDomain::VideoLevel` resolverを追加する。
2. Stage 3 source lumaを決定済みbandへ割り当てる。
3. `FalseColorMode::VideoLevel` 選択時はPreviewをpalette色へ置換する。
4. Zebra / Peaking shader処理を抑制する。
5. Effective StateでもZebra / Peakingを抑制する。
6. `FalseColorMode` からGPU用enable / domain parameterを解決する。

### 完了条件

- band境界の直前・境界・直後で正しい色になる。
- False Color ON中もZebra / Peaking raw stateが維持される。
- False Color OFF後に保存済みZebra / Peakingが復帰する。
- Runtime Statusが `FC VIDEO` とFrame系だけを表示する。
- Tone MappingやPreview色変換を判定domainへ入れない。

---

## Phase UI-5H: HDR NITS False Color

### 着手条件

- UI-5 VideoLevel False Colorが完了していること。
- D-10、D-11、D-15、D-16、D-17、D-20が決定済みであること。

### 実装

1. `FalseColorDomain::HdrNits` を実動domainとして有効化する。
2. Calibration直後のStage 3 PQ encoded R'G'B'からanalysis lumaを生成する。
3. False Color専用analysis branchでPQ EOTFを適用する。
4. PQ codeをabsolute luminance nitsへ変換する。
5. D-10のnits bandとD-11のpaletteを適用する。
6. False Color出力で通常Previewを置換する。
7. Zebra / Peakingのraw stateを維持したままvisual outputを抑制する。
8. PQ Profile解除時に `HdrNits` から `VideoLevel` へ状態を正規化する。

### 固定pipeline

```text
Stage 3 PQ code
  → PQ EOTF
  → absolute nits
  → HDR NITS palette
```

通常PreviewのTone Mapping、Gamut Conversion、Output Transfer、User LUTは通さない。

### 完了条件

- domain選択がD-15どおりに動作する。
- PQ EOTF前後の既知値でabsolute nits変換を確認できる。
- 各nits境界の直前・境界・直後で正しいband色になる。
- HDR NITS判定へPreview Tone Mapping結果を使用しない。
- False Color OFF後に保存済みZebra / Peakingが復帰する。
- VideoLevel版のSDR動作に回帰がない。
- 非PQ入力へHDR NITS処理を適用しない。

---

## Phase UI-6: Integration

### 組み合わせ試験

```text
False OFF / Zebra OFF / Peak OFF
False OFF / Zebra ON  / Peak OFF
False OFF / Zebra OFF / Peak ON
False OFF / Zebra ON  / Peak ON
False ON  / Zebra ON  / Peak ON
False HDR NITS / PQ input
Peak MONO
Frame OFF
Frame 16:9 + Cross + Safe
Frame 2.39
```

各組み合わせをLandscape / Portraitで確認する。

### 回帰条件

1. Waveform値と位置が変わらない。
2. RGB Parade値と位置が変わらない。
3. Histogram値と位置が変わらない。
4. Vectorscopeが円形を維持する。
5. CAL表示が崩れない。
6. Portrait配置が崩れない。
7. Assist OFF時に不要なPreview差分がない。
8. `CLEAN` は有効表示Assistがない場合だけ表示する。
9. False Colorの表示優先順位が守られる。
10. latest-frame方針を維持する。

### 性能確認

- USB fps
- UI fps
- decodeCallMs
- decodeTotalMs
- frameSlotDrop
- queueDrop
- render / present latency
- CPU usage
- GPU/frame time

特にPeaking実装前後を同一入力・同一端末で比較する。

---

# 6. 手動ビルド・確認の受け渡し

各PhaseでCodexは次の形式で作業結果を渡す。

```text
変更Phase:
変更ファイル:
実装した仕様:
未実装または保留:
推奨ビルドコマンド:
実機確認手順:
期待結果:
報告してほしいログまたは画面:
```

標準の推奨ビルドコマンドは以下とするが、実行はユーザーが行う。

```powershell
.\gradlew.bat assembleDebug
```

診断値が必要なPhaseでは、診断ビルド用のCMake option指定手順をその都度提示する。

---

# 7. 仕様決定一覧

未確定事項は依存順に1項目ずつ決定し、以下のとおり全項目を確定した。

| ID | 決定事項 | 依存Phase | 状態 |
|---|---|---|---|
| D-01 | Zebraの判定方式 | UI-3 | 決定済み: ハイブリッド方式 |
| D-02 | Zebra band幅と100 presetの判定 | UI-3 | 決定済み: ±3 IRE / 100 IRE以上 |
| D-03 | ZebraのFULL / LIMITED変換規則 | UI-3 | 決定済み: Range正規化方式 |
| D-04 | ZebraのSDR / PQ threshold table | UI-3 | 決定済み: 共通table |
| D-05 | Peaking LOW / MID / HIGH threshold | UI-4 | 決定済み: 0.20 / 0.12 / 0.06 |
| D-06 | PeakingのSDR / PQ別tableの要否 | UI-4 | 決定済み: 共通table |
| D-07 | Peaking highlight RGB | UI-4 | 決定済み: 赤 |
| D-08 | VideoLevel False Colorのband境界 | UI-5 | 決定済み: 10 band |
| D-09 | VideoLevel False Colorのpalette | UI-5 | 決定済み: 10色palette |
| D-10 | HDR NITS False Colorのband境界 | UI-5H | 決定済み: 11 band |
| D-11 | HDR NITS False Colorのpalette | UI-5H | 決定済み: 11色palette |
| D-12 | Function Rail各ボタンのlogical height | UI-2 | 決定済み: 56 logical px |
| D-13 | RailとPreset Menu間のlogical gap | UI-2 | 決定済み: 2 logical px |
| D-14 | LOCK中Fキーの見た目 | UI-2 | 決定済み: dim表示 |
| D-15 | False ColorのVideoLevel / HdrNits domain選択規則 | UI-5H | 決定済み: F3で手動選択 |
| D-16 | F3 Rail / Preset Menuの表示ラベル | UI-2 / UI-5H | 決定済み: OFF / VIDEO / HDR NITS |
| D-17 | Runtime Assist StatusのFalse Color表記 | UI-5 / UI-5H | 決定済み: FC VIDEO / FC NITS |
| D-18 | Zebra斜線のperiod / width | UI-3 | 決定済み: 8px / 4px |
| D-19 | Stage 3 source lumaの係数選択 | UI-3 / UI-4 / UI-5 | 決定済み: colorimetry連動 |
| D-20 | 非PQ入力でHDR NITSを選択した場合の動作 | UI-5H | 決定済み: 選択禁止／VIDEOへ復帰 |

全決定事項D-01〜D-20のうち、本実装に必要な項目は決定済み。

---

# 8. 決定ログ

| ID | 決定日 | 決定内容 | 実装への反映 |
|---|---|---|---|
| D-01 | 2026-10-04 | ハイブリッド方式。70 / 80 / 90 / 95は各IRE近傍のband判定、100はクリッピング検出のthreshold判定とする。 | UI enumから独立したZebra resolverでband presetとthreshold presetを解決する。 |
| D-02 | 2026-10-04 | 70 / 80 / 90 / 95は中心値±3 IRE。100は100 IRE以上を対象とする。 | band presetは `[center - 3, center + 3] IRE`、100 presetは下限100 IRE・上限なしとしてGPU parameterへ解決する。 |
| D-03 | 2026-10-04 | Range正規化方式。LIMITEDはcode 16→0 IRE、235→100 IRE、FULLはcode 0→0 IRE、255→100 IREとしてから共通bandを適用する。 | `SignalRange` に応じてStage 3 source video levelを正規化し、Zebra preset resolverへ0〜100 IRE domainで渡す。 |
| D-04 | 2026-10-04 | SDR / PQで共通のZebra tableを使用する。どちらもStage 3の正規化source video levelを70 / 80 / 90 / 95 / 100 IRE presetで評価する。 | PQ入力でもZebra判定へPQ EOTFを適用しない。HDR絶対輝度判定はHDR NITS False Colorへ分離する。 |
| D-05 | 2026-10-04 | 4-neighbor gradient `abs(right-left) + abs(down-up)` に対し、LOW=0.20、MID=0.12、HIGH=0.06とする。LOWは強い輪郭のみ、HIGHは細かな輪郭も検出する。 | sensitivityが上がるほどthresholdを下げる定数tableとして `PeakingShaderParams` resolverへ実装する。MONO presetも同じ3値を使用する。 |
| D-06 | 2026-10-04 | SDR / PQで共通のPeaking threshold tableを使用する。 | どちらもStage 3 encoded source luma gradientへD-05の値を適用する。resolverは将来transfer別tableへ差し替え可能な境界を維持する。 |
| D-07 | 2026-10-04 | Peaking highlight色は赤 `RGB(1.0, 0.0, 0.0)` とする。 | 通常PeakingとMONO Peakingの両方で、edge hit時の最終display RGBを不透明な赤へ置換する。 |
| D-12 | 2026-10-04 | Function RailのF1 / F2 / F3 / F4 / LOCKを各56 logical pxとする。Rail全高は280 logical px。 | MENU trigger直下から5行を隙間なく配置し、描画とhit-testで同じ行矩形を使用する。 |
| D-13 | 2026-10-04 | Function RailとPreset Menuの横方向gapは2 logical pxとする。 | `presetMenu.right = functionRail.left - 2` とし、全FunctionのPreset Menuで共通使用する。 |
| D-14 | 2026-10-04 | LOCK ON中のF1-F4はdim表示とし、文字と枠を45%グレー `RGB(0.45, 0.45, 0.45)` にする。LOCKボタンは通常の白表示を維持する。 | LOCK中は選択反転を行わず、F1-F4だけdim色を使用する。背景は不透明BLACKのままとする。 |
| D-08 | 2026-10-04 | VideoLevel False Colorは10 bandとし、境界を0 / 5 / 20 / 40 / 55 / 70 / 85 / 95 / 100 IREとする。範囲は `<0`、`[0,5)`、`[5,20)`、`[20,40)`、`[40,55)`、`[55,70)`、`[70,85)`、`[85,95)`、`[95,100)`、`>=100`。 | Range正規化後のStage 3 source video levelをIREへ変換し、下限inclusive・上限exclusiveでtableを評価する。最終bandだけ上限なしとする。 |
| D-09 | 2026-10-04 | VideoLevel False Color paletteをDark Purple `(0.35,0.00,0.50)`、Purple `(0.55,0.00,0.80)`、Blue `(0.00,0.15,1.00)`、Cyan `(0.00,0.80,1.00)`、Gray `(0.45,0.45,0.45)`、Green `(0.00,1.00,0.00)`、Yellow `(1.00,1.00,0.00)`、Orange `(1.00,0.50,0.00)`、Red `(1.00,0.00,0.00)`、White `(1.00,1.00,1.00)` とする。 | D-08の10 bandへ記載順に割り当て、False Color出力をalpha 1.0のdisplay RGBとして置換する。 |
| S-01 | 2026-10-04 | HDR NITS False Colorを今回の実装範囲へ含め、Phase UI-5Hを追加する。 | D-15でdomain選択、D-16／D-17でUI表記、D-10でnits band、D-11でpaletteを決定後に実装する。 |
| D-15 | 2026-10-04 | False Color domainはF3から手動選択する。F3を `OFF / VIDEO / HDR NITS` の3 presetへ拡張し、再tap cycleも同じ順序とする。 | raw stateをboolではなく `FalseColorMode` enumで保持し、GPU parameter解決時にenableとdomainへ変換する。Calibration ProfileのPQ flagによる自動切替は行わない。 |
| D-16 | 2026-10-04 | F3 Railの値表示とPreset Menu行は、どちらも `OFF / VIDEO / HDR NITS` と表記する。 | 86px Rail内でも省略せず `HDR NITS` を描画し、描画ラベルとhit-test presetを同じ順序で管理する。 |
| D-17 | 2026-10-04 | Runtime Assist StatusはVideoLevel選択時に `FC VIDEO`、HdrNits選択時に `FC NITS` と表示し、OFF時はFalse Color表記を出さない。 | Effective Preview StateにFalse Color domainを含め、保存状態ではなく実際の表示domainからstatus文字列を生成する。 |
| D-10 | 2026-10-04 | HDR NITS False Colorは11 bandとし、境界を0.1 / 1 / 10 / 26 / 100 / 203 / 400 / 1,000 / 4,000 / 10,000 nitsとする。 | Stage 3 PQ codeへPQ EOTFを適用したabsolute nitsを、`[0,0.1)`、`[0.1,1)`、`[1,10)`、`[10,26)`、`[26,100)`、`[100,203)`、`[203,400)`、`[400,1000)`、`[1000,4000)`、`[4000,10000)`、`>=10000` の順に評価する。 |
| D-11 | 2026-10-04 | HDR NITS paletteをBlack `(0,0,0)`、Dark Purple `(0.35,0,0.50)`、Blue `(0,0.15,1)`、Cyan `(0,0.80,1)`、Green `(0,1,0)`、Gray `(0.45,0.45,0.45)`、Yellow `(1,1,0)`、Orange `(1,0.50,0)`、Red `(1,0,0)`、Magenta `(1,0,1)`、White `(1,1,1)` とする。 | D-10の11 bandへ記載順に割り当て、alpha 1.0のFalse Color出力として使用する。 |
| D-18 | 2026-10-04 | Zebra斜線は `gl_FragCoord.x + gl_FragCoord.y` による45度、period 8 physical px、白線幅4 physical px、duty 50%とする。 | Zebra hit画素のうちpattern該当部だけを不透明WHITEへ置換し、それ以外は元のPreview表示を維持する。 |
| D-19 | 2026-10-04 | Stage 3 source lumaは入力colorimetryに連動し、BT.601=`(0.2990,0.5870,0.1140)`、BT.709=`(0.2126,0.7152,0.0722)`、BT.2020=`(0.2627,0.6780,0.0593)` とする。Profile無効時はBT.601。HDR NITSはPQ EOTF後のlinear BT.2020 RGBへBT.2020係数を使用する。 | Zebra / Peaking / VideoLevel False Colorで共通helperを使用し、calibration colorimetryから係数uniformを解決する。HDR NITS用linear luminance helperはencoded source luma helperと分離する。 |
| D-20 | 2026-10-04 | HDR NITSは `pqInput=true` の場合だけ選択可能とする。非PQまたはProfile無効時はHDR NITS行をdim表示して直接tapを無視し、F3 cycleでもスキップする。使用中にPQ Profileが解除された場合はVIDEOへ自動復帰する。 | controllerへHDR NITS availabilityを渡し、preset enable判定、cycle、Profile変更時のstate normalizationを一元管理する。Runtime Statusは復帰と同時に `FC VIDEO` へ更新する。 |

---

# 9. Phase進捗

| Phase | 状態 | 備考 |
|---|---|---|
| UI-0 | 完了 | State / Controller、描画変更なし |
| UI-1 | 完了 | Frame CLEAN化、Runtime Assist Status接続 |
| UI-2 | 完了 | MENU / LOCK / Touch、UI-only redraw |
| UI-3 | 実装済み・手動確認待ち | Zebra、Stage 3判定、colorimetry連動 |
| UI-4 | 未着手 | D-05〜D-07／D-19決定済み |
| UI-5 | 未着手 | D-08〜D-09／D-19決定済み |
| UI-5H | 未着手 | D-10／D-11／D-15〜D-17／D-20決定済み |
| UI-6 | 未着手 | Integration |

