#include "uvc_device.h"
#include "uvc_stream.h"
#include "ms2109_pu_controls.h"
#include "ms2130_jpeg_decode_diag.h"

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

static constexpr int TARGET_UVC_WIDTH = 720;
static constexpr int TARGET_UVC_HEIGHT = 480;
static constexpr uint32_t TARGET_UVC_INTERVAL_100NS = 333333;
static constexpr uint32_t TARGET_YUYV_FRAME_BYTES =
        TARGET_UVC_WIDTH * TARGET_UVC_HEIGHT * 2;

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


struct UvcStreamMode {
    bool valid = false;

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


static bool isYuy2Guid(
        const unsigned char* guid,
        size_t length)
{
    if (guid == nullptr ||
        length < 16) {

        return false;
    }

    static constexpr unsigned char YUY2_GUID[16] = {
            0x59, 0x55, 0x59, 0x32,
            0x00, 0x00, 0x10, 0x00,
            0x80, 0x00, 0x00, 0xAA,
            0x00, 0x38, 0x9B, 0x71
    };

    return
            std::memcmp(
                    guid,
                    YUY2_GUID,
                    sizeof(YUY2_GUID)
            ) == 0;
}


static void logFormatGuid(
        const unsigned char* guid)
{
    if (guid == nullptr) {
        return;
    }

    char fourcc[5] = {
            static_cast<char>(guid[0]),
            static_cast<char>(guid[1]),
            static_cast<char>(guid[2]),
            static_cast<char>(guid[3]),
            '\0'
    };

    LOGI(
            "    Format GUID FOURCC='%s' "
            "GUID=%02X%02X%02X%02X-"
            "%02X%02X-%02X%02X-"
            "%02X%02X-"
            "%02X%02X%02X%02X%02X%02X",
            fourcc,
            guid[0], guid[1], guid[2], guid[3],
            guid[4], guid[5],
            guid[6], guid[7],
            guid[8], guid[9],
            guid[10], guid[11], guid[12],
            guid[13], guid[14], guid[15]
    );
}


static bool chooseDiscrete30FpsInterval(
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


static bool chooseContinuous30FpsInterval(
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

        // The current ISO backend is validated for the classic MS2109
        // 720x480 YUYV30 path: ALT3, 3072 bytes/microframe. Keep this
        // constraint explicit instead of silently selecting an unsupported
        // ISO layout.
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

        LOGE(
                "discoverTargetUvcMode: "
                "device is null"
        );

        return false;
    }

    libusb_config_descriptor* config =
            nullptr;

    int r =
            libusb_get_active_config_descriptor(
                    device,
                    &config
            );

    if (r != LIBUSB_SUCCESS) {

        LOGE(
                "Step 9: get active config "
                "failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        return false;
    }

    LOGI(
            "Step 9: scanning UVC "
            "VideoStreaming descriptors "
            "for %dx%d YUYV @ 30 fps",
            TARGET_UVC_WIDTH,
            TARGET_UVC_HEIGHT
    );

    bool foundVsAlt0 = false;
    bool currentFormatIsYuy2 = false;

    uint8_t currentFormatIndex = 0;
    uint8_t currentBitsPerPixel = 0;

    UvcStreamMode candidate{};

    for (uint8_t i = 0;
         i < config->bNumInterfaces;
         ++i) {

        const libusb_interface& iface =
                config->interface[i];

        for (int a = 0;
             a < iface.num_altsetting;
             ++a) {

            const libusb_interface_descriptor& alt =
                    iface.altsetting[a];

            if (alt.bInterfaceNumber !=
                    UVC_VIDEO_STREAMING_INTERFACE ||
                alt.bAlternateSetting != 0 ||
                alt.bInterfaceClass !=
                    LIBUSB_CLASS_VIDEO ||
                alt.bInterfaceSubClass != 2) {

                continue;
            }

            foundVsAlt0 = true;

            LOGI(
                    "Step 9: VS IF=%u ALT=%u "
                    "extraLength=%d",
                    alt.bInterfaceNumber,
                    alt.bAlternateSetting,
                    alt.extra_length
            );

            const unsigned char* p =
                    alt.extra;

            int remaining =
                    alt.extra_length;

            while (p != nullptr &&
                   remaining >= 3) {

                const uint8_t length =
                        p[0];

                const uint8_t descriptorType =
                        p[1];

                const uint8_t descriptorSubtype =
                        p[2];

                if (length < 3 ||
                    length > remaining) {

                    LOGE(
                            "Step 9: malformed "
                            "VS descriptor "
                            "length=%u remaining=%d",
                            length,
                            remaining
                    );

                    libusb_free_config_descriptor(
                            config
                    );

                    return false;
                }

                if (descriptorType == 0x24) {

                    switch (
                        descriptorSubtype) {

                        case 0x04:
                        {
                            if (length < 27) {

                                LOGE(
                                        "Step 9: short "
                                        "VS_FORMAT_UNCOMPRESSED "
                                        "len=%u",
                                        length
                                );

                                libusb_free_config_descriptor(
                                        config
                                );

                                return false;
                            }

                            currentFormatIndex =
                                    p[3];

                            const uint8_t frameDescriptorCount =
                                    p[4];

                            const unsigned char* guid =
                                    p + 5;

                            currentBitsPerPixel =
                                    p[21];

                            const uint8_t defaultFrameIndex =
                                    p[22];

                            currentFormatIsYuy2 =
                                    isYuy2Guid(
                                            guid,
                                            16
                                    );

                            LOGI(
                                    "  VS_FORMAT_UNCOMPRESSED: "
                                    "formatIndex=%u frames=%u "
                                    "bitsPerPixel=%u "
                                    "defaultFrameIndex=%u "
                                    "YUY2=%s",
                                    currentFormatIndex,
                                    frameDescriptorCount,
                                    currentBitsPerPixel,
                                    defaultFrameIndex,
                                    currentFormatIsYuy2
                                    ? "YES"
                                    : "NO"
                            );

                            logFormatGuid(
                                    guid
                            );

                            break;
                        }

                        case 0x05:
                        {
                            if (length < 26) {

                                LOGE(
                                        "Step 9: short "
                                        "VS_FRAME_UNCOMPRESSED "
                                        "len=%u",
                                        length
                                );

                                libusb_free_config_descriptor(
                                        config
                                );

                                return false;
                            }

                            const uint8_t frameIndex =
                                    p[3];

                            const uint16_t width =
                                    readLe16(
                                            p + 5
                                    );

                            const uint16_t height =
                                    readLe16(
                                            p + 7
                                    );

                            const uint32_t minBitRate =
                                    readLe32(
                                            p + 9
                                    );

                            const uint32_t maxBitRate =
                                    readLe32(
                                            p + 13
                                    );

                            const uint32_t maxFrameBufferSize =
                                    readLe32(
                                            p + 17
                                    );

                            const uint32_t defaultInterval =
                                    readLe32(
                                            p + 21
                                    );

                            const uint8_t intervalType =
                                    p[25];

                            LOGI(
                                    "    VS_FRAME_UNCOMPRESSED: "
                                    "formatIndex=%u "
                                    "frameIndex=%u "
                                    "%ux%u maxFrame=%u "
                                    "defaultInterval=%u "
                                    "(%.3f fps) "
                                    "intervalType=%u "
                                    "bitRate=%u..%u",
                                    currentFormatIndex,
                                    frameIndex,
                                    width,
                                    height,
                                    maxFrameBufferSize,
                                    defaultInterval,
                                    interval100nsToFps(
                                            defaultInterval
                                    ),
                                    intervalType,
                                    minBitRate,
                                    maxBitRate
                            );

                            uint32_t selectedInterval =
                                    0;

                            bool has30Fps =
                                    false;

                            if (intervalType == 0) {

                                has30Fps =
                                        chooseContinuous30FpsInterval(
                                                p,
                                                length,
                                                selectedInterval
                                        );
                            }
                            else {

                                has30Fps =
                                        chooseDiscrete30FpsInterval(
                                                p,
                                                length,
                                                intervalType,
                                                selectedInterval
                                        );
                            }

                            if (currentFormatIsYuy2 &&
                                width ==
                                    TARGET_UVC_WIDTH &&
                                height ==
                                    TARGET_UVC_HEIGHT &&
                                has30Fps) {

                                candidate.valid =
                                        true;

                                candidate.formatIndex =
                                        currentFormatIndex;

                                candidate.frameIndex =
                                        frameIndex;

                                candidate.width =
                                        width;

                                candidate.height =
                                        height;

                                candidate.frameInterval100ns =
                                        selectedInterval;

                                candidate.maxVideoFrameBufferSize =
                                        maxFrameBufferSize;

                                candidate.bitsPerPixel =
                                        currentBitsPerPixel;

                                LOGI(
                                        "    >>> TARGET MATCH: "
                                        "formatIndex=%u "
                                        "frameIndex=%u "
                                        "interval=%u "
                                        "(%.3f fps) "
                                        "maxFrame=%u",
                                        candidate.formatIndex,
                                        candidate.frameIndex,
                                        candidate.frameInterval100ns,
                                        interval100nsToFps(
                                                candidate.frameInterval100ns
                                        ),
                                        candidate.maxVideoFrameBufferSize
                                );
                            }

                            break;
                        }

                        case 0x06:
                        {
                            currentFormatIsYuy2 =
                                    false;

                            currentFormatIndex =
                                    length >= 4
                                    ? p[3]
                                    : 0;

                            currentBitsPerPixel =
                                    0;

                            LOGI(
                                    "  VS_FORMAT_MJPEG: "
                                    "formatIndex=%u",
                                    currentFormatIndex
                            );

                            break;
                        }

                        case 0x07:
                        {
                            if (length >= 9) {

                                LOGI(
                                        "    VS_FRAME_MJPEG: "
                                        "formatIndex=%u "
                                        "frameIndex=%u "
                                        "%ux%u",
                                        currentFormatIndex,
                                        p[3],
                                        readLe16(
                                                p + 5
                                        ),
                                        readLe16(
                                                p + 7
                                        )
                                );
                            }

                            break;
                        }

                        default:
                            break;
                    }
                }

                p += length;
                remaining -= length;
            }
        }
    }

    libusb_free_config_descriptor(
            config
    );

    if (!foundVsAlt0) {

        LOGE(
                "Step 9: VideoStreaming "
                "IF1 ALT0 not found"
        );

        return false;
    }

    if (!candidate.valid) {

        LOGE(
                "Step 9: target YUYV "
                "%dx%d @ 30 fps "
                "not advertised",
                TARGET_UVC_WIDTH,
                TARGET_UVC_HEIGHT
        );

        return false;
    }

    LOGI(
            "Step 9: expected YUYV "
            "frame bytes=%u, "
            "descriptor maxFrame=%u",
            TARGET_YUYV_FRAME_BYTES,
            candidate.maxVideoFrameBufferSize
    );

    if (candidate.maxVideoFrameBufferSize != 0 &&
        candidate.maxVideoFrameBufferSize <
                TARGET_YUYV_FRAME_BYTES) {

        LOGE(
                "Step 9: descriptor "
                "frame buffer is too small "
                "for 720x480 YUYV"
        );

        return false;
    }

    selectedMode =
            candidate;

    LOGI(
            "Step 9 selected mode: "
            "formatIndex=%u frameIndex=%u "
            "%ux%u YUYV "
            "interval=%u (%.3f fps) "
            "bitsPerPixel=%u maxFrame=%u",
            selectedMode.formatIndex,
            selectedMode.frameIndex,
            selectedMode.width,
            selectedMode.height,
            selectedMode.frameInterval100ns,
            interval100nsToFps(
                    selectedMode.frameInterval100ns
            ),
            selectedMode.bitsPerPixel,
            selectedMode.maxVideoFrameBufferSize
    );

    return true;
}


static bool chooseMjpeg60FpsInterval(
        const unsigned char* descriptor,
        size_t descriptorLength,
        uint8_t intervalType,
        uint32_t& selectedInterval)
{
    static constexpr size_t INTERVAL_OFFSET = 26;
    static constexpr size_t CONTINUOUS_LENGTH = 38;
    static constexpr uint32_t TARGET_INTERVAL_100NS = 166667;
    static constexpr uint32_t INTERVAL_TOLERANCE_100NS = 1000;

    selectedInterval = 0;

    if (descriptor == nullptr ||
        descriptorLength < INTERVAL_OFFSET) {

        return false;
    }

    if (intervalType == 0) {

        if (descriptorLength < CONTINUOUS_LENGTH) {
            return false;
        }

        const uint32_t minInterval =
                readLe32(descriptor + 26);

        const uint32_t maxInterval =
                readLe32(descriptor + 30);

        const uint32_t step =
                readLe32(descriptor + 34);

        LOGI(
                "      MJPEG continuous interval: "
                "min=%u (%.3f fps) max=%u (%.3f fps) step=%u",
                minInterval,
                interval100nsToFps(minInterval),
                maxInterval,
                interval100nsToFps(maxInterval),
                step
        );

        if (TARGET_INTERVAL_100NS < minInterval ||
            TARGET_INTERVAL_100NS > maxInterval) {

            return false;
        }

        uint32_t candidate =
                TARGET_INTERVAL_100NS;

        if (step != 0) {

            const uint32_t delta =
                    TARGET_INTERVAL_100NS - minInterval;

            const uint32_t steps =
                    (delta + (step / 2)) / step;

            candidate =
                    minInterval + steps * step;

            if (candidate > maxInterval) {
                candidate = maxInterval;
            }
        }

        const uint32_t diff =
                candidate > TARGET_INTERVAL_100NS
                ? candidate - TARGET_INTERVAL_100NS
                : TARGET_INTERVAL_100NS - candidate;

        if (diff > INTERVAL_TOLERANCE_100NS) {
            return false;
        }

        selectedInterval = candidate;
        return true;
    }

    if (descriptorLength <
            INTERVAL_OFFSET +
            static_cast<size_t>(intervalType) * 4) {

        return false;
    }

    uint32_t bestInterval = 0;
    uint32_t bestDiff =
            std::numeric_limits<uint32_t>::max();

    for (uint8_t i = 0;
         i < intervalType;
         ++i) {

        const uint32_t interval =
                readLe32(
                        descriptor +
                        INTERVAL_OFFSET +
                        static_cast<size_t>(i) * 4
                );

        LOGI(
                "      MJPEG interval[%u]=%u (%.3f fps)",
                i,
                interval,
                interval100nsToFps(interval)
        );

        const uint32_t diff =
                interval > TARGET_INTERVAL_100NS
                ? interval - TARGET_INTERVAL_100NS
                : TARGET_INTERVAL_100NS - interval;

        if (diff < bestDiff) {
            bestDiff = diff;
            bestInterval = interval;
        }
    }

    if (bestInterval == 0 ||
        bestDiff > INTERVAL_TOLERANCE_100NS) {

        return false;
    }

    selectedInterval = bestInterval;
    return true;
}


static bool discoverMs2130Mjpeg720p60Mode(
        libusb_device* device,
        UvcStreamMode& selectedMode)
{
    static constexpr uint16_t TARGET_WIDTH = 1280;
    static constexpr uint16_t TARGET_HEIGHT = 720;

    selectedMode = {};

    if (device == nullptr) {
        LOGE("MS2130 Step 9: device is null");
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
                "MS2130 Step 9: get active config failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        return false;
    }

    LOGI(
            "MS2130 Step 9: scanning for 1280x720 MJPEG @ 60 fps"
    );

    bool foundVsAlt0 = false;
    bool currentFormatIsMjpeg = false;
    uint8_t currentFormatIndex = 0;
    UvcStreamMode candidate{};

    for (uint8_t i = 0;
         i < config->bNumInterfaces;
         ++i) {

        const libusb_interface& iface =
                config->interface[i];

        for (int a = 0;
             a < iface.num_altsetting;
             ++a) {

            const libusb_interface_descriptor& alt =
                    iface.altsetting[a];

            if (alt.bInterfaceNumber != UVC_VIDEO_STREAMING_INTERFACE ||
                alt.bAlternateSetting != 0 ||
                alt.bInterfaceClass != LIBUSB_CLASS_VIDEO ||
                alt.bInterfaceSubClass != 2) {

                continue;
            }

            foundVsAlt0 = true;

            const unsigned char* d = alt.extra;
            int remaining = alt.extra_length;

            while (d != nullptr && remaining >= 3) {

                const uint8_t length = d[0];

                if (length < 3 || length > remaining) {

                    LOGE(
                            "MS2130 Step 9: malformed VS descriptor "
                            "length=%u remaining=%d",
                            length,
                            remaining
                    );

                    libusb_free_config_descriptor(config);
                    return false;
                }

                const uint8_t descriptorType = d[1];
                const uint8_t descriptorSubtype = d[2];

                if (descriptorType == 0x24) {

                    switch (descriptorSubtype) {

                        case 0x04:  // VS_FORMAT_UNCOMPRESSED
                            currentFormatIsMjpeg = false;
                            break;

                        case 0x06:  // VS_FORMAT_MJPEG
                        {
                            if (length < 11) {

                                LOGE(
                                        "MS2130 Step 9: short VS_FORMAT_MJPEG len=%u",
                                        length
                                );

                                libusb_free_config_descriptor(config);
                                return false;
                            }

                            currentFormatIsMjpeg = true;
                            currentFormatIndex = d[3];

                            LOGI(
                                    "  MS2130 VS_FORMAT_MJPEG: "
                                    "formatIndex=%u frames=%u defaultFrameIndex=%u",
                                    currentFormatIndex,
                                    d[4],
                                    d[6]
                            );

                            break;
                        }

                        case 0x07:  // VS_FRAME_MJPEG
                        {
                            if (!currentFormatIsMjpeg) {
                                break;
                            }

                            if (length < 26) {

                                LOGE(
                                        "MS2130 Step 9: short VS_FRAME_MJPEG len=%u",
                                        length
                                );

                                libusb_free_config_descriptor(config);
                                return false;
                            }

                            const uint8_t frameIndex = d[3];
                            const uint16_t width = readLe16(d + 5);
                            const uint16_t height = readLe16(d + 7);
                            const uint32_t maxFrameBufferSize = readLe32(d + 17);
                            const uint32_t defaultInterval = readLe32(d + 21);
                            const uint8_t intervalType = d[25];

                            LOGI(
                                    "    MS2130 VS_FRAME_MJPEG: "
                                    "formatIndex=%u frameIndex=%u %ux%u "
                                    "maxFrame=%u defaultInterval=%u (%.3f fps) "
                                    "intervalType=%u",
                                    currentFormatIndex,
                                    frameIndex,
                                    width,
                                    height,
                                    maxFrameBufferSize,
                                    defaultInterval,
                                    interval100nsToFps(defaultInterval),
                                    intervalType
                            );

                            if (width == TARGET_WIDTH &&
                                height == TARGET_HEIGHT) {

                                uint32_t selectedInterval = 0;

                                if (chooseMjpeg60FpsInterval(
                                        d,
                                        length,
                                        intervalType,
                                        selectedInterval)) {

                                    candidate.valid = true;
                                    candidate.formatIndex = currentFormatIndex;
                                    candidate.frameIndex = frameIndex;
                                    candidate.width = width;
                                    candidate.height = height;
                                    candidate.frameInterval100ns = selectedInterval;
                                    candidate.maxVideoFrameBufferSize =
                                            maxFrameBufferSize;
                                    candidate.bitsPerPixel = 0;

                                    LOGI(
                                            "    >>> MS2130 TARGET MATCH: "
                                            "formatIndex=%u frameIndex=%u "
                                            "1280x720 MJPEG interval=%u (%.3f fps) "
                                            "maxFrame=%u",
                                            candidate.formatIndex,
                                            candidate.frameIndex,
                                            candidate.frameInterval100ns,
                                            interval100nsToFps(
                                                    candidate.frameInterval100ns
                                            ),
                                            candidate.maxVideoFrameBufferSize
                                    );
                                }
                            }

                            break;
                        }

                        default:
                            break;
                    }
                }

                d += length;
                remaining -= length;
            }
        }
    }

    libusb_free_config_descriptor(config);

    if (!foundVsAlt0) {
        LOGE("MS2130 Step 9: VideoStreaming IF1 ALT0 not found");
        return false;
    }

    if (!candidate.valid) {

        LOGE(
                "MS2130 Step 9: target 1280x720 MJPEG @ 60 fps "
                "not advertised"
        );

        return false;
    }

    selectedMode = candidate;

    LOGI(
            "MS2130 Step 9 selected: format=%u frame=%u "
            "1280x720 MJPEG interval=%u (%.3f fps) maxFrame=%u",
            selectedMode.formatIndex,
            selectedMode.frameIndex,
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
    // sufficient for the explicit uncompressed format/frame request below.
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

    writeLe32(
            probe.data() + 18,
            mode.maxVideoFrameBufferSize != 0
            ? mode.maxVideoFrameBufferSize
            : TARGET_YUYV_FRAME_BYTES
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
                mode.maxVideoFrameBufferSize != 0
                ? mode.maxVideoFrameBufferSize
                : TARGET_YUYV_FRAME_BYTES;

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

    if (result.maxVideoFrameSize <
            TARGET_YUYV_FRAME_BYTES) {

        LOGE(
                "Step 10: negotiated "
                "maxFrame too small: %u < %u",
                result.maxVideoFrameSize,
                TARGET_YUYV_FRAME_BYTES
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
            "format=%u frame=%u "
            "interval=%u maxFrame=%u "
            "maxPayload=%u",
            result.formatIndex,
            result.frameIndex,
            result.frameInterval100ns,
            result.maxVideoFrameSize,
            result.maxPayloadTransferSize
    );

    gProbeResult =
            result;

    return true;
}


static bool negotiateAndCommitMs2130MjpegBulk(
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
                    "MS2130 GET_CUR(PROBE seed)"
            );

    if (gotSeed) {
        logProbe("MS2130 seed PROBE", probe);
    }
    else {

        LOGI(
                "MS2130 Step 10: GET_CUR(PROBE seed) unavailable; "
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
            "MS2130 Step 10: BULK seed maxPayload=%u; "
            "not applying MS2109 alt3/3072-byte clamp",
            seedPayload
    );

    logProbe("MS2130 requested PROBE", probe);

    if (!setStreamControl(
            UVC_VS_PROBE_CONTROL,
            probe,
            "MS2130 SET_CUR(PROBE)")) {

        return false;
    }

    std::vector<unsigned char> negotiated(controlLength, 0);

    if (!getStreamControl(
            UVC_GET_CUR,
            UVC_VS_PROBE_CONTROL,
            negotiated,
            "MS2130 GET_CUR(PROBE)")) {

        return false;
    }

    logProbe("MS2130 negotiated PROBE", negotiated);

    UvcProbeResult result = decodeProbe(negotiated);

    if (!result.valid) {
        return false;
    }

    if (result.formatIndex != mode.formatIndex ||
        result.frameIndex != mode.frameIndex) {

        LOGE(
                "MS2130 Step 10: device changed mode "
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
                "MS2130 Step 10: device changed frame interval "
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
                "MS2130 Step 10: maxFrame=0; "
                "using descriptor fallback=%u",
                result.maxVideoFrameSize
        );
    }

    if (result.maxPayloadTransferSize == 0) {

        LOGE(
                "MS2130 Step 10: negotiated maxPayload=0; "
                "refusing to invent a BULK payload size"
        );

        return false;
    }

    if (!setStreamControl(
            UVC_VS_COMMIT_CONTROL,
            negotiated,
            "MS2130 SET_CUR(COMMIT)")) {

        return false;
    }

    LOGI(
            "MS2130 Step 10: COMMIT OK "
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


static const char* jpegSubsamplingName(
        uint8_t ySampling,
        uint8_t cbSampling,
        uint8_t crSampling)
{
    const uint8_t yH =
            static_cast<uint8_t>(
                    (ySampling >> 4) & 0x0f
            );

    const uint8_t yV =
            static_cast<uint8_t>(
                    ySampling & 0x0f
            );

    const uint8_t cbH =
            static_cast<uint8_t>(
                    (cbSampling >> 4) & 0x0f
            );

    const uint8_t cbV =
            static_cast<uint8_t>(
                    cbSampling & 0x0f
            );

    const uint8_t crH =
            static_cast<uint8_t>(
                    (crSampling >> 4) & 0x0f
            );

    const uint8_t crV =
            static_cast<uint8_t>(
                    crSampling & 0x0f
            );

    if (yH == 2 && yV == 2 &&
        cbH == 1 && cbV == 1 &&
        crH == 1 && crV == 1) {

        return "YCbCr 4:2:0";
    }

    if (yH == 2 && yV == 1 &&
        cbH == 1 && cbV == 1 &&
        crH == 1 && crV == 1) {

        return "YCbCr 4:2:2";
    }

    if (yH == 1 && yV == 1 &&
        cbH == 1 && cbV == 1 &&
        crH == 1 && crV == 1) {

        return "YCbCr 4:4:4";
    }

    return "unknown/custom sampling";
}


static void logJpegSampling(
        const std::vector<unsigned char>& jpeg)
{
    if (jpeg.size() < 4 ||
        jpeg[0] != 0xff ||
        jpeg[1] != 0xd8) {

        LOGE(
                "MS2130 JPEG inspect: SOI not found"
        );

        return;
    }

    size_t offset = 2;

    while (offset + 4 <= jpeg.size()) {

        if (jpeg[offset] != 0xff) {
            ++offset;
            continue;
        }

        while (offset < jpeg.size() &&
               jpeg[offset] == 0xff) {
            ++offset;
        }

        if (offset >= jpeg.size()) {
            break;
        }

        const uint8_t marker =
                jpeg[offset++];

        if (marker == 0xd9 ||
            marker == 0xda) {
            break;
        }

        if (marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }

        if (offset + 2 > jpeg.size()) {
            break;
        }

        const uint16_t segmentLength =
                static_cast<uint16_t>(
                        (static_cast<uint16_t>(
                                jpeg[offset]
                         ) << 8) |
                        jpeg[offset + 1]
                );

        if (segmentLength < 2 ||
            offset + segmentLength > jpeg.size()) {

            LOGE(
                    "MS2130 JPEG inspect: malformed marker "
                    "0xFF%02X length=%u",
                    marker,
                    segmentLength
            );

            return;
        }

        const bool isSof =
                marker == 0xc0 || marker == 0xc1 ||
                marker == 0xc2 || marker == 0xc3 ||
                marker == 0xc5 || marker == 0xc6 ||
                marker == 0xc7 || marker == 0xc9 ||
                marker == 0xca || marker == 0xcb ||
                marker == 0xcd || marker == 0xce ||
                marker == 0xcf;

        if (isSof &&
            segmentLength >= 11) {

            const size_t data =
                    offset + 2;

            const uint16_t height =
                    static_cast<uint16_t>(
                            (static_cast<uint16_t>(
                                    jpeg[data + 1]
                             ) << 8) |
                            jpeg[data + 2]
                    );

            const uint16_t width =
                    static_cast<uint16_t>(
                            (static_cast<uint16_t>(
                                    jpeg[data + 3]
                             ) << 8) |
                            jpeg[data + 4]
                    );

            const uint8_t components =
                    jpeg[data + 5];

            LOGI(
                    "MS2130 JPEG SOF: marker=0xFF%02X "
                    "%ux%u precision=%u components=%u",
                    marker,
                    width,
                    height,
                    jpeg[data],
                    components
            );

            if (components >= 3 &&
                data + 6 +
                    static_cast<size_t>(components) * 3 <=
                        offset + segmentLength) {

                const uint8_t ySampling =
                        jpeg[data + 7];

                const uint8_t cbSampling =
                        jpeg[data + 10];

                const uint8_t crSampling =
                        jpeg[data + 13];

                LOGI(
                        "MS2130 JPEG sampling: "
                        "Y=%ux%u Cb=%ux%u Cr=%ux%u -> %s",
                        (ySampling >> 4) & 0x0f,
                        ySampling & 0x0f,
                        (cbSampling >> 4) & 0x0f,
                        cbSampling & 0x0f,
                        (crSampling >> 4) & 0x0f,
                        crSampling & 0x0f,
                        jpegSubsamplingName(
                                ySampling,
                                cbSampling,
                                crSampling
                        )
                );
            }

            return;
        }

        offset += segmentLength;
    }

    LOGE(
            "MS2130 JPEG inspect: SOF marker not found"
    );
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
// MS2130 Step 11.1 diagnostic:
//   continuous asynchronous BULK reception for 1280x720 MJPEG60.
//
// This is intentionally a diagnostics-only backend. It does not decode
// or publish JPEG frames to the renderer yet. The measurements here are:
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
// the MS2130 before the first USB payload is not visible to the host.
// ------------------------------------------------------------

static constexpr int MS2130_BULK_ASYNC_TRANSFER_COUNT = 8;
static constexpr int MS2130_BULK_EVENT_TIMEOUT_US = 50'000;

static constexpr uint8_t UVC_STREAM_FID = 0x01;
static constexpr uint8_t UVC_STREAM_EOF = 0x02;
static constexpr uint8_t UVC_STREAM_PTS = 0x04;
static constexpr uint8_t UVC_STREAM_SCR = 0x08;
static constexpr uint8_t UVC_STREAM_ERR = 0x40;

struct Ms2130BulkTransferSlot {
    libusb_transfer* transfer = nullptr;
    std::vector<unsigned char> buffer;
};

struct Ms2130BulkFrameState {
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

struct Ms2130BulkStats {
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

static std::atomic<bool> gMs2130BulkRunning{false};
static std::atomic<int> gMs2130BulkInflight{0};

static std::thread gMs2130BulkEventThread;
static std::vector<Ms2130BulkTransferSlot> gMs2130BulkTransfers;

static Ms2130BulkFrameState gMs2130BulkFrame;
static Ms2130BulkStats gMs2130BulkStats;

static uint32_t gMs2130BulkMaxFrame = 0;
static bool gMs2130BulkHeaderLogged = false;

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


static void resetMs2130BulkFrame()
{
    gMs2130BulkFrame = {};
}


static void maybeLogMs2130BulkStats(uint64_t nowNs)
{
    if (gMs2130BulkStats.lastLogNs == 0) {
        gMs2130BulkStats.lastLogNs = nowNs;
        return;
    }

    const uint64_t elapsedNs =
            nowNs - gMs2130BulkStats.lastLogNs;

    if (elapsedNs < 1'000'000'000ULL) {
        return;
    }

    const double seconds =
            static_cast<double>(elapsedNs) /
            1'000'000'000.0;

    const uint64_t frameDelta =
            gMs2130BulkStats.goodFrames -
            gMs2130BulkStats.lastGoodFrames;

    const uint64_t usbDelta =
            gMs2130BulkStats.usbBytes -
            gMs2130BulkStats.lastUsbBytes;

    const uint64_t videoDelta =
            gMs2130BulkStats.videoBytes -
            gMs2130BulkStats.lastVideoBytes;

    const uint64_t dropDelta =
            gMs2130BulkStats.droppedFrames -
            gMs2130BulkStats.lastDroppedFrames;

    const uint64_t malformedDelta =
            gMs2130BulkStats.malformedHeaders -
            gMs2130BulkStats.lastMalformedHeaders;

    const uint64_t uvcErrorDelta =
            gMs2130BulkStats.uvcErrors -
            gMs2130BulkStats.lastUvcErrors;

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
                    gMs2130BulkStats.windowFrameBytesSum
              ) / static_cast<double>(frameDelta)
            : 0.0;

    const size_t frameBytesMin =
            frameDelta != 0
            ? gMs2130BulkStats.windowFrameBytesMin
            : 0;

    const size_t frameBytesMax =
            frameDelta != 0
            ? gMs2130BulkStats.windowFrameBytesMax
            : 0;

    const double wireAvg =
            !gMs2130BulkStats.wireFrameMs.empty()
            ? [&]() {
                double sum = 0.0;
                for (double v : gMs2130BulkStats.wireFrameMs) {
                    sum += v;
                }
                return sum /
                        static_cast<double>(
                                gMs2130BulkStats.wireFrameMs.size()
                        );
              }()
            : 0.0;

    const double intervalAvg =
            !gMs2130BulkStats.frameIntervalMs.empty()
            ? [&]() {
                double sum = 0.0;
                for (double v : gMs2130BulkStats.frameIntervalMs) {
                    sum += v;
                }
                return sum /
                        static_cast<double>(
                                gMs2130BulkStats.frameIntervalMs.size()
                        );
              }()
            : 0.0;

    LOGI(
            "MS2130 BULK stats: fps=%.2f usb=%.2f MiB/s video=%.2f MiB/s "
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
            percentileSample(gMs2130BulkStats.wireFrameMs, 0.50),
            percentileSample(gMs2130BulkStats.wireFrameMs, 0.95),
            percentileSample(gMs2130BulkStats.wireFrameMs, 1.00),
            intervalAvg,
            percentileSample(gMs2130BulkStats.frameIntervalMs, 0.50),
            percentileSample(gMs2130BulkStats.frameIntervalMs, 0.95),
            percentileSample(gMs2130BulkStats.frameIntervalMs, 1.00),
            static_cast<unsigned long long>(dropDelta),
            static_cast<unsigned long long>(malformedDelta),
            static_cast<unsigned long long>(uvcErrorDelta),
            static_cast<unsigned long long>(gMs2130BulkStats.goodFrames),
            gMs2130BulkInflight.load(std::memory_order_acquire)
    );

    gMs2130BulkStats.lastLogNs = nowNs;
    gMs2130BulkStats.lastGoodFrames =
            gMs2130BulkStats.goodFrames;
    gMs2130BulkStats.lastUsbBytes =
            gMs2130BulkStats.usbBytes;
    gMs2130BulkStats.lastVideoBytes =
            gMs2130BulkStats.videoBytes;
    gMs2130BulkStats.lastDroppedFrames =
            gMs2130BulkStats.droppedFrames;
    gMs2130BulkStats.lastMalformedHeaders =
            gMs2130BulkStats.malformedHeaders;
    gMs2130BulkStats.lastUvcErrors =
            gMs2130BulkStats.uvcErrors;

    gMs2130BulkStats.windowFrameBytesSum = 0;
    gMs2130BulkStats.windowFrameBytesMin =
            std::numeric_limits<size_t>::max();
    gMs2130BulkStats.windowFrameBytesMax = 0;
    gMs2130BulkStats.wireFrameMs.clear();
    gMs2130BulkStats.frameIntervalMs.clear();
}


static void processMs2130BulkPayload(
        const unsigned char* data,
        int length,
        uint64_t callbackNs)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    // Decode diagnostic runs on its own latest-pending worker. Feeding it
    // here leaves the existing transport timing/statistics path unchanged.
    ms2130_jpeg_decode_diag::processPayload(
            data,
            length,
            callbackNs
    );

    gMs2130BulkStats.usbBytes +=
            static_cast<uint64_t>(length);

    if (length < 2) {
        ++gMs2130BulkStats.malformedHeaders;
        return;
    }

    const uint8_t headerLength = data[0];
    const uint8_t flags = data[1];

    if (headerLength < 2 ||
        static_cast<int>(headerLength) > length) {

        ++gMs2130BulkStats.malformedHeaders;
        gMs2130BulkFrame.bad = true;
        return;
    }

    if (!gMs2130BulkHeaderLogged) {
        LOGI(
                "MS2130 BULK header: length=%u flags=0x%02X "
                "PTS=%s SCR=%s",
                headerLength,
                flags,
                (flags & UVC_STREAM_PTS) ? "YES" : "NO",
                (flags & UVC_STREAM_SCR) ? "YES" : "NO"
        );

        if ((flags & UVC_STREAM_PTS) != 0 &&
            headerLength >= 6) {

            LOGI(
                    "MS2130 BULK header: first PTS=%u",
                    readLe32(data + 2)
            );
        }

        gMs2130BulkHeaderLogged = true;
    }

    ++gMs2130BulkStats.payloads;

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

    gMs2130BulkStats.videoBytes +=
            static_cast<uint64_t>(payloadBytes);

    if (payloadError) {
        ++gMs2130BulkStats.uvcErrors;

        if (gMs2130BulkFrame.active) {
            gMs2130BulkFrame.bad = true;
        }

        return;
    }

    if (gMs2130BulkFrame.active &&
        fid != gMs2130BulkFrame.fid) {

        ++gMs2130BulkStats.fidResyncs;
        ++gMs2130BulkStats.droppedFrames;
        resetMs2130BulkFrame();
    }

    size_t payloadOffset = 0;

    if (!gMs2130BulkFrame.active &&
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
            gMs2130BulkFrame.active = true;
            gMs2130BulkFrame.fid = fid;
            gMs2130BulkFrame.firstPayloadNs = callbackNs;

            if (!gBulkJpeg422Checked.load(std::memory_order_acquire)) {
                gBulkJpegHeaderProbe.clear();
            }
        }
    }

    if (!gMs2130BulkFrame.active) {
        return;
    }

    ++gMs2130BulkFrame.payloads;

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

        if (gMs2130BulkFrame.lastByteValid &&
            gMs2130BulkFrame.lastByte == 0xff &&
            value == 0xd9) {

            gMs2130BulkFrame.sawEoi = true;
        }

        gMs2130BulkFrame.lastByte = value;
        gMs2130BulkFrame.lastByteValid = true;
    }

    gMs2130BulkFrame.bytes +=
            payloadBytes - payloadOffset;

    if (gMs2130BulkFrame.bytes >
        static_cast<size_t>(gMs2130BulkMaxFrame)) {

        ++gMs2130BulkStats.overflowFrames;
        ++gMs2130BulkStats.droppedFrames;
        resetMs2130BulkFrame();
        return;
    }

    if (!eof) {
        return;
    }

    const bool frameOk =
            !gMs2130BulkFrame.bad &&
            gMs2130BulkFrame.sawEoi &&
            gMs2130BulkFrame.bytes >= 4;

    if (!frameOk) {
        ++gMs2130BulkStats.droppedFrames;
        resetMs2130BulkFrame();
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
                    gMs2130BulkFrame.firstPayloadNs
            ) /
            1'000'000.0;

    ++gMs2130BulkStats.goodFrames;

    gMs2130BulkStats.windowFrameBytesSum +=
            static_cast<uint64_t>(
                    gMs2130BulkFrame.bytes
            );

    gMs2130BulkStats.windowFrameBytesMin =
            std::min(
                    gMs2130BulkStats.windowFrameBytesMin,
                    gMs2130BulkFrame.bytes
            );

    gMs2130BulkStats.windowFrameBytesMax =
            std::max(
                    gMs2130BulkStats.windowFrameBytesMax,
                    gMs2130BulkFrame.bytes
            );

    gMs2130BulkStats.wireFrameMs.push_back(
            wireFrameMs
    );

    if (gMs2130BulkStats.lastGoodEofNs != 0) {

        gMs2130BulkStats.frameIntervalMs.push_back(
                static_cast<double>(
                        callbackNs -
                        gMs2130BulkStats.lastGoodEofNs
                ) /
                1'000'000.0
        );
    }

    gMs2130BulkStats.lastGoodEofNs =
            callbackNs;

    if (gMs2130BulkStats.goodFrames <= 5) {

        LOGI(
                "MS2130 BULK FRAME #%llu: bytes=%zu payloads=%u "
                "fid=%u wireFrameMs=%.3f",
                static_cast<unsigned long long>(
                        gMs2130BulkStats.goodFrames
                ),
                gMs2130BulkFrame.bytes,
                gMs2130BulkFrame.payloads,
                gMs2130BulkFrame.fid,
                wireFrameMs
        );
    }

    resetMs2130BulkFrame();
}


static void LIBUSB_CALL onMs2130BulkTransfer(
        libusb_transfer* transfer)
{
    if (transfer == nullptr) {
        return;
    }

    gMs2130BulkInflight.fetch_sub(
            1,
            std::memory_order_acq_rel
    );

    const uint64_t callbackNs =
            steadyNowNs();

    bool canResubmit =
            gMs2130BulkRunning.load(
                    std::memory_order_acquire
            );

    if (transfer->status ==
        LIBUSB_TRANSFER_COMPLETED) {

        ++gMs2130BulkStats.transfersCompleted;

        if (transfer->actual_length > 0) {
            processMs2130BulkPayload(
                    transfer->buffer,
                    transfer->actual_length,
                    callbackNs
            );
        }
    }
    else if (transfer->status !=
             LIBUSB_TRANSFER_CANCELLED) {

        ++gMs2130BulkStats.transfersFailed;

        LOGE(
                "MS2130 BULK async transfer failed: status=%d actual=%d",
                transfer->status,
                transfer->actual_length
        );

        if (transfer->status ==
                LIBUSB_TRANSFER_NO_DEVICE) {

            gMs2130BulkRunning.store(
                    false,
                    std::memory_order_release
            );
            canResubmit = false;
        }
    }

    maybeLogMs2130BulkStats(
            callbackNs
    );

    if (!canResubmit ||
        !gMs2130BulkRunning.load(
                std::memory_order_acquire
        )) {

        return;
    }

    gMs2130BulkInflight.fetch_add(
            1,
            std::memory_order_acq_rel
    );

    const int r =
            libusb_submit_transfer(
                    transfer
            );

    if (r != LIBUSB_SUCCESS) {

        gMs2130BulkInflight.fetch_sub(
                1,
                std::memory_order_acq_rel
        );

        ++gMs2130BulkStats.submitErrors;

        LOGE(
                "MS2130 BULK async resubmit failed: %d (%s)",
                r,
                libusb_error_name(r)
        );

        gMs2130BulkRunning.store(
                false,
                std::memory_order_release
        );
    }
}


static void ms2130BulkEventLoop()
{
    LOGI(
            "MS2130 BULK async event thread started"
    );

    bool cancellationIssued = false;

    while (gMs2130BulkRunning.load(
               std::memory_order_acquire
           ) ||
           gMs2130BulkInflight.load(
               std::memory_order_acquire
           ) > 0) {

        if (!gMs2130BulkRunning.load(
                std::memory_order_acquire
            ) &&
            !cancellationIssued) {

            for (Ms2130BulkTransferSlot& slot :
                 gMs2130BulkTransfers) {

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
                MS2130_BULK_EVENT_TIMEOUT_US;

        const int r =
                libusb_handle_events_timeout_completed(
                        gUsbContext,
                        &timeout,
                        nullptr
                );

        if (r != LIBUSB_SUCCESS &&
            r != LIBUSB_ERROR_INTERRUPTED) {

            LOGE(
                    "MS2130 BULK event loop failed: %d (%s)",
                    r,
                    libusb_error_name(r)
            );

            gMs2130BulkRunning.store(
                    false,
                    std::memory_order_release
            );
        }
    }

    LOGI(
            "MS2130 BULK async event thread stopped: "
            "frames=%llu transfers=%llu failed=%llu submitErr=%llu "
            "drop=%llu malformed=%llu uvcErr=%llu fidResync=%llu overflow=%llu",
            static_cast<unsigned long long>(gMs2130BulkStats.goodFrames),
            static_cast<unsigned long long>(gMs2130BulkStats.transfersCompleted),
            static_cast<unsigned long long>(gMs2130BulkStats.transfersFailed),
            static_cast<unsigned long long>(gMs2130BulkStats.submitErrors),
            static_cast<unsigned long long>(gMs2130BulkStats.droppedFrames),
            static_cast<unsigned long long>(gMs2130BulkStats.malformedHeaders),
            static_cast<unsigned long long>(gMs2130BulkStats.uvcErrors),
            static_cast<unsigned long long>(gMs2130BulkStats.fidResyncs),
            static_cast<unsigned long long>(gMs2130BulkStats.overflowFrames)
    );
}


static void stopMs2130AsyncBulkDiagnostic()
{
    const bool hadThread =
            gMs2130BulkEventThread.joinable();

    if (!hadThread &&
        gMs2130BulkTransfers.empty()) {

        return;
    }

    gMs2130BulkRunning.store(
            false,
            std::memory_order_release
    );

    for (Ms2130BulkTransferSlot& slot :
         gMs2130BulkTransfers) {

        if (slot.transfer != nullptr) {
            libusb_cancel_transfer(
                    slot.transfer
            );
        }
    }

    if (gMs2130BulkEventThread.joinable()) {
        gMs2130BulkEventThread.join();
    }

    // No more BULK callbacks can arrive after the event thread is drained.
    ms2130_jpeg_decode_diag::stop();

    for (Ms2130BulkTransferSlot& slot :
         gMs2130BulkTransfers) {

        if (slot.transfer != nullptr) {
            libusb_free_transfer(
                    slot.transfer
            );
            slot.transfer = nullptr;
        }
    }

    gMs2130BulkTransfers.clear();
    gMs2130BulkInflight.store(
            0,
            std::memory_order_release
    );

    resetMs2130BulkFrame();
    gMs2130BulkStats = {};
    gMs2130BulkMaxFrame = 0;
    gMs2130BulkHeaderLogged = false;

    gBulkJpeg422Checked.store(false, std::memory_order_release);
    gBulkJpeg422Ok.store(false, std::memory_order_release);
    gBulkJpegHeaderProbe.clear();
}


static bool startMs2130AsyncBulkDiagnostic()
{
    if (gUsbContext == nullptr ||
        gUsbHandle == nullptr) {

        LOGE(
                "MS2130 BULK async: USB context/handle is null"
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
                "MS2130 BULK async: invalid negotiated limits "
                "maxPayload=%u maxFrame=%u",
                maxPayload,
                maxFrame
        );

        return false;
    }

    stopMs2130AsyncBulkDiagnostic();

    if (!ms2130_jpeg_decode_diag::start(maxFrame)) {
        LOGE(
                "MS2130 BULK async: JPEG decode diagnostic start failed"
        );
        return false;
    }

    gMs2130BulkMaxFrame = maxFrame;
    gMs2130BulkHeaderLogged = false;

    gBulkJpeg422Checked.store(false, std::memory_order_release);
    gBulkJpeg422Ok.store(false, std::memory_order_release);
    gBulkJpegHeaderProbe.clear();
    gBulkJpegHeaderProbe.reserve(4096);

    resetMs2130BulkFrame();
    gMs2130BulkStats = {};
    gMs2130BulkStats.lastLogNs =
            steadyNowNs();

    gMs2130BulkStats.wireFrameMs.reserve(128);
    gMs2130BulkStats.frameIntervalMs.reserve(128);

    gMs2130BulkTransfers.resize(
            MS2130_BULK_ASYNC_TRANSFER_COUNT
    );

    for (Ms2130BulkTransferSlot& slot :
         gMs2130BulkTransfers) {

        slot.buffer.resize(
                maxPayload
        );

        slot.transfer =
                libusb_alloc_transfer(0);

        if (slot.transfer == nullptr) {

            LOGE(
                    "MS2130 BULK async: libusb_alloc_transfer failed"
            );

            stopMs2130AsyncBulkDiagnostic();
            return false;
        }

        libusb_fill_bulk_transfer(
                slot.transfer,
                gUsbHandle,
                UVC_VIDEO_ENDPOINT,
                slot.buffer.data(),
                static_cast<int>(slot.buffer.size()),
                onMs2130BulkTransfer,
                &slot,
                0
        );
    }

    gMs2130BulkRunning.store(
            true,
            std::memory_order_release
    );

    gMs2130BulkEventThread =
            std::thread(
                    ms2130BulkEventLoop
            );

    int submitted = 0;

    for (Ms2130BulkTransferSlot& slot :
         gMs2130BulkTransfers) {

        gMs2130BulkInflight.fetch_add(
                1,
                std::memory_order_acq_rel
        );

        const int r =
                libusb_submit_transfer(
                        slot.transfer
                );

        if (r != LIBUSB_SUCCESS) {

            gMs2130BulkInflight.fetch_sub(
                    1,
                    std::memory_order_acq_rel
            );

            LOGE(
                    "MS2130 BULK async: initial submit failed "
                    "slot=%d: %d (%s)",
                    submitted,
                    r,
                    libusb_error_name(r)
            );

            gMs2130BulkRunning.store(
                    false,
                    std::memory_order_release
            );

            stopMs2130AsyncBulkDiagnostic();
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
            MS2130_BULK_ASYNC_TRANSFER_COUNT,
            maxPayload *
                static_cast<uint32_t>(
                        MS2130_BULK_ASYNC_TRANSFER_COUNT
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
        stopMs2130AsyncBulkDiagnostic();
        return false;
    }

    if (!gBulkJpeg422Ok.load(std::memory_order_acquire)) {
        LOGE(
                "MJPEG BULK start: JPEG sampling is not YCbCr 4:2:2"
        );
        stopMs2130AsyncBulkDiagnostic();
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


static bool dumpMs2130OneMjpegFrame()
{
    static constexpr uint8_t UVC_STREAM_FID = 0x01;
    static constexpr uint8_t UVC_STREAM_EOF = 0x02;
    static constexpr uint8_t UVC_STREAM_ERR = 0x40;

    static constexpr int BULK_TIMEOUT_MS = 500;
    static constexpr int MAX_TIMEOUTS = 6;
    static constexpr auto CAPTURE_DEADLINE =
            std::chrono::seconds(5);

    static constexpr const char* DUMP_DIR =
            "/data/user/0/com.hev.uvcfieldmonitor/files";

    static constexpr const char* DUMP_PATH =
            "/data/user/0/com.hev.uvcfieldmonitor/files/"
            "ms2130_720p60_first.jpg";

    if (gUsbHandle == nullptr) {

        LOGE(
                "MS2130 BULK dump: USB handle is null"
        );

        return false;
    }

    const uint32_t maxPayload =
            gProbeResult.maxPayloadTransferSize;

    const uint32_t maxFrame =
            gProbeResult.maxVideoFrameSize;

    if (maxPayload < 2 ||
        maxFrame < 4) {

        LOGE(
                "MS2130 BULK dump: invalid negotiated limits "
                "maxPayload=%u maxFrame=%u",
                maxPayload,
                maxFrame
        );

        return false;
    }

    std::vector<unsigned char> transferBuffer(
            maxPayload
    );

    std::vector<unsigned char> jpeg;
    jpeg.reserve(
            std::min<uint32_t>(
                    maxFrame,
                    512u * 1024u
            )
    );

    bool collecting = false;
    uint8_t collectingFid = 0;

    uint64_t transfers = 0;
    uint64_t payloads = 0;
    uint64_t usbBytes = 0;
    uint64_t videoBytes = 0;
    uint64_t malformed = 0;
    uint64_t uvcErrors = 0;
    uint64_t fidResyncs = 0;
    int timeouts = 0;

    const auto started =
            std::chrono::steady_clock::now();

    LOGI(
            "MS2130 BULK dump: start endpoint=0x%02X "
            "transferBytes=%u maxFrame=%u timeout=%dms",
            UVC_VIDEO_ENDPOINT,
            maxPayload,
            maxFrame,
            BULK_TIMEOUT_MS
    );

    for (;;) {

        if (std::chrono::steady_clock::now() -
                started > CAPTURE_DEADLINE) {

            LOGE(
                    "MS2130 BULK dump: capture deadline exceeded"
            );

            return false;
        }

        int actualLength = 0;

        const int r =
                libusb_bulk_transfer(
                        gUsbHandle,
                        UVC_VIDEO_ENDPOINT,
                        transferBuffer.data(),
                        static_cast<int>(
                                transferBuffer.size()
                        ),
                        &actualLength,
                        BULK_TIMEOUT_MS
                );

        if (r == LIBUSB_ERROR_TIMEOUT) {

            ++timeouts;

            LOGI(
                    "MS2130 BULK dump: timeout %d/%d",
                    timeouts,
                    MAX_TIMEOUTS
            );

            if (timeouts >= MAX_TIMEOUTS) {
                return false;
            }

            continue;
        }

        if (r != LIBUSB_SUCCESS) {

            LOGE(
                    "MS2130 BULK dump: libusb_bulk_transfer "
                    "failed: %d (%s)",
                    r,
                    libusb_error_name(r)
            );

            return false;
        }

        timeouts = 0;
        ++transfers;

        if (actualLength <= 0) {
            continue;
        }

        usbBytes +=
                static_cast<uint64_t>(
                        actualLength
                );

        if (actualLength < 2) {

            ++malformed;
            continue;
        }

        const uint8_t headerLength =
                transferBuffer[0];

        const uint8_t flags =
                transferBuffer[1];

        if (headerLength < 2 ||
            headerLength > actualLength) {

            ++malformed;

            LOGE(
                    "MS2130 BULK dump: malformed UVC header "
                    "headerLength=%u actual=%d",
                    headerLength,
                    actualLength
            );

            collecting = false;
            jpeg.clear();
            continue;
        }

        ++payloads;

        const uint8_t fid =
                (flags & UVC_STREAM_FID)
                ? 1
                : 0;

        const bool eof =
                (flags & UVC_STREAM_EOF) != 0;

        const bool payloadError =
                (flags & UVC_STREAM_ERR) != 0;

        const unsigned char* payload =
                transferBuffer.data() +
                headerLength;

        const size_t payloadBytes =
                static_cast<size_t>(
                        actualLength -
                        headerLength
                );

        videoBytes +=
                static_cast<uint64_t>(
                        payloadBytes
                );

        if (payloadError) {

            ++uvcErrors;
            collecting = false;
            jpeg.clear();
            continue;
        }

        if (collecting &&
            fid != collectingFid) {

            ++fidResyncs;
            collecting = false;
            jpeg.clear();
        }

        size_t payloadOffset = 0;

        if (!collecting) {

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

            if (!foundSoi) {
                continue;
            }

            collecting = true;
            collectingFid = fid;
            jpeg.clear();

            LOGI(
                    "MS2130 BULK dump: JPEG SOI synchronized "
                    "transfer=%llu payload=%llu fid=%u offset=%zu",
                    static_cast<unsigned long long>(
                            transfers
                    ),
                    static_cast<unsigned long long>(
                            payloads
                    ),
                    fid,
                    payloadOffset
            );
        }

        if (payloadOffset < payloadBytes) {

            const size_t appendBytes =
                    payloadBytes -
                    payloadOffset;

            if (jpeg.size() + appendBytes >
                static_cast<size_t>(maxFrame)) {

                LOGE(
                        "MS2130 BULK dump: frame overflow "
                        "%zu + %zu > %u",
                        jpeg.size(),
                        appendBytes,
                        maxFrame
                );

                collecting = false;
                jpeg.clear();
                continue;
            }

            jpeg.insert(
                    jpeg.end(),
                    payload + payloadOffset,
                    payload + payloadBytes
            );
        }

        if (!eof ||
            !collecting) {
            continue;
        }

        size_t eoiEnd = 0;

        for (size_t i = jpeg.size();
             i >= 2;
             --i) {

            if (jpeg[i - 2] == 0xff &&
                jpeg[i - 1] == 0xd9) {

                eoiEnd = i;
                break;
            }
        }

        if (jpeg.size() < 4 ||
            jpeg[0] != 0xff ||
            jpeg[1] != 0xd8 ||
            eoiEnd == 0) {

            LOGE(
                    "MS2130 BULK dump: EOF frame is not a complete JPEG "
                    "bytes=%zu SOI=%s EOI=%s",
                    jpeg.size(),
                    (jpeg.size() >= 2 &&
                     jpeg[0] == 0xff &&
                     jpeg[1] == 0xd8)
                    ? "YES"
                    : "NO",
                    eoiEnd != 0
                    ? "YES"
                    : "NO"
            );

            collecting = false;
            jpeg.clear();
            continue;
        }

        if (eoiEnd != jpeg.size()) {

            LOGI(
                    "MS2130 BULK dump: trimming %zu bytes after JPEG EOI",
                    jpeg.size() - eoiEnd
            );

            jpeg.resize(
                    eoiEnd
            );
        }

        if (::mkdir(
                DUMP_DIR,
                0700
            ) != 0 &&
            errno != EEXIST) {

            LOGE(
                    "MS2130 BULK dump: mkdir failed errno=%d",
                    errno
            );

            return false;
        }

        FILE* file =
                std::fopen(
                        DUMP_PATH,
                        "wb"
                );

        if (file == nullptr) {

            LOGE(
                    "MS2130 BULK dump: fopen failed path=%s errno=%d",
                    DUMP_PATH,
                    errno
            );

            return false;
        }

        const size_t written =
                std::fwrite(
                        jpeg.data(),
                        1,
                        jpeg.size(),
                        file
                );

        const int closeResult =
                std::fclose(
                        file
                );

        if (written != jpeg.size() ||
            closeResult != 0) {

            LOGE(
                    "MS2130 BULK dump: file write failed "
                    "written=%zu expected=%zu fclose=%d errno=%d",
                    written,
                    jpeg.size(),
                    closeResult,
                    errno
            );

            return false;
        }

        const double elapsedMs =
                std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        started
                ).count();

        LOGI(
                "MS2130 BULK DUMP PASS: bytes=%zu fid=%u "
                "transfers=%llu payloads=%llu usbBytes=%llu "
                "videoBytes=%llu malformed=%llu uvcErr=%llu "
                "fidResync=%llu elapsed=%.3f ms path=%s",
                jpeg.size(),
                collectingFid,
                static_cast<unsigned long long>(transfers),
                static_cast<unsigned long long>(payloads),
                static_cast<unsigned long long>(usbBytes),
                static_cast<unsigned long long>(videoBytes),
                static_cast<unsigned long long>(malformed),
                static_cast<unsigned long long>(uvcErrors),
                static_cast<unsigned long long>(fidResyncs),
                elapsedMs,
                DUMP_PATH
        );

        logJpegSampling(
                jpeg
        );

        return true;
    }
}


static void closeLocked()
{
    // Stop and drain MS2130 asynchronous BULK transfers before
    // releasing the shared libusb handle/context.
    stopMs2130AsyncBulkDiagnostic();

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
    // Useful for checking whether an MS2130 is running over the
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
            useMjpegBulk
            ? discoverMs2130Mjpeg720p60Mode(
                    device,
                    gSelectedUvcMode
            )
            : discoverTargetUvcMode(
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
                "MS2130 test: skipping MS2109 PU fixed preset"
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
            ? negotiateAndCommitMs2130MjpegBulk(
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

        if (!startMs2130AsyncBulkDiagnostic()) {

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
                "JPEG decode timing enabled; render intentionally not started",
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
            "720x480 YUYV30 committed, "
            "ISO alt=%u active",
            transport.altSetting
    );

    // --------------------------------------------------------
    // Step 11:
    //   Start asynchronous isochronous reception.
    //
    //   This stage only validates UVC payload parsing and
    //   reconstruction of complete 691200-byte YUYV frames.
    //   Rendering is intentionally not connected yet.
    // --------------------------------------------------------

    uvc_stream::Config streamConfig{};

    streamConfig.context =
            gUsbContext;

    streamConfig.handle =
            gUsbHandle;

    streamConfig.endpoint =
            transport.endpointAddress;

    streamConfig.isoPacketBytes =
            static_cast<int>(
                    transport.capacityBytes
            );

    streamConfig.expectedFrameBytes =
            TARGET_YUYV_FRAME_BYTES;

    if (!uvc_stream::start(
            streamConfig)) {

        LOGE(
                "Step 11: async ISO stream start failed"
        );

        closeLocked();

        return false;
    }

    LOGI(
            "Step 11 complete: "
            "async ISO ring running; "
            "frame reconstruction enabled"
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
