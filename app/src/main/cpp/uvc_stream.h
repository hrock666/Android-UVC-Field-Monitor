#pragma once

#include <cstddef>
#include <cstdint>

#include <libusb.h>


namespace uvc_stream {


struct FrameTiming {
    uint64_t sequence = 0;

    // CLOCK_MONOTONIC_RAW timestamps.
    //
    // frameReadyNs:
    //   complete UVC frame assembled at EOF/FID boundary and
    //   immediately before publishing the READY slot.
    //
    // copyDoneNs:
    //   newest frame copied into the renderer's mapped PBO.
    uint64_t frameReadyNs = 0;
    uint64_t copyDoneNs = 0;

    // Same events in CLOCK_MONOTONIC domain.
    // Used only for EGL actual-presentation latency.
    uint64_t frameReadyMonotonicNs = 0;
    uint64_t copyDoneMonotonicNs = 0;
};


struct Config {
    libusb_context* context = nullptr;
    libusb_device_handle* handle = nullptr;

    uint8_t endpoint = 0;

    int isoPacketBytes = 0;

    size_t expectedFrameBytes = 0;
};


// Start the asynchronous UVC isochronous receive ring.
bool start(const Config& config);


// Copy only the newest completed frame into dst.
//
// There is deliberately no FIFO.
// If the renderer misses frames, older READY frames are discarded.
//
// inOutSequence:
//   input  = last frame sequence consumed by the renderer
//   output = sequence copied into dst
//
// Returns true only when a newer complete frame was copied.
bool copyLatestFrame(
        uint8_t* dst,
        size_t capacity,
        uint64_t& inOutSequence,
        FrameTiming& outTiming);


// Wait until a frame newer than afterSequence has been published.
//
// This is only a renderer wake-up signal; it does not dequeue a frame.
// The renderer must still call copyLatestFrame(), which preserves
// newest-frame semantics.
//
// timeoutMs >= 0:
//   maximum sleep time. A timeout returns false.
//
// outReadySequence:
//   newest READY publication sequence observed by the wake-up path.
bool waitForNewFrame(
        uint64_t afterSequence,
        uint64_t& outReadySequence,
        int timeoutMs);


// True while the asynchronous USB stream is active.
bool isRunning();


// Cancel all outstanding transfers, drain callbacks,
// stop the libusb event thread, and release stream state.
void stop();

}  // namespace uvc_stream
