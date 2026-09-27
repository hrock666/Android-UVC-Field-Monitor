#pragma once

#include <cstddef>
#include <cstdint>

#include <libusb.h>

namespace uvc_stream {

struct Config {
    libusb_context* context = nullptr;
    libusb_device_handle* handle = nullptr;

    uint8_t endpoint = 0;
    int isoPacketBytes = 0;

    // Upper bound from negotiated UVC dwMaxVideoFrameSize.
    size_t maxCompressedFrameBytes = 0;
};

// Start the asynchronous UVC isochronous MJPEG transport ring.
// Each valid ISO packet is forwarded, including its UVC payload header,
// to uvc_mjpeg_decoder. This layer does not assemble or publish video frames.
bool start(const Config& config);

// True while the asynchronous USB ISO transport is active.
bool isRunning();

// Cancel all outstanding transfers, drain callbacks, stop the shared MJPEG
// decoder owned by this ISO stream, and release transport state.
void stop();

}  // namespace uvc_stream
