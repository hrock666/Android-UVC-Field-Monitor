#include "ms2109_pu_controls.h"
#include "calibration_profile.h"

#include <android/log.h>
#include <libusb.h>

#include <cstdint>

#define LOG_TAG "UvcFieldMonitor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ms2109_pu_controls {
namespace {

static constexpr unsigned int CONTROL_TIMEOUT_MS = 1000;
static constexpr uint8_t USB_DT_CS_INTERFACE = 0x24;
static constexpr uint8_t UVC_VC_PROCESSING_UNIT = 0x05;

static constexpr uint8_t UVC_SET_CUR = 0x01;
static constexpr uint8_t UVC_GET_CUR = 0x81;
static constexpr uint8_t UVC_GET_MIN = 0x82;
static constexpr uint8_t UVC_GET_MAX = 0x83;
static constexpr uint8_t UVC_GET_RES = 0x84;
static constexpr uint8_t UVC_GET_DEF = 0x87;

static constexpr uint8_t UVC_PU_BRIGHTNESS_CONTROL = 0x02;
static constexpr uint8_t UVC_PU_CONTRAST_CONTROL = 0x03;
static constexpr uint8_t UVC_PU_HUE_CONTROL = 0x06;
static constexpr uint8_t UVC_PU_SATURATION_CONTROL = 0x07;

static constexpr uint8_t UVC_REQ_OUT =
        LIBUSB_ENDPOINT_OUT |
        LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE;

static constexpr uint8_t UVC_REQ_IN =
        LIBUSB_ENDPOINT_IN |
        LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE;

struct ProcessingUnitLocation {
    bool valid = false;
    uint8_t interfaceNumber = 0;
    uint8_t unitId = 0;
};

static uint16_t readLe16(const unsigned char* p)
{
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

static void writeLe16(unsigned char* p, uint16_t value)
{
    p[0] = static_cast<unsigned char>(value & 0xffu);
    p[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
}

static bool findProcessingUnit(
        libusb_device* device,
        ProcessingUnitLocation& result)
{
    result = {};

    if (device == nullptr) {
        LOGE("Step 15.5: libusb device is null");
        return false;
    }

    libusb_config_descriptor* config = nullptr;
    const int r = libusb_get_active_config_descriptor(device, &config);

    if (r != LIBUSB_SUCCESS) {
        LOGE("Step 15.5: get active config failed: %d (%s)",
             r, libusb_error_name(r));
        return false;
    }

    for (uint8_t i = 0; i < config->bNumInterfaces && !result.valid; ++i) {
        const libusb_interface& iface = config->interface[i];

        for (int a = 0; a < iface.num_altsetting && !result.valid; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];

            if (alt.bAlternateSetting != 0 ||
                alt.bInterfaceClass != LIBUSB_CLASS_VIDEO ||
                alt.bInterfaceSubClass != 1) {
                continue;
            }

            const unsigned char* p = alt.extra;
            int remaining = alt.extra_length;

            while (p != nullptr && remaining >= 3) {
                const uint8_t length = p[0];
                if (length < 3 || length > remaining) {
                    LOGE("Step 15.5: malformed VC descriptor IF=%u len=%u remaining=%d",
                         alt.bInterfaceNumber, length, remaining);
                    break;
                }

                const uint8_t descriptorType = p[1];
                const uint8_t descriptorSubtype = p[2];

                if (descriptorType == USB_DT_CS_INTERFACE &&
                    descriptorSubtype == UVC_VC_PROCESSING_UNIT &&
                    length >= 5) {
                    result.valid = true;
                    result.interfaceNumber = alt.bInterfaceNumber;
                    result.unitId = p[3];

                    LOGI("Step 15.5: Processing Unit discovered VC_IF=%u unitId=%u descriptorLen=%u",
                         result.interfaceNumber, result.unitId, length);
                    break;
                }

                p += length;
                remaining -= length;
            }
        }
    }

    libusb_free_config_descriptor(config);

    if (!result.valid) {
        LOGE("Step 15.5: UVC Processing Unit descriptor not found");
        return false;
    }

    return true;
}

static int puTransfer(
        libusb_device_handle* handle,
        const ProcessingUnitLocation& pu,
        bool deviceToHost,
        uint8_t request,
        uint8_t controlSelector,
        unsigned char* data,
        uint16_t length)
{
    const uint16_t wValue =
            static_cast<uint16_t>(controlSelector) << 8;
    const uint16_t wIndex =
            (static_cast<uint16_t>(pu.unitId) << 8) |
            pu.interfaceNumber;

    return libusb_control_transfer(
            handle,
            deviceToHost ? UVC_REQ_IN : UVC_REQ_OUT,
            request,
            wValue,
            wIndex,
            data,
            length,
            CONTROL_TIMEOUT_MS);
}

static bool getRaw16(
        libusb_device_handle* handle,
        const ProcessingUnitLocation& pu,
        uint8_t request,
        uint8_t selector,
        uint16_t& value)
{
    unsigned char data[2]{};
    const int r = puTransfer(
            handle, pu, true, request, selector, data, sizeof(data));

    if (r != static_cast<int>(sizeof(data))) {
        return false;
    }

    value = readLe16(data);
    return true;
}

static void logRangeBestEffort(
        libusb_device_handle* handle,
        const ProcessingUnitLocation& pu,
        const char* name,
        uint8_t selector,
        bool signedValue)
{
    uint16_t minRaw = 0;
    uint16_t maxRaw = 0;
    uint16_t resRaw = 0;
    uint16_t defRaw = 0;

    const bool haveMin = getRaw16(handle, pu, UVC_GET_MIN, selector, minRaw);
    const bool haveMax = getRaw16(handle, pu, UVC_GET_MAX, selector, maxRaw);
    const bool haveRes = getRaw16(handle, pu, UVC_GET_RES, selector, resRaw);
    const bool haveDef = getRaw16(handle, pu, UVC_GET_DEF, selector, defRaw);

    if (!(haveMin || haveMax || haveRes || haveDef)) {
        LOGI("Step 15.5: PU %s range query unavailable (non-fatal)", name);
        return;
    }

    if (signedValue) {
        LOGI("Step 15.5: PU %s range min=%d%s max=%d%s res=%d%s def=%d%s",
             name,
             static_cast<int16_t>(minRaw), haveMin ? "" : "?",
             static_cast<int16_t>(maxRaw), haveMax ? "" : "?",
             static_cast<int16_t>(resRaw), haveRes ? "" : "?",
             static_cast<int16_t>(defRaw), haveDef ? "" : "?");
    } else {
        LOGI("Step 15.5: PU %s range min=%u%s max=%u%s res=%u%s def=%u%s",
             name,
             minRaw, haveMin ? "" : "?",
             maxRaw, haveMax ? "" : "?",
             resRaw, haveRes ? "" : "?",
             defRaw, haveDef ? "" : "?");
    }
}

static bool setAndVerify16(
        libusb_device_handle* handle,
        const ProcessingUnitLocation& pu,
        const char* name,
        uint8_t selector,
        uint16_t requestedRaw,
        bool signedValue)
{
    uint16_t beforeRaw = 0;
    const bool haveBefore =
            getRaw16(handle, pu, UVC_GET_CUR, selector, beforeRaw);

    logRangeBestEffort(handle, pu, name, selector, signedValue);

    unsigned char data[2]{};
    writeLe16(data, requestedRaw);

    const int setResult = puTransfer(
            handle, pu, false, UVC_SET_CUR, selector, data, sizeof(data));

    if (setResult != static_cast<int>(sizeof(data))) {
        LOGE("Step 15.5: PU SET_CUR %s failed: %d (%s)",
             name,
             setResult,
             setResult < 0 ? libusb_error_name(setResult) : "short transfer");
        return false;
    }

    uint16_t actualRaw = 0;
    if (!getRaw16(handle, pu, UVC_GET_CUR, selector, actualRaw)) {
        LOGE("Step 15.5: PU GET_CUR verify %s failed", name);
        return false;
    }

    if (actualRaw != requestedRaw) {
        if (signedValue) {
            LOGE("Step 15.5: PU verify %s mismatch requested=%d actual=%d",
                 name,
                 static_cast<int16_t>(requestedRaw),
                 static_cast<int16_t>(actualRaw));
        } else {
            LOGE("Step 15.5: PU verify %s mismatch requested=%u actual=%u",
                 name, requestedRaw, actualRaw);
        }
        return false;
    }

    if (signedValue) {
        if (haveBefore) {
            LOGI("Step 15.5: PU %s %d -> %d verified",
                 name,
                 static_cast<int16_t>(beforeRaw),
                 static_cast<int16_t>(actualRaw));
        } else {
            LOGI("Step 15.5: PU %s requested=%d -> %d verified (initial GET_CUR unavailable)",
                 name,
                 static_cast<int16_t>(requestedRaw),
                 static_cast<int16_t>(actualRaw));
        }
    } else {
        if (haveBefore) {
            LOGI("Step 15.5: PU %s %u -> %u verified",
                 name, beforeRaw, actualRaw);
        } else {
            LOGI("Step 15.5: PU %s requested=%u -> %u verified (initial GET_CUR unavailable)",
                 name, requestedRaw, actualRaw);
        }
    }

    return true;
}

}  // namespace

bool applyFixedPreset(libusb_device_handle* handle)
{
    if (handle == nullptr) {
        LOGE("Step 15.5: applyFixedPreset handle is null");
        return false;
    }

    libusb_device* device = libusb_get_device(handle);
    ProcessingUnitLocation pu{};

    // TEST PATCH: PU configuration is best-effort. It must not prevent
    // VideoStreaming from starting.
    if (!findProcessingUnit(device, pu)) {
        LOGW("Step 15.5 TEST: PU descriptor unavailable; skipping preset (non-fatal)");
        return true;
    }

    const calibration_profile::Profile profile = calibration_profile::snapshot();
    if (!profile.enabled) {
        LOGI("Phase 8: no matching calibration profile; PU unchanged");
        return true;
    }

    // PU requests are class-specific control transfers on EP0. Do not claim
    // VC IF0 here: Android's USB stack may already own that interface.
    const int kernelState =
            libusb_kernel_driver_active(handle, pu.interfaceNumber);
    LOGI("Step 15.5 TEST: VC_IF=%u unitId=%u kernelDriver=%d; "
         "trying EP0 without claiming VC interface",
         pu.interfaceNumber, pu.unitId, kernelState);

    // Execute all controls independently so one failure does not suppress the
    // remaining requests.
    const bool brightnessOk = setAndVerify16(
            handle, pu, "brightness", UVC_PU_BRIGHTNESS_CONTROL,
            static_cast<uint16_t>(static_cast<int16_t>(profile.pu[0])), true);

    const bool contrastOk = setAndVerify16(
            handle, pu, "contrast", UVC_PU_CONTRAST_CONTROL,
            static_cast<uint16_t>(profile.pu[1]), false);

    const bool saturationOk = setAndVerify16(
            handle, pu, "saturation", UVC_PU_SATURATION_CONTROL,
            static_cast<uint16_t>(profile.pu[2]), false);

    const bool hueOk = setAndVerify16(
            handle, pu, "hue", UVC_PU_HUE_CONTROL,
            static_cast<uint16_t>(static_cast<int16_t>(profile.pu[3])), true);

    const bool presetApplied =
            brightnessOk && contrastOk && saturationOk && hueOk;

    if (presetApplied) {
        LOGI("Phase 8: profile PU applied brightness=%d contrast=%d saturation=%d hue=%d",
             profile.pu[0], profile.pu[1], profile.pu[2], profile.pu[3]);
    } else {
        LOGW("Step 15.5 TEST: PU preset incomplete "
             "(brightness=%s contrast=%s saturation=%s hue=%s); "
             "continuing to VideoStreaming",
             brightnessOk ? "OK" : "FAIL",
             contrastOk ? "OK" : "FAIL",
             saturationOk ? "OK" : "FAIL",
             hueOk ? "OK" : "FAIL");
    }

    // Deliberately non-fatal for this isolation test.
    return true;
}

}  // namespace ms2109_pu_controls
