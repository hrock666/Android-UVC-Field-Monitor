#pragma once

#include <cstddef>
#include <cstdint>

namespace uvc_mjpeg_decoder {

struct DecodedFrameTiming {
    uint64_t sequence = 0;

    // CLOCK_MONOTONIC_RAW timestamps used for in-process stage timing.
    uint64_t decodeDoneRawNs = 0;
    uint64_t copyDoneRawNs = 0;

    // CLOCK_MONOTONIC timestamps used with EGL_ANDROID_get_frame_timestamps.
    uint64_t decodeDoneMonotonicNs = 0;
    uint64_t copyDoneMonotonicNs = 0;

    // B0 is the callback containing JPEG SOI. B2 is planar decode completion.
    double b0ToB2Ms = 0.0;

    int width = 0;
    int height = 0;

    size_t yStride = 0;
    size_t cbStride = 0;
    size_t crStride = 0;

    size_t yBytes = 0;
    size_t cbBytes = 0;
    size_t crBytes = 0;
    size_t frameBytes = 0;
};

// Start the UVC MJPEG -> planar YCbCr 4:2:2 decoder/latest-frame publisher.
// maxFrameBytes is negotiated UVC dwMaxVideoFrameSize.
bool start(uint32_t maxFrameBytes);

// Feed one complete UVC payload packet. callbackNs must use the same
// steady/monotonic clock domain for every call.
void processPayload(
        const unsigned char* data,
        int length,
        uint64_t callbackNs);

// Wake-up only. copyLatestFrame() still chooses the newest READY frame.
bool waitForDecodedFrame(
        uint64_t afterSequence,
        uint64_t& outReadySequence,
        int timeoutMs);

// Copy the newest planar frame into dst as one contiguous buffer:
//   [Y 1280x720][Cb 640x720][Cr 640x720]
// No RGB conversion is performed on the CPU.
bool copyLatestFrame(
        uint8_t* dst,
        size_t capacity,
        uint64_t& inOutSequence,
        DecodedFrameTiming& outTiming);

bool isRunning();

void stop();

}  // namespace uvc_mjpeg_decoder
