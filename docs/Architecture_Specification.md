# Android UVC Field Monitor

## Architecture Specification — Current Baseline

**Revision:** 2026-09-27  
**Status:** MS2109互換経路 + MS2130 720p60/HDR(PQ)経路統合版  
**Primary implementation:** Native C++ / libusb / TurboJPEG / OpenGL ES

---

# 1. 目的

Android端末をUSB Video Class（UVC）キャプチャデバイスと組み合わせ、低レイテンシのフィールドモニターとして構成する。

一般的なAndroid動画再生経路やMediaCodec系パイプラインを使用せず、USB入力からGPU描画・映像解析までをNative中心で実装する。

重視する項目：

- 低レイテンシ表示
- UVC入力フレームのFIFO滞留防止
- 最新フレーム優先
- CPUコピーの最小化
- CPU側RGB変換の回避
- GPU内での映像処理
- Scope処理をPreview critical pathから分離
- Waveform / RGB Parade / Histogram / Vectorscope
- Zebra / Peaking / False Color等への拡張性
- SDR / HDR(PQ)入力への対応
- 実測値に基づくレイテンシ評価

本システムでは「全フレームを順番通り表示すること」よりも、リアルタイム監視における「現在に近い映像」を優先する。

基本原則：

```text
latest wins
busy -> wait ではなく busy -> skip
no video FIFO
measurement != preview processing
```

---

# 2. 対象ハードウェア

## 2.1 Android端末

### 2.1.1 ZTE Libero 5G III A202ZT

既存の実機検証ベースライン。

```text
Device      : ZTE Libero 5G III A202ZT
OS          : Android 13 / API 33
GPU         : Mali-G57 MC2
OpenGL ES   : 3.2
EGL         : 1.4
```

主な確認済みGPU/EGL機能：

- `GL_EXT_buffer_storage`
- `GL_EXT_disjoint_timer_query`
- `GL_OES_EGL_image`
- `GL_OES_EGL_image_external`
- `GL_EXT_YUV_target`
- `EGL_KHR_fence_sync`
- `EGL_ANDROID_native_fence_sync`
- `EGL_ANDROID_get_frame_timestamps`
- `EGL_ANDROID_presentation_time`
- `EGL_KHR_image`
- `EGL_ANDROID_image_native_buffer`
- `EGL_ANDROID_get_native_client_buffer`

A202ZTでは`AHARDWAREBUFFER_USAGE_FRONT_BUFFER`がvendor Gralloc側で拒否されるため、通常運用はEGL / BufferQueue経路を使用する。

---

### 2.1.2 Teclast P30T / Unisoc T7250

Android 16移植・次期評価端末。

```text
Device      : Teclast P30T
SoC         : Unisoc T7250
OS          : Android 16
RAM         : 3 GB
GMS         : 搭載
Root        : 前提としない
```

本機ではA202ZT固有のMali/Gralloc挙動を前提にせず、Android標準API / NDK APIで成立する構成を維持する。

主な評価対象：

- USB Host API → native FD受け渡し
- libusb bulk / isochronous転送
- MS2109 YUYV 30 fps
- MS2130 MJPEG 60 fps
- TurboJPEG decode性能
- OpenGL ES 3.1+ Compute Shader
- Persistent mapped PBO
- EGL extension構成
- `EGL_ANDROID_get_frame_timestamps`
- `EGL_ANDROID_presentation_time`
- AHardwareBuffer / EGLImage interop
- `AHARDWAREBUFFER_USAGE_FRONT_BUFFER`
- SurfaceControl NDK経路
- 3 GB RAM環境でのメモリ使用量
- 長時間動作時のthermal throttling

### Unisoc機での設計方針

Unisoc機専用コードを増やすのではなく、

```text
Capability Probe
      ↓
supported
  -> optimized path

unsupported
  -> existing fallback path
```

とする。

root、vendor private API、端末固有system propertyへの依存は基本的に避ける。

Android 16 / API 36ではStep 15DのFront Buffer / SurfaceControl経路を評価対象とするが、**実機での対応可否とレイテンシ改善量は未確定**とする。

---

## 2.2 UVCキャプチャデバイス

現在はMacroSilicon系の2経路を維持する。

| Device | Format | Resolution / FPS | USB transport | 主用途 |
|---|---|---|---|---|
| MS2109 | YUYV / YUY2 | 720×480 @ 30 | USB 2.0 HS Isochronous | SDR互換ベースライン |
| MS2130 | MJPEG | 1280×720 @ 約60 | USB 2.0 HS Bulk | 現行主経路 / HDR(PQ) |

---

### 2.2.1 MS2109

USB識別：

```text
VID : 0x534D
PID : 0x2109
```

映像条件：

```text
Resolution    : 720 × 480
Pixel Format  : YUYV / YUY2
Frame Rate    : 30 fps
Frame Size    : 691,200 bytes
Data Rate     : 約20.736 MB/s / 165.9 Mbps
Transport     : USB 2.0 High-Speed Isochronous
Interface     : 1
Alt Setting   : 3
Max Payload   : 3072 bytes / microframe
Frame Interval: 333333
```

#### Processing Unit固定値

起動時に以下へ固定する。

```text
brightness = 0
contrast   = 128
saturation = 132
hue        = 0
```

Processing Unit IDは固定値ではなくVideoControl descriptorから探索する。

各Controlについて、

```text
SET_CUR
  ↓
GET_CUR
  ↓
設定値完全一致
```

まで確認する。

---

### 2.2.2 MS2130

現行主経路。

```text
Input       : HDMI
UVC Format  : MJPEG
Resolution  : 1280 × 720
Frame Rate  : 約60 fps
Transport   : USB 2.0 High-Speed Bulk
Decode      : TurboJPEG
Output      : planar YCbCr 4:2:2 / 8-bit
```

現行descriptor処理ではMS2130をMS2109と分離し、Bulk endpointを使用する。

代表的な実測：

```text
fps              ≈ 59–60 fps
USB throughput   ≈ 5.9 MiB/s
JPEG size        ≈ 104 KB/frame
wireFrame        ≈ 13.2–13.4 ms
inflight         = 7
```

MS2109用Processing Unit固定値はMS2130には適用しない。

---

# 3. ソフトウェア構成

主要処理はNative C++で実装する。

Android Java/Kotlin側：

- Activity / SurfaceView管理
- USB Host APIによるデバイス認識
- USB Permission取得
- USB File DescriptorのNative層への受け渡し
- Surfaceライフサイクル管理

Native C++側：

- libusb
- UVC descriptor / Probe / Commit
- Isochronous / Bulk transport
- UVC payload parser
- MJPEG frame assembler
- TurboJPEG decode
- 最新フレーム管理
- Persistent mapped PBO
- EGL
- OpenGL ES
- YUYV / planar YCbCr texture upload
- Preview shader
- HDR/PQ Preview processing
- GPU Scope処理
- Presentation timing
- レイテンシ計測

libusbはAndroid USB Host APIから取得したFDを使用し、Android側でlibusb独自のdevice discoveryは行わない。

```text
LIBUSB_OPTION_NO_DEVICE_DISCOVERY
```

---

# 4. 全体アーキテクチャ

## 4.1 MS2109互換経路

```text
HDMI Source
    │
    ▼
MS2109
    │
    │ UVC YUYV 720×480@30
    │ USB 2.0 HS Isochronous
    ▼
libusb Async Isochronous
    │
    ▼
UVC Payload Parser
    │
    ▼
Frame Assembler
    │
    ▼
Latest-Frame Triple Buffer
    │
    ▼
Persistent Mapped PBO ×2
    │
    ▼
Packed YUYV Texture
RGBA8 360×480
    │
    ├─ Preview Fragment Shader
    │    └─ BT.601 limited YUYV → RGB
    │
    └─ Scope Compute Shader
         ├─ Waveform
         ├─ RGB Parade
         ├─ Histogram
         └─ Vectorscope
```

---

## 4.2 MS2130現行経路

```text
HDMI Source
    │
    │ SDR or HDR / ST2084 PQ
    ▼
MS2130
    │
    │ UVC MJPEG 1280×720@≈60
    │ USB 2.0 HS Bulk
    ▼
Bulk Transfer / UVC Payload Parser
    │
    ▼
MJPEG Frame Assembler
    │
    ▼
Latest pending JPEG
    │
    ▼
TurboJPEG Worker
    │
    │ planar YCbCr 4:2:2
    ▼
Latest Decoded Frame Slots
    │
    ▼
Persistent Mapped PBO ×2
    │
    ▼
3-plane GL_R8 textures
    │
    ├───────────────────────┐
    │                       │
    ▼                       ▼
Scope Path              Preview Path
source-domain           display-domain
    │                       │
    ▼                       ▼
GPU Compute             Fragment Shader
    │                       │
    └──────────┬────────────┘
               ▼
        GLES UI Composition
               │
        ┌──────┴────────┐
        │               │
        ▼               ▼
EGL / BufferQueue   Step15D Front Buffer
(default/fallback)  (Android 16 experiment)
```

---

# 5. USB / UVC Transport

## 5.1 MS2109 Isochronous

構成：

```text
12 transfers
×
8 iso packets
×
3072 bytes
```

複数transferを常時in-flightとし、専用libusb event threadでcompletionを処理する。

UVC payload headerから以下を判定：

- FID
- EOF
- Error
- Payload length

フレーム境界はEOFを基本とし、FID toggleも有効な境界として利用する。

---

## 5.2 MS2130 Bulk

MS2130はBulk streamingとして扱う。

設計方針：

```text
USB completion
    ↓
UVC payload parser
    ↓
MJPEG bytes append
    ↓
EOF / FID boundary
    ↓
completed JPEG
```

JPEG queueを通常FIFOとして成長させない。

decode workerが処理中に新しいJPEGが完成した場合は、pending JPEGを最新フレームへ置換できる構造とする。

```text
old pending JPEG
      ↓ replace
newest pending JPEG
```

これによりdecode負荷上昇時もqueue latencyを蓄積しない。

---

# 6. MJPEG / TurboJPEG Decode

MS2130 MJPEGはTurboJPEGで直接planar YCbCr 4:2:2へdecodeする。

```text
JPEG bitstream
      ↓
TurboJPEG
      ↓
Y plane  : 1280×720
Cb plane :  640×720
Cr plane :  640×720
```

CPU上でRGBAへ変換しない。

```text
JPEG
 ↓
YCbCr 4:2:2
 ↓
GPU
```

とし、CPU RGB conversionを省く。

メモリlayout：

```text
Y    : 1280 × 720 = 921,600 bytes
Cb   :  640 × 720 = 460,800 bytes
Cr   :  640 × 720 = 460,800 bytes

Total = 1,843,200 bytes / frame
```

代表実測：

```text
TurboJPEG decodeCall
avg       ≈ 6–7 ms
p95       ≈ 8–9 ms

B0 → B2
avg       ≈ 20 ms
```

概念上：

```text
B0 = MS2130 wire frame受信開始側
B2 = TurboJPEG decode完了 / decoded frame publish
```

---

# 7. フレーム管理

## 7.1 基本方針

通常FIFO queueは使用しない。

```text
latest wins
```

を基本とする。

---

## 7.2 MS2109 Triple Buffer

3スロット：

```text
FREE
WRITING
READY
READING
```

複数READYが存在する場合は最新sequenceのみ取得し、古いREADYを破棄する。

---

## 7.3 MS2130 Decoded Latest-Frame Slots

TurboJPEG workerからrendererへdecoded frameを公開する。

公開時には、

- sequence
- width / height
- Y/Cb/Cr stride
- plane bytes
- decode completion timestamp
- B0→B2 timing

を確定させた後にREADYとする。

Rendererは`waitForDecodedFrame()`で起床し、`copyLatestFrame()`で最新のcompleted frameのみ取得する。

---

# 8. Event-Driven Renderer

レンダリングは継続60/120 Hzループではなく、新しいcamera frameの到着をトリガーとする。

```text
new frame READY
      ↓
condition_variable
      ↓
renderer wake
      ↓
latest frame取得
      ↓
GPU upload
      ↓
preview + scopes UI compose
      ↓
present once
      ↓
scope compute queue
      ↓
sleep
```

基本：

```text
1 new source frame
=
1 render
=
1 present
```

同一映像を複数回presentしない。

---

# 9. GPU Upload

MS2109 / MS2130ともPersistent Mapped PBO ×2を使用するが、両経路は別PBO pairとする。

```text
PBO available -> use
PBO busy      -> do not wait
both busy     -> skip current frame
```

fence確認：

```text
glClientWaitSync(..., 0, 0)
```

zero-timeoutとする。

`glFinish()`をsteady-state preview critical pathへ入れない。

---

# 10. GPU Texture Representation

## 10.1 MS2109

```text
Source : YUYV 720×480
Texture: RGBA8 360×480

R = Y0
G = U
B = Y1
A = V
```

1 texel = 2 source pixels。

---

## 10.2 MS2130

```text
Y  : GL_R8 1280×720
Cb : GL_R8  640×720
Cr : GL_R8  640×720
```

1 contiguous PBOから3回の`glTexSubImage2D()`で各planeへuploadする。

CPU側でinterleaveやRGBA conversionは行わない。

---

# 11. Color Range / Matrix

## 11.1 MS2109

既存実測・運用条件ではBT.601 limited rangeを使用する。

```text
Y  = limited
Cb = limited
Cr = limited
```

---

## 11.2 MS2130

HDMI sourceそのものの色域/transferと、MS2130がUVC MJPEGへ出力したdecoded YCbCrのmatrix/rangeは分けて扱う。

実機range / color-bar診断結果を基準に、**現行decoded interfaceではBT.601 limited-rangeとして復元する**。

```text
Y  = (Ycode  - 16)  / 219
Cb = (Cbcode - 128) / 224
Cr = (Crcode - 128) / 224
```

これは、

```text
MS2130 decoded JPEG YCbCr -> RGB
```

の復元条件であり、HDMI HDR source自体をBT.601色域と定義するものではない。

---

# 12. HDR / PQ Architecture

MS2130ではHDR / ST2084 PQ入力を実機確認済み。

PQテストパターン代表値：

```text
100 nit     ≈ 51 %
203 nit     ≈ 58 %
1000 nit    ≈ 75 %
4000 nit    ≈ 90 %
10000 nit   = 100 %
```

Waveform上でも上記geometryが確認できている。

ただしMS2130 UVC出力は8-bit MJPEGであるため、

```text
HDMI HDR source
    ↓
MS2130
    ↓
8-bit MJPEG
```

となり、HDMI側の10-bit精度そのものを保持する構成ではない。

---

## 12.1 Measurement Path

ScopeにはPQ EOTF / Tone Mappingを入れない。

```text
MS2130 source-domain
      │
      ├─ Luma Waveform
      ├─ RGB Parade
      ├─ RGB Histogram
      └─ Vectorscope
```

特にWaveformではdecoded Y codeを0–255のまま保持する。

```text
Y=16  -> bin 16
Y=235 -> bin 235
```

MS2130入力を再度16–235へsqueezeしない。

これによりsuper-black / super-whiteおよびPQ code geometryを保持する。

---

## 12.2 Preview Path

PreviewのみHDR→SDR表示変換を行う。

```text
MS2130 YCbCr
      ↓
BT.601 limited YCbCr → R'G'B'
      ↓
ST2084 PQ EOTF
      ↓
absolute linear RGB / nits
      ↓
HDR → SDR Tone Mapping
      ↓
BT.2020 → BT.709
      ↓
sRGB OETF
      ↓
Android SDR framebuffer
```

設計原則：

```text
Preview processing
!=
Measurement processing
```

Previewが白飛びしてもScope値は変更しない。

Tone Mappingを変更してもWaveform geometryを変更しない。

---

## 12.3 Tone Mapping

現在のPreviewは203 nit付近をSDR reference whiteの基準として扱う。

```text
203 nit ≈ SDR white reference
```

それ以上のHDR highlightは圧縮する。

1000 nit / 4000 nitパッチではPreview側で強いhighlight / 白飛びが発生する一方、Waveformは75 % / 90 %付近を維持することを確認済み。

SHARP 8Kリファレンスモニターとの目視比較では大きな差は確認されていない。

現状の観察：

```text
Luminance / PQ behavior : 大きな差なし
Tone Mapping            : 実用上近い
Color                    : 若干Yellow方向が強く見える可能性
```

Yellow方向の微差は現時点で固定gain補正を入れず、原因を切り分ける。

候補：

- MS2130内部HDMI→MJPEG処理
- decoded YCbCr matrix/rangeの微差
- BT.2020→BT.709変換
- gamut clipping / compression
- tone-map時の色相保持

---

## 12.4 HDR Metadata / Mode Detection

現時点ではUVC側からHDR10 Static Metadataを取得して自動判定する方式は確立していない。

したがってPQ Preview処理は、

```text
input mode is known / selected as PQ
```

を前提とする。

今後の検討：

- HDMI InfoFrame / HDR Static Metadata取得可否
- SDR / PQ / HLG判定
- HDR mode自動切替
- MaxCLL / mastering metadata利用

---

# 13. GPU Scope Architecture

4スコープを1回のcombined Compute dispatchで処理する。

MS2109：

```text
RGBA8 Packed YUYV 360×480
```

MS2130：

```text
Y / Cb / Cr planar textures
1280×720 / 640×720 / 640×720
```

出力：

```text
Waveform      : 720 × 256
RGB Parade    : 224 × 161
Histogram     : 3 × 256
Vectorscope   : 141 × 141
```

Scope結果はdouble-buffer SSBOで保持する。

```text
Front SSBO -> UI表示
Back SSBO  -> Compute書き込み
```

GPU fenceはzero-timeout pollし、完了したBackのみFrontへ昇格する。

---

# 14. Luma Waveform

内部測定データは8-bit code value全域を保持する。

```text
Y = 0..255
```

BT.601 limited基準の表示目安：

```text
Y=16   ->   0 IRE
Y=235  -> 100 IRE
Y=0    -> 約 -7.3 IRE
Y=255  -> 約109.1 IRE
```

MS2109 720×480では1 frameあたり：

```text
720 × 480 = 345,600 samples
```

MS2130では1280 source pixelを720 waveform X座標へdown-mapして表示する。

HDR/PQ時もWaveformはsource code geometryを維持する。

---

# 15. RGB Parade

RGB Paradeはdecoded YCbCrからRGBへ変換した値をR/G/Bそれぞれ独立して集計する。

MS2130では現行のBT.601 limited復元を使用する。

```text
Parade plot : 224 × 161
```

Tone Mapping後のPreview RGBを測定しない。

---

# 16. RGB Histogram

```text
3 × 256 bins
```

R/G/B各channelをsource-domain RGBとして集計する。

HDR Preview用Tone Mapping後の値は使用しない。

---

# 17. Vectorscope

VectorscopeはRGB round-tripを行わず、source YCbCrのCb/Crを直接使用する。

MS2109 / MS2130 limited-range基準：

```text
Cb = (code - 128) / 224
Cr = (code - 128) / 224
```

内部SSBO：

```text
141 × 141
```

表示：

- center cross
- circle graticule
- R
- M
- B
- CY
- G
- Y

Mali-G57互換性のため、readonly SSBO elementを関数へ直接渡さず、一度local variableへコピーする。

GLSL予約語との衝突も避ける。

---

# 18. Scope Scheduling

ScopeはPreviewのcritical pathを待たせない。

通常EGL path：

```text
Preview draw
    ↓
eglPresentationTimeANDROID(now)
    ↓
eglSwapBuffers()
    ↓
Scope fence poll
    ↓
Scope texture update / queue
    ↓
Compute dispatch
    ↓
glFenceSync()
    ↓
glFlush()
```

Front Buffer pathでも同じ思想を維持し、Preview present/submissionを優先する。

---

# 19. Scope UI

映像領域をclean containerとして扱う。

映像上に残すもの：

- center cross
- safe guide
- zebra
- peaking
- false color

映像外へ置くもの：

- Input device / source
- Resolution
- Frame rate
- Matrix / Range
- Bit depth
- HDR / SDR mode
- CLEAN
- UI FPS
- Scope labels

Landscape / Portrait双方で映像領域とscope panelを分離する。

---

# 20. Presentation Architecture

## 20.1 EGL / BufferQueue

既存の安定fallback。

```text
GLES composition
      ↓
EGLSurface
      ↓
eglPresentationTimeANDROID(now)
      ↓
eglSwapBuffers()
      ↓
SurfaceFlinger
      ↓
Display
```

`EGL_ANDROID_get_frame_timestamps`が利用可能な端末では、

- COMPOSITE_DEADLINE
- COMPOSITE_INTERVAL
- COMPOSITE_TO_PRESENT_LATENCY
- COMPOSITION_LATCH_TIME
- DISPLAY_PRESENT_TIME

を取得して実表示時刻を計測する。

---

## 20.2 Step 15D — Android 16 Front Buffer Experiment

Android 16 / API 36向け評価経路。

```text
AHardwareBuffer
USAGE_FRONT_BUFFER
      ↓
EGLImage
      ↓
GL texture / FBO
      ↓
persistent single buffer
      ↓
ASurfaceControl
      ↓
SurfaceTransaction
      ↓
Display
```

steady-stateでは同じAHardwareBufferへ描画し、

```text
glFlush()
    ↓
same AHB setBuffer transaction
```

を行う。

目的：

```text
EGL window BufferQueue
dequeue / queue cycle
```

の回避。

対応しない端末では自動的にEGL / BufferQueueへfallbackする。

A202ZTではFront Buffer usageが利用できない。

Teclast P30T / Unisoc T7250 / Android 16では本経路を評価対象とするが、**対応可否および改善量は実機検証前提**とする。

---

# 21. レイテンシ計測

## 21.1 MS2109

既存基準点：

```text
T0 = UVC complete frame READY
T1 = CPU → PBO copy完了
T2 = texture upload発行完了
T3 = draw commands完了
T4 = present / swap直前
T5 = eglSwapBuffers return
PRESENT = EGL_DISPLAY_PRESENT_TIME_ANDROID
```

A202ZT / MS2109旧ベースライン：

```text
T0 → T5       avg 約11.7 ms
T0 → PRESENT  約50～58 ms
```

---

## 21.2 MS2130

追加基準：

```text
B0 = MS2130 wire frame受信開始側
B2 = TurboJPEG decode完了 / frame publish
```

最新代表実測：

```text
B0 → B2        ≈ 18.5～21 ms
B2 → PRESENT   ≈ 21.5～22.5 ms
B0 → PRESENT   ≈ 40～43 ms
```

代表値：

```text
39.986 ms
42.556 ms
43.087 ms
```

平均約：

```text
41.9 ms
```

約60 fps換算：

```text
約2.5 frames
```

この時点のログは、

```text
path = EGL_SWAP
```

であり、Step15D Front Bufferの結果ではない。

HDMI source内部生成遅延やcamera sensor exposureは含まない。

---

# 22. 性能・負荷方針

本システムでは低レイテンシを最優先する。

避けるもの：

- video FIFO
- blocking GPU fence wait
- steady-state `glFinish()`
- CPU RGB conversion
- GPU→CPU frame readback
- duplicate redraw
- duplicate present

許容するもの：

- PBO busy時のframe skip
- decode pending JPEGのlatest replacement
- scope result更新skip
- 古いREADY frame破棄

---

# 23. Unisoc T7250移植チェックリスト

Teclast P30Tでは以下を順に確認する。

## 23.1 Platform

- Android 16 / API level確認
- USB Host有効
- UVC device permission取得
- native FD受け渡し
- NDK r28 build動作
- GLES version / extensions列挙
- EGL extensions列挙

## 23.2 USB

MS2109：

- Isochronous transfer
- Alt setting 3
- 3072-byte payload
- 30 fps維持

MS2130：

- Bulk endpoint認識
- 1280×720 MJPEG
- 約60 fps維持
- inflight transfer安定性
- drop / malformed / UVC error統計

## 23.3 Decode

- TurboJPEG build / ABI
- YUV422 planar decode
- 60 fps時decodeCall
- queueDrop
- frameSlotDrop
- thermal throttling後のdecode時間

## 23.4 GPU

- OpenGL ES 3.1以上
- Compute Shader
- `GL_EXT_buffer_storage`
- persistent mapped PBO
- GL_R8 3-plane upload
- Scope shader compile
- shader予約語 / readonly SSBO driver差

## 23.5 Presentation

- `EGL_ANDROID_presentation_time`
- `EGL_ANDROID_get_frame_timestamps`
- AHardwareBuffer
- EGLImage
- `AHARDWAREBUFFER_USAGE_FRONT_BUFFER`
- SurfaceControl NDK symbols
- Step15D activate/fallback判定

## 23.6 Memory / Thermal

RAM 3 GBのため、巨大なframe historyやsoftware FIFOを追加しない。

現行latest-frame構造はUnisoc機でも維持する。

長時間60 fps運用で、

- CPU温度
- GPU温度
- decode time
- dropped frame
- PBO busy skip
- render latency

の変化を確認する。

---

# 24. 設計原則

## 24.1 最新フレーム優先

```text
latest wins
```

古い映像を順番に表示しない。

---

## 24.2 Blocking回避

```text
busy
 -> wait
```

ではなく、

```text
busy
 -> skip
```

を選択する。

---

## 24.3 FIFOを作らない

映像フレームを複数段queueしない。

MS2130 JPEG decode queueもlatest pending semanticsとする。

---

## 24.4 GPU内処理

映像解析のためにGPU→CPU readbackを常用しない。

CPU readbackは初期論理検証・diagnostic用途へ限定する。

---

## 24.5 Preview優先

Scope処理がPreview presentationを待たせない。

---

## 24.6 Measurement / Preview分離

HDR対応では特に、

```text
Source
  ├─ Measurement
  │    └─ source-domain
  │
  └─ Preview
       └─ display-domain
```

を厳守する。

PQ EOTF、Tone Mapping、gamut mappingはPreview専用。

---

## 24.7 実測優先

性能改善は体感のみで判断しない。

主要計測点：

```text
B0 / B2
T0 / T1 / T2 / T3 / T4 / T5
LATCH
PRESENT
```

Scopeでは、

```text
waveform_sum
parade_sum
histogram_sum
vector_sum
busySkip
preDispatchPromote
```

等を用いて論理検証する。

---

# 25. 現在の安定ベースライン

## MS2109 Compatibility Path

```text
Input
MS2109
720×480 YUYV 30 fps

USB
libusb async isochronous

Frame Management
latest-frame triple buffer

GPU Upload
persistent mapped PBO ×2

GPU Format
RGBA8 360×480 packed YUYV

Rendering
OpenGL ES
event-driven

Scopes
Waveform / RGB Parade / Histogram / Vectorscope

Presentation
EGL / BufferQueue
eglPresentationTimeANDROID(now)

Measured
UVC READY → Display Present ≈50～58 ms
```

---

## MS2130 Current Main Path

```text
Input
MS2130
1280×720 MJPEG ≈60 fps
SDR / HDR(PQ)

USB
libusb async Bulk
latest pending JPEG

Decode
TurboJPEG
planar YCbCr 4:2:2 8-bit

Frame Management
latest decoded frame slots

GPU Upload
persistent mapped PBO ×2
3-plane GL_R8

Decoded Color Interpretation
BT.601 limited-range
(empirically selected at MS2130 decoded interface)

HDR Measurement
PQ code geometry preserved
no EOTF / no Tone Mapping in scopes

HDR Preview
PQ EOTF
→ HDR-to-SDR Tone Mapping
→ BT.2020 to BT.709
→ sRGB

Rendering
event-driven
one source frame -> one present

Presentation
EGL / BufferQueue current measured path
Step15D Front Buffer available as Android 16 experiment

Measured
B0 → PRESENT ≈40～43 ms
```

---

# 26. 未確定・今後の検証項目

- MS2130内部HDMI→MJPEG色変換の正確な仕様
- HDR10 Static MetadataのUVC側取得方法
- SDR / PQ / HLG自動判定
- MS2130 10-bit入力→8-bit MJPEG量子化特性
- chroma processing詳細
- SHARP 8K比較で観察した微小なYellow方向差の原因
- gamut compression方式
- HDR Tone Mapping最終カーブ
- Unisoc T7250のGPU/EGL extension実機一覧
- Unisoc T7250でのPersistent PBO性能
- Unisoc T7250でのStep15D Front Buffer可否
- Unisoc T7250でのB0→PRESENT実測
- Android 16長時間60 fps動作時のthermal behavior

---

# 27. Version

```text
Document : Android UVC Field Monitor Architecture Specification
Revision : Current / 2026-09-27
Baseline : MS2109 compatibility + MS2130 HDR main path
Targets  : A202ZT / Teclast P30T (Unisoc T7250)
```
