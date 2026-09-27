#include "uvc_mjpeg_decoder.h"

#include <android/log.h>
#include <turbojpeg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <time.h>
#include <vector>

#define LOG_TAG "UvcFieldMonitor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace uvc_mjpeg_decoder {
namespace {

static constexpr uint8_t UVC_STREAM_FID = 0x01;
static constexpr uint8_t UVC_STREAM_EOF = 0x02;
static constexpr uint8_t UVC_STREAM_ERR = 0x40;

static constexpr int EXPECTED_WIDTH = 1280;
static constexpr int EXPECTED_HEIGHT = 720;
static constexpr int EXPECTED_SUBSAMP = TJSAMP_422;

static constexpr size_t Y_STRIDE = 1280;
static constexpr size_t C_STRIDE = 640;
static constexpr size_t Y_BYTES = Y_STRIDE * 720u;
static constexpr size_t C_BYTES = C_STRIDE * 720u;
static constexpr size_t YUV422_FRAME_BYTES = Y_BYTES + C_BYTES + C_BYTES;
static constexpr size_t CB_OFFSET = Y_BYTES;
static constexpr size_t CR_OFFSET = Y_BYTES + C_BYTES;

static constexpr int DECODED_FRAME_SLOT_COUNT = 3;

// Color-bar/range diagnostics are development-only and consume CPU on
// decoded frames. Keep them off during normal field-monitor operation.
static constexpr bool ENABLE_PATTERN_DIAG = false;

enum DecodedSlotState : int {
    SLOT_FREE = 0,
    SLOT_WRITING = 1,
    SLOT_READY = 2,
    SLOT_READING = 3
};

struct FrameAssembler {
    bool active = false;
    bool bad = false;
    bool sawEoi = false;
    bool lastByteValid = false;

    uint8_t fid = 0;
    unsigned char lastByte = 0;

    uint64_t firstPayloadNs = 0;
    size_t eoiBytes = 0;

    std::vector<unsigned char> jpeg;
};

struct DecodeJob {
    std::vector<unsigned char> jpeg;
    uint64_t firstPayloadNs = 0;
    uint64_t eofNs = 0;
    uint64_t sequence = 0;
};

struct DecodedFrameSlot {
    // One contiguous 4:2:2 planar buffer:
    //   Y [0, CB_OFFSET)
    //   Cb [CB_OFFSET, CR_OFFSET)
    //   Cr [CR_OFFSET, YUV422_FRAME_BYTES)
    std::vector<unsigned char> yuv;

    std::atomic<int> state{SLOT_FREE};
    std::atomic<uint64_t> sequence{0};

    uint64_t decodeDoneRawNs = 0;
    uint64_t decodeDoneMonotonicNs = 0;
    double b0ToB2Ms = 0.0;
};

struct DecodeStats {
    uint64_t decoded = 0;
    uint64_t failed = 0;
    uint64_t queueDrops = 0;
    uint64_t frameSlotDrops = 0;

    uint64_t lastDecoded = 0;
    uint64_t lastFailed = 0;
    uint64_t lastQueueDrops = 0;
    uint64_t lastFrameSlotDrops = 0;
    uint64_t lastLogNs = 0;

    uint64_t jpegBytesSum = 0;
    size_t jpegBytesMin = std::numeric_limits<size_t>::max();
    size_t jpegBytesMax = 0;

    std::vector<double> queueMs;
    std::vector<double> headerMs;
    std::vector<double> decodeCallMs;
    std::vector<double> totalDecodeMs;
    std::vector<double> hostB0toB2Ms;
};

static FrameAssembler gFrame;
static uint32_t gMaxFrameBytes = 0;
static tjhandle gDecoder = nullptr;

static std::mutex gMutex;
static std::condition_variable gCv;
static std::condition_variable gFrameReadyCv;
static std::thread gWorker;
static std::atomic<bool> gRunning{false};
static bool gPendingValid = false;
static DecodeJob gPending;
static uint64_t gSubmittedSequence = 0;
static DecodeStats gStats;
static bool gOutputInfoLogged = false;

static std::array<DecodedFrameSlot, DECODED_FRAME_SLOT_COUNT> gDecodedSlots;
static std::atomic<uint64_t> gLatestReadySequence{0};


// ------------------------------------------------------------
// BT.601 / BT.709 color-bar diagnostic
//
// Diagnostic only: it never changes the live preview matrix.  A standard
// seven-bar top row is sampled at W/Y/C/G/M/R/B centers.  Both BT.601 and
// BT.709 inverse matrices are scored against the known RGB on/off pattern.
// Full-range and studio-range interpretations are scored separately as a
// secondary range hint.  Three consecutive matching diagnostics are required
// before a matrix result is locked/logged.
// ------------------------------------------------------------

enum class MatrixKind : int {
    Unknown = 0,
    Bt601 = 1,
    Bt709 = 2,
};

struct ColorBarSample {
    double y = 0.0;
    double cb = 128.0;
    double cr = 128.0;
    double maxStdDev = 0.0;
};

struct ColorModelScore {
    MatrixKind matrix = MatrixKind::Unknown;
    bool limitedRange = false;
    double rmse = std::numeric_limits<double>::infinity();
    double low = 0.0;
    double high = 0.0;
    double span = 0.0;
};

struct ColorBarDiagState {
    MatrixKind lastCandidate = MatrixKind::Unknown;
    MatrixKind locked = MatrixKind::Unknown;
    int consecutive = 0;
    uint64_t runs = 0;
};

static ColorBarDiagState gColorBarDiag;

static const char* matrixName(MatrixKind matrix)
{
    switch (matrix) {
        case MatrixKind::Bt601: return "BT.601";
        case MatrixKind::Bt709: return "BT.709";
        default: return "UNKNOWN";
    }
}


static ColorBarSample sampleColorBarRoi(
        const std::vector<unsigned char>& yuv,
        int barIndex)
{
    ColorBarSample out{};

    if (yuv.size() < YUV422_FRAME_BYTES ||
        barIndex < 0 || barIndex >= 7) {
        out.maxStdDev = std::numeric_limits<double>::infinity();
        return out;
    }

    // This SMPTE-style pattern has 12.5% gray guards on both sides; the seven
    // W/Y/C/G/M/R/B bars occupy the center 75%. Sample their exact centers.
    const int centerX = ((barIndex * 3 + 5) * EXPECTED_WIDTH) / 28;
    const int halfWidth = 18;
    const int x0 = std::max(0, centerX - halfWidth);
    const int x1 = std::min(EXPECTED_WIDTH - 1, centerX + halfWidth);
    const int y0 = (EXPECTED_HEIGHT * 20) / 100;
    const int y1 = (EXPECTED_HEIGHT * 40) / 100;

    double sumY = 0.0;
    double sumCb = 0.0;
    double sumCr = 0.0;
    double sumY2 = 0.0;
    double sumCb2 = 0.0;
    double sumCr2 = 0.0;
    uint64_t count = 0;

    const unsigned char* yPlane = yuv.data();
    const unsigned char* cbPlane = yuv.data() + CB_OFFSET;
    const unsigned char* crPlane = yuv.data() + CR_OFFSET;

    for (int yy = y0; yy <= y1; yy += 4) {
        for (int xx = x0; xx <= x1; xx += 4) {
            const double yCode =
                    static_cast<double>(yPlane[yy * Y_STRIDE + xx]);
            const int cx = xx >> 1;
            const double cbCode =
                    static_cast<double>(cbPlane[yy * C_STRIDE + cx]);
            const double crCode =
                    static_cast<double>(crPlane[yy * C_STRIDE + cx]);

            sumY += yCode;
            sumCb += cbCode;
            sumCr += crCode;
            sumY2 += yCode * yCode;
            sumCb2 += cbCode * cbCode;
            sumCr2 += crCode * crCode;
            ++count;
        }
    }

    if (count == 0) {
        out.maxStdDev = std::numeric_limits<double>::infinity();
        return out;
    }

    const double invCount = 1.0 / static_cast<double>(count);
    out.y = sumY * invCount;
    out.cb = sumCb * invCount;
    out.cr = sumCr * invCount;

    const auto stddev = [invCount](double sum, double sum2) {
        const double mean = sum * invCount;
        const double variance = std::max(0.0, sum2 * invCount - mean * mean);
        return std::sqrt(variance);
    };

    out.maxStdDev = std::max({
            stddev(sumY, sumY2),
            stddev(sumCb, sumCb2),
            stddev(sumCr, sumCr2),
    });

    return out;
}


static std::array<double, 3> decodeColorModel(
        const ColorBarSample& sample,
        MatrixKind matrix,
        bool limitedRange)
{
    double y = 0.0;
    double cb = 0.0;
    double cr = 0.0;

    if (limitedRange) {
        y = (sample.y - 16.0) / 219.0;
        cb = (sample.cb - 128.0) / 224.0;
        cr = (sample.cr - 128.0) / 224.0;
    }
    else {
        y = sample.y / 255.0;
        cb = (sample.cb - 128.0) / 255.0;
        cr = (sample.cr - 128.0) / 255.0;
    }

    std::array<double, 3> rgb{};

    if (matrix == MatrixKind::Bt709) {
        rgb[0] = y + 1.574800 * cr;
        rgb[1] = y - 0.187324 * cb - 0.468124 * cr;
        rgb[2] = y + 1.855600 * cb;
    }
    else {
        rgb[0] = y + 1.402000 * cr;
        rgb[1] = y - 0.344136 * cb - 0.714136 * cr;
        rgb[2] = y + 1.772000 * cb;
    }

    // Do not clamp here. Wrong matrices produce useful under/overshoot that
    // increases the score separation between BT.601 and BT.709.
    return rgb;
}


static ColorModelScore scoreColorBars(
        const std::array<ColorBarSample, 7>& bars,
        MatrixKind matrix,
        bool limitedRange)
{
    // Ignore the white bar for level fitting so 75% and 100% bar variants
    // both work.  Remaining order: Y, C, G, M, R, B.
    static constexpr int expected[6][3] = {
        {1, 1, 0},
        {0, 1, 1},
        {0, 1, 0},
        {1, 0, 1},
        {1, 0, 0},
        {0, 0, 1},
    };

    std::array<std::array<double, 3>, 6> rgb{};

    double onSum = 0.0;
    double offSum = 0.0;
    int onCount = 0;
    int offCount = 0;

    for (int bar = 0; bar < 6; ++bar) {
        rgb[bar] = decodeColorModel(bars[bar + 1], matrix, limitedRange);

        for (int channel = 0; channel < 3; ++channel) {
            if (expected[bar][channel] != 0) {
                onSum += rgb[bar][channel];
                ++onCount;
            }
            else {
                offSum += rgb[bar][channel];
                ++offCount;
            }
        }
    }

    ColorModelScore score{};
    score.matrix = matrix;
    score.limitedRange = limitedRange;

    if (onCount == 0 || offCount == 0) {
        return score;
    }

    score.high = onSum / static_cast<double>(onCount);
    score.low = offSum / static_cast<double>(offCount);
    score.span = score.high - score.low;

    if (score.span < 0.20) {
        return score;
    }

    double error2 = 0.0;
    int errorCount = 0;

    for (int bar = 0; bar < 6; ++bar) {
        for (int channel = 0; channel < 3; ++channel) {
            const double normalized =
                    (rgb[bar][channel] - score.low) / score.span;
            const double error =
                    normalized - static_cast<double>(expected[bar][channel]);
            error2 += error * error;
            ++errorCount;
        }
    }

    score.rmse =
            errorCount > 0
            ? std::sqrt(error2 / static_cast<double>(errorCount))
            : std::numeric_limits<double>::infinity();

    return score;
}


static void runColorBarDiagnostic(
        const std::vector<unsigned char>& yuv,
        uint64_t sequence)
{
    // 2 Hz at 60p. Tiny ROI sampling only; normal decode timing B2 is captured
    // and published before this diagnostic runs.
    if ((sequence % 30u) != 0u) {
        return;
    }

    ++gColorBarDiag.runs;

    std::array<ColorBarSample, 7> bars{};
    double maxStdDev = 0.0;

    for (int i = 0; i < 7; ++i) {
        bars[i] = sampleColorBarRoi(yuv, i);
        maxStdDev = std::max(maxStdDev, bars[i].maxStdDev);
    }

    const ColorModelScore s601Full =
            scoreColorBars(bars, MatrixKind::Bt601, false);
    const ColorModelScore s601Limited =
            scoreColorBars(bars, MatrixKind::Bt601, true);
    const ColorModelScore s709Full =
            scoreColorBars(bars, MatrixKind::Bt709, false);
    const ColorModelScore s709Limited =
            scoreColorBars(bars, MatrixKind::Bt709, true);

    const ColorModelScore best601 =
            s601Full.rmse <= s601Limited.rmse ? s601Full : s601Limited;
    const ColorModelScore best709 =
            s709Full.rmse <= s709Limited.rmse ? s709Full : s709Limited;

    const MatrixKind candidate =
            best601.rmse <= best709.rmse
            ? MatrixKind::Bt601
            : MatrixKind::Bt709;

    const ColorModelScore best =
            candidate == MatrixKind::Bt601 ? best601 : best709;

    const double matrixDelta = std::abs(best601.rmse - best709.rmse);
    const double whiteChroma = std::sqrt(
            (bars[0].cb - 128.0) * (bars[0].cb - 128.0) +
            (bars[0].cr - 128.0) * (bars[0].cr - 128.0));

    double colorChroma = 0.0;
    for (int i = 1; i < 7; ++i) {
        colorChroma += std::sqrt(
                (bars[i].cb - 128.0) * (bars[i].cb - 128.0) +
                (bars[i].cr - 128.0) * (bars[i].cr - 128.0));
    }
    colorChroma /= 6.0;

    const bool looksLikeBars =
            std::isfinite(best.rmse) &&
            maxStdDev <= 12.0 &&
            whiteChroma <= 20.0 &&
            colorChroma >= 30.0 &&
            best.span >= 0.35 &&
            best.rmse <= 0.060 &&
            matrixDelta >= 0.018;

    if (!looksLikeBars) {
        gColorBarDiag.lastCandidate = MatrixKind::Unknown;
        gColorBarDiag.consecutive = 0;

        if (gColorBarDiag.runs <= 3 || (gColorBarDiag.runs % 10u) == 0u) {
            LOGI(
                    "UVC MJPEG COLORBAR DIAG: no stable 7-bar match seq=%llu "
                    "score601=%.4f score709=%.4f delta=%.4f "
                    "roiStdMax=%.2f whiteChroma=%.2f colorChroma=%.2f",
                    static_cast<unsigned long long>(sequence),
                    best601.rmse,
                    best709.rmse,
                    matrixDelta,
                    maxStdDev,
                    whiteChroma,
                    colorChroma
            );
        }
        return;
    }

    if (gColorBarDiag.lastCandidate == candidate) {
        ++gColorBarDiag.consecutive;
    }
    else {
        gColorBarDiag.lastCandidate = candidate;
        gColorBarDiag.consecutive = 1;
    }

    const ColorModelScore candidateFull =
            candidate == MatrixKind::Bt601 ? s601Full : s709Full;
    const ColorModelScore candidateLimited =
            candidate == MatrixKind::Bt601 ? s601Limited : s709Limited;
    const double rangeDelta =
            std::abs(candidateFull.rmse - candidateLimited.rmse);
    const char* rangeHint =
            rangeDelta < 0.004
            ? "AMBIGUOUS"
            : (candidateFull.rmse < candidateLimited.rmse ? "FULL" : "LIMITED");

    if (gColorBarDiag.consecutive <= 3 ||
        (gColorBarDiag.runs % 10u) == 0u) {
        LOGI(
                "UVC MJPEG COLORBAR DIAG: seq=%llu candidate=%s streak=%d/3 "
                "score601=%.4f score709=%.4f delta=%.4f "
                "rangeHint=%s full=%.4f limited=%.4f "
                "roiStdMax=%.2f whiteChroma=%.2f",
                static_cast<unsigned long long>(sequence),
                matrixName(candidate),
                gColorBarDiag.consecutive,
                best601.rmse,
                best709.rmse,
                matrixDelta,
                rangeHint,
                candidateFull.rmse,
                candidateLimited.rmse,
                maxStdDev,
                whiteChroma
        );
    }

    if (gColorBarDiag.consecutive >= 3 &&
        gColorBarDiag.locked != candidate) {

        gColorBarDiag.locked = candidate;

        LOGI(
                "UVC MJPEG COLORBAR MATRIX LOCK: %s rangeHint=%s "
                "score601=%.4f score709=%.4f "
                "(diagnostic only; preview/scope matrix is NOT auto-switched)",
                matrixName(candidate),
                rangeHint,
                best601.rmse,
                best709.rmse
        );

        LOGI(
                "UVC MJPEG COLORBAR YCbCr means: "
                "W=%.1f/%.1f/%.1f Y=%.1f/%.1f/%.1f "
                "C=%.1f/%.1f/%.1f G=%.1f/%.1f/%.1f "
                "M=%.1f/%.1f/%.1f R=%.1f/%.1f/%.1f "
                "B=%.1f/%.1f/%.1f",
                bars[0].y, bars[0].cb, bars[0].cr,
                bars[1].y, bars[1].cb, bars[1].cr,
                bars[2].y, bars[2].cb, bars[2].cr,
                bars[3].y, bars[3].cb, bars[3].cr,
                bars[4].y, bars[4].cb, bars[4].cr,
                bars[5].y, bars[5].cb, bars[5].cr,
                bars[6].y, bars[6].cb, bars[6].cr
        );
    }
}


// ------------------------------------------------------------
// HDMI/JPEG luma range diagnostic
//
// Use with the companion 9-bar grayscale generator. The source framebuffer
// levels are:
//
//   0, 8, 16, 64, 128, 192, 235, 247, 255
//
// This diagnostic reports the effective source-code -> decoded JPEG-Y mapping
// for the HDMI source configuration actually under test. It distinguishes an
// approximately identity/full mapping from a 16..235 studio mapping and also
// reports a least-squares affine fit. It does not change preview/scope state.
// ------------------------------------------------------------

enum class RangeMapKind : int {
    Unknown = 0,
    FullMap = 1,
    StudioMap = 2,
};

struct RangeDiagState {
    RangeMapKind lastCandidate = RangeMapKind::Unknown;
    RangeMapKind locked = RangeMapKind::Unknown;
    int consecutive = 0;
    uint64_t runs = 0;
};

struct RangeModelScore {
    RangeMapKind kind = RangeMapKind::Unknown;
    double rmse = std::numeric_limits<double>::infinity();
};

static RangeDiagState gRangeDiag;

static constexpr std::array<int, 9> RANGE_SOURCE_CODES = {
        0, 8, 16, 64, 128, 192, 235, 247, 255,
};

static const char* rangeMapName(RangeMapKind kind)
{
    switch (kind) {
        case RangeMapKind::FullMap: return "FULL_MAP";
        case RangeMapKind::StudioMap: return "STUDIO_MAP";
        default: return "UNKNOWN";
    }
}


static ColorBarSample sampleRangeRoi(
        const std::vector<unsigned char>& yuv,
        int barIndex)
{
    ColorBarSample out{};

    if (yuv.size() < YUV422_FRAME_BYTES ||
        barIndex < 0 || barIndex >= static_cast<int>(RANGE_SOURCE_CODES.size())) {
        out.maxStdDev = std::numeric_limits<double>::infinity();
        return out;
    }

    // Companion Python pattern is nine equal-width vertical gray bars.
    const int barCount = static_cast<int>(RANGE_SOURCE_CODES.size());
    const int centerX = ((barIndex * 2 + 1) * EXPECTED_WIDTH) / (barCount * 2);
    const int halfWidth = 22;
    const int x0 = std::max(0, centerX - halfWidth);
    const int x1 = std::min(EXPECTED_WIDTH - 1, centerX + halfWidth);
    const int y0 = (EXPECTED_HEIGHT * 35) / 100;
    const int y1 = (EXPECTED_HEIGHT * 65) / 100;

    double sumY = 0.0;
    double sumCb = 0.0;
    double sumCr = 0.0;
    double sumY2 = 0.0;
    double sumCb2 = 0.0;
    double sumCr2 = 0.0;
    uint64_t count = 0;

    const unsigned char* yPlane = yuv.data();
    const unsigned char* cbPlane = yuv.data() + CB_OFFSET;
    const unsigned char* crPlane = yuv.data() + CR_OFFSET;

    for (int yy = y0; yy <= y1; yy += 4) {
        for (int xx = x0; xx <= x1; xx += 4) {
            const double yCode =
                    static_cast<double>(yPlane[yy * Y_STRIDE + xx]);
            const int cx = xx >> 1;
            const double cbCode =
                    static_cast<double>(cbPlane[yy * C_STRIDE + cx]);
            const double crCode =
                    static_cast<double>(crPlane[yy * C_STRIDE + cx]);

            sumY += yCode;
            sumCb += cbCode;
            sumCr += crCode;
            sumY2 += yCode * yCode;
            sumCb2 += cbCode * cbCode;
            sumCr2 += crCode * crCode;
            ++count;
        }
    }

    if (count == 0) {
        out.maxStdDev = std::numeric_limits<double>::infinity();
        return out;
    }

    const double invCount = 1.0 / static_cast<double>(count);
    out.y = sumY * invCount;
    out.cb = sumCb * invCount;
    out.cr = sumCr * invCount;

    const auto stddev = [invCount](double sum, double sum2) {
        const double mean = sum * invCount;
        const double variance = std::max(0.0, sum2 * invCount - mean * mean);
        return std::sqrt(variance);
    };

    out.maxStdDev = std::max({
            stddev(sumY, sumY2),
            stddev(sumCb, sumCb2),
            stddev(sumCr, sumCr2),
    });

    return out;
}


static RangeModelScore scoreRangeModel(
        const std::array<ColorBarSample, 9>& bars,
        RangeMapKind kind)
{
    RangeModelScore score{};
    score.kind = kind;

    double error2 = 0.0;

    for (size_t i = 0; i < RANGE_SOURCE_CODES.size(); ++i) {
        const double source = static_cast<double>(RANGE_SOURCE_CODES[i]);
        const double expected =
                kind == RangeMapKind::StudioMap
                ? 16.0 + source * (219.0 / 255.0)
                : source;
        const double error = bars[i].y - expected;
        error2 += error * error;
    }

    score.rmse = std::sqrt(
            error2 / static_cast<double>(RANGE_SOURCE_CODES.size()));
    return score;
}


static void fitRangeAffine(
        const std::array<ColorBarSample, 9>& bars,
        double& slope,
        double& intercept,
        double& rmse)
{
    double meanX = 0.0;
    double meanY = 0.0;

    for (size_t i = 0; i < RANGE_SOURCE_CODES.size(); ++i) {
        meanX += static_cast<double>(RANGE_SOURCE_CODES[i]);
        meanY += bars[i].y;
    }

    meanX /= static_cast<double>(RANGE_SOURCE_CODES.size());
    meanY /= static_cast<double>(RANGE_SOURCE_CODES.size());

    double cov = 0.0;
    double varX = 0.0;

    for (size_t i = 0; i < RANGE_SOURCE_CODES.size(); ++i) {
        const double dx = static_cast<double>(RANGE_SOURCE_CODES[i]) - meanX;
        const double dy = bars[i].y - meanY;
        cov += dx * dy;
        varX += dx * dx;
    }

    slope = varX > 0.0 ? cov / varX : 0.0;
    intercept = meanY - slope * meanX;

    double error2 = 0.0;
    for (size_t i = 0; i < RANGE_SOURCE_CODES.size(); ++i) {
        const double predicted =
                slope * static_cast<double>(RANGE_SOURCE_CODES[i]) + intercept;
        const double error = bars[i].y - predicted;
        error2 += error * error;
    }

    rmse = std::sqrt(
            error2 / static_cast<double>(RANGE_SOURCE_CODES.size()));
}


static void runRangeDiagnostic(
        const std::vector<unsigned char>& yuv,
        uint64_t sequence)
{
    // Same 2 Hz cadence as the matrix diagnostic. Both run after B2 publish.
    if ((sequence % 30u) != 0u) {
        return;
    }

    ++gRangeDiag.runs;

    std::array<ColorBarSample, 9> bars{};
    double maxStdDev = 0.0;
    double maxNeutralChroma = 0.0;
    bool monotonic = true;

    for (size_t i = 0; i < bars.size(); ++i) {
        bars[i] = sampleRangeRoi(yuv, static_cast<int>(i));
        maxStdDev = std::max(maxStdDev, bars[i].maxStdDev);

        const double neutralChroma = std::sqrt(
                (bars[i].cb - 128.0) * (bars[i].cb - 128.0) +
                (bars[i].cr - 128.0) * (bars[i].cr - 128.0));
        maxNeutralChroma = std::max(maxNeutralChroma, neutralChroma);

        if (i > 0 && bars[i].y <= bars[i - 1].y + 2.0) {
            monotonic = false;
        }
    }

    const RangeModelScore full =
            scoreRangeModel(bars, RangeMapKind::FullMap);
    const RangeModelScore studio =
            scoreRangeModel(bars, RangeMapKind::StudioMap);

    const RangeMapKind candidate =
            full.rmse <= studio.rmse
            ? RangeMapKind::FullMap
            : RangeMapKind::StudioMap;
    const double bestRmse = std::min(full.rmse, studio.rmse);
    const double modelDelta = std::abs(full.rmse - studio.rmse);
    const double span = bars.back().y - bars.front().y;

    double fitSlope = 0.0;
    double fitIntercept = 0.0;
    double fitRmse = 0.0;
    fitRangeAffine(bars, fitSlope, fitIntercept, fitRmse);

    const bool looksLikeRangePattern =
            std::isfinite(bestRmse) &&
            maxStdDev <= 3.0 &&
            maxNeutralChroma <= 8.0 &&
            monotonic &&
            span >= 180.0 &&
            bestRmse <= 6.0 &&
            modelDelta >= 6.0;

    if (!looksLikeRangePattern) {
        gRangeDiag.lastCandidate = RangeMapKind::Unknown;
        gRangeDiag.consecutive = 0;

        if (gRangeDiag.runs <= 3 || (gRangeDiag.runs % 10u) == 0u) {
            LOGI(
                    "UVC MJPEG RANGE DIAG: no stable 9-gray match seq=%llu "
                    "rmseFull=%.2f rmseStudio=%.2f delta=%.2f "
                    "fit=%.5f*x%+.2f fitRmse=%.2f span=%.1f "
                    "roiStdMax=%.2f neutralChromaMax=%.2f monotonic=%s",
                    static_cast<unsigned long long>(sequence),
                    full.rmse,
                    studio.rmse,
                    modelDelta,
                    fitSlope,
                    fitIntercept,
                    fitRmse,
                    span,
                    maxStdDev,
                    maxNeutralChroma,
                    monotonic ? "YES" : "NO"
            );
        }
        return;
    }

    if (gRangeDiag.lastCandidate == candidate) {
        ++gRangeDiag.consecutive;
    }
    else {
        gRangeDiag.lastCandidate = candidate;
        gRangeDiag.consecutive = 1;
    }

    if (gRangeDiag.consecutive <= 3 ||
        (gRangeDiag.runs % 10u) == 0u) {
        LOGI(
                "UVC MJPEG RANGE DIAG: seq=%llu candidate=%s streak=%d/3 "
                "rmseFull=%.2f rmseStudio=%.2f delta=%.2f "
                "fit=%.5f*x%+.2f fitRmse=%.2f "
                "Y=[%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f]",
                static_cast<unsigned long long>(sequence),
                rangeMapName(candidate),
                gRangeDiag.consecutive,
                full.rmse,
                studio.rmse,
                modelDelta,
                fitSlope,
                fitIntercept,
                fitRmse,
                bars[0].y,
                bars[1].y,
                bars[2].y,
                bars[3].y,
                bars[4].y,
                bars[5].y,
                bars[6].y,
                bars[7].y,
                bars[8].y
        );
    }

    if (gRangeDiag.consecutive >= 3 &&
        gRangeDiag.locked != candidate) {

        gRangeDiag.locked = candidate;

        LOGI(
                "UVC MJPEG RANGE LOCK: %s rmseFull=%.2f rmseStudio=%.2f "
                "fitY=%.5f*x%+.2f fitRmse=%.2f "
                "black=%.1f code16=%.1f code235=%.1f white=%.1f "
                "(effective HDMI source-code -> decoded JPEG-Y mapping; "
                "diagnostic only)",
                rangeMapName(candidate),
                full.rmse,
                studio.rmse,
                fitSlope,
                fitIntercept,
                fitRmse,
                bars[0].y,
                bars[2].y,
                bars[6].y,
                bars[8].y
        );
    }
}


static uint64_t nowNs()
{
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
            ).count()
    );
}


static uint64_t nowMonotonicRawNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}


static uint64_t nowMonotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}


static int claimFreeDecodedSlot()
{
    for (int i = 0; i < DECODED_FRAME_SLOT_COUNT; ++i) {
        int expected = SLOT_FREE;
        if (gDecodedSlots[i].state.compare_exchange_strong(
                expected,
                SLOT_WRITING,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return i;
        }
    }
    return -1;
}


static int reclaimOldestReadyDecodedSlot()
{
    int candidate = -1;
    uint64_t oldestSequence = std::numeric_limits<uint64_t>::max();

    for (int i = 0; i < DECODED_FRAME_SLOT_COUNT; ++i) {
        if (gDecodedSlots[i].state.load(std::memory_order_acquire) != SLOT_READY) {
            continue;
        }

        const uint64_t sequence =
                gDecodedSlots[i].sequence.load(std::memory_order_acquire);

        if (sequence < oldestSequence) {
            oldestSequence = sequence;
            candidate = i;
        }
    }

    if (candidate < 0) {
        return -1;
    }

    int expected = SLOT_READY;
    if (!gDecodedSlots[candidate].state.compare_exchange_strong(
            expected,
            SLOT_WRITING,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return -1;
    }

    return candidate;
}


static int acquireDecodedWriteSlot()
{
    const int freeSlot = claimFreeDecodedSlot();
    return freeSlot >= 0 ? freeSlot : reclaimOldestReadyDecodedSlot();
}


static void resetDecodedSlots()
{
    for (;;) {
        bool reading = false;
        for (DecodedFrameSlot& slot : gDecodedSlots) {
            if (slot.state.load(std::memory_order_acquire) == SLOT_READING) {
                reading = true;
                break;
            }
        }
        if (!reading) {
            break;
        }
        std::this_thread::yield();
    }

    for (DecodedFrameSlot& slot : gDecodedSlots) {
        slot.state.store(SLOT_FREE, std::memory_order_release);
        slot.sequence.store(0, std::memory_order_release);
        slot.decodeDoneRawNs = 0;
        slot.decodeDoneMonotonicNs = 0;
        slot.b0ToB2Ms = 0.0;
        slot.yuv.clear();
    }

    gLatestReadySequence.store(0, std::memory_order_release);
}


static double average(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }

    double sum = 0.0;
    for (double value : values) {
        sum += value;
    }
    return sum / static_cast<double>(values.size());
}


static double percentile(
        const std::vector<double>& values,
        double q)
{
    if (values.empty()) {
        return 0.0;
    }

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());

    const size_t index =
            static_cast<size_t>(
                    std::min<double>(
                            static_cast<double>(sorted.size() - 1),
                            q * static_cast<double>(sorted.size() - 1)
                    )
            );

    return sorted[index];
}


static void resetFrame()
{
    gFrame.active = false;
    gFrame.bad = false;
    gFrame.sawEoi = false;
    gFrame.lastByteValid = false;
    gFrame.fid = 0;
    gFrame.lastByte = 0;
    gFrame.firstPayloadNs = 0;
    gFrame.eoiBytes = 0;
    gFrame.jpeg.clear();
}


static void maybeLogStatsLocked(uint64_t timestampNs)
{
    if (gStats.lastLogNs == 0) {
        gStats.lastLogNs = timestampNs;
        return;
    }

    const uint64_t elapsedNs = timestampNs - gStats.lastLogNs;
    if (elapsedNs < 1'000'000'000ULL) {
        return;
    }

    const uint64_t decodedDelta = gStats.decoded - gStats.lastDecoded;
    const uint64_t failedDelta = gStats.failed - gStats.lastFailed;
    const uint64_t queueDropDelta = gStats.queueDrops - gStats.lastQueueDrops;
    const uint64_t slotDropDelta =
            gStats.frameSlotDrops - gStats.lastFrameSlotDrops;

    const double elapsedSeconds =
            static_cast<double>(elapsedNs) / 1'000'000'000.0;

    const double decodeFps =
            elapsedSeconds > 0.0
            ? static_cast<double>(decodedDelta) / elapsedSeconds
            : 0.0;

    const size_t sampleCount = gStats.decodeCallMs.size();
    const double jpegAvg =
            sampleCount > 0
            ? static_cast<double>(gStats.jpegBytesSum) /
              static_cast<double>(sampleCount)
            : 0.0;

    const size_t jpegMin =
            gStats.jpegBytesMin == std::numeric_limits<size_t>::max()
            ? 0
            : gStats.jpegBytesMin;

    LOGI(
            "UVC MJPEG TurboJPEG YUV422 stats: fps=%.2f "
            "jpegBytes avg=%.0f min=%zu max=%zu "
            "queueMs avg=%.3f p95=%.3f max=%.3f "
            "headerMs avg=%.3f p95=%.3f max=%.3f "
            "decodeCallMs avg=%.3f p50=%.3f p95=%.3f max=%.3f "
            "decodeTotalMs avg=%.3f p50=%.3f p95=%.3f max=%.3f "
            "B0toB2Ms avg=%.3f p50=%.3f p95=%.3f max=%.3f "
            "decodeErr=%llu queueDrop=%llu frameSlotDrop=%llu totalDecoded=%llu",
            decodeFps,
            jpegAvg,
            jpegMin,
            gStats.jpegBytesMax,
            average(gStats.queueMs),
            percentile(gStats.queueMs, 0.95),
            percentile(gStats.queueMs, 1.00),
            average(gStats.headerMs),
            percentile(gStats.headerMs, 0.95),
            percentile(gStats.headerMs, 1.00),
            average(gStats.decodeCallMs),
            percentile(gStats.decodeCallMs, 0.50),
            percentile(gStats.decodeCallMs, 0.95),
            percentile(gStats.decodeCallMs, 1.00),
            average(gStats.totalDecodeMs),
            percentile(gStats.totalDecodeMs, 0.50),
            percentile(gStats.totalDecodeMs, 0.95),
            percentile(gStats.totalDecodeMs, 1.00),
            average(gStats.hostB0toB2Ms),
            percentile(gStats.hostB0toB2Ms, 0.50),
            percentile(gStats.hostB0toB2Ms, 0.95),
            percentile(gStats.hostB0toB2Ms, 1.00),
            static_cast<unsigned long long>(failedDelta),
            static_cast<unsigned long long>(queueDropDelta),
            static_cast<unsigned long long>(slotDropDelta),
            static_cast<unsigned long long>(gStats.decoded)
    );

    gStats.lastLogNs = timestampNs;
    gStats.lastDecoded = gStats.decoded;
    gStats.lastFailed = gStats.failed;
    gStats.lastQueueDrops = gStats.queueDrops;
    gStats.lastFrameSlotDrops = gStats.frameSlotDrops;

    gStats.jpegBytesSum = 0;
    gStats.jpegBytesMin = std::numeric_limits<size_t>::max();
    gStats.jpegBytesMax = 0;

    gStats.queueMs.clear();
    gStats.headerMs.clear();
    gStats.decodeCallMs.clear();
    gStats.totalDecodeMs.clear();
    gStats.hostB0toB2Ms.clear();
}


static void workerLoop()
{
    pthread_setname_np(pthread_self(), "UVC-MJPEG");

    LOGI(
            "UVC MJPEG TurboJPEG worker started "
            "(persistent decoder, planar Y/Cb/Cr 4:2:2, latest-frame output)"
    );

    for (;;) {
        DecodeJob job;

        {
            std::unique_lock<std::mutex> lock(gMutex);

            gCv.wait(
                    lock,
                    []() {
                        return !gRunning.load(std::memory_order_acquire) ||
                               gPendingValid;
                    }
            );

            if (!gRunning.load(std::memory_order_acquire) && !gPendingValid) {
                break;
            }

            job = gPending;
            gPendingValid = false;
        }

        const uint64_t decodeStartNs = nowNs();

        int width = 0;
        int height = 0;
        int subsamp = -1;
        int colorspace = -1;

        const int headerResult =
                tjDecompressHeader3(
                        gDecoder,
                        job.jpeg.data(),
                        static_cast<unsigned long>(job.jpeg.size()),
                        &width,
                        &height,
                        &subsamp,
                        &colorspace
                );

        const uint64_t decodeCallStartNs = nowNs();

        bool ok = headerResult == 0;
        int writeSlot = -1;
        int decodeResult = -1;

        if (ok) {
            const int yPlaneWidth = tjPlaneWidth(0, width, subsamp);
            const int cbPlaneWidth = tjPlaneWidth(1, width, subsamp);
            const int crPlaneWidth = tjPlaneWidth(2, width, subsamp);
            const int yPlaneHeight = tjPlaneHeight(0, height, subsamp);
            const int cbPlaneHeight = tjPlaneHeight(1, height, subsamp);
            const int crPlaneHeight = tjPlaneHeight(2, height, subsamp);

            if (width != EXPECTED_WIDTH ||
                height != EXPECTED_HEIGHT ||
                subsamp != EXPECTED_SUBSAMP ||
                colorspace != TJCS_YCbCr ||
                yPlaneWidth != EXPECTED_WIDTH ||
                cbPlaneWidth != EXPECTED_WIDTH / 2 ||
                crPlaneWidth != EXPECTED_WIDTH / 2 ||
                yPlaneHeight != EXPECTED_HEIGHT ||
                cbPlaneHeight != EXPECTED_HEIGHT ||
                crPlaneHeight != EXPECTED_HEIGHT) {

                LOGE(
                        "UVC MJPEG TurboJPEG geometry unsupported: "
                        "%dx%d subsamp=%d colorspace=%d planes="
                        "Y=%dx%d Cb=%dx%d Cr=%dx%d expected=1280x720 TJSAMP_422/TJCS_YCbCr",
                        width,
                        height,
                        subsamp,
                        colorspace,
                        yPlaneWidth,
                        yPlaneHeight,
                        cbPlaneWidth,
                        cbPlaneHeight,
                        crPlaneWidth,
                        crPlaneHeight
                );
                ok = false;
            }
        }

        if (ok) {
            writeSlot = acquireDecodedWriteSlot();

            if (writeSlot < 0) {
                std::lock_guard<std::mutex> lock(gMutex);
                ++gStats.frameSlotDrops;
                ok = false;
            }
        }

        if (ok) {
            DecodedFrameSlot& slot = gDecodedSlots[writeSlot];
            slot.yuv.resize(YUV422_FRAME_BYTES);

            unsigned char* planes[3] = {
                    slot.yuv.data(),
                    slot.yuv.data() + CB_OFFSET,
                    slot.yuv.data() + CR_OFFSET,
            };

            int strides[3] = {
                    static_cast<int>(Y_STRIDE),
                    static_cast<int>(C_STRIDE),
                    static_cast<int>(C_STRIDE),
            };

            decodeResult =
                    tjDecompressToYUVPlanes(
                            gDecoder,
                            job.jpeg.data(),
                            static_cast<unsigned long>(job.jpeg.size()),
                            planes,
                            EXPECTED_WIDTH,
                            strides,
                            EXPECTED_HEIGHT,
                            0
                    );

            ok = decodeResult == 0;

            if (!ok && gStats.failed < 10) {
                LOGE(
                        "UVC MJPEG TurboJPEG planar decode error: %s",
                        tjGetErrorStr2(gDecoder)
                );
            }
        }

        const uint64_t decodeDoneNs = nowNs();
        const uint64_t decodeDoneRawNs = nowMonotonicRawNs();
        const uint64_t decodeDoneMonotonicNs = nowMonotonicNs();

        const double queueMs =
                static_cast<double>(decodeStartNs - job.eofNs) / 1'000'000.0;

        const double headerMs =
                static_cast<double>(decodeCallStartNs - decodeStartNs) /
                1'000'000.0;

        const double decodeCallMs =
                static_cast<double>(decodeDoneNs - decodeCallStartNs) /
                1'000'000.0;

        const double totalDecodeMs =
                static_cast<double>(decodeDoneNs - decodeStartNs) /
                1'000'000.0;

        const double hostB0toB2Ms =
                static_cast<double>(decodeDoneNs - job.firstPayloadNs) /
                1'000'000.0;

        if (ok) {
            DecodedFrameSlot& slot = gDecodedSlots[writeSlot];

            slot.decodeDoneRawNs = decodeDoneRawNs;
            slot.decodeDoneMonotonicNs = decodeDoneMonotonicNs;
            slot.b0ToB2Ms = hostB0toB2Ms;
            slot.sequence.store(job.sequence, std::memory_order_relaxed);

            slot.state.store(SLOT_READY, std::memory_order_release);
            gLatestReadySequence.store(job.sequence, std::memory_order_release);
            gFrameReadyCv.notify_one();

            // Low-rate color-bar diagnostic runs after B2 timestamp/publication
            // so it does not inflate the measured JPEG decode completion time.
            if (ENABLE_PATTERN_DIAG) {
                runColorBarDiagnostic(slot.yuv, job.sequence);
                runRangeDiagnostic(slot.yuv, job.sequence);
            }
        }
        else if (writeSlot >= 0) {
            gDecodedSlots[writeSlot].state.store(SLOT_FREE, std::memory_order_release);
        }

        std::lock_guard<std::mutex> lock(gMutex);

        if (ok) {
            ++gStats.decoded;

            gStats.jpegBytesSum += static_cast<uint64_t>(job.jpeg.size());
            gStats.jpegBytesMin = std::min(gStats.jpegBytesMin, job.jpeg.size());
            gStats.jpegBytesMax = std::max(gStats.jpegBytesMax, job.jpeg.size());

            gStats.queueMs.push_back(queueMs);
            gStats.headerMs.push_back(headerMs);
            gStats.decodeCallMs.push_back(decodeCallMs);
            gStats.totalDecodeMs.push_back(totalDecodeMs);
            gStats.hostB0toB2Ms.push_back(hostB0toB2Ms);

            if (!gOutputInfoLogged) {
                LOGI(
                        "UVC MJPEG TurboJPEG output: %dx%d subsamp=TJSAMP_422 "
                        "colorspace=%d Y=%zux%d Cb=%zux%d Cr=%zux%d "
                        "frameBytes=%zu (no CPU YUV->RGB)",
                        width,
                        height,
                        colorspace,
                        Y_STRIDE,
                        EXPECTED_HEIGHT,
                        C_STRIDE,
                        EXPECTED_HEIGHT,
                        C_STRIDE,
                        EXPECTED_HEIGHT,
                        YUV422_FRAME_BYTES
                );
                gOutputInfoLogged = true;
            }

            if (gStats.decoded <= 5) {
                LOGI(
                        "UVC MJPEG TurboJPEG DECODE #%llu: jpeg=%zu queueMs=%.3f "
                        "headerMs=%.3f decodeCallMs=%.3f totalMs=%.3f "
                        "B0toB2Ms=%.3f slot=%d",
                        static_cast<unsigned long long>(gStats.decoded),
                        job.jpeg.size(),
                        queueMs,
                        headerMs,
                        decodeCallMs,
                        totalDecodeMs,
                        hostB0toB2Ms,
                        writeSlot
                );
            }
        }
        else {
            ++gStats.failed;

            if (gStats.failed <= 10) {
                LOGE(
                        "UVC MJPEG TurboJPEG decode failed: seq=%llu "
                        "header=%d decode=%d bytes=%zu slot=%d error=%s",
                        static_cast<unsigned long long>(job.sequence),
                        headerResult,
                        decodeResult,
                        job.jpeg.size(),
                        writeSlot,
                        gDecoder != nullptr ? tjGetErrorStr2(gDecoder) : "decoder=null"
                );
            }
        }

        maybeLogStatsLocked(decodeDoneNs);
    }

    LOGI("UVC MJPEG TurboJPEG worker stopped");
}


static void enqueueFrame(
        const std::vector<unsigned char>& jpeg,
        uint64_t firstPayloadNs,
        uint64_t eofNs)
{
    std::lock_guard<std::mutex> lock(gMutex);

    if (!gRunning.load(std::memory_order_acquire)) {
        return;
    }

    if (gPendingValid) {
        ++gStats.queueDrops;
    }

    gPending.jpeg.assign(jpeg.begin(), jpeg.end());
    gPending.firstPayloadNs = firstPayloadNs;
    gPending.eofNs = eofNs;
    gPending.sequence = ++gSubmittedSequence;
    gPendingValid = true;

    gCv.notify_one();
}

}  // namespace


bool start(uint32_t maxFrameBytes)
{
    stop();

    if (maxFrameBytes < 4) {
        LOGE("UVC MJPEG TurboJPEG: invalid maxFrame=%u", maxFrameBytes);
        return false;
    }

    gDecoder = tjInitDecompress();

    if (gDecoder == nullptr) {
        LOGE("UVC MJPEG TurboJPEG: tjInitDecompress failed");
        return false;
    }

    gMaxFrameBytes = maxFrameBytes;
    resetFrame();
    resetDecodedSlots();

    gFrame.jpeg.reserve(
            std::min<uint32_t>(
                    maxFrameBytes,
                    512u * 1024u
            )
    );

    for (DecodedFrameSlot& slot : gDecodedSlots) {
        slot.yuv.reserve(YUV422_FRAME_BYTES);
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);

        gRunning.store(true, std::memory_order_release);
        gPendingValid = false;
        gPending = {};
        gSubmittedSequence = 0;
        gStats = {};
        gStats.lastLogNs = nowNs();
        gStats.queueMs.reserve(128);
        gStats.headerMs.reserve(128);
        gStats.decodeCallMs.reserve(128);
        gStats.totalDecodeMs.reserve(128);
        gStats.hostB0toB2Ms.reserve(128);
        gOutputInfoLogged = false;
        gColorBarDiag = {};
        gRangeDiag = {};
    }

    gWorker = std::thread(workerLoop);

    LOGI(
            "UVC MJPEG TurboJPEG decoder START: maxFrame=%u "
            "output=YCbCr422 planar 1280x720 bytes=%zu",
            maxFrameBytes,
            YUV422_FRAME_BYTES
    );

    return true;
}


void processPayload(
        const unsigned char* data,
        int length,
        uint64_t callbackNs)
{
    if (data == nullptr ||
        length < 2 ||
        !gRunning.load(std::memory_order_acquire)) {
        return;
    }

    const uint8_t headerLength = data[0];
    const uint8_t flags = data[1];

    if (headerLength < 2 ||
        static_cast<int>(headerLength) > length) {

        if (gFrame.active) {
            gFrame.bad = true;
        }
        return;
    }

    const uint8_t fid = (flags & UVC_STREAM_FID) ? 1 : 0;
    const bool eof = (flags & UVC_STREAM_EOF) != 0;
    const bool payloadError = (flags & UVC_STREAM_ERR) != 0;

    const unsigned char* payload = data + headerLength;
    const size_t payloadBytes =
            static_cast<size_t>(length - static_cast<int>(headerLength));

    if (payloadError) {
        if (gFrame.active) {
            gFrame.bad = true;
        }
        return;
    }

    if (gFrame.active && fid != gFrame.fid) {
        resetFrame();
    }

    size_t payloadOffset = 0;

    if (!gFrame.active && payloadBytes > 0) {
        bool foundSoi = false;

        for (size_t i = 0; i + 1 < payloadBytes; ++i) {
            if (payload[i] == 0xff && payload[i + 1] == 0xd8) {
                payloadOffset = i;
                foundSoi = true;
                break;
            }
        }

        if (foundSoi) {
            resetFrame();
            gFrame.active = true;
            gFrame.fid = fid;
            gFrame.firstPayloadNs = callbackNs;
        }
    }

    if (!gFrame.active) {
        return;
    }

    const size_t bytesBeforePayload = gFrame.jpeg.size();

    for (size_t i = payloadOffset; i < payloadBytes; ++i) {
        const unsigned char value = payload[i];

        if (gFrame.lastByteValid &&
            gFrame.lastByte == 0xff &&
            value == 0xd9) {

            gFrame.sawEoi = true;
            gFrame.eoiBytes =
                    bytesBeforePayload +
                    (i - payloadOffset) + 1;
        }

        gFrame.lastByte = value;
        gFrame.lastByteValid = true;
    }

    gFrame.jpeg.insert(
            gFrame.jpeg.end(),
            payload + payloadOffset,
            payload + payloadBytes
    );

    if (gFrame.jpeg.size() > static_cast<size_t>(gMaxFrameBytes)) {
        resetFrame();
        return;
    }

    if (!eof) {
        return;
    }

    const bool frameOk =
            !gFrame.bad &&
            gFrame.sawEoi &&
            gFrame.eoiBytes >= 4 &&
            gFrame.eoiBytes <= gFrame.jpeg.size();

    if (frameOk) {
        gFrame.jpeg.resize(gFrame.eoiBytes);

        enqueueFrame(
                gFrame.jpeg,
                gFrame.firstPayloadNs,
                callbackNs
        );
    }

    resetFrame();
}


bool waitForDecodedFrame(
        uint64_t afterSequence,
        uint64_t& outReadySequence,
        int timeoutMs)
{
    outReadySequence = afterSequence;

    if (timeoutMs < 0) {
        timeoutMs = 0;
    }

    std::unique_lock<std::mutex> lock(gMutex);

    const auto predicate =
            [afterSequence]() {
                return
                        !gRunning.load(std::memory_order_acquire) ||
                        gLatestReadySequence.load(std::memory_order_acquire) >
                            afterSequence;
            };

    if (!predicate()) {
        const bool signaled =
                gFrameReadyCv.wait_for(
                        lock,
                        std::chrono::milliseconds(timeoutMs),
                        predicate
                );

        if (!signaled) {
            return false;
        }
    }

    const uint64_t readySequence =
            gLatestReadySequence.load(std::memory_order_acquire);

    if (readySequence <= afterSequence) {
        return false;
    }

    outReadySequence = readySequence;
    return true;
}


bool copyLatestFrame(
        uint8_t* dst,
        size_t capacity,
        uint64_t& inOutSequence,
        DecodedFrameTiming& outTiming)
{
    outTiming = {};

    if (dst == nullptr ||
        capacity < YUV422_FRAME_BYTES ||
        !gRunning.load(std::memory_order_acquire)) {
        return false;
    }

    for (int attempt = 0;
         attempt < DECODED_FRAME_SLOT_COUNT;
         ++attempt) {

        int candidate = -1;
        uint64_t newestSequence = inOutSequence;

        for (int i = 0; i < DECODED_FRAME_SLOT_COUNT; ++i) {
            if (gDecodedSlots[i].state.load(std::memory_order_acquire) != SLOT_READY) {
                continue;
            }

            const uint64_t sequence =
                    gDecodedSlots[i].sequence.load(std::memory_order_acquire);

            if (sequence > newestSequence) {
                newestSequence = sequence;
                candidate = i;
            }
        }

        if (candidate < 0) {
            return false;
        }

        int expected = SLOT_READY;
        if (!gDecodedSlots[candidate].state.compare_exchange_strong(
                expected,
                SLOT_READING,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        DecodedFrameSlot& slot = gDecodedSlots[candidate];
        const uint64_t sequence =
                slot.sequence.load(std::memory_order_acquire);

        if (sequence <= inOutSequence ||
            slot.yuv.size() != YUV422_FRAME_BYTES) {

            slot.state.store(SLOT_FREE, std::memory_order_release);
            return false;
        }

        std::memcpy(dst, slot.yuv.data(), YUV422_FRAME_BYTES);

        const uint64_t copyDoneRawNs = nowMonotonicRawNs();
        const uint64_t copyDoneMonotonicNs = nowMonotonicNs();

        inOutSequence = sequence;

        outTiming.sequence = sequence;
        outTiming.decodeDoneRawNs = slot.decodeDoneRawNs;
        outTiming.copyDoneRawNs = copyDoneRawNs;
        outTiming.decodeDoneMonotonicNs = slot.decodeDoneMonotonicNs;
        outTiming.copyDoneMonotonicNs = copyDoneMonotonicNs;
        outTiming.b0ToB2Ms = slot.b0ToB2Ms;
        outTiming.width = EXPECTED_WIDTH;
        outTiming.height = EXPECTED_HEIGHT;
        outTiming.yStride = Y_STRIDE;
        outTiming.cbStride = C_STRIDE;
        outTiming.crStride = C_STRIDE;
        outTiming.yBytes = Y_BYTES;
        outTiming.cbBytes = C_BYTES;
        outTiming.crBytes = C_BYTES;
        outTiming.frameBytes = YUV422_FRAME_BYTES;

        slot.state.store(SLOT_FREE, std::memory_order_release);

        for (int i = 0; i < DECODED_FRAME_SLOT_COUNT; ++i) {
            if (i == candidate ||
                gDecodedSlots[i].state.load(std::memory_order_acquire) != SLOT_READY) {
                continue;
            }

            const uint64_t otherSequence =
                    gDecodedSlots[i].sequence.load(std::memory_order_acquire);

            if (otherSequence >= sequence) {
                continue;
            }

            int ready = SLOT_READY;
            gDecodedSlots[i].state.compare_exchange_strong(
                    ready,
                    SLOT_FREE,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
            );
        }

        return true;
    }

    return false;
}


bool isRunning()
{
    return gRunning.load(std::memory_order_acquire);
}


void stop()
{
    {
        std::lock_guard<std::mutex> lock(gMutex);

        if (!gRunning.load(std::memory_order_acquire) &&
            !gWorker.joinable()) {

            if (gDecoder != nullptr) {
                tjDestroy(gDecoder);
                gDecoder = nullptr;
            }

            resetFrame();
            gMaxFrameBytes = 0;
            return;
        }

        gRunning.store(false, std::memory_order_release);
        gPendingValid = false;
    }

    gCv.notify_all();
    gFrameReadyCv.notify_all();

    if (gWorker.joinable()) {
        gWorker.join();
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gPending = {};
        gStats = {};
    }

    resetFrame();
    resetDecodedSlots();
    gMaxFrameBytes = 0;
    gOutputInfoLogged = false;

    if (gDecoder != nullptr) {
        tjDestroy(gDecoder);
        gDecoder = nullptr;
    }
}

}  // namespace uvc_mjpeg_decoder
