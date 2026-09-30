# Capture Calibration Profile適用（Phase 8）

## 概要

Calibration AppがExportしたProfile Format v1 JSONをField MonitorへImportし、対象DeviceとCapture Modeが一致した場合だけ補正を有効にする。

処理順は次のとおり。

```text
UVC Capture
→ Profile Match
→ ProfileのPU値を設定
→ JPEG YCbCrをBT.601 limited-rangeとしてR'G'B'へ復元
→ RGB Offset + 3×3 Matrix
→ Preview / Waveform / RGB Parade / Histogram / Vectorscope
→ PQ入力の場合のみPreviewへPQ EOTF・Tone Mapping
```

Calibration Search、Matrix Solve、Patch RecognitionはField Monitorでは行わない。

## Import

Androidの常駐通知にある`LOAD`からJSONを選択する。正常なProfileはアプリ内部へAtomic Commitし、次回起動時に再読込する。接続済みDeviceがある場合はUSBセッションを開き直し、ProfileのPU値を適用する。

通知の`UNLOAD`は保存JSONを削除し、Native補正を無効化する。接続済みDeviceがある場合はUSBセッションを開き直し、Profileなしの状態を即時反映する。画面内にはImportボタンを設けない。

`CALIBRATION_VALID`と`CALIBRATION_POOR_FIT`を適用対象とする。Profile未読込時はCAL badgeをグレー、読込時は緑の`CAL`と解像度、Colorimetry、FULL / LIMITEDを表示する。詳細な状態・エラー文字列を画面下部には表示せず、Logcatへ記録する。

## Matching

次の条件を満たした場合だけ補正を有効にする。

- Profile Format Versionが1
- VID / PIDが接続Deviceと一致
- ProfileにSerialが存在する場合、接続DeviceのSerialと一致
- Pixel FormatがMJPEG
- Resolutionが1280×720
- Frame Rateが60
- Input Encodingが`RGB`、`YCBCR_444`、`YCBCR_422`のいずれか
- Rangeが`FULL`または`LIMITED`
- Colorimetryが`BT601`、`BT709`、`BT2020`のいずれか
- Transfer Characteristicsが`SDR`または`PQ`

不一致またはJSON不正時はPUとRGB補正を無効にし、`PROFILE_MODE_MISMATCH`またはImport failureの詳細をLogcatへ記録する。推測、補間、部分適用は行わない。

## GPU適用

Profileの行列は行優先で受け取り、補正を次式で行う。

```text
correctedCode = Matrix × capturedCode + OffsetCode
```

Shader内部ではRGBを0〜1へ正規化しているため、Offsetのみ255で除算して同値の演算を行う。補正後は0〜1へClampする。

WaveformとVectorscopeは補正後RGBから再生成する。RGBからLuma／色差への変換係数はProfileのColorimetryを使用する。PQ EOTFとTone Mappingは従来どおりPreview専用で、Scopesは補正後のcode-domain信号を表示する。
