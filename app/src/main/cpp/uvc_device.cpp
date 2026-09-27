#include "uvc_device.h"
#include "uvc_stream.h"
#include "ms2109_pu_controls.h"
#include "uvc_mjpeg_decoder.h"

#include <android/log.h>
#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <vector>


#define LOG_TAG "UvcFieldMonitor"

#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


namespace uvc_device {
namespace {

struct SupportedUsbDevice {
    uint16_t vendorId;
    uint16_t productId;
    const char* name;
};

static constexpr SupportedUsbDevice SUPPORTED_USB_DEVICES[] = {
        {0x534D, 0x2109, "MS2109"},
        {0x345F, 0x2109, "MS2109"},
        {0x345F, 0x2130, "MS2130"},
};

static const SupportedUsbDevice* findSupportedUsbDevice(
        uint16_t vendorId,
        uint16_t productId)
{
    for (const auto& supported : SUPPORTED_USB_DEVICES) {
        if (vendorId == supported.vendorId &&
            productId == supported.productId) {
            return &supported;
        }
    }

    return nullptr;
}

static constexpr int UVC_VIDEO_STREAMING_INTERFACE = 1;
static constexpr uint8_t UVC_VIDEO_ENDPOINT = 0x83;

static constexpr int TARGET_UVC_WIDTH = 1280;
static constexpr int TARGET_UVC_HEIGHT = 720;
static constexpr uint32_t TARGET_UVC_INTERVAL_100NS = 166666;

static constexpr int TARGET_STREAM_ALT_SETTING = 3;
static constexpr uint32_t TARGET_STREAM_ALT_CAPACITY = 3072;

enum class UvcTransportType {
    Unsupported = 0,
    Isochronous,
    Bulk,
};

struct UvcTransportInfo {
    UvcTransportType type = UvcTransportType::Unsupported;
    uint8_t interfaceNumber = 0;
    uint8_t endpointAddress = 0;
    uint8_t altSetting = 0;
    uint32_t capacityBytes = 0;
};

static constexpr unsigned int CONTROL_TIMEOUT_MS = 1000;

// UVC request codes.
static constexpr uint8_t UVC_SET_CUR = 0x01;
static constexpr uint8_t UVC_GET_CUR = 0x81;

// VideoStreaming control selectors.
static constexpr uint8_t UVC_VS_PROBE_CONTROL = 0x01;
static constexpr uint8_t UVC_VS_COMMIT_CONTROL = 0x02;

// Class / Interface requests.
static constexpr uint8_t UVC_REQ_OUT =
        LIBUSB_ENDPOINT_OUT |
        LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE;

static constexpr uint8_t UVC_REQ_IN =
        LIBUSB_ENDPOINT_IN |
        LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE;


enum class UvcVideoFormat {
    Unknown = 0,
    Mjpeg,
};


static const char* uvcVideoFormatName(
        UvcVideoFormat format)
{
    switch (format) {
        case UvcVideoFormat::Mjpeg:
            return "MJPEG";

        case UvcVideoFormat::Unknown:
        default:
            return "UNKNOWN";
    }
}


struct UvcStreamMode {
    bool valid = false;

    UvcVideoFormat format =
            UvcVideoFormat::Unknown;

    uint8_t formatIndex = 0;
    uint8_t frameIndex = 0;

    uint16_t width = 0;
    uint16_t height = 0;

    uint32_t frameInterval100ns = 0;
    uint32_t maxVideoFrameBufferSize = 0;

    uint8_t bitsPerPixel = 0;
};


struct UvcProbeResult {
    bool valid = false;

    uint16_t controlLength = 0;

    uint8_t formatIndex = 0;
    uint8_t frameIndex = 0;

    uint32_t frameInterval100ns = 0;
    uint32_t maxVideoFrameSize = 0;
    uint32_t maxPayloadTransferSize = 0;
};


static std::mutex gUsbMutex;

static libusb_context* gUsbContext = nullptr;
static libusb_device_handle* gUsbHandle = nullptr;

static bool gVideoInterfaceClaimed = false;
static int gCurrentAltSetting = 0;

static UvcStreamMode gSelectedUvcMode;
static UvcProbeResult gProbeResult;


static uint16_t readLe16(const unsigned char* p)
{
    return
            static_cast<uint16_t>(p[0]) |
            (static_cast<uint16_t>(p[1]) << 8);
}


static uint32_t readLe32(const unsigned char* p)
{
    return
            static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
}


static void writeLe16(
        unsigned char* p,
        uint16_t value)
{
    p[0] =
            static_cast<unsigned char>(
                    value & 0xff
            );

    p[1] =
            static_cast<unsigned char>(
                    (value >> 8) & 0xff
            );
}


static void writeLe32(
        unsigned char* p,
        uint32_t value)
{
    p[0] =
            static_cast<unsigned char>(
                    value & 0xff
            );

    p[1] =
            static_cast<unsigned char>(
                    (value >> 8) & 0xff
            );

    p[2] =
            static_cast<unsigned char>(
                    (value >> 16) & 0xff
            );

    p[3] =
            static_cast<unsigned char>(
                    (value >> 24) & 0xff
            );
}


static double interval100nsToFps(
        uint32_t interval100ns)
{
    if (interval100ns == 0) {
        return 0.0;
    }

    return
            10000000.0 /
            static_cast<double>(interval100ns);
}


static const char* usbTransferTypeName(
        uint8_t attributes)
{
    switch (
        attributes &
        LIBUSB_TRANSFER_TYPE_MASK) {

        case LIBUSB_TRANSFER_TYPE_CONTROL:
            return "CONTROL";

        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
            return "ISO";

        case LIBUSB_TRANSFER_TYPE_BULK:
            return "BULK";

        case LIBUSB_TRANSFER_TYPE_INTERRUPT:
            return "INT";

        default:
            return "UNKNOWN";
    }
}


static int getIsoEffectiveBytesPerMicroframe(
        const libusb_endpoint_descriptor& ep)
{
    const uint16_t raw =
            ep.wMaxPacketSize;

    const int bytesPerTransaction =
            raw & 0x07ff;

    const int transactions =
            1 + ((raw >> 11) & 0x3);

    if ((ep.bmAttributes &
         LIBUSB_TRANSFER_TYPE_MASK) ==
        LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {

        return
                bytesPerTransaction *
                transactions;
    }

    return bytesPerTransaction;
}


static bool chooseDiscreteTargetInterval(
        const unsigned char* descriptor,
        size_t descriptorLength,
        uint8_t intervalCount,
        uint32_t& selectedInterval)
{
    static constexpr size_t INTERVAL_OFFSET = 26;

    if (descriptor == nullptr ||
        descriptorLength <
                INTERVAL_OFFSET +
                static_cast<size_t>(
                        intervalCount
                ) * 4) {

        return false;
    }

    uint32_t bestInterval = 0;
    uint32_t bestDiff =
            std::numeric_limits<uint32_t>::max();

    for (uint8_t i = 0;
         i < intervalCount;
         ++i) {

        const uint32_t interval =
                readLe32(
                        descriptor +
                        INTERVAL_OFFSET +
                        static_cast<size_t>(i) * 4
                );

        const double fps =
                interval100nsToFps(interval);

        LOGI(
                "      interval[%u]=%u (%.3f fps)",
                i,
                interval,
                fps
        );

        const uint32_t diff =
                interval >
                    TARGET_UVC_INTERVAL_100NS
                ? interval -
                    TARGET_UVC_INTERVAL_100NS
                : TARGET_UVC_INTERVAL_100NS -
                    interval;

        if (diff < bestDiff) {

            bestDiff = diff;
            bestInterval = interval;
        }
    }

    if (bestInterval != 0 &&
        bestDiff <= 1000) {

        selectedInterval =
                bestInterval;

        return true;
    }

    return false;
}


static bool chooseContinuousTargetInterval(
        const unsigned char* descriptor,
        size_t descriptorLength,
        uint32_t& selectedInterval)
{
    static constexpr size_t CONTINUOUS_LENGTH = 38;

    if (descriptor == nullptr ||
        descriptorLength <
                CONTINUOUS_LENGTH) {

        return false;
    }

    const uint32_t minInterval =
            readLe32(descriptor + 26);

    const uint32_t maxInterval =
            readLe32(descriptor + 30);

    const uint32_t step =
            readLe32(descriptor + 34);

    LOGI(
            "      continuous interval: "
            "min=%u (%.3f fps) "
            "max=%u (%.3f fps) step=%u",
            minInterval,
            interval100nsToFps(
                    minInterval
            ),
            maxInterval,
            interval100nsToFps(
                    maxInterval
            ),
            step
    );

    if (TARGET_UVC_INTERVAL_100NS <
            minInterval ||
        TARGET_UVC_INTERVAL_100NS >
            maxInterval) {

        return false;
    }

    uint32_t candidate =
            TARGET_UVC_INTERVAL_100NS;

    if (step != 0) {

        const uint32_t delta =
                TARGET_UVC_INTERVAL_100NS -
                minInterval;

        const uint32_t steps =
                (delta + (step / 2)) /
                step;

        candidate =
                minInterval +
                steps * step;

        if (candidate > maxInterval) {

            candidate =
                    maxInterval;
        }
    }

    const uint32_t diff =
            candidate >
                TARGET_UVC_INTERVAL_100NS
            ? candidate -
                TARGET_UVC_INTERVAL_100NS
            : TARGET_UVC_INTERVAL_100NS -
                candidate;

    if (diff > 1000) {

        return false;
    }

    selectedInterval =
            candidate;

    return true;
}


static bool detectUvcTransport(
        libusb_device* device,
        UvcTransportInfo& transport)
{
    transport = {};

    libusb_config_descriptor* config =
            nullptr;

    const int r =
            libusb_get_active_config_descriptor(
                    device,
                    &config
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "libusb_get_active_config_descriptor "
                "failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        return false;
    }

    LOGI(
            "Active Configuration: "
            "value=%u interfaces=%u totalLength=%u",
            config->bConfigurationValue,
            config->bNumInterfaces,
            config->wTotalLength
    );

    bool foundVsInterface = false;
    bool foundBulkEndpoint = false;
    bool foundIsoEndpoint = false;

    uint16_t bulkMaxPacket = 0;
    uint8_t bestIsoAlt = 0;
    uint32_t bestIsoCapacity = 0;

    for (uint8_t i = 0;
         i < config->bNumInterfaces;
         ++i) {

        const libusb_interface& iface =
                config->interface[i];

        LOGI(
                "Config interface slot[%u]: "
                "altsettings=%d",
                i,
                iface.num_altsetting
        );

        for (int a = 0;
             a < iface.num_altsetting;
             ++a) {

            const libusb_interface_descriptor& alt =
                    iface.altsetting[a];

            LOGI(
                    "  IF=%u ALT=%u "
                    "class=%u subClass=%u "
                    "protocol=%u endpoints=%u",
                    alt.bInterfaceNumber,
                    alt.bAlternateSetting,
                    alt.bInterfaceClass,
                    alt.bInterfaceSubClass,
                    alt.bInterfaceProtocol,
                    alt.bNumEndpoints
            );

            const bool isVideoStreaming =
                    alt.bInterfaceNumber ==
                        UVC_VIDEO_STREAMING_INTERFACE &&
                    alt.bInterfaceClass ==
                        LIBUSB_CLASS_VIDEO &&
                    alt.bInterfaceSubClass == 2;

            if (isVideoStreaming) {
                foundVsInterface = true;
            }

            for (uint8_t e = 0;
                 e < alt.bNumEndpoints;
                 ++e) {

                const libusb_endpoint_descriptor& ep =
                        alt.endpoint[e];

                const bool isIn =
                        (ep.bEndpointAddress &
                         LIBUSB_ENDPOINT_DIR_MASK) ==
                        LIBUSB_ENDPOINT_IN;

                const int effectiveBytes =
                        getIsoEffectiveBytesPerMicroframe(
                                ep
                        );

                LOGI(
                        "    EP=0x%02X dir=%s "
                        "type=%s rawMaxPacket=%u "
                        "effectiveBytes=%d interval=%u",
                        ep.bEndpointAddress,
                        isIn ? "IN" : "OUT",
                        usbTransferTypeName(
                                ep.bmAttributes
                        ),
                        ep.wMaxPacketSize,
                        effectiveBytes,
                        ep.bInterval
                );

                if (!isVideoStreaming ||
                    ep.bEndpointAddress !=
                        UVC_VIDEO_ENDPOINT ||
                    !isIn) {

                    continue;
                }

                const uint8_t transferType =
                        ep.bmAttributes &
                        LIBUSB_TRANSFER_TYPE_MASK;

                if (transferType ==
                        LIBUSB_TRANSFER_TYPE_BULK &&
                    alt.bAlternateSetting == 0) {

                    foundBulkEndpoint = true;
                    bulkMaxPacket = ep.wMaxPacketSize & 0x07ff;
                }
                else if (transferType ==
                             LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {

                    foundIsoEndpoint = true;

                    if (effectiveBytes > 0 &&
                        static_cast<uint32_t>(effectiveBytes) >
                            bestIsoCapacity) {

                        bestIsoAlt =
                                alt.bAlternateSetting;

                        bestIsoCapacity =
                                static_cast<uint32_t>(
                                        effectiveBytes
                                );
                    }
                }
            }
        }
    }

    libusb_free_config_descriptor(
            config
    );

    LOGI(
            "UVC transport discovery: "
            "VS_IF=%s EP_0x83_BULK_ALT0=%s "
            "EP_0x83_ISO=%s bestIsoAlt=%u bestIsoCapacity=%u",
            foundVsInterface ? "YES" : "NO",
            foundBulkEndpoint ? "YES" : "NO",
            foundIsoEndpoint ? "YES" : "NO",
            bestIsoAlt,
            bestIsoCapacity
    );

    if (!foundVsInterface) {
        LOGE("UVC transport discovery failed: VideoStreaming IF1 not found");
        return false;
    }

    // Transport is selected from the endpoint descriptor, not from VID/PID.
    // Prefer BULK when EP 0x83 exists on VS ALT0. Otherwise use the
    // highest-bandwidth ISO alternate setting advertised for EP 0x83.
    if (foundBulkEndpoint) {

        transport.type =
                UvcTransportType::Bulk;

        transport.interfaceNumber =
                UVC_VIDEO_STREAMING_INTERFACE;

        transport.endpointAddress =
                UVC_VIDEO_ENDPOINT;

        transport.altSetting = 0;
        transport.capacityBytes = bulkMaxPacket;

        LOGI(
                "UVC transport selected: BULK "
                "IF=%u ALT=%u EP=0x%02X maxPacket=%u",
                transport.interfaceNumber,
                transport.altSetting,
                transport.endpointAddress,
                transport.capacityBytes
        );

        return true;
    }

    if (foundIsoEndpoint) {

        // The ISO backend is validated for the classic MS2109 layout:
        // ALT3, 3072 bytes/microframe. The required video format is MJPEG;
        // the endpoint scheduling/layout constraint remains unchanged.
        if (bestIsoAlt != TARGET_STREAM_ALT_SETTING ||
            bestIsoCapacity != TARGET_STREAM_ALT_CAPACITY) {

            LOGE(
                    "UVC ISO endpoint found but backend layout is unsupported: "
                    "bestAlt=%u capacity=%u (expected alt=%d capacity=%u)",
                    bestIsoAlt,
                    bestIsoCapacity,
                    TARGET_STREAM_ALT_SETTING,
                    TARGET_STREAM_ALT_CAPACITY
            );

            return false;
        }

        transport.type =
                UvcTransportType::Isochronous;

        transport.interfaceNumber =
                UVC_VIDEO_STREAMING_INTERFACE;

        transport.endpointAddress =
                UVC_VIDEO_ENDPOINT;

        transport.altSetting =
                bestIsoAlt;

        transport.capacityBytes =
                bestIsoCapacity;

        LOGI(
                "UVC transport selected: ISO "
                "IF=%u ALT=%u EP=0x%02X capacity=%u B/microframe",
                transport.interfaceNumber,
                transport.altSetting,
                transport.endpointAddress,
                transport.capacityBytes
        );

        return true;
    }

    LOGE(
            "UVC transport discovery failed: "
            "EP 0x%02X not found as BULK or ISO on VS IF%d",
            UVC_VIDEO_ENDPOINT,
            UVC_VIDEO_STREAMING_INTERFACE
    );

    return false;
}

static bool discoverTargetUvcMode(
        libusb_device* device,
        UvcStreamMode& selectedMode)
{
    selectedMode = {};

    if (device == nullptr) {
        LOGE("discoverTargetUvcMode: device is null");
        return false;
    }

    libusb_config_descriptor* config = nullptr;
    const int r =
            libusb_get_active_config_descriptor(
                    device,
                    &config
            );

    if (r != LIBUSB_SUCCESS) {
        LOGE(
                "Step 9: get active config failed: %d (%s)",
                r,
                libusb_error_name(r)
        );
        return false;
    }

    LOGI(
            "Step 9: scanning UVC VideoStreaming descriptors "
            "for required %dx%d MJPEG @ %.3f fps",
            TARGET_UVC_WIDTH,
            TARGET_UVC_HEIGHT,
            interval100nsToFps(TARGET_UVC_INTERVAL_100NS)
    );

    bool foundVsAlt0 = false;
    uint8_t currentMjpegFormatIndex = 0;
    UvcStreamMode mjpegCandidate{};

    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& iface = config->interface[i];

        for (int a = 0; a < iface.num_altsetting; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];

            if (alt.bInterfaceNumber != UVC_VIDEO_STREAMING_INTERFACE ||
                alt.bAlternateSetting != 0 ||
                alt.bInterfaceClass != LIBUSB_CLASS_VIDEO ||
                alt.bInterfaceSubClass != 2) {
                continue;
            }

            foundVsAlt0 = true;

            LOGI(
                    "Step 9: VS IF=%u ALT=%u extraLength=%d",
                    alt.bInterfaceNumber,
                    alt.bAlternateSetting,
                    alt.extra_length
            );

            const unsigned char* p = alt.extra;
            int remaining = alt.extra_length;

            while (p != nullptr && remaining >= 3) {
                const uint8_t length = p[0];
                const uint8_t descriptorType = p[1];
                const uint8_t descriptorSubtype = p[2];

                if (length < 3 || length > remaining) {
                    LOGE(
                            "Step 9: malformed VS descriptor "
                            "length=%u remaining=%d",
                            length,
                            remaining
                    );
                    libusb_free_config_descriptor(config);
                    return false;
                }

                if (descriptorType == 0x24) {
                    switch (descriptorSubtype) {
                        case 0x06:  // VS_FORMAT_MJPEG
                        {
                            if (length < 11) {
                                LOGE(
                                        "Step 9: short VS_FORMAT_MJPEG len=%u",
                                        length
                                );
                                libusb_free_config_descriptor(config);
                                return false;
                            }

                            currentMjpegFormatIndex = p[3];

                            LOGI(
                                    "  VS_FORMAT_MJPEG: formatIndex=%u "
                                    "frames=%u defaultFrameIndex=%u",
                                    currentMjpegFormatIndex,
                                    p[4],
                                    p[6]
                            );
                            break;
                        }

                        case 0x07:  // VS_FRAME_MJPEG
                        {
                            if (length < 26) {
                                LOGE(
                                        "Step 9: short VS_FRAME_MJPEG len=%u",
                                        length
                                );
                                libusb_free_config_descriptor(config);
                                return false;
                            }

                            if (currentMjpegFormatIndex == 0) {
                                break;
                            }

                            const uint8_t frameIndex = p[3];
                            const uint16_t width = readLe16(p + 5);
                            const uint16_t height = readLe16(p + 7);
                            const uint32_t maxFrameBufferSize = readLe32(p + 17);
                            const uint32_t defaultInterval = readLe32(p + 21);
                            const uint8_t intervalType = p[25];

                            LOGI(
                                    "    VS_FRAME_MJPEG: formatIndex=%u "
                                    "frameIndex=%u %ux%u maxFrame=%u "
                                    "defaultInterval=%u (%.3f fps) intervalType=%u",
                                    currentMjpegFormatIndex,
                                    frameIndex,
                                    width,
                                    height,
                                    maxFrameBufferSize,
                                    defaultInterval,
                                    interval100nsToFps(defaultInterval),
                                    intervalType
                            );

                            if (width != TARGET_UVC_WIDTH ||
                                height != TARGET_UVC_HEIGHT) {
                                break;
                            }

                            uint32_t selectedInterval = 0;
                            const bool hasTargetInterval =
                                    intervalType == 0
                                    ? chooseContinuousTargetInterval(
                                            p,
                                            length,
                                            selectedInterval
                                    )
                                    : chooseDiscreteTargetInterval(
                                            p,
                                            length,
                                            intervalType,
                                            selectedInterval
                                    );

                            if (!hasTargetInterval) {
                                break;
                            }

                            mjpegCandidate.valid = true;
                            mjpegCandidate.format = UvcVideoFormat::Mjpeg;
                            mjpegCandidate.formatIndex = currentMjpegFormatIndex;
                            mjpegCandidate.frameIndex = frameIndex;
                            mjpegCandidate.width = width;
                            mjpegCandidate.height = height;
                            mjpegCandidate.frameInterval100ns = selectedInterval;
                            mjpegCandidate.maxVideoFrameBufferSize =
                                    maxFrameBufferSize;
                            mjpegCandidate.bitsPerPixel = 0;

                            LOGI(
                                    "    >>> REQUIRED MJPEG MATCH: "
                                    "formatIndex=%u frameIndex=%u "
                                    "interval=%u (%.3f fps) maxFrame=%u",
                                    mjpegCandidate.formatIndex,
                                    mjpegCandidate.frameIndex,
                                    mjpegCandidate.frameInterval100ns,
                                    interval100nsToFps(
                                            mjpegCandidate.frameInterval100ns
                                    ),
                                    mjpegCandidate.maxVideoFrameBufferSize
                            );
                            break;
                        }

                        // Any non-MJPEG format descriptor ends the current
                        // MJPEG-format context. Uncompressed formats are never
                        // candidates in the field-monitor pipeline.
                        case 0x04:  // VS_FORMAT_UNCOMPRESSED
                        case 0x10:  // VS_FORMAT_FRAME_BASED
                        case 0x12:  // VS_FORMAT_STREAM_BASED
                            currentMjpegFormatIndex = 0;
                            break;

                        default:
                            break;
                    }
                }

                p += length;
                remaining -= length;
            }
        }
    }

    libusb_free_config_descriptor(config);

    if (!foundVsAlt0) {
        LOGE("Step 9: VideoStreaming IF1 ALT0 not found");
        return false;
    }

    if (!mjpegCandidate.valid) {
        LOGE(
                "Step 9: required %dx%d MJPEG @ %.3f fps "
                "is not advertised; fallback is disabled",
                TARGET_UVC_WIDTH,
                TARGET_UVC_HEIGHT,
                interval100nsToFps(TARGET_UVC_INTERVAL_100NS)
        );
        return false;
    }

    if (mjpegCandidate.maxVideoFrameBufferSize < 4) {
        LOGE(
                "Step 9: MJPEG descriptor maxFrame is invalid: %u",
                mjpegCandidate.maxVideoFrameBufferSize
        );
        return false;
    }

    selectedMode = mjpegCandidate;

    LOGI(
            "Step 9 selected mode: formatIndex=%u frameIndex=%u "
            "%ux%u MJPEG interval=%u (%.3f fps) maxFrame=%u",
            selectedMode.formatIndex,
            selectedMode.frameIndex,
            selectedMode.width,
            selectedMode.height,
            selectedMode.frameInterval100ns,
            interval100nsToFps(selectedMode.frameInterval100ns),
            selectedMode.maxVideoFrameBufferSize
    );

    return true;
}


static bool detectUvcVersion(
        libusb_device* device,
        uint16_t& uvcVersion)
{
    uvcVersion = 0;

    if (device == nullptr) {
        return false;
    }

    libusb_config_descriptor* config = nullptr;

    const int r =
            libusb_get_active_config_descriptor(
                    device,
                    &config
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "Step 10: get config for bcdUVC failed: "
                "%d (%s)",
                r,
                libusb_error_name(r)
        );

        return false;
    }

    bool found = false;

    for (uint8_t i = 0;
         i < config->bNumInterfaces &&
         !found;
         ++i) {

        const libusb_interface& iface =
                config->interface[i];

        for (int a = 0;
             a < iface.num_altsetting &&
             !found;
             ++a) {

            const libusb_interface_descriptor& alt =
                    iface.altsetting[a];

            // VideoControl interface, alt 0.
            if (alt.bInterfaceNumber != 0 ||
                alt.bAlternateSetting != 0 ||
                alt.bInterfaceClass != LIBUSB_CLASS_VIDEO ||
                alt.bInterfaceSubClass != 1) {

                continue;
            }

            const unsigned char* p =
                    alt.extra;

            int remaining =
                    alt.extra_length;

            while (p != nullptr &&
                   remaining >= 5) {

                const uint8_t length =
                        p[0];

                if (length < 3 ||
                    length > remaining) {

                    break;
                }

                const uint8_t descriptorType =
                        p[1];

                const uint8_t descriptorSubtype =
                        p[2];

                // CS_INTERFACE / VC_HEADER
                if (descriptorType == 0x24 &&
                    descriptorSubtype == 0x01 &&
                    length >= 5) {

                    uvcVersion =
                            readLe16(
                                    p + 3
                            );

                    found = true;
                    break;
                }

                p += length;
                remaining -= length;
            }
        }
    }

    libusb_free_config_descriptor(
            config
    );

    if (!found) {

        LOGE(
                "Step 10: VC_HEADER / bcdUVC not found"
        );

        return false;
    }

    LOGI(
            "Step 10: bcdUVC=0x%04X (%u.%02u)",
            uvcVersion,
            (uvcVersion >> 8) & 0xff,
            uvcVersion & 0xff
    );

    return true;
}


static bool getProbeControlLength(
        uint16_t& controlLength)
{
    if (gUsbHandle == nullptr) {

        return false;
    }

    libusb_device* device =
            libusb_get_device(
                    gUsbHandle
            );

    uint16_t uvcVersion = 0;

    if (!detectUvcVersion(
            device,
            uvcVersion)) {

        return false;
    }

    // Same rule used by the Linux UVC driver:
    //   UVC < 1.1  -> 26 bytes
    //   UVC < 1.5  -> 34 bytes
    //   UVC >= 1.5 -> 48 bytes
    if (uvcVersion < 0x0110) {

        controlLength = 26;
    }
    else if (uvcVersion < 0x0150) {

        controlLength = 34;
    }
    else {

        controlLength = 48;
    }

    LOGI(
            "Step 10: PROBE/COMMIT control length=%u "
            "(derived from bcdUVC, GET_LEN not used)",
            controlLength
    );

    return true;
}


static int streamControlTransfer(
        bool deviceToHost,
        uint8_t request,
        uint8_t controlSelector,
        unsigned char* data,
        uint16_t length)
{
    if (gUsbHandle == nullptr) {

        return LIBUSB_ERROR_NO_DEVICE;
    }

    return
            libusb_control_transfer(
                    gUsbHandle,
                    deviceToHost
                    ? UVC_REQ_IN
                    : UVC_REQ_OUT,
                    request,
                    static_cast<uint16_t>(
                            controlSelector << 8
                    ),
                    UVC_VIDEO_STREAMING_INTERFACE,
                    data,
                    length,
                    CONTROL_TIMEOUT_MS
            );
}


static bool getStreamControl(
        uint8_t request,
        uint8_t controlSelector,
        std::vector<unsigned char>& buffer,
        const char* label)
{
    const int r =
            streamControlTransfer(
                    true,
                    request,
                    controlSelector,
                    buffer.data(),
                    static_cast<uint16_t>(
                            buffer.size()
                    )
            );

    if (r !=
        static_cast<int>(
                buffer.size()
        )) {

        LOGE(
                "Step 10: %s failed: "
                "%d (%s)",
                label,
                r,
                r < 0
                ? libusb_error_name(r)
                : "short transfer"
        );

        return false;
    }

    return true;
}


static bool setStreamControl(
        uint8_t controlSelector,
        const std::vector<unsigned char>& buffer,
        const char* label)
{
    const int r =
            streamControlTransfer(
                    false,
                    UVC_SET_CUR,
                    controlSelector,
                    const_cast<unsigned char*>(
                            buffer.data()
                    ),
                    static_cast<uint16_t>(
                            buffer.size()
                    )
            );

    if (r !=
        static_cast<int>(
                buffer.size()
        )) {

        LOGE(
                "Step 10: %s failed: "
                "%d (%s)",
                label,
                r,
                r < 0
                ? libusb_error_name(r)
                : "short transfer"
        );

        return false;
    }

    return true;
}


static UvcProbeResult decodeProbe(
        const std::vector<unsigned char>& buffer)
{
    UvcProbeResult result{};

    if (buffer.size() < 26) {
        return result;
    }

    result.valid =
            true;

    result.controlLength =
            static_cast<uint16_t>(
                    buffer.size()
            );

    result.formatIndex =
            buffer[2];

    result.frameIndex =
            buffer[3];

    result.frameInterval100ns =
            readLe32(
                    buffer.data() + 4
            );

    result.maxVideoFrameSize =
            readLe32(
                    buffer.data() + 18
            );

    result.maxPayloadTransferSize =
            readLe32(
                    buffer.data() + 22
            );

    return result;
}


static void logProbe(
        const char* label,
        const std::vector<unsigned char>& buffer)
{
    const UvcProbeResult probe =
            decodeProbe(
                    buffer
            );

    if (!probe.valid) {

        LOGE(
                "Step 10: %s "
                "decode failed",
                label
        );

        return;
    }

    LOGI(
            "Step 10: %s "
            "len=%u format=%u frame=%u "
            "interval=%u (%.3f fps) "
            "maxFrame=%u maxPayload=%u",
            label,
            probe.controlLength,
            probe.formatIndex,
            probe.frameIndex,
            probe.frameInterval100ns,
            interval100nsToFps(
                    probe.frameInterval100ns
            ),
            probe.maxVideoFrameSize,
            probe.maxPayloadTransferSize
    );
}


static bool negotiateAndCommitStream(
        const UvcStreamMode& mode)
{
    uint16_t controlLength = 0;

    if (!getProbeControlLength(
            controlLength)) {

        return false;
    }

    std::vector<unsigned char> probe(
            controlLength,
            0
    );

    // Preserve current device defaults when GET_CUR(PROBE) works.
    // If it does not, a zero-initialized UVC control block is still
    // sufficient because format/frame/interval/maxFrame are set explicitly.
    const bool gotSeed =
            getStreamControl(
                    UVC_GET_CUR,
                    UVC_VS_PROBE_CONTROL,
                    probe,
                    "GET_CUR(PROBE seed)"
            );

    if (gotSeed) {

        logProbe(
                "seed PROBE",
                probe
        );
    }
    else {

        LOGI(
                "Step 10: GET_CUR(PROBE seed) unavailable; "
                "using zero-initialized control block"
        );

        std::fill(
                probe.begin(),
                probe.end(),
                0
        );
    }

    uint16_t bmHint =
            readLe16(
                    probe.data()
            );

    bmHint |= 0x0001;

    writeLe16(
            probe.data(),
            bmHint
    );

    probe[2] =
            mode.formatIndex;

    probe[3] =
            mode.frameIndex;

    writeLe32(
            probe.data() + 4,
            mode.frameInterval100ns
    );

    const uint32_t requestedMaxFrame =
            mode.maxVideoFrameBufferSize;

    if (requestedMaxFrame == 0) {

        LOGE(
                "Step 10: %s maxFrame is unavailable",
                uvcVideoFormatName(mode.format)
        );

        return false;
    }

    writeLe32(
            probe.data() + 18,
            requestedMaxFrame
    );

    const uint32_t seedPayload =
            readLe32(
                    probe.data() + 22
            );

    LOGI(
            "Step 10: seed maxPayload=%u; "
            "requesting alt3 host capacity=%u",
            seedPayload,
            TARGET_STREAM_ALT_CAPACITY
    );

    writeLe32(
            probe.data() + 22,
            TARGET_STREAM_ALT_CAPACITY
    );

    logProbe(
            "requested PROBE",
            probe
    );

    if (!setStreamControl(
            UVC_VS_PROBE_CONTROL,
            probe,
            "SET_CUR(PROBE)")) {

        return false;
    }

    std::vector<unsigned char> negotiated(
            controlLength,
            0
    );

    if (!getStreamControl(
            UVC_GET_CUR,
            UVC_VS_PROBE_CONTROL,
            negotiated,
            "GET_CUR(PROBE)")) {

        return false;
    }

    logProbe(
            "negotiated PROBE",
            negotiated
    );

    UvcProbeResult result =
            decodeProbe(
                    negotiated
            );

    if (!result.valid) {

        return false;
    }

    if (result.formatIndex !=
            mode.formatIndex ||
        result.frameIndex !=
            mode.frameIndex) {

        LOGE(
                "Step 10: device changed "
                "requested mode "
                "(wanted format=%u frame=%u, "
                "got format=%u frame=%u)",
                mode.formatIndex,
                mode.frameIndex,
                result.formatIndex,
                result.frameIndex
        );

        return false;
    }

    const uint32_t intervalDiff =
            result.frameInterval100ns >
                mode.frameInterval100ns
            ? result.frameInterval100ns -
                mode.frameInterval100ns
            : mode.frameInterval100ns -
                result.frameInterval100ns;

    if (intervalDiff > 1000) {

        LOGE(
                "Step 10: device changed "
                "frame interval too far "
                "(wanted=%u got=%u)",
                mode.frameInterval100ns,
                result.frameInterval100ns
        );

        return false;
    }

    if (result.maxVideoFrameSize == 0) {

        LOGI(
                "Step 10: negotiated "
                "maxFrame=0; using descriptor "
                "fallback=%u",
                mode.maxVideoFrameBufferSize
        );

        result.maxVideoFrameSize =
                requestedMaxFrame;

        writeLe32(
                negotiated.data() + 18,
                result.maxVideoFrameSize
        );
    }

    if (result.maxPayloadTransferSize == 0) {

        LOGI(
                "Step 10: negotiated "
                "maxPayload=0; using alt3 "
                "capacity fallback=%u",
                TARGET_STREAM_ALT_CAPACITY
        );

        result.maxPayloadTransferSize =
                TARGET_STREAM_ALT_CAPACITY;

        writeLe32(
                negotiated.data() + 22,
                result.maxPayloadTransferSize
        );
    }

    if (result.maxVideoFrameSize < 4) {

        LOGE(
                "Step 10: negotiated MJPEG maxFrame is invalid: %u",
                result.maxVideoFrameSize
        );

        return false;
    }

    if (result.maxPayloadTransferSize >
            TARGET_STREAM_ALT_CAPACITY) {

        LOGE(
                "Step 10: negotiated "
                "maxPayload=%u exceeds "
                "alt3 capacity=%u",
                result.maxPayloadTransferSize,
                TARGET_STREAM_ALT_CAPACITY
        );

        return false;
    }

    if (!setStreamControl(
            UVC_VS_COMMIT_CONTROL,
            negotiated,
            "SET_CUR(COMMIT)")) {

        return false;
    }

    LOGI(
            "Step 10: COMMIT OK "
            "format=%u frame=%u %s "
            "interval=%u maxFrame=%u "
            "maxPayload=%u",
            result.formatIndex,
            result.frameIndex,
            uvcVideoFormatName(mode.format),
            result.frameInterval100ns,
            result.maxVideoFrameSize,
            result.maxPayloadTransferSize
    );

    gProbeResult =
            result;

    return true;
}


static bool negotiateAndCommitMjpegBulk(
        const UvcStreamMode& mode)
{
    uint16_t controlLength = 0;

    if (!getProbeControlLength(controlLength)) {
        return false;
    }

    std::vector<unsigned char> probe(controlLength, 0);

    const bool gotSeed =
            getStreamControl(
                    UVC_GET_CUR,
                    UVC_VS_PROBE_CONTROL,
                    probe,
                    "MJPEG BULK GET_CUR(PROBE seed)"
            );

    if (gotSeed) {
        logProbe("MJPEG BULK seed PROBE", probe);
    }
    else {

        LOGI(
                "MJPEG BULK Step 10: GET_CUR(PROBE seed) unavailable; "
                "using zero-initialized control block"
        );

        std::fill(probe.begin(), probe.end(), 0);
    }

    uint16_t bmHint = readLe16(probe.data());
    bmHint |= 0x0001;
    writeLe16(probe.data(), bmHint);

    probe[2] = mode.formatIndex;
    probe[3] = mode.frameIndex;
    writeLe32(probe.data() + 4, mode.frameInterval100ns);

    if (mode.maxVideoFrameBufferSize != 0) {
        writeLe32(
                probe.data() + 18,
                mode.maxVideoFrameBufferSize
        );
    }

    const uint32_t seedPayload =
            readLe32(probe.data() + 22);

    LOGI(
            "MJPEG BULK Step 10: BULK seed maxPayload=%u; "
            "not applying MS2109 alt3/3072-byte clamp",
            seedPayload
    );

    logProbe("MJPEG BULK requested PROBE", probe);

    if (!setStreamControl(
            UVC_VS_PROBE_CONTROL,
            probe,
            "MJPEG BULK SET_CUR(PROBE)")) {

        return false;
    }

    std::vector<unsigned char> negotiated(controlLength, 0);

    if (!getStreamControl(
            UVC_GET_CUR,
            UVC_VS_PROBE_CONTROL,
            negotiated,
            "MJPEG BULK GET_CUR(PROBE)")) {

        return false;
    }

    logProbe("MJPEG BULK negotiated PROBE", negotiated);

    UvcProbeResult result = decodeProbe(negotiated);

    if (!result.valid) {
        return false;
    }

    if (result.formatIndex != mode.formatIndex ||
        result.frameIndex != mode.frameIndex) {

        LOGE(
                "MJPEG BULK Step 10: device changed mode "
                "wanted format=%u frame=%u got format=%u frame=%u",
                mode.formatIndex,
                mode.frameIndex,
                result.formatIndex,
                result.frameIndex
        );

        return false;
    }

    const uint32_t intervalDiff =
            result.frameInterval100ns > mode.frameInterval100ns
            ? result.frameInterval100ns - mode.frameInterval100ns
            : mode.frameInterval100ns - result.frameInterval100ns;

    if (intervalDiff > 1000) {

        LOGE(
                "MJPEG BULK Step 10: device changed frame interval "
                "wanted=%u got=%u",
                mode.frameInterval100ns,
                result.frameInterval100ns
        );

        return false;
    }

    if (result.maxVideoFrameSize == 0 &&
        mode.maxVideoFrameBufferSize != 0) {

        result.maxVideoFrameSize =
                mode.maxVideoFrameBufferSize;

        writeLe32(
                negotiated.data() + 18,
                result.maxVideoFrameSize
        );

        LOGI(
                "MJPEG BULK Step 10: maxFrame=0; "
                "using descriptor fallback=%u",
                result.maxVideoFrameSize
        );
    }

    if (result.maxPayloadTransferSize == 0) {

        LOGE(
                "MJPEG BULK Step 10: negotiated maxPayload=0; "
                "refusing to invent a BULK payload size"
        );

        return false;
    }

    if (!setStreamControl(
            UVC_VS_COMMIT_CONTROL,
            negotiated,
            "MJPEG BULK SET_CUR(COMMIT)")) {

        return false;
    }

    LOGI(
            "MJPEG BULK Step 10: COMMIT OK "
            "format=%u frame=%u 1280x720 MJPEG "
            "interval=%u (%.3f fps) maxFrame=%u maxPayload=%u",
            result.formatIndex,
            result.frameIndex,
            result.frameInterval100ns,
            interval100nsToFps(result.frameInterval100ns),
            result.maxVideoFrameSize,
            result.maxPayloadTransferSize
    );

    gProbeResult = result;
    return true;
}


enum class Jpeg422ProbeResult {
    NeedMoreData,
    Is422,
    Not422
};


static Jpeg422ProbeResult probeJpeg422Sampling(
        const std::vector<unsigned char>& jpeg)
{
    if (jpeg.size() < 2) {
        return Jpeg422ProbeResult::NeedMoreData;
    }

    if (jpeg[0] != 0xff || jpeg[1] != 0xd8) {
        LOGE("MJPEG 4:2:2 check: JPEG SOI missing");
        return Jpeg422ProbeResult::Not422;
    }

    size_t offset = 2;

    while (offset < jpeg.size()) {
        if (jpeg[offset] != 0xff) {
            ++offset;
            continue;
        }

        while (offset < jpeg.size() && jpeg[offset] == 0xff) {
            ++offset;
        }

        if (offset >= jpeg.size()) {
            return Jpeg422ProbeResult::NeedMoreData;
        }

        const uint8_t marker = jpeg[offset++];

        if (marker == 0xd8) {
            continue;
        }

        if (marker == 0xd9 || marker == 0xda) {
            LOGE("MJPEG 4:2:2 check: reached EOI/SOS before SOF");
            return Jpeg422ProbeResult::Not422;
        }

        if (marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }

        if (offset + 2 > jpeg.size()) {
            return Jpeg422ProbeResult::NeedMoreData;
        }

        const uint16_t segmentLength =
                static_cast<uint16_t>(
                        (static_cast<uint16_t>(jpeg[offset]) << 8) |
                        jpeg[offset + 1]
                );

        if (segmentLength < 2) {
            LOGE(
                    "MJPEG 4:2:2 check: invalid marker 0xFF%02X length=%u",
                    marker,
                    segmentLength
            );
            return Jpeg422ProbeResult::Not422;
        }

        if (offset + segmentLength > jpeg.size()) {
            return Jpeg422ProbeResult::NeedMoreData;
        }

        const bool isSof =
                marker == 0xc0 || marker == 0xc1 ||
                marker == 0xc2 || marker == 0xc3 ||
                marker == 0xc5 || marker == 0xc6 ||
                marker == 0xc7 || marker == 0xc9 ||
                marker == 0xca || marker == 0xcb ||
                marker == 0xcd || marker == 0xce ||
                marker == 0xcf;

        if (isSof) {
            if (segmentLength < 11) {
                LOGE("MJPEG 4:2:2 check: SOF segment too short");
                return Jpeg422ProbeResult::Not422;
            }

            const size_t data = offset + 2;
            const uint8_t components = jpeg[data + 5];

            if (components < 3 ||
                data + 6 + static_cast<size_t>(components) * 3 >
                        offset + segmentLength) {

                LOGE(
                        "MJPEG 4:2:2 check: unsupported SOF components=%u",
                        components
                );
                return Jpeg422ProbeResult::Not422;
            }

            const uint8_t ySampling = jpeg[data + 7];
            const uint8_t cbSampling = jpeg[data + 10];
            const uint8_t crSampling = jpeg[data + 13];

            const bool is422 =
                    ((ySampling >> 4) & 0x0f) == 2 &&
                    (ySampling & 0x0f) == 1 &&
                    ((cbSampling >> 4) & 0x0f) == 1 &&
                    (cbSampling & 0x0f) == 1 &&
                    ((crSampling >> 4) & 0x0f) == 1 &&
                    (crSampling & 0x0f) == 1;

            LOGI(
                    "MJPEG JPEG sampling: Y=%ux%u Cb=%ux%u Cr=%ux%u -> %s",
                    (ySampling >> 4) & 0x0f,
                    ySampling & 0x0f,
                    (cbSampling >> 4) & 0x0f,
                    cbSampling & 0x0f,
                    (crSampling >> 4) & 0x0f,
                    crSampling & 0x0f,
                    is422 ? "YCbCr 4:2:2 PASS" : "NOT 4:2:2 FAIL"
            );

            return is422
                    ? Jpeg422ProbeResult::Is422
                    : Jpeg422ProbeResult::Not422;
        }

        offset += segmentLength;
    }

    return Jpeg422ProbeResult::NeedMoreData;
}


// ------------------------------------------------------------
// MJPEG BULK transport:
//   continuous asynchronous BULK reception for required 1280x720 MJPEG60.
//
// This transport feeds each UVC payload into the shared MJPEG decoder,
// which publishes planar YCbCr 4:2:2 frames to the common renderer.
// Transport measurements retained here are:
//
//   wireFrameMs:
//     host-observed time from the callback containing JPEG SOI to the
//     callback containing UVC EOF for the same frame.
//
//   frameIntervalMs:
//     host-observed EOF-to-EOF interval between consecutive good frames.
//
// These are useful for proving sustained 60 fps and USB delivery jitter.
// They are NOT HDMI-input-to-display latency because the time spent inside
// the capture device before the first USB payload is not visible to the host.
// ------------------------------------------------------------

static constexpr int MJPEG_BULK_ASYNC_TRANSFER_COUNT = 8;
static constexpr int MJPEG_BULK_EVENT_TIMEOUT_US = 50'000;

static constexpr uint8_t UVC_STREAM_FID = 0x01;
static constexpr uint8_t UVC_STREAM_EOF = 0x02;
static constexpr uint8_t UVC_STREAM_PTS = 0x04;
static constexpr uint8_t UVC_STREAM_SCR = 0x08;
static constexpr uint8_t UVC_STREAM_ERR = 0x40;

struct MjpegBulkTransferSlot {
    libusb_transfer* transfer = nullptr;
    std::vector<unsigned char> buffer;
};

struct MjpegBulkFrameState {
    bool active = false;
    bool bad = false;
    bool sawEoi = false;
    bool lastByteValid = false;

    uint8_t fid = 0;
    unsigned char lastByte = 0;

    size_t bytes = 0;
    uint32_t payloads = 0;

    uint64_t firstPayloadNs = 0;
};

struct MjpegBulkStats {
    uint64_t transfersCompleted = 0;
    uint64_t transfersFailed = 0;
    uint64_t submitErrors = 0;

    uint64_t usbBytes = 0;
    uint64_t videoBytes = 0;
    uint64_t payloads = 0;

    uint64_t goodFrames = 0;
    uint64_t droppedFrames = 0;
    uint64_t malformedHeaders = 0;
    uint64_t uvcErrors = 0;
    uint64_t fidResyncs = 0;
    uint64_t overflowFrames = 0;

    uint64_t lastGoodFrames = 0;
    uint64_t lastUsbBytes = 0;
    uint64_t lastVideoBytes = 0;
    uint64_t lastDroppedFrames = 0;
    uint64_t lastMalformedHeaders = 0;
    uint64_t lastUvcErrors = 0;

    uint64_t lastLogNs = 0;
    uint64_t lastGoodEofNs = 0;

    uint64_t windowFrameBytesSum = 0;
    size_t windowFrameBytesMin = std::numeric_limits<size_t>::max();
    size_t windowFrameBytesMax = 0;

    std::vector<double> wireFrameMs;
    std::vector<double> frameIntervalMs;
};

static std::atomic<bool> gMjpegBulkRunning{false};
static std::atomic<int> gMjpegBulkInflight{0};

static std::thread gMjpegBulkEventThread;
static std::vector<MjpegBulkTransferSlot> gMjpegBulkTransfers;

static MjpegBulkFrameState gMjpegBulkFrame;
static MjpegBulkStats gMjpegBulkStats;

static uint32_t gMjpegBulkMaxFrame = 0;
static bool gMjpegBulkHeaderLogged = false;

// One-shot JPEG SOF sampling validation.  The transport is accepted only
// when the first complete MJPEG frame reports conventional JPEG 4:2:2:
//   Y  = 2x1
//   Cb = 1x1
//   Cr = 1x1
// After validation succeeds, no further JPEG-header copy is performed.
static std::atomic<bool> gBulkJpeg422Checked{false};
static std::atomic<bool> gBulkJpeg422Ok{false};
static std::vector<unsigned char> gBulkJpegHeaderProbe;


static uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
            ).count()
    );
}


static double percentileSample(
        const std::vector<double>& input,
        double fraction)
{
    if (input.empty()) {
        return 0.0;
    }

    std::vector<double> values = input;
    std::sort(values.begin(), values.end());

    const double clamped =
            std::max(0.0, std::min(1.0, fraction));

    const size_t index =
            static_cast<size_t>(
                    clamped *
                    static_cast<double>(values.size() - 1) +
                    0.5
            );

    return values[index];
}


static void resetMjpegBulkFrame()
{
    gMjpegBulkFrame = {};
}


static void maybeLogMjpegBulkStats(uint64_t nowNs)
{
    if (gMjpegBulkStats.lastLogNs == 0) {
        gMjpegBulkStats.lastLogNs = nowNs;
        return;
    }

    const uint64_t elapsedNs =
            nowNs - gMjpegBulkStats.lastLogNs;

    if (elapsedNs < 1'000'000'000ULL) {
        return;
    }

    const double seconds =
            static_cast<double>(elapsedNs) /
            1'000'000'000.0;

    const uint64_t frameDelta =
            gMjpegBulkStats.goodFrames -
            gMjpegBulkStats.lastGoodFrames;

    const uint64_t usbDelta =
            gMjpegBulkStats.usbBytes -
            gMjpegBulkStats.lastUsbBytes;

    const uint64_t videoDelta =
            gMjpegBulkStats.videoBytes -
            gMjpegBulkStats.lastVideoBytes;

    const uint64_t dropDelta =
            gMjpegBulkStats.droppedFrames -
            gMjpegBulkStats.lastDroppedFrames;

    const uint64_t malformedDelta =
            gMjpegBulkStats.malformedHeaders -
            gMjpegBulkStats.lastMalformedHeaders;

    const uint64_t uvcErrorDelta =
            gMjpegBulkStats.uvcErrors -
            gMjpegBulkStats.lastUvcErrors;

    const double fps =
            static_cast<double>(frameDelta) /
            seconds;

    const double usbMiBps =
            static_cast<double>(usbDelta) /
            (1024.0 * 1024.0) /
            seconds;

    const double videoMiBps =
            static_cast<double>(videoDelta) /
            (1024.0 * 1024.0) /
            seconds;

    const double frameBytesAvg =
            frameDelta != 0
            ? static_cast<double>(
                    gMjpegBulkStats.windowFrameBytesSum
              ) / static_cast<double>(frameDelta)
            : 0.0;

    const size_t frameBytesMin =
            frameDelta != 0
            ? gMjpegBulkStats.windowFrameBytesMin
            : 0;

    const size_t frameBytesMax =
            frameDelta != 0
            ? gMjpegBulkStats.windowFrameBytesMax
            : 0;

    const double wireAvg =
            !gMjpegBulkStats.wireFrameMs.empty()
            ? [&]() {
                double sum = 0.0;
                for (double v : gMjpegBulkStats.wireFrameMs) {
                    sum += v;
                }
                return sum /
                        static_cast<double>(
                                gMjpegBulkStats.wireFrameMs.size()
                        );
              }()
            : 0.0;

    const double intervalAvg =
            !gMjpegBulkStats.frameIntervalMs.empty()
            ? [&]() {
                double sum = 0.0;
                for (double v : gMjpegBulkStats.frameIntervalMs) {
                    sum += v;
                }
                return sum /
                        static_cast<double>(
                                gMjpegBulkStats.frameIntervalMs.size()
                        );
              }()
            : 0.0;

    LOGI(
            "MJPEG BULK stats: fps=%.2f usb=%.2f MiB/s video=%.2f MiB/s "
            "jpegBytes avg=%.0f min=%zu max=%zu "
            "wireFrameMs avg=%.3f p50=%.3f p95=%.3f max=%.3f "
            "frameIntervalMs avg=%.3f p50=%.3f p95=%.3f max=%.3f "
            "drop=%llu malformed=%llu uvcErr=%llu "
            "totalFrames=%llu inflight=%d",
            fps,
            usbMiBps,
            videoMiBps,
            frameBytesAvg,
            frameBytesMin,
            frameBytesMax,
            wireAvg,
            percentileSample(gMjpegBulkStats.wireFrameMs, 0.50),
            percentileSample(gMjpegBulkStats.wireFrameMs, 0.95),
            percentileSample(gMjpegBulkStats.wireFrameMs, 1.00),
            intervalAvg,
            percentileSample(gMjpegBulkStats.frameIntervalMs, 0.50),
            percentileSample(gMjpegBulkStats.frameIntervalMs, 0.95),
            percentileSample(gMjpegBulkStats.frameIntervalMs, 1.00),
            static_cast<unsigned long long>(dropDelta),
            static_cast<unsigned long long>(malformedDelta),
            static_cast<unsigned long long>(uvcErrorDelta),
            static_cast<unsigned long long>(gMjpegBulkStats.goodFrames),
            gMjpegBulkInflight.load(std::memory_order_acquire)
    );

    gMjpegBulkStats.lastLogNs = nowNs;
    gMjpegBulkStats.lastGoodFrames =
            gMjpegBulkStats.goodFrames;
    gMjpegBulkStats.lastUsbBytes =
            gMjpegBulkStats.usbBytes;
    gMjpegBulkStats.lastVideoBytes =
            gMjpegBulkStats.videoBytes;
    gMjpegBulkStats.lastDroppedFrames =
            gMjpegBulkStats.droppedFrames;
    gMjpegBulkStats.lastMalformedHeaders =
            gMjpegBulkStats.malformedHeaders;
    gMjpegBulkStats.lastUvcErrors =
            gMjpegBulkStats.uvcErrors;

    gMjpegBulkStats.windowFrameBytesSum = 0;
    gMjpegBulkStats.windowFrameBytesMin =
            std::numeric_limits<size_t>::max();
    gMjpegBulkStats.windowFrameBytesMax = 0;
    gMjpegBulkStats.wireFrameMs.clear();
    gMjpegBulkStats.frameIntervalMs.clear();
}


static void processMjpegBulkPayload(
        const unsigned char* data,
        int length,
        uint64_t callbackNs)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    // The shared decoder owns JPEG assembly/decode/latest-frame publication.
    // Feeding it here leaves BULK transport timing/statistics independent.
    uvc_mjpeg_decoder::processPayload(
            data,
            length,
            callbackNs
    );

    gMjpegBulkStats.usbBytes +=
            static_cast<uint64_t>(length);

    if (length < 2) {
        ++gMjpegBulkStats.malformedHeaders;
        return;
    }

    const uint8_t headerLength = data[0];
    const uint8_t flags = data[1];

    if (headerLength < 2 ||
        static_cast<int>(headerLength) > length) {

        ++gMjpegBulkStats.malformedHeaders;
        gMjpegBulkFrame.bad = true;
        return;
    }

    if (!gMjpegBulkHeaderLogged) {
        LOGI(
                "MJPEG BULK header: length=%u flags=0x%02X "
                "PTS=%s SCR=%s",
                headerLength,
                flags,
                (flags & UVC_STREAM_PTS) ? "YES" : "NO",
                (flags & UVC_STREAM_SCR) ? "YES" : "NO"
        );

        if ((flags & UVC_STREAM_PTS) != 0 &&
            headerLength >= 6) {

            LOGI(
                    "MJPEG BULK header: first PTS=%u",
                    readLe32(data + 2)
            );
        }

        gMjpegBulkHeaderLogged = true;
    }

    ++gMjpegBulkStats.payloads;

    const uint8_t fid =
            (flags & UVC_STREAM_FID) ? 1 : 0;

    const bool eof =
            (flags & UVC_STREAM_EOF) != 0;

    const bool payloadError =
            (flags & UVC_STREAM_ERR) != 0;

    const unsigned char* payload =
            data + headerLength;

    const size_t payloadBytes =
            static_cast<size_t>(
                    length -
                    static_cast<int>(headerLength)
            );

    gMjpegBulkStats.videoBytes +=
            static_cast<uint64_t>(payloadBytes);

    if (payloadError) {
        ++gMjpegBulkStats.uvcErrors;

        if (gMjpegBulkFrame.active) {
            gMjpegBulkFrame.bad = true;
        }

        return;
    }

    if (gMjpegBulkFrame.active &&
        fid != gMjpegBulkFrame.fid) {

        ++gMjpegBulkStats.fidResyncs;
        ++gMjpegBulkStats.droppedFrames;
        resetMjpegBulkFrame();
    }

    size_t payloadOffset = 0;

    if (!gMjpegBulkFrame.active &&
        payloadBytes > 0) {

        bool foundSoi = false;

        for (size_t i = 0;
             i + 1 < payloadBytes;
             ++i) {

            if (payload[i] == 0xff &&
                payload[i + 1] == 0xd8) {

                payloadOffset = i;
                foundSoi = true;
                break;
            }
        }

        if (foundSoi) {
            gMjpegBulkFrame.active = true;
            gMjpegBulkFrame.fid = fid;
            gMjpegBulkFrame.firstPayloadNs = callbackNs;

            if (!gBulkJpeg422Checked.load(std::memory_order_acquire)) {
                gBulkJpegHeaderProbe.clear();
            }
        }
    }

    if (!gMjpegBulkFrame.active) {
        return;
    }

    ++gMjpegBulkFrame.payloads;

    if (!gBulkJpeg422Checked.load(std::memory_order_acquire) &&
        payloadOffset < payloadBytes) {

        static constexpr size_t JPEG_HEADER_PROBE_LIMIT = 64 * 1024;

        const size_t available = payloadBytes - payloadOffset;
        const size_t remaining =
                JPEG_HEADER_PROBE_LIMIT > gBulkJpegHeaderProbe.size()
                ? JPEG_HEADER_PROBE_LIMIT - gBulkJpegHeaderProbe.size()
                : 0;

        const size_t copyBytes = std::min(available, remaining);

        if (copyBytes > 0) {
            gBulkJpegHeaderProbe.insert(
                    gBulkJpegHeaderProbe.end(),
                    payload + payloadOffset,
                    payload + payloadOffset + copyBytes
            );
        }

        const Jpeg422ProbeResult sampling =
                probeJpeg422Sampling(gBulkJpegHeaderProbe);

        if (sampling != Jpeg422ProbeResult::NeedMoreData) {
            const bool ok = sampling == Jpeg422ProbeResult::Is422;
            gBulkJpeg422Ok.store(ok, std::memory_order_release);
            gBulkJpeg422Checked.store(true, std::memory_order_release);
            gBulkJpegHeaderProbe.clear();
        }
        else if (gBulkJpegHeaderProbe.size() >= JPEG_HEADER_PROBE_LIMIT) {
            LOGE(
                    "MJPEG 4:2:2 check: SOF not found in first %zu bytes",
                    JPEG_HEADER_PROBE_LIMIT
            );
            gBulkJpeg422Ok.store(false, std::memory_order_release);
            gBulkJpeg422Checked.store(true, std::memory_order_release);
            gBulkJpegHeaderProbe.clear();
        }
    }

    for (size_t i = payloadOffset;
         i < payloadBytes;
         ++i) {

        const unsigned char value = payload[i];

        if (gMjpegBulkFrame.lastByteValid &&
            gMjpegBulkFrame.lastByte == 0xff &&
            value == 0xd9) {

            gMjpegBulkFrame.sawEoi = true;
        }

        gMjpegBulkFrame.lastByte = value;
        gMjpegBulkFrame.lastByteValid = true;
    }

    gMjpegBulkFrame.bytes +=
            payloadBytes - payloadOffset;

    if (gMjpegBulkFrame.bytes >
        static_cast<size_t>(gMjpegBulkMaxFrame)) {

        ++gMjpegBulkStats.overflowFrames;
        ++gMjpegBulkStats.droppedFrames;
        resetMjpegBulkFrame();
        return;
    }

    if (!eof) {
        return;
    }

    const bool frameOk =
            !gMjpegBulkFrame.bad &&
            gMjpegBulkFrame.sawEoi &&
            gMjpegBulkFrame.bytes >= 4;

    if (!frameOk) {
        ++gMjpegBulkStats.droppedFrames;
        resetMjpegBulkFrame();
        return;
    }

    if (!gBulkJpeg422Checked.load(std::memory_order_acquire)) {
        LOGE(
                "MJPEG 4:2:2 check: complete JPEG reached EOF without usable SOF sampling"
        );
        gBulkJpeg422Ok.store(false, std::memory_order_release);
        gBulkJpeg422Checked.store(true, std::memory_order_release);
        gBulkJpegHeaderProbe.clear();
    }

    const double wireFrameMs =
            static_cast<double>(
                    callbackNs -
                    gMjpegBulkFrame.firstPayloadNs
            ) /
            1'000'000.0;

    ++gMjpegBulkStats.goodFrames;

    gMjpegBulkStats.windowFrameBytesSum +=
            static_cast<uint64_t>(
                    gMjpegBulkFrame.bytes
            );

    gMjpegBulkStats.windowFrameBytesMin =
            std::min(
                    gMjpegBulkStats.windowFrameBytesMin,
                    gMjpegBulkFrame.bytes
            );

    gMjpegBulkStats.windowFrameBytesMax =
            std::max(
                    gMjpegBulkStats.windowFrameBytesMax,
                    gMjpegBulkFrame.bytes
            );

    gMjpegBulkStats.wireFrameMs.push_back(
            wireFrameMs
    );

    if (gMjpegBulkStats.lastGoodEofNs != 0) {

        gMjpegBulkStats.frameIntervalMs.push_back(
                static_cast<double>(
                        callbackNs -
                        gMjpegBulkStats.lastGoodEofNs
                ) /
                1'000'000.0
        );
    }

    gMjpegBulkStats.lastGoodEofNs =
            callbackNs;

    if (gMjpegBulkStats.goodFrames <= 5) {

        LOGI(
                "MJPEG BULK FRAME #%llu: bytes=%zu payloads=%u "
                "fid=%u wireFrameMs=%.3f",
                static_cast<unsigned long long>(
                        gMjpegBulkStats.goodFrames
                ),
                gMjpegBulkFrame.bytes,
                gMjpegBulkFrame.payloads,
                gMjpegBulkFrame.fid,
                wireFrameMs
        );
    }

    resetMjpegBulkFrame();
}


static void LIBUSB_CALL onMjpegBulkTransfer(
        libusb_transfer* transfer)
{
    if (transfer == nullptr) {
        return;
    }

    gMjpegBulkInflight.fetch_sub(
            1,
            std::memory_order_acq_rel
    );

    const uint64_t callbackNs =
            steadyNowNs();

    bool canResubmit =
            gMjpegBulkRunning.load(
                    std::memory_order_acquire
            );

    if (transfer->status ==
        LIBUSB_TRANSFER_COMPLETED) {

        ++gMjpegBulkStats.transfersCompleted;

        if (transfer->actual_length > 0) {
            processMjpegBulkPayload(
                    transfer->buffer,
                    transfer->actual_length,
                    callbackNs
            );
        }
    }
    else if (transfer->status !=
             LIBUSB_TRANSFER_CANCELLED) {

        ++gMjpegBulkStats.transfersFailed;

        LOGE(
                "MJPEG BULK async transfer failed: status=%d actual=%d",
                transfer->status,
                transfer->actual_length
        );

        if (transfer->status ==
                LIBUSB_TRANSFER_NO_DEVICE) {

            gMjpegBulkRunning.store(
                    false,
                    std::memory_order_release
            );
            canResubmit = false;
        }
    }

    maybeLogMjpegBulkStats(
            callbackNs
    );

    if (!canResubmit ||
        !gMjpegBulkRunning.load(
                std::memory_order_acquire
        )) {

        return;
    }

    gMjpegBulkInflight.fetch_add(
            1,
            std::memory_order_acq_rel
    );

    const int r =
            libusb_submit_transfer(
                    transfer
            );

    if (r != LIBUSB_SUCCESS) {

        gMjpegBulkInflight.fetch_sub(
                1,
                std::memory_order_acq_rel
        );

        ++gMjpegBulkStats.submitErrors;

        LOGE(
                "MJPEG BULK async resubmit failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        gMjpegBulkRunning.store(
                false,
                std::memory_order_release
        );
    }
}


static void mjpegBulkEventLoop()
{
    LOGI(
            "MJPEG BULK async event thread started"
    );

    bool cancellationIssued = false;

    while (gMjpegBulkRunning.load(
               std::memory_order_acquire
           ) ||
           gMjpegBulkInflight.load(
               std::memory_order_acquire
           ) > 0) {

        if (!gMjpegBulkRunning.load(
                std::memory_order_acquire
            ) &&
            !cancellationIssued) {

            for (MjpegBulkTransferSlot& slot :
                 gMjpegBulkTransfers) {

                if (slot.transfer != nullptr) {
                    libusb_cancel_transfer(
                            slot.transfer
                    );
                }
            }

            cancellationIssued = true;
        }

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec =
                MJPEG_BULK_EVENT_TIMEOUT_US;

        const int r =
                libusb_handle_events_timeout_completed(
                        gUsbContext,
                        &timeout,
                        nullptr
                );

        if (r != LIBUSB_SUCCESS &&
            r != LIBUSB_ERROR_INTERRUPTED) {

            LOGE(
                    "MJPEG BULK event loop failed: %d (%s)",
                    r,
                    libusb_error_name(r)
            );

            gMjpegBulkRunning.store(
                    false,
                    std::memory_order_release
            );
        }
    }

    LOGI(
            "MJPEG BULK async event thread stopped: "
            "frames=%llu transfers=%llu failed=%llu submitErr=%llu "
            "drop=%llu malformed=%llu uvcErr=%llu fidResync=%llu overflow=%llu",
            static_cast<unsigned long long>(gMjpegBulkStats.goodFrames),
            static_cast<unsigned long long>(gMjpegBulkStats.transfersCompleted),
            static_cast<unsigned long long>(gMjpegBulkStats.transfersFailed),
            static_cast<unsigned long long>(gMjpegBulkStats.submitErrors),
            static_cast<unsigned long long>(gMjpegBulkStats.droppedFrames),
            static_cast<unsigned long long>(gMjpegBulkStats.malformedHeaders),
            static_cast<unsigned long long>(gMjpegBulkStats.uvcErrors),
            static_cast<unsigned long long>(gMjpegBulkStats.fidResyncs),
            static_cast<unsigned long long>(gMjpegBulkStats.overflowFrames)
    );
}


static void stopMjpegAsyncBulkTransport()
{
    const bool hadThread =
            gMjpegBulkEventThread.joinable();

    if (!hadThread &&
        gMjpegBulkTransfers.empty()) {

        return;
    }

    gMjpegBulkRunning.store(
            false,
            std::memory_order_release
    );

    for (MjpegBulkTransferSlot& slot :
         gMjpegBulkTransfers) {

        if (slot.transfer != nullptr) {
            libusb_cancel_transfer(
                    slot.transfer
            );
        }
    }

    if (gMjpegBulkEventThread.joinable()) {
        gMjpegBulkEventThread.join();
    }

    // No more BULK callbacks can arrive after the event thread is drained.
    uvc_mjpeg_decoder::stop();

    for (MjpegBulkTransferSlot& slot :
         gMjpegBulkTransfers) {

        if (slot.transfer != nullptr) {
            libusb_free_transfer(
                    slot.transfer
            );
            slot.transfer = nullptr;
        }
    }

    gMjpegBulkTransfers.clear();
    gMjpegBulkInflight.store(
            0,
            std::memory_order_release
    );

    resetMjpegBulkFrame();
    gMjpegBulkStats = {};
    gMjpegBulkMaxFrame = 0;
    gMjpegBulkHeaderLogged = false;

    gBulkJpeg422Checked.store(false, std::memory_order_release);
    gBulkJpeg422Ok.store(false, std::memory_order_release);
    gBulkJpegHeaderProbe.clear();
}


static bool startMjpegAsyncBulkTransport()
{
    if (gUsbContext == nullptr ||
        gUsbHandle == nullptr) {

        LOGE(
                "MJPEG BULK async: USB context/handle is null"
        );

        return false;
    }

    const uint32_t maxPayload =
            gProbeResult.maxPayloadTransferSize;

    const uint32_t maxFrame =
            gProbeResult.maxVideoFrameSize;

    if (maxPayload < 2 ||
        maxFrame < 4 ||
        maxPayload >
            static_cast<uint32_t>(
                    std::numeric_limits<int>::max()
            )) {

        LOGE(
                "MJPEG BULK async: invalid negotiated limits "
                "maxPayload=%u maxFrame=%u",
                maxPayload,
                maxFrame
        );

        return false;
    }

    stopMjpegAsyncBulkTransport();

    if (!uvc_mjpeg_decoder::start(maxFrame)) {
        LOGE(
                "MJPEG BULK async: shared JPEG decoder start failed"
        );
        return false;
    }

    gMjpegBulkMaxFrame = maxFrame;
    gMjpegBulkHeaderLogged = false;

    gBulkJpeg422Checked.store(false, std::memory_order_release);
    gBulkJpeg422Ok.store(false, std::memory_order_release);
    gBulkJpegHeaderProbe.clear();
    gBulkJpegHeaderProbe.reserve(4096);

    resetMjpegBulkFrame();
    gMjpegBulkStats = {};
    gMjpegBulkStats.lastLogNs =
            steadyNowNs();

    gMjpegBulkStats.wireFrameMs.reserve(128);
    gMjpegBulkStats.frameIntervalMs.reserve(128);

    gMjpegBulkTransfers.resize(
            MJPEG_BULK_ASYNC_TRANSFER_COUNT
    );

    for (MjpegBulkTransferSlot& slot :
         gMjpegBulkTransfers) {

        slot.buffer.resize(
                maxPayload
        );

        slot.transfer =
                libusb_alloc_transfer(0);

        if (slot.transfer == nullptr) {

            LOGE(
                    "MJPEG BULK async: libusb_alloc_transfer failed"
            );

            stopMjpegAsyncBulkTransport();
            return false;
        }

        libusb_fill_bulk_transfer(
                slot.transfer,
                gUsbHandle,
                UVC_VIDEO_ENDPOINT,
                slot.buffer.data(),
                static_cast<int>(slot.buffer.size()),
                onMjpegBulkTransfer,
                &slot,
                0
        );
    }

    gMjpegBulkRunning.store(
            true,
            std::memory_order_release
    );

    gMjpegBulkEventThread =
            std::thread(
                    mjpegBulkEventLoop
            );

    int submitted = 0;

    for (MjpegBulkTransferSlot& slot :
         gMjpegBulkTransfers) {

        gMjpegBulkInflight.fetch_add(
                1,
                std::memory_order_acq_rel
        );

        const int r =
                libusb_submit_transfer(
                        slot.transfer
                );

        if (r != LIBUSB_SUCCESS) {

            gMjpegBulkInflight.fetch_sub(
                    1,
                    std::memory_order_acq_rel
            );

            LOGE(
                    "MJPEG BULK async: initial submit failed "
                    "slot=%d: %d (%s)",
                    submitted,
                    r,
                    libusb_error_name(r)
            );

            gMjpegBulkRunning.store(
                    false,
                    std::memory_order_release
            );

            stopMjpegAsyncBulkTransport();
            return false;
        }

        ++submitted;
    }

    LOGI(
            "MJPEG BULK async START: endpoint=0x%02X "
            "transferBytes=%u transfers=%d queuedBytes=%u "
            "maxFrame=%u target=1280x720 MJPEG60",
            UVC_VIDEO_ENDPOINT,
            maxPayload,
            MJPEG_BULK_ASYNC_TRANSFER_COUNT,
            maxPayload *
                static_cast<uint32_t>(
                        MJPEG_BULK_ASYNC_TRANSFER_COUNT
                ),
            maxFrame
    );

    // Validate the actual JPEG coding, not only the UVC format descriptor.
    // SOF is near the start of a JPEG, so one frame is normally sufficient.
    static constexpr int JPEG_422_CHECK_TIMEOUT_MS = 1000;

    for (int waitedMs = 0;
         waitedMs < JPEG_422_CHECK_TIMEOUT_MS &&
         !gBulkJpeg422Checked.load(std::memory_order_acquire);
         ++waitedMs) {

        std::this_thread::sleep_for(
                std::chrono::milliseconds(1)
        );
    }

    if (!gBulkJpeg422Checked.load(std::memory_order_acquire)) {
        LOGE(
                "MJPEG BULK start: timed out waiting for JPEG 4:2:2 SOF check"
        );
        stopMjpegAsyncBulkTransport();
        return false;
    }

    if (!gBulkJpeg422Ok.load(std::memory_order_acquire)) {
        LOGE(
                "MJPEG BULK start: JPEG sampling is not YCbCr 4:2:2"
        );
        stopMjpegAsyncBulkTransport();
        return false;
    }

    LOGI(
            "MJPEG BULK start: JPEG YCbCr 4:2:2 verified"
    );

    LOGI(
            "MJPEG BULK latency note: wireFrameMs=SOI callback->EOF callback; "
            "frameIntervalMs=EOF->EOF. Capture-device internal encode delay is not "
            "observable in this host-only test."
    );

    return true;
}


static void closeLocked()
{
    // Stop and drain asynchronous MJPEG BULK transfers before
    // releasing the shared libusb handle/context.
    stopMjpegAsyncBulkTransport();

    // Stop and drain asynchronous ISO URBs before changing
    // the streaming alternate setting or releasing IF1.
    uvc_stream::stop();

    if (gUsbHandle != nullptr &&
        gVideoInterfaceClaimed &&
        gCurrentAltSetting != 0) {

        const int r =
                libusb_set_interface_alt_setting(
                        gUsbHandle,
                        UVC_VIDEO_STREAMING_INTERFACE,
                        0
                );

        if (r == LIBUSB_SUCCESS) {

            LOGI(
                    "UVC IF%d alt %d -> alt 0 OK",
                    UVC_VIDEO_STREAMING_INTERFACE,
                    gCurrentAltSetting
            );
        }
        else {

            LOGE(
                    "UVC IF%d return to alt 0 "
                    "failed: %d (%s)",
                    UVC_VIDEO_STREAMING_INTERFACE,
                    r,
                    libusb_error_name(r)
            );
        }

        gCurrentAltSetting =
                0;
    }

    if (gUsbHandle != nullptr &&
        gVideoInterfaceClaimed) {

        const int r =
                libusb_release_interface(
                        gUsbHandle,
                        UVC_VIDEO_STREAMING_INTERFACE
                );

        if (r == LIBUSB_SUCCESS) {

            LOGI(
                    "libusb_release_interface(%d) OK",
                    UVC_VIDEO_STREAMING_INTERFACE
            );
        }
        else {

            LOGE(
                    "libusb_release_interface(%d) "
                    "failed: %d (%s)",
                    UVC_VIDEO_STREAMING_INTERFACE,
                    r,
                    libusb_error_name(r)
            );
        }

        gVideoInterfaceClaimed =
                false;
    }

    if (gUsbHandle != nullptr) {

        LOGI(
                "libusb_close()"
        );

        libusb_close(
                gUsbHandle
        );

        gUsbHandle =
                nullptr;
    }

    if (gUsbContext != nullptr) {

        LOGI(
                "libusb_exit()"
        );

        libusb_exit(
                gUsbContext
        );

        gUsbContext =
                nullptr;
    }

    gSelectedUvcMode = {};
    gProbeResult = {};
    gCurrentAltSetting = 0;
}

}  // namespace


bool openFromAndroidFd(int fd)
{
    LOGI(
            "native USB open(fd=%d)",
            fd
    );

    if (fd < 0) {

        LOGE(
                "Invalid USB fd"
        );

        return false;
    }

    std::lock_guard<std::mutex> lock(
            gUsbMutex
    );

    closeLocked();

    const libusb_version* version =
            libusb_get_version();

    LOGI(
            "libusb version = "
            "%d.%d.%d.%d%s",
            version->major,
            version->minor,
            version->micro,
            version->nano,
            version->rc
            ? version->rc
            : ""
    );

    libusb_init_option options[2]{};

    options[0].option =
            LIBUSB_OPTION_NO_DEVICE_DISCOVERY;

    options[1].option =
            LIBUSB_OPTION_LOG_LEVEL;

    options[1].value.ival =
            LIBUSB_LOG_LEVEL_INFO;

    int r =
            libusb_init_context(
                    &gUsbContext,
                    options,
                    2
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "libusb_init_context failed: "
                "%d (%s)",
                r,
                libusb_error_name(r)
        );

        gUsbContext =
                nullptr;

        return false;
    }

    LOGI(
            "libusb_init_context OK"
    );

    r =
            libusb_wrap_sys_device(
                    gUsbContext,
                    static_cast<intptr_t>(fd),
                    &gUsbHandle
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "libusb_wrap_sys_device failed: "
                "%d (%s)",
                r,
                libusb_error_name(r)
        );

        closeLocked();

        return false;
    }

    if (gUsbHandle == nullptr) {

        LOGE(
                "libusb_wrap_sys_device "
                "returned null handle"
        );

        closeLocked();

        return false;
    }

    LOGI(
            "libusb_wrap_sys_device OK"
    );

    libusb_device* device =
            libusb_get_device(
                    gUsbHandle
            );

    if (device == nullptr) {

        LOGE(
                "libusb_get_device "
                "returned null"
        );

        closeLocked();

        return false;
    }

    // --------------------------------------------------------
    // USB negotiated link speed diagnostic.
    //
    // Unlike bcdUSB in the device descriptor, this reports the
    // speed at which this device is actually connected right now.
    // Useful for checking whether the capture device is running over the
    // P30T/T7250 host path at USB 2.0 High-Speed or USB 3.x
    // SuperSpeed. This is diagnostic-only and does not affect
    // streaming setup.
    // --------------------------------------------------------
    const int usbSpeed =
            libusb_get_device_speed(device);

    const char* usbSpeedName =
            "UNKNOWN";

    switch (usbSpeed) {
        case LIBUSB_SPEED_LOW:
            usbSpeedName = "LOW 1.5 Mbit/s (USB 1.x)";
            break;

        case LIBUSB_SPEED_FULL:
            usbSpeedName = "FULL 12 Mbit/s (USB 1.x)";
            break;

        case LIBUSB_SPEED_HIGH:
            usbSpeedName = "HIGH 480 Mbit/s (USB 2.0)";
            break;

        case LIBUSB_SPEED_SUPER:
            usbSpeedName = "SUPER 5 Gbit/s (USB 3.x)";
            break;

        case LIBUSB_SPEED_SUPER_PLUS:
            usbSpeedName = "SUPER_PLUS 10 Gbit/s or higher (USB 3.x)";
            break;

        case LIBUSB_SPEED_UNKNOWN:
        default:
            break;
    }

    LOGI(
            "USB negotiated link speed: enum=%d %s",
            usbSpeed,
            usbSpeedName
    );

    libusb_device_descriptor desc{};

    r =
            libusb_get_device_descriptor(
                    device,
                    &desc
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "libusb_get_device_descriptor "
                "failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        closeLocked();

        return false;
    }

    LOGI(
            "libusb Device Descriptor:"
    );

    LOGI(
            "VID=0x%04X PID=0x%04X",
            desc.idVendor,
            desc.idProduct
    );

    LOGI(
            "USB=%x.%02x Device=%x.%02x",
            (desc.bcdUSB >> 8),
            (desc.bcdUSB & 0xff),
            (desc.bcdDevice >> 8),
            (desc.bcdDevice & 0xff)
    );

    LOGI(
            "Class=%u SubClass=%u Protocol=%u",
            desc.bDeviceClass,
            desc.bDeviceSubClass,
            desc.bDeviceProtocol
    );

    LOGI(
            "Configurations=%u",
            desc.bNumConfigurations
    );

    const SupportedUsbDevice* supportedDevice =
            findSupportedUsbDevice(
                    desc.idVendor,
                    desc.idProduct
            );

    if (supportedDevice == nullptr) {

        LOGE(
                "Unexpected USB device VID=0x%04X PID=0x%04X",
                desc.idVendor,
                desc.idProduct
        );

        closeLocked();

        return false;
    }

    LOGI(
            "%s confirmed via libusb (VID=0x%04X PID=0x%04X)",
            supportedDevice->name,
            desc.idVendor,
            desc.idProduct
    );

    const bool isMs2130 =
            supportedDevice->productId == 0x2130;

    UvcTransportInfo transport{};

    if (!detectUvcTransport(
            device,
            transport)) {

        LOGE(
                "%s UVC descriptor layout check failed",
                supportedDevice->name
        );

        closeLocked();

        return false;
    }

    const bool useMjpegBulk =
            transport.type == UvcTransportType::Bulk;

    LOGI(
            "UVC backend dispatch: %s",
            useMjpegBulk ? "BULK backend" : "ISO backend"
    );

    const bool modeDiscovered =
            discoverTargetUvcMode(
                    device,
                    gSelectedUvcMode
            );

    if (!modeDiscovered) {

        LOGE(
                "Step 9 UVC mode discovery failed for %s",
                supportedDevice->name
        );

        closeLocked();

        return false;
    }

    // Step 15.5 preset is intentionally MS2109-only. Do not write
    // MS2109-tuned PU values into MS2130 during transport bring-up.
    if (!isMs2130) {

        if (!ms2109_pu_controls::applyFixedPreset(gUsbHandle)) {

            LOGE(
                    "Step 15.5 MS2109 PU control preset failed"
            );

            closeLocked();

            return false;
        }
    }
    else {

        LOGI(
                "MS2130: skipping MS2109 PU fixed preset"
        );
    }

    const int kernelDriverState =
            libusb_kernel_driver_active(
                    gUsbHandle,
                    UVC_VIDEO_STREAMING_INTERFACE
            );

    LOGI(
            "kernel driver active "
            "on IF %d = %d",
            UVC_VIDEO_STREAMING_INTERFACE,
            kernelDriverState
    );

    r =
            libusb_claim_interface(
                    gUsbHandle,
                    UVC_VIDEO_STREAMING_INTERFACE
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "libusb_claim_interface(%d) "
                "failed: %d (%s)",
                UVC_VIDEO_STREAMING_INTERFACE,
                r,
                libusb_error_name(r)
        );

        closeLocked();

        return false;
    }

    gVideoInterfaceClaimed =
            true;

    LOGI(
            "libusb_claim_interface(%d) OK",
            UVC_VIDEO_STREAMING_INTERFACE
    );

    LOGI(
            "Step 8 complete: IF1 claimed, "
            "alt setting unchanged"
    );

    LOGI(
            "Step 9 complete: target UVC "
            "mode discovered"
    );

    // --------------------------------------------------------
    // Step 10:
    //   PROBE / COMMIT negotiation.
    // --------------------------------------------------------

    const bool probeCommitOk =
            useMjpegBulk
            ? negotiateAndCommitMjpegBulk(
                    gSelectedUvcMode
            )
            : negotiateAndCommitStream(
                    gSelectedUvcMode
            );

    if (!probeCommitOk) {

        LOGE(
                "Step 10 PROBE/COMMIT failed for %s",
                supportedDevice->name
        );

        closeLocked();

        return false;
    }

    if (useMjpegBulk) {

        LOGI(
                "%s Step 11.1: starting 1280x720 MJPEG60 async BULK",
                supportedDevice->name
        );

        if (!startMjpegAsyncBulkTransport()) {

            LOGE(
                    "%s Step 11.1: async BULK MJPEG start/4:2:2 check failed",
                    supportedDevice->name
            );

            closeLocked();

            return false;
        }

        LOGI(
                "%s BULK MJPEG RUNNING: 1280x720 MJPEG60, JPEG 4:2:2 verified; "
                "1-second throughput/fps/latency stats enabled; "
                "JPEG decode timing enabled; shared planar renderer active",
                supportedDevice->name
        );

        return true;
    }

    // Only after a successful COMMIT do we reserve the
    // high-bandwidth VideoStreaming alternate setting.
    r =
            libusb_set_interface_alt_setting(
                    gUsbHandle,
                    UVC_VIDEO_STREAMING_INTERFACE,
                    transport.altSetting
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "Step 10: "
                "set IF%d alt %d failed: "
                "%d (%s)",
                UVC_VIDEO_STREAMING_INTERFACE,
                transport.altSetting,
                r,
                libusb_error_name(r)
        );

        closeLocked();

        return false;
    }

    gCurrentAltSetting =
            transport.altSetting;

    LOGI(
            "Step 10: IF%d alt=%d selected "
            "(EP 0x%02X, %u B/microframe)",
            UVC_VIDEO_STREAMING_INTERFACE,
            transport.altSetting,
            transport.endpointAddress,
            transport.capacityBytes
    );

    LOGI(
            "Step 10 complete: "
            "%ux%u %s %.3f fps committed, "
            "ISO alt=%u active",
            gSelectedUvcMode.width,
            gSelectedUvcMode.height,
            uvcVideoFormatName(
                    gSelectedUvcMode.format
            ),
            interval100nsToFps(gSelectedUvcMode.frameInterval100ns),
            transport.altSetting
    );

    // --------------------------------------------------------
    // Step 11:
    //   Start asynchronous isochronous MJPEG reception.
    //   uvc_stream owns USB transport only; uvc_mjpeg_decoder owns
    //   JPEG assembly/decode/latest-frame publication.
    // --------------------------------------------------------

    uvc_stream::Config streamConfig{};

    streamConfig.context = gUsbContext;
    streamConfig.handle = gUsbHandle;
    streamConfig.endpoint = transport.endpointAddress;
    streamConfig.isoPacketBytes =
            static_cast<int>(transport.capacityBytes);
    streamConfig.maxCompressedFrameBytes =
            gProbeResult.maxVideoFrameSize;

    if (!uvc_stream::start(
            streamConfig)) {

        LOGE(
                "Step 11: async ISO stream start failed"
        );

        closeLocked();

        return false;
    }

    LOGI(
            "Step 11 complete: async ISO MJPEG ring running; "
            "output=shared planar YCbCr422"
    );

    return true;
}


void close()
{
    LOGI(
            "nativeCloseUsb()"
    );

    std::lock_guard<std::mutex> lock(
            gUsbMutex
    );

    closeLocked();
}

}  // namespace uvc_device
