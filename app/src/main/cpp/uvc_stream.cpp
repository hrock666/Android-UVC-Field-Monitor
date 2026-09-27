#include "uvc_stream.h"
#include "uvc_mjpeg_decoder.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sys/time.h>
#include <thread>
#include <vector>

#define LOG_TAG "UvcFieldMonitor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace uvc_stream {
namespace {

// High-speed USB:
//   one microframe = 125 us
//
// 8 ISO packets / transfer = ~1 ms callback granularity.
// 12 transfers queued       = ~12 ms host-controller work queued.
//
// This module owns transport only. JPEG frame assembly, latest-pending policy,
// TurboJPEG decode and decoded latest-frame publication live in
// uvc_mjpeg_decoder.
static constexpr int ISO_PACKETS_PER_TRANSFER = 8;
static constexpr int ISO_TRANSFER_COUNT = 12;
static constexpr int EVENT_TIMEOUT_US = 50'000;

static constexpr uint8_t UVC_STREAM_EOF = 0x02;
static constexpr uint8_t UVC_STREAM_ERR = 0x40;

struct TransferSlot {
    libusb_transfer* transfer = nullptr;
    std::vector<unsigned char> buffer;
};

struct StreamStats {
    uint64_t transfersCompleted = 0;
    uint64_t transfersFailed = 0;

    uint64_t isoPackets = 0;
    uint64_t isoPacketErrors = 0;

    uint64_t usbBytes = 0;
    uint64_t videoBytes = 0;

    uint64_t malformedHeaders = 0;
    uint64_t uvcErrorPayloads = 0;
    uint64_t eofBoundaries = 0;

    std::chrono::steady_clock::time_point lastLog =
            std::chrono::steady_clock::now();

    uint64_t lastUsbBytes = 0;
    uint64_t lastVideoBytes = 0;
};

static std::mutex gStateMutex;
static std::atomic<bool> gRunning{false};
static std::atomic<int> gInflightTransfers{0};

static libusb_context* gContext = nullptr;
static libusb_device_handle* gHandle = nullptr;
static uint8_t gEndpoint = 0;
static int gIsoPacketBytes = 0;
static size_t gMaxCompressedFrameBytes = 0;

static std::thread gEventThread;
static std::vector<TransferSlot> gTransfers;
static StreamStats gStats;

// Ownership is explicit so an unrelated transport can never stop the decoder.
static bool gOwnsSharedMjpegDecoder = false;

static uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
            ).count()
    );
}

static void resetStats()
{
    gStats = {};
    gStats.lastLog = std::chrono::steady_clock::now();
}

static void maybeLogStats()
{
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
            std::chrono::duration<double>(now - gStats.lastLog).count();

    if (seconds < 1.0) {
        return;
    }

    const uint64_t usbByteDelta =
            gStats.usbBytes - gStats.lastUsbBytes;
    const uint64_t videoByteDelta =
            gStats.videoBytes - gStats.lastVideoBytes;

    const double usbMiBps =
            static_cast<double>(usbByteDelta) /
            (1024.0 * 1024.0) /
            seconds;

    const double videoMiBps =
            static_cast<double>(videoByteDelta) /
            (1024.0 * 1024.0) /
            seconds;

    LOGI(
            "Step 12 ISO stats: input=MJPEG usb=%.2f MiB/s "
            "video=%.2f MiB/s uvcErr=%llu malformed=%llu "
            "isoErr=%llu EOF=%llu inflight=%d",
            usbMiBps,
            videoMiBps,
            static_cast<unsigned long long>(gStats.uvcErrorPayloads),
            static_cast<unsigned long long>(gStats.malformedHeaders),
            static_cast<unsigned long long>(gStats.isoPacketErrors),
            static_cast<unsigned long long>(gStats.eofBoundaries),
            gInflightTransfers.load(std::memory_order_acquire)
    );

    gStats.lastLog = now;
    gStats.lastUsbBytes = gStats.usbBytes;
    gStats.lastVideoBytes = gStats.videoBytes;
}

static void processUvcPayload(
        const unsigned char* data,
        int length,
        uint64_t callbackNs)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    ++gStats.isoPackets;
    gStats.usbBytes += static_cast<uint64_t>(length);

    if (length < 2) {
        ++gStats.malformedHeaders;
        return;
    }

    const uint8_t headerLength = data[0];
    const uint8_t flags = data[1];

    if (headerLength < 2 ||
        static_cast<int>(headerLength) > length) {

        ++gStats.malformedHeaders;
        return;
    }

    const int payloadBytes =
            length - static_cast<int>(headerLength);

    if (payloadBytes > 0) {
        gStats.videoBytes += static_cast<uint64_t>(payloadBytes);
    }

    if ((flags & UVC_STREAM_ERR) != 0) {
        ++gStats.uvcErrorPayloads;
    }

    if ((flags & UVC_STREAM_EOF) != 0) {
        ++gStats.eofBoundaries;
    }

    // Pass the complete UVC payload packet, including its header. The shared
    // decoder owns FID/EOF/ERR handling and JPEG SOI/EOI assembly.
    uvc_mjpeg_decoder::processPayload(
            data,
            length,
            callbackNs
    );
}

static void LIBUSB_CALL onIsoTransfer(
        libusb_transfer* transfer)
{
    if (transfer == nullptr) {
        return;
    }

    const libusb_transfer_status status = transfer->status;

    if (status == LIBUSB_TRANSFER_COMPLETED) {
        const uint64_t callbackNs = steadyNowNs();
        ++gStats.transfersCompleted;

        for (int i = 0; i < transfer->num_iso_packets; ++i) {
            const libusb_iso_packet_descriptor& packet =
                    transfer->iso_packet_desc[i];

            if (packet.status != LIBUSB_TRANSFER_COMPLETED) {
                ++gStats.isoPacketErrors;
                continue;
            }

            if (packet.actual_length == 0) {
                continue;
            }

            unsigned char* packetData =
                    libusb_get_iso_packet_buffer_simple(
                            transfer,
                            static_cast<unsigned int>(i)
                    );

            processUvcPayload(
                    packetData,
                    static_cast<int>(packet.actual_length),
                    callbackNs
            );
        }

        maybeLogStats();
    }
    else if (status != LIBUSB_TRANSFER_CANCELLED) {
        ++gStats.transfersFailed;

        LOGE(
                "Step 12: ISO transfer status=%d",
                static_cast<int>(status)
        );

        if (status == LIBUSB_TRANSFER_NO_DEVICE) {
            gRunning.store(false, std::memory_order_release);
        }
    }

    if (gRunning.load(std::memory_order_acquire) &&
        status == LIBUSB_TRANSFER_COMPLETED) {

        const int r = libusb_submit_transfer(transfer);

        if (r == LIBUSB_SUCCESS) {
            return;
        }

        LOGE(
                "Step 12: ISO resubmit failed: %d (%s)",
                r,
                libusb_error_name(r)
        );
    }

    gInflightTransfers.fetch_sub(1, std::memory_order_acq_rel);
}

static void eventLoop()
{
    LOGI("Step 12: libusb event thread started");

    while (gRunning.load(std::memory_order_acquire) ||
           gInflightTransfers.load(std::memory_order_acquire) > 0) {

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = EVENT_TIMEOUT_US;

        const int r =
                libusb_handle_events_timeout(
                        gContext,
                        &timeout
                );

        if (r == LIBUSB_SUCCESS ||
            r == LIBUSB_ERROR_INTERRUPTED) {
            continue;
        }

        LOGE(
                "Step 12: libusb_handle_events_timeout failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        gRunning.store(false, std::memory_order_release);

        if (gInflightTransfers.load(std::memory_order_acquire) <= 0) {
            break;
        }
    }

    LOGI("Step 12: libusb event thread stopped");
}

static void cancelTransfers()
{
    for (TransferSlot& slot : gTransfers) {
        if (slot.transfer == nullptr) {
            continue;
        }

        const int r = libusb_cancel_transfer(slot.transfer);

        if (r != LIBUSB_SUCCESS &&
            r != LIBUSB_ERROR_NOT_FOUND) {

            LOGE(
                    "Step 12: libusb_cancel_transfer failed: %d (%s)",
                    r,
                    libusb_error_name(r)
            );
        }
    }
}

static void freeTransfers()
{
    for (TransferSlot& slot : gTransfers) {
        if (slot.transfer != nullptr) {
            libusb_free_transfer(slot.transfer);
            slot.transfer = nullptr;
        }

        slot.buffer.clear();
    }

    gTransfers.clear();
}

static void stopLocked()
{
    if (!gRunning.load(std::memory_order_acquire) &&
        !gEventThread.joinable() &&
        !gOwnsSharedMjpegDecoder &&
        gTransfers.empty()) {
        return;
    }

    LOGI("Step 12: stopping MJPEG ISO stream");

    gRunning.store(false, std::memory_order_release);
    cancelTransfers();

    if (gEventThread.joinable()) {
        gEventThread.join();
    }

    // No more ISO callbacks can feed the shared decoder after the event thread
    // has drained. Stop it only when this ISO stream started it.
    if (gOwnsSharedMjpegDecoder) {
        uvc_mjpeg_decoder::stop();
        gOwnsSharedMjpegDecoder = false;
    }

    LOGI(
            "Step 12 ISO final: input=MJPEG usbBytes=%llu videoBytes=%llu "
            "transfers=%llu transferFail=%llu isoPackets=%llu isoErr=%llu "
            "uvcErr=%llu malformed=%llu EOF=%llu",
            static_cast<unsigned long long>(gStats.usbBytes),
            static_cast<unsigned long long>(gStats.videoBytes),
            static_cast<unsigned long long>(gStats.transfersCompleted),
            static_cast<unsigned long long>(gStats.transfersFailed),
            static_cast<unsigned long long>(gStats.isoPackets),
            static_cast<unsigned long long>(gStats.isoPacketErrors),
            static_cast<unsigned long long>(gStats.uvcErrorPayloads),
            static_cast<unsigned long long>(gStats.malformedHeaders),
            static_cast<unsigned long long>(gStats.eofBoundaries)
    );

    freeTransfers();
    gInflightTransfers.store(0, std::memory_order_release);

    gContext = nullptr;
    gHandle = nullptr;
    gEndpoint = 0;
    gIsoPacketBytes = 0;
    gMaxCompressedFrameBytes = 0;
    gOwnsSharedMjpegDecoder = false;
    gStats = {};
}

}  // namespace

bool start(const Config& config)
{
    std::lock_guard<std::mutex> lock(gStateMutex);

    stopLocked();

    if (config.context == nullptr ||
        config.handle == nullptr ||
        config.endpoint == 0 ||
        config.isoPacketBytes <= 0 ||
        config.maxCompressedFrameBytes < 4 ||
        config.maxCompressedFrameBytes >
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {

        LOGE("Step 12 MJPEG: invalid ISO stream config");
        return false;
    }

    gContext = config.context;
    gHandle = config.handle;
    gEndpoint = config.endpoint;
    gIsoPacketBytes = config.isoPacketBytes;
    gMaxCompressedFrameBytes = config.maxCompressedFrameBytes;

    resetStats();

    gTransfers.resize(ISO_TRANSFER_COUNT);

    const size_t transferBytes =
            static_cast<size_t>(ISO_PACKETS_PER_TRANSFER) *
            static_cast<size_t>(gIsoPacketBytes);

    for (TransferSlot& slot : gTransfers) {
        slot.transfer = libusb_alloc_transfer(ISO_PACKETS_PER_TRANSFER);

        if (slot.transfer == nullptr) {
            LOGE("Step 12: libusb_alloc_transfer failed");
            stopLocked();
            return false;
        }

        slot.buffer.resize(transferBytes);

        libusb_fill_iso_transfer(
                slot.transfer,
                gHandle,
                gEndpoint,
                slot.buffer.data(),
                static_cast<int>(slot.buffer.size()),
                ISO_PACKETS_PER_TRANSFER,
                onIsoTransfer,
                &slot,
                0
        );

        libusb_set_iso_packet_lengths(
                slot.transfer,
                static_cast<unsigned int>(gIsoPacketBytes)
        );
    }

    if (!uvc_mjpeg_decoder::start(
            static_cast<uint32_t>(gMaxCompressedFrameBytes))) {

        LOGE(
                "Step 12 MJPEG: shared planar decoder start failed "
                "maxCompressed=%zu",
                gMaxCompressedFrameBytes
        );

        stopLocked();
        return false;
    }

    gOwnsSharedMjpegDecoder = true;

    LOGI(
            "Step 12 MJPEG: ISO payloads routed exclusively to "
            "shared planar YCbCr422 decoder"
    );

    gRunning.store(true, std::memory_order_release);
    gEventThread = std::thread(eventLoop);

    int submitted = 0;

    for (TransferSlot& slot : gTransfers) {
        gInflightTransfers.fetch_add(1, std::memory_order_acq_rel);

        const int r = libusb_submit_transfer(slot.transfer);

        if (r != LIBUSB_SUCCESS) {
            gInflightTransfers.fetch_sub(1, std::memory_order_acq_rel);

            LOGE(
                    "Step 12: initial libusb_submit_transfer failed "
                    "at slot=%d: %d (%s)",
                    submitted,
                    r,
                    libusb_error_name(r)
            );

            gRunning.store(false, std::memory_order_release);
            stopLocked();
            return false;
        }

        ++submitted;
    }

    LOGI(
            "Step 12: ISO MJPEG ring started endpoint=0x%02X "
            "packet=%d B packets/transfer=%d transfers=%d "
            "transferBytes=%zu queuedBytes=%zu maxCompressed=%zu",
            gEndpoint,
            gIsoPacketBytes,
            ISO_PACKETS_PER_TRANSFER,
            ISO_TRANSFER_COUNT,
            transferBytes,
            transferBytes * static_cast<size_t>(ISO_TRANSFER_COUNT),
            gMaxCompressedFrameBytes
    );

    return true;
}

bool isRunning()
{
    return gRunning.load(std::memory_order_acquire);
}

void stop()
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    stopLocked();
}

}  // namespace uvc_stream
