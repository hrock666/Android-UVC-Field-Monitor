#include "uvc_stream.h"

#include <android/log.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <sys/time.h>
#include <time.h>
#include <thread>
#include <vector>


#define LOG_TAG "UvcFieldMonitor"

#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


namespace uvc_stream {
namespace {

// High-speed USB:
//   one microframe = 125 us
//
// 8 ISO packets / transfer = ~1 ms callback granularity.
// 12 transfers queued       = ~12 ms host-controller work queued.
//
// This is a USB transport ring only.
// Completed VIDEO frames are not queued in a FIFO.
static constexpr int ISO_PACKETS_PER_TRANSFER = 8;
static constexpr int ISO_TRANSFER_COUNT = 12;

static constexpr int EVENT_TIMEOUT_US = 50'000;


// Three frame slots are enough for:
//   1 x USB writer
//   1 x newest READY frame
//   1 x renderer READING frame
//
// Older READY frames are explicitly reclaimable.
static constexpr int FRAME_SLOT_COUNT = 3;


// UVC payload header bmHeaderInfo bits.
static constexpr uint8_t UVC_STREAM_FID = 0x01;
static constexpr uint8_t UVC_STREAM_EOF = 0x02;
static constexpr uint8_t UVC_STREAM_ERR = 0x40;


enum FrameSlotState : int {
    SLOT_FREE = 0,
    SLOT_WRITING = 1,
    SLOT_READY = 2,
    SLOT_READING = 3
};


struct TransferSlot {
    libusb_transfer* transfer = nullptr;
    std::vector<unsigned char> buffer;
};


struct FrameSlot {
    std::vector<unsigned char> buffer;

    std::atomic<int> state{
            SLOT_FREE
    };

    std::atomic<uint64_t> sequence{
            0
    };

    // Published together with SLOT_READY via release/acquire ordering.
    uint64_t frameReadyNs = 0;
    uint64_t frameReadyMonotonicNs = 0;
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

    uint64_t goodFrames = 0;
    uint64_t droppedFrames = 0;
    uint64_t overflowFrames = 0;

    // READY frame discarded because a newer frame must become writable.
    // This is intentional latest-frame behavior, not a USB error.
    uint64_t staleReadyDrops = 0;

    // No FREE/old READY slot was available for the USB writer.
    uint64_t noWriteSlot = 0;

    uint64_t fidBoundaries = 0;
    uint64_t eofBoundaries = 0;

    std::chrono::steady_clock::time_point lastLog =
            std::chrono::steady_clock::now();

    uint64_t lastGoodFrames = 0;
    uint64_t lastUsbBytes = 0;
    uint64_t lastVideoBytes = 0;
};


struct FrameAssembler {
    size_t expectedBytes = 0;
    size_t bytes = 0;

    bool active = false;
    bool bad = false;

    uint8_t fid = 0;
};


static std::mutex gStateMutex;

static std::atomic<bool> gRunning{
        false
};

static std::atomic<int> gInflightTransfers{
        0
};

static std::atomic<uint64_t> gPublishedSequence{
        0
};


// Renderer wake-up sequence.
//
// Unlike gPublishedSequence, this is updated only AFTER the slot has
// transitioned to SLOT_READY. That prevents a wake-up from observing
// a sequence before the corresponding frame is actually claimable.
static std::atomic<uint64_t> gLatestReadySequence{
        0
};

static std::mutex gFrameReadyMutex;
static std::condition_variable gFrameReadyCv;

static libusb_context* gContext = nullptr;
static libusb_device_handle* gHandle = nullptr;

static uint8_t gEndpoint = 0;
static int gIsoPacketBytes = 0;

static std::thread gEventThread;
static std::vector<TransferSlot> gTransfers;

static std::array<
        FrameSlot,
        FRAME_SLOT_COUNT
> gFrameSlots;

static int gWriteSlot = -1;

static FrameAssembler gFrame;
static StreamStats gStats;


static uint64_t nowMonotonicRawNs()
{
    timespec ts{};

    clock_gettime(
            CLOCK_MONOTONIC_RAW,
            &ts
    );

    return
            static_cast<uint64_t>(
                    ts.tv_sec
            ) *
            1'000'000'000ULL +
            static_cast<uint64_t>(
                    ts.tv_nsec
            );
}


static uint64_t nowMonotonicNs()
{
    timespec ts{};

    clock_gettime(
            CLOCK_MONOTONIC,
            &ts
    );

    return
            static_cast<uint64_t>(
                    ts.tv_sec
            ) *
            1'000'000'000ULL +
            static_cast<uint64_t>(
                    ts.tv_nsec
            );
}


static void resetStats()
{
    gStats = {};

    gStats.lastLog =
            std::chrono::steady_clock::now();
}


static void waitForFrameReaders()
{
    // copyLatestFrame() only holds SLOT_READING around one
    // full-frame memcpy (~691 KB for the current mode).
    // Stop/reset must not resize a slot while that copy is active.
    for (;;) {

        bool reading = false;

        for (FrameSlot& slot :
             gFrameSlots) {

            if (slot.state.load(
                    std::memory_order_acquire
                ) == SLOT_READING) {

                reading = true;
                break;
            }
        }

        if (!reading) {
            return;
        }

        std::this_thread::yield();
    }
}


static void resetFrameSlots(
        size_t expectedBytes)
{
    waitForFrameReaders();

    for (FrameSlot& slot :
         gFrameSlots) {

        slot.state.store(
                SLOT_FREE,
                std::memory_order_release
        );

        slot.sequence.store(
                0,
                std::memory_order_release
        );

        slot.frameReadyNs = 0;
        slot.frameReadyMonotonicNs = 0;

        if (expectedBytes != 0 &&
            slot.buffer.size() !=
                expectedBytes) {

            slot.buffer.resize(
                    expectedBytes
            );
        }
    }

    gPublishedSequence.store(
            0,
            std::memory_order_release
    );

    gLatestReadySequence.store(
            0,
            std::memory_order_release
    );

    gWriteSlot = -1;

    gFrame = {};
    gFrame.expectedBytes =
            expectedBytes;
}


static int claimFreeSlot()
{
    for (int i = 0;
         i < FRAME_SLOT_COUNT;
         ++i) {

        int expected =
                SLOT_FREE;

        if (gFrameSlots[i].state
                .compare_exchange_strong(
                        expected,
                        SLOT_WRITING,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                )) {

            return i;
        }
    }

    return -1;
}


static int reclaimOldestReadySlot(
        int excludeSlot)
{
    int candidate = -1;

    uint64_t oldestSequence =
            std::numeric_limits<uint64_t>::max();

    for (int i = 0;
         i < FRAME_SLOT_COUNT;
         ++i) {

        if (i == excludeSlot) {
            continue;
        }

        if (gFrameSlots[i].state.load(
                std::memory_order_acquire
            ) != SLOT_READY) {

            continue;
        }

        const uint64_t sequence =
                gFrameSlots[i].sequence.load(
                        std::memory_order_acquire
                );

        if (sequence <
            oldestSequence) {

            oldestSequence =
                    sequence;

            candidate = i;
        }
    }

    if (candidate < 0) {
        return -1;
    }

    int expected =
            SLOT_READY;

    if (!gFrameSlots[candidate].state
            .compare_exchange_strong(
                    expected,
                    SLOT_WRITING,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
            )) {

        return -1;
    }

    ++gStats.staleReadyDrops;

    return candidate;
}


static int acquireWriteSlot(
        int excludeSlot = -1)
{
    int slot =
            claimFreeSlot();

    if (slot >= 0) {
        return slot;
    }

    // No free slot: intentionally discard the oldest completed
    // frame that the renderer has not claimed.
    return
            reclaimOldestReadySlot(
                    excludeSlot
            );
}


static bool ensureWriteSlot()
{
    if (gWriteSlot >= 0 &&
        gFrameSlots[gWriteSlot].state.load(
                std::memory_order_acquire
            ) == SLOT_WRITING) {

        return true;
    }

    gWriteSlot =
            acquireWriteSlot();

    if (gWriteSlot < 0) {

        ++gStats.noWriteSlot;

        return false;
    }

    return true;
}


static void beginFrame(
        uint8_t fid)
{
    if (!ensureWriteSlot()) {

        gFrame.active = false;
        gFrame.bad = true;
        gFrame.bytes = 0;

        return;
    }

    gFrame.active = true;
    gFrame.bad = false;
    gFrame.bytes = 0;
    gFrame.fid = fid;
}


static void finishFrame(
        const char* boundary)
{
    if (!gFrame.active) {
        return;
    }

    const bool sizeOk =
            gFrame.bytes ==
            gFrame.expectedBytes;

    const bool frameOk =
            !gFrame.bad &&
            sizeOk &&
            gWriteSlot >= 0;

    if (frameOk) {

        const int completedSlot =
                gWriteSlot;

        FrameSlot& slot =
                gFrameSlots[
                        completedSlot
                ];

        // T0: complete UVC frame assembled.
        // Take the timestamp before publishing SLOT_READY.
        slot.frameReadyNs =
                nowMonotonicRawNs();

        slot.frameReadyMonotonicNs =
                nowMonotonicNs();

        const uint64_t sequence =
                gPublishedSequence.fetch_add(
                        1,
                        std::memory_order_acq_rel
                ) + 1;

        slot.sequence.store(
                sequence,
                std::memory_order_relaxed
        );

        // Release publishes both the packet writes and sequence.
        slot.state.store(
                SLOT_READY,
                std::memory_order_release
        );


        // Step 14:
        // wake the renderer only after the complete frame is claimable.
        gLatestReadySequence.store(
                sequence,
                std::memory_order_release
        );

        gFrameReadyCv.notify_one();


        ++gStats.goodFrames;

        if (gStats.goodFrames <= 5 ||
            (gStats.goodFrames % 300) == 0) {

            LOGI(
                    "Step 12: FRAME READY "
                    "#%llu seq=%llu "
                    "bytes=%zu fid=%u "
                    "boundary=%s slot=%d",
                    static_cast<unsigned long long>(
                            gStats.goodFrames
                    ),
                    static_cast<unsigned long long>(
                            sequence
                    ),
                    gFrame.bytes,
                    gFrame.fid,
                    boundary,
                    completedSlot
            );
        }

        // Immediately obtain the next writer buffer.
        // Never wait for the renderer.
        gWriteSlot =
                acquireWriteSlot(
                        completedSlot
                );

        if (gWriteSlot < 0) {

            ++gStats.noWriteSlot;
        }
    }
    else {

        ++gStats.droppedFrames;

        if (gStats.droppedFrames <= 10 ||
            (gStats.droppedFrames % 30) == 0) {

            LOGE(
                    "Step 12: FRAME DROP "
                    "#%llu bytes=%zu/%zu "
                    "fid=%u bad=%s "
                    "boundary=%s",
                    static_cast<unsigned long long>(
                            gStats.droppedFrames
                    ),
                    gFrame.bytes,
                    gFrame.expectedBytes,
                    gFrame.fid,
                    gFrame.bad ? "YES" : "NO",
                    boundary
            );
        }

        // A bad/incomplete frame was never published.
        // Reuse its WRITING slot immediately.
    }

    gFrame.active = false;
    gFrame.bad = false;
    gFrame.bytes = 0;
}


static void maybeLogStats()
{
    const auto now =
            std::chrono::steady_clock::now();

    const double seconds =
            std::chrono::duration<double>(
                    now -
                    gStats.lastLog
            ).count();

    if (seconds < 1.0) {
        return;
    }

    const uint64_t frameDelta =
            gStats.goodFrames -
            gStats.lastGoodFrames;

    const uint64_t usbByteDelta =
            gStats.usbBytes -
            gStats.lastUsbBytes;

    const uint64_t videoByteDelta =
            gStats.videoBytes -
            gStats.lastVideoBytes;

    const double fps =
            static_cast<double>(
                    frameDelta
            ) / seconds;

    const double usbMiBps =
            static_cast<double>(
                    usbByteDelta
            ) /
            (1024.0 * 1024.0) /
            seconds;

    const double videoMiBps =
            static_cast<double>(
                    videoByteDelta
            ) /
            (1024.0 * 1024.0) /
            seconds;

    LOGI(
            "Step 12 stats: "
            "fps=%.2f usb=%.2f MiB/s "
            "video=%.2f MiB/s "
            "good=%llu drop=%llu "
            "staleReady=%llu noWrite=%llu "
            "uvcErr=%llu malformed=%llu "
            "isoErr=%llu overflow=%llu "
            "inflight=%d",
            fps,
            usbMiBps,
            videoMiBps,
            static_cast<unsigned long long>(
                    gStats.goodFrames
            ),
            static_cast<unsigned long long>(
                    gStats.droppedFrames
            ),
            static_cast<unsigned long long>(
                    gStats.staleReadyDrops
            ),
            static_cast<unsigned long long>(
                    gStats.noWriteSlot
            ),
            static_cast<unsigned long long>(
                    gStats.uvcErrorPayloads
            ),
            static_cast<unsigned long long>(
                    gStats.malformedHeaders
            ),
            static_cast<unsigned long long>(
                    gStats.isoPacketErrors
            ),
            static_cast<unsigned long long>(
                    gStats.overflowFrames
            ),
            gInflightTransfers.load(
                    std::memory_order_acquire
            )
    );

    gStats.lastLog =
            now;

    gStats.lastGoodFrames =
            gStats.goodFrames;

    gStats.lastUsbBytes =
            gStats.usbBytes;

    gStats.lastVideoBytes =
            gStats.videoBytes;
}


static void processUvcPayload(
        const unsigned char* data,
        int length)
{
    if (data == nullptr ||
        length <= 0) {

        return;
    }

    ++gStats.isoPackets;

    gStats.usbBytes +=
            static_cast<uint64_t>(
                    length
            );

    if (length < 2) {

        ++gStats.malformedHeaders;

        return;
    }

    const uint8_t headerLength =
            data[0];

    const uint8_t flags =
            data[1];

    if (headerLength < 2 ||
        static_cast<int>(
                headerLength
        ) > length) {

        ++gStats.malformedHeaders;

        if (gFrame.active) {
            gFrame.bad = true;
        }

        return;
    }

    const uint8_t fid =
            (flags & UVC_STREAM_FID)
            ? 1
            : 0;

    const bool eof =
            (flags & UVC_STREAM_EOF) != 0;

    const bool payloadError =
            (flags & UVC_STREAM_ERR) != 0;

    const int payloadBytes =
            length -
            static_cast<int>(
                    headerLength
            );

    const unsigned char* payload =
            data +
            headerLength;

    if (gFrame.active &&
        fid != gFrame.fid) {

        ++gStats.fidBoundaries;

        finishFrame(
                "FID"
        );
    }

    if (!gFrame.active &&
        payloadBytes > 0) {

        beginFrame(
                fid
        );
    }

    if (payloadError) {

        ++gStats.uvcErrorPayloads;

        if (gFrame.active) {
            gFrame.bad = true;
        }
    }

    if (gFrame.active &&
        payloadBytes > 0 &&
        gWriteSlot >= 0) {

        const size_t payloadSize =
                static_cast<size_t>(
                        payloadBytes
                );

        if (gFrame.bytes +
                payloadSize >
            gFrame.expectedBytes) {

            ++gStats.overflowFrames;

            gFrame.bad = true;
        }
        else {

            FrameSlot& slot =
                    gFrameSlots[
                            gWriteSlot
                    ];

            std::memcpy(
                    slot.buffer.data() +
                    gFrame.bytes,
                    payload,
                    payloadSize
            );

            gFrame.bytes +=
                    payloadSize;

            gStats.videoBytes +=
                    payloadSize;
        }
    }

    if (eof) {

        ++gStats.eofBoundaries;

        finishFrame(
                "EOF"
        );
    }
}


static void LIBUSB_CALL onIsoTransfer(
        libusb_transfer* transfer)
{
    if (transfer == nullptr) {
        return;
    }

    const libusb_transfer_status status =
            transfer->status;

    if (status ==
        LIBUSB_TRANSFER_COMPLETED) {

        ++gStats.transfersCompleted;

        for (int i = 0;
             i < transfer->num_iso_packets;
             ++i) {

            const libusb_iso_packet_descriptor& packet =
                    transfer->iso_packet_desc[i];

            if (packet.status !=
                LIBUSB_TRANSFER_COMPLETED) {

                ++gStats.isoPacketErrors;

                continue;
            }

            if (packet.actual_length == 0) {
                continue;
            }

            unsigned char* packetData =
                    libusb_get_iso_packet_buffer_simple(
                            transfer,
                            static_cast<unsigned int>(
                                    i
                            )
                    );

            processUvcPayload(
                    packetData,
                    static_cast<int>(
                            packet.actual_length
                    )
            );
        }

        maybeLogStats();
    }
    else if (status !=
             LIBUSB_TRANSFER_CANCELLED) {

        ++gStats.transfersFailed;

        LOGE(
                "Step 12: ISO transfer "
                "status=%d",
                static_cast<int>(
                        status
                )
        );

        if (status ==
            LIBUSB_TRANSFER_NO_DEVICE) {

            gRunning.store(
                    false,
                    std::memory_order_release
            );
        }
    }

    if (gRunning.load(
            std::memory_order_acquire) &&
        status ==
            LIBUSB_TRANSFER_COMPLETED) {

        const int r =
                libusb_submit_transfer(
                        transfer
                );

        if (r == LIBUSB_SUCCESS) {

            return;
        }

        LOGE(
                "Step 12: ISO resubmit "
                "failed: %d (%s)",
                r,
                libusb_error_name(r)
        );
    }

    gInflightTransfers.fetch_sub(
            1,
            std::memory_order_acq_rel
    );
}


static void eventLoop()
{
    LOGI(
            "Step 12: libusb event "
            "thread started"
    );

    while (
        gRunning.load(
                std::memory_order_acquire
        ) ||
        gInflightTransfers.load(
                std::memory_order_acquire
        ) > 0) {

        timeval timeout{};

        timeout.tv_sec = 0;
        timeout.tv_usec =
                EVENT_TIMEOUT_US;

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
                "Step 12: "
                "libusb_handle_events_timeout "
                "failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        gRunning.store(
                false,
                std::memory_order_release
        );

        if (gInflightTransfers.load(
                std::memory_order_acquire
            ) <= 0) {

            break;
        }
    }

    LOGI(
            "Step 12: libusb event "
            "thread stopped"
    );
}


static void cancelTransfers()
{
    for (TransferSlot& slot :
         gTransfers) {

        if (slot.transfer == nullptr) {
            continue;
        }

        const int r =
                libusb_cancel_transfer(
                        slot.transfer
                );

        if (r != LIBUSB_SUCCESS &&
            r != LIBUSB_ERROR_NOT_FOUND) {

            LOGE(
                    "Step 12: "
                    "libusb_cancel_transfer "
                    "failed: %d (%s)",
                    r,
                    libusb_error_name(r)
            );
        }
    }
}


static void freeTransfers()
{
    for (TransferSlot& slot :
         gTransfers) {

        if (slot.transfer != nullptr) {

            libusb_free_transfer(
                    slot.transfer
            );

            slot.transfer = nullptr;
        }

        slot.buffer.clear();
    }

    gTransfers.clear();
}


static void stopLocked()
{
    if (!gRunning.load(
            std::memory_order_acquire) &&
        !gEventThread.joinable() &&
        gTransfers.empty()) {

        return;
    }

    LOGI(
            "Step 12: stopping ISO stream"
    );

    // Prevent new renderer claims before stream structures reset.
    gRunning.store(
            false,
            std::memory_order_release
    );

    // Wake an event-driven renderer immediately on stream shutdown.
    gFrameReadyCv.notify_all();

    cancelTransfers();

    if (gEventThread.joinable()) {

        gEventThread.join();
    }

    if (gFrame.active) {

        finishFrame(
                "STOP"
        );
    }

    waitForFrameReaders();

    LOGI(
            "Step 12 final: "
            "good=%llu drop=%llu "
            "staleReady=%llu noWrite=%llu "
            "usbBytes=%llu videoBytes=%llu "
            "transfers=%llu transferFail=%llu "
            "isoPackets=%llu isoErr=%llu "
            "uvcErr=%llu malformed=%llu "
            "overflow=%llu "
            "FID=%llu EOF=%llu",
            static_cast<unsigned long long>(
                    gStats.goodFrames
            ),
            static_cast<unsigned long long>(
                    gStats.droppedFrames
            ),
            static_cast<unsigned long long>(
                    gStats.staleReadyDrops
            ),
            static_cast<unsigned long long>(
                    gStats.noWriteSlot
            ),
            static_cast<unsigned long long>(
                    gStats.usbBytes
            ),
            static_cast<unsigned long long>(
                    gStats.videoBytes
            ),
            static_cast<unsigned long long>(
                    gStats.transfersCompleted
            ),
            static_cast<unsigned long long>(
                    gStats.transfersFailed
            ),
            static_cast<unsigned long long>(
                    gStats.isoPackets
            ),
            static_cast<unsigned long long>(
                    gStats.isoPacketErrors
            ),
            static_cast<unsigned long long>(
                    gStats.uvcErrorPayloads
            ),
            static_cast<unsigned long long>(
                    gStats.malformedHeaders
            ),
            static_cast<unsigned long long>(
                    gStats.overflowFrames
            ),
            static_cast<unsigned long long>(
                    gStats.fidBoundaries
            ),
            static_cast<unsigned long long>(
                    gStats.eofBoundaries
            )
    );

    freeTransfers();

    gInflightTransfers.store(
            0,
            std::memory_order_release
    );

    resetFrameSlots(
            0
    );

    gContext = nullptr;
    gHandle = nullptr;
    gEndpoint = 0;
    gIsoPacketBytes = 0;

    gStats = {};
}

}  // namespace


bool start(const Config& config)
{
    std::lock_guard<std::mutex> lock(
            gStateMutex
    );

    stopLocked();

    if (config.context == nullptr ||
        config.handle == nullptr ||
        config.endpoint == 0 ||
        config.isoPacketBytes <= 0 ||
        config.expectedFrameBytes == 0) {

        LOGE(
                "Step 12: invalid stream config"
        );

        return false;
    }

    gContext =
            config.context;

    gHandle =
            config.handle;

    gEndpoint =
            config.endpoint;

    gIsoPacketBytes =
            config.isoPacketBytes;

    resetFrameSlots(
            config.expectedFrameBytes
    );

    gWriteSlot =
            claimFreeSlot();

    if (gWriteSlot < 0) {

        LOGE(
                "Step 12: failed to acquire "
                "initial frame write slot"
        );

        stopLocked();

        return false;
    }

    resetStats();

    gTransfers.resize(
            ISO_TRANSFER_COUNT
    );

    const size_t transferBytes =
            static_cast<size_t>(
                    ISO_PACKETS_PER_TRANSFER
            ) *
            static_cast<size_t>(
                    gIsoPacketBytes
            );

    for (TransferSlot& slot :
         gTransfers) {

        slot.transfer =
                libusb_alloc_transfer(
                        ISO_PACKETS_PER_TRANSFER
                );

        if (slot.transfer == nullptr) {

            LOGE(
                    "Step 12: "
                    "libusb_alloc_transfer failed"
            );

            stopLocked();

            return false;
        }

        slot.buffer.resize(
                transferBytes
        );

        libusb_fill_iso_transfer(
                slot.transfer,
                gHandle,
                gEndpoint,
                slot.buffer.data(),
                static_cast<int>(
                        slot.buffer.size()
                ),
                ISO_PACKETS_PER_TRANSFER,
                onIsoTransfer,
                &slot,
                0
        );

        libusb_set_iso_packet_lengths(
                slot.transfer,
                static_cast<unsigned int>(
                        gIsoPacketBytes
                )
        );
    }

    gRunning.store(
            true,
            std::memory_order_release
    );

    gFrameReadyCv.notify_all();

    gEventThread =
            std::thread(
                    eventLoop
            );

    int submitted = 0;

    for (TransferSlot& slot :
         gTransfers) {

        gInflightTransfers.fetch_add(
                1,
                std::memory_order_acq_rel
        );

        const int r =
                libusb_submit_transfer(
                        slot.transfer
                );

        if (r != LIBUSB_SUCCESS) {

            gInflightTransfers.fetch_sub(
                    1,
                    std::memory_order_acq_rel
            );

            LOGE(
                    "Step 12: initial "
                    "libusb_submit_transfer "
                    "failed at slot=%d: "
                    "%d (%s)",
                    submitted,
                    r,
                    libusb_error_name(r)
            );

            gRunning.store(
                    false,
                    std::memory_order_release
            );

            cancelTransfers();

            if (gEventThread.joinable()) {
                gEventThread.join();
            }

            freeTransfers();

            gInflightTransfers.store(
                    0,
                    std::memory_order_release
            );

            resetFrameSlots(
                    0
            );

            gContext = nullptr;
            gHandle = nullptr;
            gEndpoint = 0;
            gIsoPacketBytes = 0;

            gStats = {};

            return false;
        }

        ++submitted;
    }

    LOGI(
            "Step 12: ISO ring started "
            "endpoint=0x%02X "
            "packet=%d B "
            "packets/transfer=%d "
            "transfers=%d "
            "transferBytes=%zu "
            "queuedBytes=%zu "
            "expectedFrame=%zu",
            gEndpoint,
            gIsoPacketBytes,
            ISO_PACKETS_PER_TRANSFER,
            ISO_TRANSFER_COUNT,
            transferBytes,
            transferBytes *
            static_cast<size_t>(
                    ISO_TRANSFER_COUNT
            ),
            config.expectedFrameBytes
    );

    LOGI(
            "Step 12: latest-frame "
            "triple buffer active"
    );

    return true;
}


bool copyLatestFrame(
        uint8_t* dst,
        size_t capacity,
        uint64_t& inOutSequence,
        FrameTiming& outTiming)
{
    outTiming = {};

    if (dst == nullptr ||
        capacity == 0 ||
        !gRunning.load(
                std::memory_order_acquire
        )) {

        return false;
    }

    // A few retries are enough if the USB thread changes one
    // candidate from READY -> WRITING while we are scanning.
    for (int attempt = 0;
         attempt < FRAME_SLOT_COUNT;
         ++attempt) {

        int candidate = -1;
        uint64_t newestSequence =
                inOutSequence;

        for (int i = 0;
             i < FRAME_SLOT_COUNT;
             ++i) {

            if (gFrameSlots[i].state.load(
                    std::memory_order_acquire
                ) != SLOT_READY) {

                continue;
            }

            const uint64_t sequence =
                    gFrameSlots[i].sequence.load(
                            std::memory_order_acquire
                    );

            if (sequence >
                newestSequence) {

                newestSequence =
                        sequence;

                candidate = i;
            }
        }

        if (candidate < 0) {

            return false;
        }

        int expected =
                SLOT_READY;

        if (!gFrameSlots[candidate].state
                .compare_exchange_strong(
                        expected,
                        SLOT_READING,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                )) {

            continue;
        }

        FrameSlot& slot =
                gFrameSlots[
                        candidate
                ];

        const uint64_t sequence =
                slot.sequence.load(
                        std::memory_order_acquire
                );

        if (sequence <=
                inOutSequence ||
            slot.buffer.size() >
                capacity) {

            slot.state.store(
                    SLOT_FREE,
                    std::memory_order_release
            );

            if (slot.buffer.size() >
                capacity) {

                LOGE(
                        "Step 12: renderer "
                        "destination too small "
                        "%zu < %zu",
                        capacity,
                        slot.buffer.size()
                );
            }

            return false;
        }

        // The only full-frame application-level copy:
        // completed UVC frame -> persistent mapped PBO.
        std::memcpy(
                dst,
                slot.buffer.data(),
                slot.buffer.size()
        );

        // T1: full frame is now in the persistent mapped PBO.
        const uint64_t copyDoneNs =
                nowMonotonicRawNs();

        const uint64_t copyDoneMonotonicNs =
                nowMonotonicNs();

        inOutSequence =
                sequence;

        outTiming.sequence =
                sequence;

        outTiming.frameReadyNs =
                slot.frameReadyNs;

        outTiming.copyDoneNs =
                copyDoneNs;

        outTiming.frameReadyMonotonicNs =
                slot.frameReadyMonotonicNs;

        outTiming.copyDoneMonotonicNs =
                copyDoneMonotonicNs;

        slot.state.store(
                SLOT_FREE,
                std::memory_order_release
        );

        // Drop any older READY frames. They are stale by definition
        // once this newest sequence has been consumed.
        for (int i = 0;
             i < FRAME_SLOT_COUNT;
             ++i) {

            if (i == candidate) {
                continue;
            }

            if (gFrameSlots[i].state.load(
                    std::memory_order_acquire
                ) != SLOT_READY) {

                continue;
            }

            const uint64_t otherSequence =
                    gFrameSlots[i].sequence.load(
                            std::memory_order_acquire
                    );

            if (otherSequence >=
                sequence) {

                continue;
            }

            int ready =
                    SLOT_READY;

            gFrameSlots[i].state
                    .compare_exchange_strong(
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


bool waitForNewFrame(
        uint64_t afterSequence,
        uint64_t& outReadySequence,
        int timeoutMs)
{
    outReadySequence =
            afterSequence;

    if (timeoutMs < 0) {
        timeoutMs = 0;
    }

    std::unique_lock<std::mutex> lock(
            gFrameReadyMutex
    );

    const auto predicate =
            [afterSequence]() {

                return
                        !gRunning.load(
                                std::memory_order_acquire
                        ) ||
                        gLatestReadySequence.load(
                                std::memory_order_acquire
                        ) >
                        afterSequence;
            };


    if (!predicate()) {

        const bool signaled =
                gFrameReadyCv.wait_for(
                        lock,
                        std::chrono::milliseconds(
                                timeoutMs
                        ),
                        predicate
                );

        if (!signaled) {
            return false;
        }
    }


    if (!gRunning.load(
            std::memory_order_acquire
        )) {

        return false;
    }


    const uint64_t readySequence =
            gLatestReadySequence.load(
                    std::memory_order_acquire
            );

    if (readySequence <=
        afterSequence) {

        return false;
    }


    outReadySequence =
            readySequence;

    return true;
}


bool isRunning()
{
    return
            gRunning.load(
                    std::memory_order_acquire
            );
}


void stop()
{
    std::lock_guard<std::mutex> lock(
            gStateMutex
    );

    stopLocked();
}

}  // namespace uvc_stream
