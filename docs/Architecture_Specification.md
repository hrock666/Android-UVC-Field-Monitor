# Android UVC Field Monitor

## Architecture Specification — Current Baseline

**Revision:** 2026-09-30
**Status:** MS2109 / MS2130 1280×720p60 MJPEG共通パイプライン統合版  
**Primary implementation:** Native C++ / libusb / TurboJPEG / OpenGL ES

---

# 1. 目的

Android端末をUSB Video Class（UVC）キャプチャデバイスと組み合わせ、低レイテンシのフィールドモニターとして構成する。

一般的なAndroid動画再生経路やMediaCodec系パイプラインを使用せず、USB入力からMJPEG decode、GPU描画、映像解析までをNative中心で実装する。

重視する項目：

- 低レイテンシ表示
- UVC入力フレームのFIFO滞留防止
- 最新フレーム優先
- CPUコピーの最小化
- CPU側RGB変換の回避
- GPU内での映像処理
- Scope処理をPreview critical pathから分離
- Waveform / RGB Parade / Histogram / Vectorscope
- SDR / HDR(PQ)入力への対応
- 実測値に基づくレイテンシ評価
- USB transportと映像decode/render処理の責務分離

本システムでは「全フレームを順番通り表示すること」よりも、リアルタイム監視における「現在に近い映像」を優先する。

基本原則：

```text
latest wins
busy -> wait ではなく busy -> skip
no video FIFO
measurement != preview processing
transport != decoder
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

Android 16移植・評価端末。

```text
Device      : Teclast P30T
SoC         : Unisoc T7250
OS          : Android 16
RAM         : 3 GB
GMS         : 搭載
Root        : 前提としない
```

A202ZT固有のMali/Gralloc挙動を前提にせず、Android標準API / NDK APIで成立する構成を維持する。

主な評価対象：

- USB Host API → native FD受け渡し
- libusb Bulk / Isochronous転送
- MS2109 / MS2130 1280×720 MJPEG 約60 fps
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

端末固有コードを増やすのではなく、

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

Android 16 / API 36ではStep 15DのFront Buffer / SurfaceControl経路を評価対象とするが、対応可否とレイテンシ改善量は実機検証前提とする。

---

## 2.2 UVCキャプチャデバイス

現在はMacroSilicon系のMS2109 / MS2130クラスを主対象とする。

現行実装ではデバイス名だけで転送方式を固定せず、UVC VideoStreaming interfaceとendpoint descriptorを探索し、**ISO / BULK transportを判定する**。

代表的な構成：

| Device class | UVC Format | Resolution / FPS | Typical USB transport | Decode / Render |
|---|---|---|---|---|
| MS2109-class | MJPEG | 1280×720 @ 約60 | USB 2.0 HS Isochronous | 共通MJPEG pipeline |
| MS2130-class | MJPEG | 1280×720 @ 約60 | USB 2.0 HS Bulk | 共通MJPEG pipeline |

旧720×480 YUYV fallbackは現行baselineから削除済みであり、1280×720 MJPEGを必須入力とする。

---

### 2.2.1 MS2109-class

代表的なUSB識別例：

```text
VID : 0x534D
PID : 0x2109
```

ただしtransport選択はVID/PIDのみで決定せず、UVC VS interface / endpoint descriptorを基準とする。

代表的なISO構成：

```text
UVC Format    : MJPEG
Resolution    : 1280 × 720
Frame Rate    : 約60 fps
Transport     : USB 2.0 High-Speed Isochronous
Endpoint      : 0x83 IN
Transfers     : 12
Packets/Tx    : 8
```

MS2109ではProcessing Unit controlを使用する構成があるため、VideoControl descriptorからProcessing Unit IDを探索し、必要なcontrolのみ適用する。

既存評価で使用した代表値：

```text
brightness = 0
contrast   = 128
saturation = 132
hue        = 0
```

Control設定時は可能な範囲で、

```text
SET_CUR
  ↓
GET_CUR
  ↓
設定値確認
```

まで実施する。

---

### 2.2.2 MS2130-class

代表構成：

```text
Input       : HDMI
UVC Format  : MJPEG
Resolution  : 1280 × 720
Frame Rate  : 約60 fps
Transport   : USB 2.0 High-Speed Bulk
Decode      : TurboJPEG
Output      : planar YCbCr 4:2:2 / 8-bit
```

代表的な実測例：

```text
fps              ≈ 59–60 fps
JPEG size        : source依存
wireFrame        ≈ 13 ms級
```

MS2109向けProcessing Unit固定値はMS2130-classへ機械的に適用しない。

---

## 2.3 Android Host依存性

UVCデバイス単体だけでなく、Android端末側のUSB Host実装、VBUS品質、GND、OTGアダプタ、コネクタ、USB PHY / signal integrityの影響を受ける。

MS2109-classの評価では、同一キャプチャデバイス・同一映像でもAndroid端末を変更するとMJPEG decode error / 下部フリッカーが再現しなくなるケースを確認した。

このケースではthermal要因は再現せず、端末側hardware条件の影響が疑われる。ただし、電源品質とsignal integrityのどちらが支配的かは未確定とする。

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
- latest decoded frame管理
- Persistent mapped PBO
- EGL
- OpenGL ES
- planar YCbCr texture upload
- Preview shader
- HDR/PQ Preview processing
- GPU Scope処理
- Presentation timing
- レイテンシ計測
- UVC / decoder error statistics

libusbはAndroid USB Host APIから取得したFDを使用し、Android側でlibusb独自のdevice discoveryは行わない。

```text
LIBUSB_OPTION_NO_DEVICE_DISCOVERY
```

---

# 4. 全体アーキテクチャ

現行baselineでは、**USB transportのみ分離し、UVC MJPEG payload以降を共通化**する。

```text
MS2109-class
ISO transport ─────┐
                   │
                   ├─> Common UVC MJPEG Payload Parser
                   │            ↓
MS2130-class       │       MJPEG Frame Assembler
BULK transport ────┘            ↓
                         Latest Pending JPEG
                                ↓
                          TurboJPEG Worker
                                ↓
                     planar YCbCr 4:2:2 8-bit
                                ↓
                      Latest Decoded Frame Slots
                                ↓
                      Persistent Mapped PBO ×2
                                ↓
                     3-plane GL_R8 Textures
                         ┌──────┴──────┐
                         ↓             ↓
                    Scope Path     Preview Path
                  source-domain   display-domain
                         ↓             ↓
                    GPU Compute   Fragment Shader
                         └──────┬──────┘
                                ↓
                         GLES UI Composition
                                ↓
                      EGL / BufferQueue
                     or Front Buffer path
```

重要な責務分離：

```text
ISO / BULK
= USB transportの違い

MJPEG parser / TurboJPEG / renderer / scope
= 共通処理
```

---

# 5. USB / UVC Transport

## 5.1 Endpoint選択

UVC VideoStreaming interfaceを探索し、対象endpointのtransfer typeを確認する。

概念：

```text
VID / PID
   ↓
UVC VS interface探索
   ↓
IN endpoint探索
   ↓
ISO  -> ISO backend
BULK -> BULK backend
```

デバイス名だけでbackendを固定しない。

---

## 5.2 Isochronous backend

MS2109-classで主に使用する。

代表構成：

```text
12 transfers
×
8 iso packets
```

複数transferを常時in-flightとし、専用libusb event threadでcompletionを処理する。

各完了ISO packetはUVC headerを保持したまま共通`uvc_mjpeg_decoder`へ渡す。

ISO transport側はJPEG decodeやrenderer用frame formatを持たない。

---

## 5.3 Bulk backend

MS2130-classで主に使用する。

Bulk completionで受信したUVC payloadを共通`uvc_mjpeg_decoder`へ渡す。

Bulk側もJPEG decodeを所有せず、transportに限定する。

---

## 5.4 UVC Payload

共通parserでは以下を確認する。

- header length
- FID
- EOF
- UVC STREAM ERR
- payload bytes
- SOI / EOI
- maximum compressed frame size

UVC payload error付きframe、EOI不成立frame、上限超過frameはdecode queueへ公開しない。

---

# 6. MJPEG / TurboJPEG Decode

共通decoderは`uvc_mjpeg_decoder`とする。

```text
UVC payload
    ↓
MJPEG assembler
    ↓
completed JPEG
    ↓
latest pending semantics
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

メモリlayout：

```text
Y    : 1280 × 720 = 921,600 bytes
Cb   :  640 × 720 = 460,800 bytes
Cr   :  640 × 720 = 460,800 bytes

Total = 1,843,200 bytes / frame
```

decoderは以下を要求する。

```text
width      = 1280
height     = 720
subsampling= TJSAMP_422
colorspace = YCbCr
```

decode失敗frameはREADYへpublishしない。

```text
decode success -> READY
decode failure -> DROP / slot FREE
```

queueをFIFOとして成長させず、worker処理中に新しいJPEGが完成した場合はpendingを最新へ置換できる。

---

# 7. フレーム管理

## 7.1 基本方針

```text
latest wins
```

通常FIFO queueは使用しない。

---

## 7.2 Latest Pending JPEG

decode前はcompleted JPEGを1件のpending slotとして扱う。

```text
old pending
    ↓ replace
newest pending
```

decode遅延時に古いJPEGを順番に処理しない。

---

## 7.3 Latest Decoded Frame Slots

TurboJPEG workerからrendererへplanar decoded frameを公開する。

READY化前に、

- sequence
- decoded Y/Cb/Cr
- decode completion timestamp
- B0→B2 timing

を確定する。

Rendererは`waitForDecodedFrame()`で起床し、`copyLatestFrame()`で最新completed frameのみ取得する。

decode error frameはこの層へ到達しない。

---

# 8. Event-Driven Renderer

レンダリングは固定60/120 Hzループではなく、新しいdecoded camera frameの到着をトリガーとする。

```text
new decoded frame READY
      ↓
condition_variable
      ↓
renderer wake
      ↓
latest frame取得
      ↓
PBO copy
      ↓
3-plane texture upload
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
1 new decoded source frame
=
1 render
=
1 present
```

decode失敗時は新しいREADYが発生しないため、前回の正常表示を保持する。

---

# 9. GPU Upload

共通MJPEG rendererではPersistent Mapped PBO ×2を使用する。

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

共通planar MJPEG texture：

```text
Y  : GL_R8 1280×720
Cb : GL_R8  640×720
Cr : GL_R8  640×720
```

1 contiguous PBOから3回の`glTexSubImage2D()`で各planeへuploadする。

CPU側でinterleaveやRGBA conversionは行わない。

旧packed YUYV / RGBA8 360×480 texture経路は現行baselineから削除済み。

---

# 11. Color Range / Matrix

HDMI sourceの色域 / transferと、UVC MJPEG decoded YCbCrの復元条件は分けて扱う。

MS2130-classでは実機range / color-bar診断結果を基準に、decoded interfaceをBT.601 limited-rangeとして復元している。

```text
Y  = (Ycode  - 16)  / 219
Cb = (Cbcode - 128) / 224
Cr = (Crcode - 128) / 224
```

これは、

```text
decoded JPEG YCbCr -> RGB
```

の復元条件であり、HDMI HDR source自体をBT.601色域と定義するものではない。

MS2109-classも現行共通rendererを通るため同じdecoded YCbCr処理を使用するが、device / firmware差を含む厳密なmatrix/range特性は継続検証対象とする。

---

# 12. HDR / PQ Architecture

MS2109 / MS2130の共通planar MJPEG rendererでは、入力modeがPQとして既知・選択されている場合にPreviewのみHDR→SDR変換を行う。

UVC側は8-bit MJPEGであるため、HDMI側の10-bit精度そのものを保持する構成ではない。

---

## 12.1 Measurement Path

ScopeにはPQ EOTF / Tone Mappingを入れない。

```text
decoded YCbCr source-domain
      │
      ├─ Luma Waveform
      ├─ RGB Parade
      ├─ RGB Histogram
      └─ Vectorscope
```

Calibration Profile無効時、Waveformはdecoded Y codeを0–255のまま保持する。Profile有効時は補正後RGBからProfileのColorimetry係数でLumaを再生成する。

```text
Y=16  -> bin 16
Y=235 -> bin 235
```

PQ入力を再度16–235へsqueezeしない。

これによりsuper-black / super-whiteおよびPQ code geometryを保持する。

---

## 12.2 Preview Path

PreviewのみHDR→SDR表示変換を行う。

```text
decoded YCbCr
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

Preview側のTone Mappingを変更してもScope geometryを変更しない。

---

## 12.3 Tone Mapping

Previewは203 nit付近をSDR reference whiteの基準として扱う。

```text
203 nit ≈ SDR white reference
```

代表的PQ code geometry：

```text
100 nit     ≈ 51 %
203 nit     ≈ 58 %
1000 nit    ≈ 75 %
4000 nit    ≈ 90 %
10000 nit   = 100 %
```

高輝度highlightはPreview側で圧縮する一方、Scopeはsource-domain code geometryを維持する。

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

4スコープをGPU Computeで処理する。

入力：

```text
Y  : 1280×720
Cb :  640×720
Cr :  640×720
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

Scope computeがbusyの場合は今回の解析をskipし、前回完了結果を保持する。

---

# 14. Luma Waveform

内部測定データは8-bit code value全域を保持する。

```text
Y = 0..255
```

表示グリッドは入力Rangeによって動的変更せず、Full Range固定とする。

```text
Left  : 0 / 25 / 50 / 75 / 100 %
Right : code 0 / 64 / 128 / 191 / 255
```

1280 source pixelを720 waveform X座標へdown-mapして表示する。

表示密度は平方根変換とし、入力高と同じ720 samples / source columnを基準にする。

```text
intensity = sqrt(count / 720)
```

HDR/PQ時もWaveformはsource code geometryを維持する。

---

# 15. RGB Parade

RGB Paradeはdecoded YCbCrからRGBへ変換した値をR/G/Bそれぞれ独立して集計する。

```text
Parade plot : 224 × 161
```

Tone Mapping後のPreview RGBを測定しない。

R/G/Bごとにframe内最大binで正規化し、表示密度は平方根変換する。Graticuleを先に描き、Parade信号を最後に合成する。

---

# 16. RGB Histogram

```text
3 × 256 bins
```

R/G/B各channelをsource-domain RGBとして集計する。

各channelを独立した最大binで正規化し、平方根密度で描画する。code 0と255はclipping binとして通常binより明るく表示する。

HDR Preview用Tone Mapping後の値は使用しない。

---

# 17. Vectorscope

Calibration Profile無効時はsource YCbCrのCb/Crを使用する。Profile有効時は補正後RGBから色差を再生成し、ProfileのBT.601 / BT.709 / BT.2020係数を使用する。

limited-range基準：

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

100% target geometryを維持し、target座標と測定座標は同じchroma scale 117を使用する。表示密度はframe内最大bin基準の平方根変換とする。Graticuleを先に描き、測定信号を最後に合成する。

Mali-G57互換性のため、readonly SSBO elementを関数へ直接渡さず、一度local variableへコピーする。

GLSL予約語との衝突も避ける。

---

# 18. Scope Scheduling

ScopeはPreviewのcritical pathを待たせない。

通常EGL path：

```text
Preview draw
    ↓
eglSwapBuffers()
    ↓
Scope fence poll
    ↓
Scope compute queue
    ↓
glFenceSync()
    ↓
glFlush()
```

`UVCFM_ENABLE_DIAGNOSTICS=ON`の場合だけ、swap直前に`eglPresentationTimeANDROID(now)`を追加し、frame timestampを追跡する。

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

- Calibration state
- Profile resolution
- Profile Colorimetry / FULL・LIMITED
- CLEAN
- UI FPS
- Scope labels

Profile未読込時はグレーのCAL badgeのみ表示する。読込時は緑の`CAL`とProfile parameterを表示する。固定のダミー入力文字列と画面下部の詳細Calibration状態表示は使用しない。ProfileのLOAD / UNLOADはAndroid常駐通知から行う。

Landscape / Portrait双方で映像領域とscope panelを分離する。

LandscapeではWaveformの直下に同幅のRGB Paradeを置き、その右列へVectorscopeとHistogramを配置する。Vectorscopeは不要な余白を除いた239×200基準で描画する。

---

# 20. Presentation Architecture

## 20.1 EGL / BufferQueue

既存の安定fallback。

```text
GLES composition
      ↓
EGLSurface
      ↓
eglSwapBuffers()
      ↓
SurfaceFlinger
      ↓
Display
```

Diagnostic buildで`EGL_ANDROID_get_frame_timestamps`が利用可能な端末では、

- COMPOSITE_DEADLINE
- COMPOSITE_INTERVAL
- COMPOSITE_TO_PRESENT_LATENCY
- COMPOSITION_LATCH_TIME
- DISPLAY_PRESENT_TIME

を取得して実表示時刻を計測できる。

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

目的はEGL window BufferQueueのdequeue / queue cycle回避。

対応しない端末ではEGL / BufferQueueへfallbackする。

A202ZTではFront Buffer usageが利用できない。

---

# 21. レイテンシ計測

## 21.1 共通MJPEG基準点

```text
B0 = UVC JPEG frame受信開始側
B2 = TurboJPEG decode完了 / decoded frame publish

T0 = decoded frame取得開始
T1 = CPU → PBO copy完了
T2 = texture upload発行完了
T3 = draw commands完了
T4 = present / swap直前
T5 = eglSwapBuffers return
PRESENT = EGL_DISPLAY_PRESENT_TIME_ANDROID
```

MS2130-classの既存安定実測例では、

```text
B0 → B2        ≈ 18.5～21 ms
B2 → PRESENT   ≈ 21.5～22.5 ms
B0 → PRESENT   ≈ 40～43 ms
```

を確認している。

MS2109-class 720p60 MJPEGについてはhost依存のdecode errorが確認された環境があるため、安定hostでのbaselineを別途取得する。

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
- decode error frameのdrop
- scope result更新skip
- 古いREADY frame破棄

---

# 23. Diagnostic / Logging

Native診断機能はCMake optionで一括制御する。

```text
UVCFM_ENABLE_DIAGNOSTICS=OFF  # default / field operation
UVCFM_ENABLE_DIAGNOSTICS=ON   # development measurement
```

OFF時に停止するもの：

```text
EGL frame timestamps / compositor timing
eglPresentationTimeANDROID diagnostic hint
per-frame latency history
ISO / BULK periodic statistics
TurboJPEG timing samples
Scope SSBO readback validation / periodic statistics
Colorbar / Range diagnostic
```

代表的なMJPEG decoder統計：

```text
fps
jpegBytes avg/min/max
queueMs
decodeCallMs
decodeTotalMs
B0toB2Ms
decodeErr
queueDrop
frameSlotDrop
totalDecoded
```

UVC PROBE / COMMITはUVC streaming protocolに必須であり診断機能ではないため、OFFでも実行する。EGL/GLES capability fallback、Shader compile/link確認、USB・decode・初期化失敗のエラーログも維持する。

---

# 24. Hardware / Signal Integrity診断方針

MS2109-classでdecode errorや局所フリッカーが発生した場合、software decoderだけでなくhost hardware条件も切り分ける。

確認項目：

- Android端末変更
- OTGアダプタ変更
- USBケーブル変更
- セルフパワーHub経由
- VBUS電圧 / droop
- GND品質
- connector接触
- USB PHY / signal integrity
- SDR / HDR入力差
- UVC ERR / malformed / ISO packet error / TurboJPEG decode error

特に、

```text
isoErr = 0
queueDrop = 0
frameSlotDrop = 0
decodeErr > 0
```

のようなケースでは、ホスト側で明示的なISO packet errorが検出されていなくても、capture device内部処理やhost電源条件を含めたhardware依存性を疑う。

thermal要因は別途切り分け、端末変更で症状が消える場合はhost側条件を優先して評価する。

---

# 25. 移植チェックリスト

## 25.1 Platform

- Android API level確認
- USB Host有効
- UVC device permission取得
- native FD受け渡し
- NDK build動作
- GLES version / extensions列挙
- EGL extensions列挙

## 25.2 USB

- UVC VS interface探索
- endpoint type判定
- ISO backend
- BULK backend
- 1280×720 MJPEG
- 約60 fps
- inflight transfer安定性
- UVC error / malformed / transfer error統計

## 25.3 Decode

- TurboJPEG build / ABI
- YUV422 planar decode
- decodeCall
- decodeErr
- queueDrop
- frameSlotDrop
- long-run安定性

## 25.4 GPU

- OpenGL ES 3.1以上
- Compute Shader
- `GL_EXT_buffer_storage`
- persistent mapped PBO
- GL_R8 3-plane upload
- Scope shader compile
- shader予約語 / readonly SSBO driver差

## 25.5 Presentation

- `EGL_ANDROID_presentation_time`
- `EGL_ANDROID_get_frame_timestamps`
- AHardwareBuffer
- EGLImage
- `AHARDWAREBUFFER_USAGE_FRONT_BUFFER`
- SurfaceControl NDK symbols
- Step15D activate/fallback判定

## 25.6 Memory / Thermal / USB Host

- RAM使用量
- CPU/GPU温度
- decode time
- dropped frame
- PBO busy skip
- render latency
- VBUS / OTG / cable依存性
- 端末間USB Host差

---

# 26. 設計原則

## 26.1 最新フレーム優先

```text
latest wins
```

古い映像を順番に表示しない。

## 26.2 Blocking回避

```text
busy -> skip
```

を優先する。

## 26.3 FIFOを作らない

映像フレームを複数段queueしない。

JPEG decode queueもlatest pending semanticsとする。

## 26.4 GPU内処理

映像解析のためにGPU→CPU readbackを常用しない。

CPU readbackは初期論理検証・diagnostic用途へ限定する。

## 26.5 Preview優先

Scope処理がPreview presentationを待たせない。

## 26.6 Measurement / Preview分離

```text
Source
  ├─ Measurement
  │    └─ source-domain
  │
  └─ Preview
       └─ display-domain
```

PQ EOTF、Tone Mapping、gamut mappingはPreview専用。

## 26.7 Transport / Decode分離

```text
ISO / BULK transport
       ↓
common UVC MJPEG decoder
       ↓
common renderer / scope
```

device-specific transport差をdecode / rendererへ持ち込まない。

## 26.8 実測優先

性能改善・不具合解析は体感のみで判断しない。

主要統計：

```text
UVC ERR / malformed / transfer error
JPEG bytes
decodeErr / queueDrop / frameSlotDrop
B0 / B2
T0 / T1 / T2 / T3 / T4 / T5
LATCH / PRESENT
scope busySkip
```

---

# 27. 現在の安定ベースライン

```text
Input
MS2109 / MS2130-class
1280×720 MJPEG ≈60 fps
SDR / HDR(PQ)

USB
descriptor-driven transport selection
ISO or BULK
libusb async

Decode
common uvc_mjpeg_decoder
TurboJPEG
planar YCbCr 4:2:2 8-bit
latest pending JPEG
latest decoded frame slots

GPU Upload
persistent mapped PBO ×2
3-plane GL_R8

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
one decoded source frame -> one present

Presentation
EGL / BufferQueue
Step15D Front Buffer available as Android 16 experiment
```

MS2109-classについては一部Android hostでhardware依存のMJPEG decode error / flickerを確認しており、安定hostでは再現しない。capture device / host / OTG /電源 / signal integrityの組み合わせを含めて評価する。

---

# 28. 未確定・今後の検証項目

- MS2109 / MS2130内部HDMI→MJPEG色変換の正確な仕様
- HDR10 Static MetadataのUVC側取得方法
- SDR / PQ / HLG自動判定
- 10-bit HDMI入力→8-bit MJPEG量子化特性
- chroma processing詳細
- BT.601 / BT.709 / BT.2020 matrixのdevice別厳密特性
- gamut compression方式
- HDR Tone Mapping最終カーブ
- Android host別VBUS / signal integrity差
- MS2109-classで観測したhost依存decode errorの電源/SI切り分け
- Unisoc T7250のGPU/EGL extension実機一覧
- Unisoc T7250でのPersistent PBO性能
- Unisoc T7250でのStep15D Front Buffer可否
- Android 16長時間60 fps動作時のthermal behavior

---

# 29. Version

```text
Document : Android UVC Field Monitor Architecture Specification
Revision : Current / 2026-09-30
Baseline : Shared 720p60 MJPEG pipeline for MS2109 / MS2130
Targets  : Android 13+ / A202ZT / Teclast P30T (Unisoc T7250)
```
