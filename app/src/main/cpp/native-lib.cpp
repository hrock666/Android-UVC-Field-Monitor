#include <jni.h>

#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES3/gl31.h>
#include <GLES3/gl3ext.h>

#include "uvc_device.h"
#include "uvc_stream.h"
#include "uvc_mjpeg_decoder.h"
#include "field_monitor_layout.h"
#include "monitor_ui_controller.h"
#include "monitor_ui_geometry.h"
#include "scope_ui.h"
#include "scope_gpu.h"
#include "diagnostic_config.h"
#include "calibration_profile.h"

#include <algorithm>
#include <numeric>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <time.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>


// ------------------------------------------------------------
// GL_EXT_buffer_storage
//
// NDK GLES3 headers may not expose these declarations even when
// the runtime driver supports GL_EXT_buffer_storage.
// ------------------------------------------------------------

#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#endif

#ifndef GL_MAP_COHERENT_BIT_EXT
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#endif

using PFNGLBUFFERSTORAGEEXTPROC_LOCAL =
        void (GL_APIENTRYP)(
                GLenum target,
                GLsizeiptr size,
                const void* data,
                GLbitfield flags
        );


using PFNEGLGETNEXTFRAMEIDANDROIDPROC_LOCAL =
        EGLBoolean (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLSurface surface,
                uint64_t* frameId
        );

using PFNEGLGETFRAMETIMESTAMPSANDROIDPROC_LOCAL =
        EGLBoolean (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLSurface surface,
                uint64_t frameId,
                EGLint numTimestamps,
                const EGLint* timestamps,
                int64_t* values
        );

using PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC_LOCAL =
        EGLBoolean (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLSurface surface,
                EGLint timestamp
        );


using PFNEGLGETCOMPOSITORTIMINGANDROIDPROC_LOCAL =
        EGLBoolean (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLSurface surface,
                EGLint numTimestamps,
                const EGLint* names,
                int64_t* values
        );

using PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC_LOCAL =
        EGLBoolean (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLSurface surface,
                EGLint name
        );


using PFNEGLPRESENTATIONTIMEANDROIDPROC_LOCAL =
        EGLBoolean (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLSurface surface,
                int64_t timeNs
        );


using PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC_LOCAL =
        EGLClientBuffer (EGLAPIENTRYP)(
                const AHardwareBuffer* buffer
        );

using PFNEGLCREATEIMAGEKHRPROC_LOCAL =
        EGLImageKHR (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLContext ctx,
                EGLenum target,
                EGLClientBuffer buffer,
                const EGLint* attribList
        );

using PFNEGLDESTROYIMAGEKHRPROC_LOCAL =
        EGLBoolean (EGLAPIENTRYP)(
                EGLDisplay dpy,
                EGLImageKHR image
        );

using PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_LOCAL =
        void (GL_APIENTRYP)(
                GLenum target,
                void* image
        );


#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif


// ------------------------------------------------------------
// API 29 weak symbol shim
//
// NDK 28 rejects the public API 29 declaration when this translation
// unit targets android26. We therefore reference the platform symbol
// through a separate weak alias instead of calling that declaration.
//
// Android 29+ resolves the weak alias. Older Android leaves it null.
// ------------------------------------------------------------

extern "C"
int AHardwareBuffer_isSupported_weak(
        const AHardwareBuffer_Desc* desc
) __asm__("AHardwareBuffer_isSupported")
  __attribute__((weak));


static bool ahbIsSupportedCompat(
        const AHardwareBuffer_Desc* desc)
{
    if (desc == nullptr) {
        return false;
    }

    if (android_get_device_api_level() < 29) {
        return false;
    }

    if (AHardwareBuffer_isSupported_weak == nullptr) {
        return false;
    }

    return
            AHardwareBuffer_isSupported_weak(
                    desc
            ) == 1;
}


// ------------------------------------------------------------
// Step 15D SurfaceControl weak-symbol shim.
//
// This translation unit still targets android26 so the API 29+
// SurfaceControl declarations cannot be referenced directly.  Keep the
// old-device build/runtime path intact and resolve the public libandroid
// symbols as weak aliases on Android 10+.
// ------------------------------------------------------------

struct ASurfaceControl;
struct ASurfaceTransaction;

extern "C"
ASurfaceControl* ASurfaceControl_createFromWindow_weak(
        ANativeWindow* parent,
        const char* debugName
) __asm__("ASurfaceControl_createFromWindow")
  __attribute__((weak));

extern "C"
void ASurfaceControl_release_weak(
        ASurfaceControl* surfaceControl
) __asm__("ASurfaceControl_release")
  __attribute__((weak));

extern "C"
ASurfaceTransaction* ASurfaceTransaction_create_weak()
        __asm__("ASurfaceTransaction_create")
        __attribute__((weak));

extern "C"
void ASurfaceTransaction_delete_weak(
        ASurfaceTransaction* transaction
) __asm__("ASurfaceTransaction_delete")
  __attribute__((weak));

extern "C"
void ASurfaceTransaction_apply_weak(
        ASurfaceTransaction* transaction
) __asm__("ASurfaceTransaction_apply")
  __attribute__((weak));

extern "C"
void ASurfaceTransaction_setBuffer_weak(
        ASurfaceTransaction* transaction,
        ASurfaceControl* surfaceControl,
        AHardwareBuffer* buffer,
        int acquireFenceFd
) __asm__("ASurfaceTransaction_setBuffer")
  __attribute__((weak));

extern "C"
void ASurfaceTransaction_setVisibility_weak(
        ASurfaceTransaction* transaction,
        ASurfaceControl* surfaceControl,
        int8_t visibility
) __asm__("ASurfaceTransaction_setVisibility")
  __attribute__((weak));

extern "C"
void ASurfaceTransaction_setZOrder_weak(
        ASurfaceTransaction* transaction,
        ASurfaceControl* surfaceControl,
        int32_t zOrder
) __asm__("ASurfaceTransaction_setZOrder")
  __attribute__((weak));

extern "C"
void ASurfaceTransaction_setBufferTransparency_weak(
        ASurfaceTransaction* transaction,
        ASurfaceControl* surfaceControl,
        int8_t transparency
) __asm__("ASurfaceTransaction_setBufferTransparency")
  __attribute__((weak));

extern "C"
void ASurfaceTransaction_setEnableBackPressure_weak(
        ASurfaceTransaction* transaction,
        ASurfaceControl* surfaceControl,
        bool enableBackPressure
) __asm__("ASurfaceTransaction_setEnableBackPressure")
  __attribute__((weak));

static constexpr int8_t STEP15D_VISIBILITY_HIDE = 0;
static constexpr int8_t STEP15D_VISIBILITY_SHOW = 1;
static constexpr int8_t STEP15D_TRANSPARENCY_OPAQUE = 2;


#ifndef EGL_TIMESTAMPS_ANDROID
#define EGL_TIMESTAMPS_ANDROID 0x3430
#endif

#ifndef EGL_COMPOSITE_DEADLINE_ANDROID
#define EGL_COMPOSITE_DEADLINE_ANDROID 0x3431
#endif

#ifndef EGL_COMPOSITE_INTERVAL_ANDROID
#define EGL_COMPOSITE_INTERVAL_ANDROID 0x3432
#endif

#ifndef EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID
#define EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID 0x3433
#endif

#ifndef EGL_COMPOSITION_LATCH_TIME_ANDROID
#define EGL_COMPOSITION_LATCH_TIME_ANDROID 0x3436
#endif

#ifndef EGL_DISPLAY_PRESENT_TIME_ANDROID
#define EGL_DISPLAY_PRESENT_TIME_ANDROID 0x343A
#endif

#ifndef EGL_TIMESTAMP_PENDING_ANDROID
#define EGL_TIMESTAMP_PENDING_ANDROID (-2)
#endif

#ifndef EGL_TIMESTAMP_INVALID_ANDROID
#define EGL_TIMESTAMP_INVALID_ANDROID (-1)
#endif


#define LOG_TAG "UvcFieldMonitor"

#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGW(...) \
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#define LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


static std::mutex gMutex;
static std::thread gRenderThread;
static std::atomic<bool> gRunning{false};
static ANativeWindow* gWindow = nullptr;

struct PendingSurfaceTap {
    float x = 0.0f;
    float y = 0.0f;
};

static std::mutex gUiInputMutex;
static std::deque<PendingSurfaceTap> gPendingSurfaceTaps;
static constexpr size_t MAX_PENDING_SURFACE_TAPS = 32;

using field_monitor::UiCanvasViewport;
using field_monitor::UiLayout;
using field_monitor::UiLayoutMode;
using field_monitor::Viewport;
using field_monitor::aspectFitRect;
using field_monitor::calculateUiCanvasViewport;
using field_monitor::selectUiLayout;
using field_monitor::uiLogicalRectToViewport;

static void enqueueSurfaceTap(float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(gUiInputMutex);
        if (gPendingSurfaceTaps.size() >= MAX_PENDING_SURFACE_TAPS) {
            gPendingSurfaceTaps.pop_front();
        }
        gPendingSurfaceTaps.push_back({x, y});
    }

    uvc_mjpeg_decoder::notifyRenderWake();
}

static std::deque<PendingSurfaceTap> takePendingSurfaceTaps()
{
    std::deque<PendingSurfaceTap> pending;
    std::lock_guard<std::mutex> lock(gUiInputMutex);
    pending.swap(gPendingSurfaceTaps);
    return pending;
}

static bool applyMonitorPresetAction(
        field_monitor::MonitorUiController& controller,
        field_monitor::MonitorPresetAction action)
{
    using field_monitor::FalseColorMode;
    using field_monitor::FrameAspect;
    using field_monitor::MonitorPresetAction;
    using field_monitor::PeakingPreset;
    using field_monitor::ZebraPreset;

    switch (action) {
    case MonitorPresetAction::ZebraOff:
        return controller.selectZebraPreset(ZebraPreset::Off);
    case MonitorPresetAction::Zebra70:
        return controller.selectZebraPreset(ZebraPreset::Ire70);
    case MonitorPresetAction::Zebra80:
        return controller.selectZebraPreset(ZebraPreset::Ire80);
    case MonitorPresetAction::Zebra90:
        return controller.selectZebraPreset(ZebraPreset::Ire90);
    case MonitorPresetAction::Zebra95:
        return controller.selectZebraPreset(ZebraPreset::Ire95);
    case MonitorPresetAction::Zebra100:
        return controller.selectZebraPreset(ZebraPreset::Ire100);

    case MonitorPresetAction::PeakingOff:
        return controller.selectPeakingPreset(PeakingPreset::Off);
    case MonitorPresetAction::PeakingLow:
        return controller.selectPeakingPreset(PeakingPreset::Low);
    case MonitorPresetAction::PeakingMid:
        return controller.selectPeakingPreset(PeakingPreset::Mid);
    case MonitorPresetAction::PeakingHigh:
        return controller.selectPeakingPreset(PeakingPreset::High);
    case MonitorPresetAction::PeakingMonoLow:
        return controller.selectPeakingPreset(PeakingPreset::MonoLow);
    case MonitorPresetAction::PeakingMonoMid:
        return controller.selectPeakingPreset(PeakingPreset::MonoMid);
    case MonitorPresetAction::PeakingMonoHigh:
        return controller.selectPeakingPreset(PeakingPreset::MonoHigh);

    case MonitorPresetAction::FalseColorOff:
        return controller.selectFalseColorMode(FalseColorMode::Off);
    case MonitorPresetAction::FalseColorVideo:
        return controller.selectFalseColorMode(FalseColorMode::VideoLevel);
    case MonitorPresetAction::FalseColorHdrNits:
        return controller.selectFalseColorMode(FalseColorMode::HdrNits);

    case MonitorPresetAction::FrameOff:
        return controller.selectFrameOff();
    case MonitorPresetAction::Frame16x9:
        return controller.selectFrameAspect(FrameAspect::Ratio16x9);
    case MonitorPresetAction::Frame1_85:
        return controller.selectFrameAspect(FrameAspect::Ratio1_85);
    case MonitorPresetAction::Frame2_00:
        return controller.selectFrameAspect(FrameAspect::Ratio2_00);
    case MonitorPresetAction::Frame2_39:
        return controller.selectFrameAspect(FrameAspect::Ratio2_39);
    case MonitorPresetAction::Frame4x3:
        return controller.selectFrameAspect(FrameAspect::Ratio4x3);
    case MonitorPresetAction::Frame1x1:
        return controller.selectFrameAspect(FrameAspect::Ratio1x1);
    case MonitorPresetAction::Frame9x16:
        return controller.selectFrameAspect(FrameAspect::Ratio9x16);
    case MonitorPresetAction::ToggleCross:
        return controller.toggleCenterCross();
    case MonitorPresetAction::ToggleSafe:
        return controller.toggleSafeArea();

    case MonitorPresetAction::None:
    default:
        return false;
    }
}

static bool processSurfaceTap(
        const PendingSurfaceTap& tap,
        EGLint surfaceHeight,
        const UiLayout& layout,
        const UiCanvasViewport& canvas,
        field_monitor::MonitorUiController& controller)
{
    const float canvasSurfaceTop =
            static_cast<float>(
                    surfaceHeight - canvas.y - canvas.height);
    const bool insideCanvas =
            tap.x >= static_cast<float>(canvas.x) &&
            tap.y >= canvasSurfaceTop &&
            tap.x < static_cast<float>(canvas.x + canvas.width) &&
            tap.y < canvasSurfaceTop + static_cast<float>(canvas.height) &&
            canvas.scale > 0.0f;

    if (!insideCanvas) {
        return controller.tapOutsideMenuUi();
    }

    const float logicalX =
            (tap.x - static_cast<float>(canvas.x)) / canvas.scale;
    const float logicalY =
            (tap.y - canvasSurfaceTop) / canvas.scale;

    const field_monitor::MonitorUiSnapshot snapshot = controller.snapshot();
    const field_monitor::MonitorUiGeometry geometry =
            field_monitor::calculateMonitorUiGeometry(layout, snapshot);

    if (field_monitor::monitorRectContains(
            geometry.menuTrigger, logicalX, logicalY)) {
        return controller.toggleMenu();
    }

    if (!geometry.railVisible) {
        return false;
    }

    if (geometry.presetVisible) {
        for (std::size_t index = 0;
             index < geometry.presetRowCount;
             ++index) {
            const field_monitor::MonitorPresetRowGeometry& row =
                    geometry.presetRows[index];
            if (field_monitor::monitorRectContains(
                    row.rect, logicalX, logicalY)) {
                return
                        row.enabled &&
                        applyMonitorPresetAction(controller, row.action);
            }
        }

        if (field_monitor::monitorRectContains(
                geometry.presetPanel, logicalX, logicalY)) {
            return false;
        }
    }

    if (field_monitor::monitorRectContains(
            geometry.lockButton, logicalX, logicalY)) {
        return controller.toggleLock();
    }

    for (const field_monitor::MonitorFunctionButtonGeometry& button :
         geometry.functionButtons) {
        if (field_monitor::monitorRectContains(
                button.rect, logicalX, logicalY)) {
            return button.enabled && controller.tapFunction(button.key);
        }
    }

    if (field_monitor::monitorRectContains(
            geometry.functionRail, logicalX, logicalY)) {
        return false;
    }

    return controller.tapOutsideMenuUi();
}

static bool processPendingSurfaceTaps(
        EGLint surfaceHeight,
        const UiLayout& layout,
        const UiCanvasViewport& canvas,
        field_monitor::MonitorUiController& controller)
{
    bool changed = false;
    std::deque<PendingSurfaceTap> pending = takePendingSurfaceTaps();
    for (const PendingSurfaceTap& tap : pending) {
        changed =
                processSurfaceTap(
                        tap,
                        surfaceHeight,
                        layout,
                        canvas,
                        controller) ||
                changed;
    }
    return changed;
}

static constexpr int PLANAR_MJPEG_FRAME_W = 1280;
static constexpr int PLANAR_MJPEG_FRAME_H = 720;
static constexpr int PLANAR_MJPEG_CHROMA_W = PLANAR_MJPEG_FRAME_W / 2;
static constexpr size_t PLANAR_MJPEG_Y_BYTES =
        static_cast<size_t>(PLANAR_MJPEG_FRAME_W) * PLANAR_MJPEG_FRAME_H;
static constexpr size_t PLANAR_MJPEG_C_BYTES =
        static_cast<size_t>(PLANAR_MJPEG_CHROMA_W) * PLANAR_MJPEG_FRAME_H;
static constexpr size_t PLANAR_MJPEG_CB_OFFSET = PLANAR_MJPEG_Y_BYTES;
static constexpr size_t PLANAR_MJPEG_CR_OFFSET = PLANAR_MJPEG_Y_BYTES + PLANAR_MJPEG_C_BYTES;
static constexpr size_t PLANAR_MJPEG_YUV422_FRAME_BYTES =
        PLANAR_MJPEG_Y_BYTES + PLANAR_MJPEG_C_BYTES + PLANAR_MJPEG_C_BYTES;

// Normal operation keeps Logcat focused on transport/decode health.
// Enable only when detailed render/presentation timing is needed.
static constexpr bool ENABLE_VERBOSE_TIMING_LOG =
        UVCFM_DIAGNOSTICS_ENABLED;

// Step 13.2 result retained for Step 14.
//
// true:
//   eglPresentationTimeANDROID(CLOCK_MONOTONIC now)
//   immediately before each event-driven eglSwapBuffers().
static constexpr bool STEP13_2_PRESENTATION_TIME_HINT_NOW =
        true;


struct FrontBufferProbeOutcome {
    bool supported = false;
    bool allocated = false;
    bool eglImageCreated = false;
    bool framebufferComplete = false;

    uint32_t stride = 0;
};


static AHardwareBuffer_Desc makeAhbDesc(
        uint32_t width,
        uint32_t height,
        uint64_t usage)
{
    AHardwareBuffer_Desc desc{};

    desc.width = width;
    desc.height = height;

    desc.layers = 1;

    desc.format =
            AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;

    desc.usage = usage;

    // Input stride must be zero. The allocator fills the actual stride.
    desc.stride = 0;

    return desc;
}


static bool queryAhbSupport(
        uint32_t width,
        uint32_t height,
        uint64_t usage)
{
    const AHardwareBuffer_Desc desc =
            makeAhbDesc(
                    width,
                    height,
                    usage
            );

    return
            ahbIsSupportedCompat(
                    &desc
            );
}


static FrontBufferProbeOutcome probeFrontBufferAllocationAndEgl(
        EGLDisplay display,
        uint32_t width,
        uint32_t height,
        uint64_t usage,
        bool testEglInterop)
{
    FrontBufferProbeOutcome outcome{};

    const AHardwareBuffer_Desc request =
            makeAhbDesc(
                    width,
                    height,
                    usage
            );


    outcome.supported =
            ahbIsSupportedCompat(
                    &request
            );


    LOGI(
            "Step 15A: AHB support "
            "%ux%u usage=0x%016llX -> %s",
            width,
            height,
            static_cast<unsigned long long>(
                    usage
            ),
            outcome.supported
            ? "YES"
            : "NO"
    );


    if (!outcome.supported) {
        return outcome;
    }


    AHardwareBuffer* buffer = nullptr;

    const int allocResult =
            AHardwareBuffer_allocate(
                    &request,
                    &buffer
            );


    if (allocResult != 0 ||
        buffer == nullptr) {

        LOGE(
                "Step 15A: AHardwareBuffer_allocate "
                "%ux%u failed rc=%d",
                width,
                height,
                allocResult
        );

        return outcome;
    }


    outcome.allocated = true;


    AHardwareBuffer_Desc actual{};

    AHardwareBuffer_describe(
            buffer,
            &actual
    );

    outcome.stride =
            actual.stride;


    LOGI(
            "Step 15A: AHB allocate OK "
            "%ux%u stride=%u "
            "format=%u usage=0x%016llX",
            actual.width,
            actual.height,
            actual.stride,
            actual.format,
            static_cast<unsigned long long>(
                    actual.usage
            )
    );


    if (!testEglInterop) {

        AHardwareBuffer_release(
                buffer
        );

        return outcome;
    }


    auto p_eglGetNativeClientBufferANDROID =
            reinterpret_cast<
                PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC_LOCAL
            >(
                eglGetProcAddress(
                        "eglGetNativeClientBufferANDROID"
                )
            );

    auto p_eglCreateImageKHR =
            reinterpret_cast<
                PFNEGLCREATEIMAGEKHRPROC_LOCAL
            >(
                eglGetProcAddress(
                        "eglCreateImageKHR"
                )
            );

    auto p_eglDestroyImageKHR =
            reinterpret_cast<
                PFNEGLDESTROYIMAGEKHRPROC_LOCAL
            >(
                eglGetProcAddress(
                        "eglDestroyImageKHR"
                )
            );

    auto p_glEGLImageTargetTexture2DOES =
            reinterpret_cast<
                PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_LOCAL
            >(
                eglGetProcAddress(
                        "glEGLImageTargetTexture2DOES"
                )
            );


    if (p_eglGetNativeClientBufferANDROID == nullptr ||
        p_eglCreateImageKHR == nullptr ||
        p_eglDestroyImageKHR == nullptr ||
        p_glEGLImageTargetTexture2DOES == nullptr) {

        LOGE(
                "Step 15A: EGLImage interop "
                "entry point missing "
                "getNative=%s createImage=%s "
                "destroyImage=%s imageTarget=%s",
                p_eglGetNativeClientBufferANDROID
                ? "YES"
                : "NO",
                p_eglCreateImageKHR
                ? "YES"
                : "NO",
                p_eglDestroyImageKHR
                ? "YES"
                : "NO",
                p_glEGLImageTargetTexture2DOES
                ? "YES"
                : "NO"
        );

        AHardwareBuffer_release(
                buffer
        );

        return outcome;
    }


    const EGLClientBuffer clientBuffer =
            p_eglGetNativeClientBufferANDROID(
                    buffer
            );


    if (clientBuffer == nullptr) {

        LOGE(
                "Step 15A: "
                "eglGetNativeClientBufferANDROID failed"
        );

        AHardwareBuffer_release(
                buffer
        );

        return outcome;
    }


    const EGLint imageAttribs[] = {
            EGL_NONE
    };


    EGLImageKHR image =
            p_eglCreateImageKHR(
                    display,
                    EGL_NO_CONTEXT,
                    EGL_NATIVE_BUFFER_ANDROID,
                    clientBuffer,
                    imageAttribs
            );


    if (image == EGL_NO_IMAGE_KHR) {

        LOGE(
                "Step 15A: eglCreateImageKHR "
                "%ux%u failed EGL=0x%04X",
                width,
                height,
                eglGetError()
        );

        AHardwareBuffer_release(
                buffer
        );

        return outcome;
    }


    outcome.eglImageCreated = true;


    GLuint texture = 0;
    GLuint framebuffer = 0;


    glGenTextures(
            1,
            &texture
    );

    glBindTexture(
            GL_TEXTURE_2D,
            texture
    );

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MIN_FILTER,
            GL_NEAREST
    );

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAG_FILTER,
            GL_NEAREST
    );

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_S,
            GL_CLAMP_TO_EDGE
    );

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_WRAP_T,
            GL_CLAMP_TO_EDGE
    );


    while (glGetError() != GL_NO_ERROR) {
        // Clear pre-existing GL errors before the import test.
    }


    p_glEGLImageTargetTexture2DOES(
            GL_TEXTURE_2D,
            reinterpret_cast<void*>(
                    image
            )
    );


    const GLenum imageBindError =
            glGetError();


    if (imageBindError != GL_NO_ERROR) {

        LOGE(
                "Step 15A: "
                "glEGLImageTargetTexture2DOES "
                "failed GL=0x%04X",
                imageBindError
        );
    }
    else {

        glGenFramebuffers(
                1,
                &framebuffer
        );

        glBindFramebuffer(
                GL_FRAMEBUFFER,
                framebuffer
        );

        glFramebufferTexture2D(
                GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D,
                texture,
                0
        );


        const GLenum framebufferStatus =
                glCheckFramebufferStatus(
                        GL_FRAMEBUFFER
                );


        outcome.framebufferComplete =
                framebufferStatus ==
                GL_FRAMEBUFFER_COMPLETE;


        LOGI(
                "Step 15A: EGLImage/FBO "
                "%ux%u -> %s "
                "(status=0x%04X)",
                width,
                height,
                outcome.framebufferComplete
                ? "COMPLETE"
                : "INCOMPLETE",
                framebufferStatus
        );


        if (outcome.framebufferComplete) {

            // Offscreen-only smoke test. This HardwareBuffer is not
            // submitted to SurfaceFlinger or displayed.
            glClearColor(
                    0.0f,
                    0.0f,
                    0.0f,
                    1.0f
            );

            glClear(
                    GL_COLOR_BUFFER_BIT
            );

            glFlush();
        }
    }


    glBindFramebuffer(
            GL_FRAMEBUFFER,
            0
    );

    glBindTexture(
            GL_TEXTURE_2D,
            0
    );


    if (framebuffer != 0) {

        glDeleteFramebuffers(
                1,
                &framebuffer
        );
    }


    if (texture != 0) {

        glDeleteTextures(
                1,
                &texture
        );
    }


    p_eglDestroyImageKHR(
            display,
            image
    );


    AHardwareBuffer_release(
            buffer
    );


    return outcome;
}


static void runStep15AFrontBufferCapabilityProbe(
        EGLDisplay display,
        EGLint surfaceWidth,
        EGLint surfaceHeight)
{
    const int apiLevel =
            android_get_device_api_level();


    const uint64_t baseFlags =
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    const uint64_t frontFlags =
            baseFlags |
            AHARDWAREBUFFER_USAGE_FRONT_BUFFER;


    LOGI(
            "=================================================="
    );

    LOGI(
            "Step 15A: Front-buffer capability probe"
    );

    LOGI(
            "Step 15A: Android API level = %d",
            apiLevel
    );

    LOGI(
            "Step 15A: BaseFlags "
            "= SAMPLED|COLOR_OUTPUT|COMPOSER_OVERLAY "
            "= 0x%016llX",
            static_cast<unsigned long long>(
                    baseFlags
            )
    );

    LOGI(
            "Step 15A: FrontFlags "
            "= BaseFlags|FRONT_BUFFER "
            "= 0x%016llX",
            static_cast<unsigned long long>(
                    frontFlags
            )
    );


    if (apiLevel < 29) {

        LOGI(
                "Step 15A RESULT: "
                "UNSUPPORTED - "
                "AHardwareBuffer_isSupported requires API 29+"
        );

        LOGI(
                "=================================================="
        );

        return;
    }


    // Mirror AndroidX FrontBufferUtils' 1x1 capability style,
    // while also diagnosing which usage combination fails.
    const bool colorOutput1x1 =
            queryAhbSupport(
                    1,
                    1,
                    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
            );

    const bool sampledColor1x1 =
            queryAhbSupport(
                    1,
                    1,
                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
            );

    const bool base1x1 =
            queryAhbSupport(
                    1,
                    1,
                    baseFlags
            );

    const bool front1x1 =
            queryAhbSupport(
                    1,
                    1,
                    frontFlags
            );


    LOGI(
            "Step 15A matrix 1x1 RGBA8: "
            "COLOR=%s "
            "SAMPLED|COLOR=%s "
            "BASE=%s "
            "BASE|FRONT=%s",
            colorOutput1x1
            ? "YES"
            : "NO",
            sampledColor1x1
            ? "YES"
            : "NO",
            base1x1
            ? "YES"
            : "NO",
            front1x1
            ? "YES"
            : "NO"
    );


    const FrontBufferProbeOutcome source =
            probeFrontBufferAllocationAndEgl(
                    display,
                    PLANAR_MJPEG_FRAME_W,
                    PLANAR_MJPEG_FRAME_H,
                    frontFlags,
                    true
            );


    FrontBufferProbeOutcome surface{};


    if (surfaceWidth > 0 &&
        surfaceHeight > 0) {

        surface =
                probeFrontBufferAllocationAndEgl(
                        display,
                        static_cast<uint32_t>(
                                surfaceWidth
                        ),
                        static_cast<uint32_t>(
                                surfaceHeight
                        ),
                        frontFlags,
                        true
                );
    }


    const bool sourceReady =
            source.supported &&
            source.allocated &&
            source.eglImageCreated &&
            source.framebufferComplete;

    const bool surfaceReady =
            surface.supported &&
            surface.allocated &&
            surface.eglImageCreated &&
            surface.framebufferComplete;


    if (front1x1 &&
        sourceReady &&
        surfaceReady) {

        LOGI(
                "Step 15A RESULT: READY - "
                "FRONT_BUFFER AHB + EGLImage/FBO works "
                "for source and full Surface sizes"
        );
    }
    else if (front1x1 &&
             sourceReady) {

        LOGI(
                "Step 15A RESULT: SOURCE_READY - "
                "1280x720 FRONT_BUFFER AHB + EGLImage/FBO works; "
                "full Surface path is not fully supported"
        );
    }
    else if (front1x1) {

        LOGI(
                "Step 15A RESULT: PARTIAL - "
                "FRONT_BUFFER usage is advertised "
                "but allocation/EGL/FBO path is incomplete"
        );
    }
    else {

        LOGI(
                "Step 15A RESULT: FRONT_BUFFER_UNSUPPORTED"
        );
    }


    LOGI(
            "=================================================="
    );
}


// ------------------------------------------------------------
// Step 15D: persistent AHardwareBuffer front-buffer presenter.
//
// The normal EGL window surface remains alive only to host the GLES context.
// Video/UI rendering is redirected to one persistent AHardwareBuffer imported
// as EGLImage -> GL texture -> FBO.  The buffer is attached to a child
// ASurfaceControl exactly once.  Subsequent camera frames update the same
// buffer.  Each camera frame performs glFlush() and re-submits the SAME AHB
// through a lightweight SurfaceControl transaction; there is no BufferQueue
// dequeue/queue cycle and no per-frame eglSwapBuffers().
//
// AndroidX Graphics uses the same core policy for a visible USAGE_FRONT_BUFFER
// layer: persistent one-buffer rendering, glFlush() without a new fence, then
// a SurfaceControl transaction carrying the persistent HardwareBuffer.
// ------------------------------------------------------------

static constexpr bool STEP15D_FRONT_BUFFER_ENABLE = true;
static constexpr int STEP15D_MIN_API_LEVEL = 36;  // Android 16 experiment only.

struct Step15DFrontBuffer {
    bool active = false;
    bool published = false;

    AHardwareBuffer* hardwareBuffer = nullptr;
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;

    GLuint texture = 0;
    GLuint framebuffer = 0;

    ASurfaceControl* layer = nullptr;

    PFNEGLDESTROYIMAGEKHRPROC_LOCAL destroyImage = nullptr;
};


static bool step15DHaveRequiredSurfaceControlSymbols()
{
    return
            ASurfaceControl_createFromWindow_weak != nullptr &&
            ASurfaceControl_release_weak != nullptr &&
            ASurfaceTransaction_create_weak != nullptr &&
            ASurfaceTransaction_delete_weak != nullptr &&
            ASurfaceTransaction_apply_weak != nullptr &&
            ASurfaceTransaction_setBuffer_weak != nullptr &&
            ASurfaceTransaction_setVisibility_weak != nullptr &&
            ASurfaceTransaction_setZOrder_weak != nullptr;
}


static void destroyStep15DFrontBuffer(
        EGLDisplay display,
        Step15DFrontBuffer& front)
{
    if (front.layer != nullptr) {

        if (ASurfaceTransaction_create_weak != nullptr &&
            ASurfaceTransaction_delete_weak != nullptr &&
            ASurfaceTransaction_apply_weak != nullptr &&
            ASurfaceTransaction_setVisibility_weak != nullptr) {

            ASurfaceTransaction* tx =
                    ASurfaceTransaction_create_weak();

            if (tx != nullptr) {
                ASurfaceTransaction_setVisibility_weak(
                        tx,
                        front.layer,
                        STEP15D_VISIBILITY_HIDE
                );
                ASurfaceTransaction_apply_weak(tx);
                ASurfaceTransaction_delete_weak(tx);
            }
        }

        if (ASurfaceControl_release_weak != nullptr) {
            ASurfaceControl_release_weak(front.layer);
        }

        front.layer = nullptr;
    }

    if (front.framebuffer != 0) {
        glDeleteFramebuffers(1, &front.framebuffer);
        front.framebuffer = 0;
    }

    if (front.texture != 0) {
        glDeleteTextures(1, &front.texture);
        front.texture = 0;
    }

    if (front.eglImage != EGL_NO_IMAGE_KHR &&
        front.destroyImage != nullptr) {

        front.destroyImage(
                display,
                front.eglImage
        );
        front.eglImage = EGL_NO_IMAGE_KHR;
    }

    if (front.hardwareBuffer != nullptr) {
        AHardwareBuffer_release(front.hardwareBuffer);
        front.hardwareBuffer = nullptr;
    }

    front.active = false;
    front.published = false;
    front.destroyImage = nullptr;
}


static bool createStep15DFrontBuffer(
        EGLDisplay display,
        ANativeWindow* parentWindow,
        uint32_t width,
        uint32_t height,
        Step15DFrontBuffer& front)
{
    if (!STEP15D_FRONT_BUFFER_ENABLE) {
        return false;
    }

    const int apiLevel =
            android_get_device_api_level();

    if (apiLevel < STEP15D_MIN_API_LEVEL) {
        LOGI(
                "Step 15D: disabled on API %d; "
                "Android 16/API 36+ experiment only",
                apiLevel
        );
        return false;
    }

    if (parentWindow == nullptr ||
        width == 0 ||
        height == 0) {

        LOGE("Step 15D: invalid parent window / size");
        return false;
    }

    if (!step15DHaveRequiredSurfaceControlSymbols()) {
        LOGE(
                "Step 15D: required SurfaceControl NDK symbols missing; "
                "falling back to EGL/BufferQueue"
        );
        return false;
    }

    const uint64_t usage =
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
            AHARDWAREBUFFER_USAGE_FRONT_BUFFER;

    const AHardwareBuffer_Desc desc =
            makeAhbDesc(
                    width,
                    height,
                    usage
            );

    if (!ahbIsSupportedCompat(&desc)) {
        LOGI(
                "Step 15D: full-surface FRONT_BUFFER unsupported "
                "%ux%u usage=0x%016llX; fallback",
                width,
                height,
                static_cast<unsigned long long>(usage)
        );
        return false;
    }

    const int allocRc =
            AHardwareBuffer_allocate(
                    &desc,
                    &front.hardwareBuffer
            );

    if (allocRc != 0 ||
        front.hardwareBuffer == nullptr) {

        LOGE(
                "Step 15D: AHardwareBuffer_allocate failed rc=%d",
                allocRc
        );
        destroyStep15DFrontBuffer(display, front);
        return false;
    }

    AHardwareBuffer_Desc actual{};
    AHardwareBuffer_describe(
            front.hardwareBuffer,
            &actual
    );

    auto getNativeClientBuffer =
            reinterpret_cast<
                PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC_LOCAL
            >(
                eglGetProcAddress(
                        "eglGetNativeClientBufferANDROID"
                )
            );

    auto createImage =
            reinterpret_cast<
                PFNEGLCREATEIMAGEKHRPROC_LOCAL
            >(
                eglGetProcAddress(
                        "eglCreateImageKHR"
                )
            );

    front.destroyImage =
            reinterpret_cast<
                PFNEGLDESTROYIMAGEKHRPROC_LOCAL
            >(
                eglGetProcAddress(
                        "eglDestroyImageKHR"
                )
            );

    auto imageTargetTexture =
            reinterpret_cast<
                PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_LOCAL
            >(
                eglGetProcAddress(
                        "glEGLImageTargetTexture2DOES"
                )
            );

    if (getNativeClientBuffer == nullptr ||
        createImage == nullptr ||
        front.destroyImage == nullptr ||
        imageTargetTexture == nullptr) {

        LOGE(
                "Step 15D: EGLImage interop entry point missing; fallback"
        );
        destroyStep15DFrontBuffer(display, front);
        return false;
    }

    const EGLClientBuffer clientBuffer =
            getNativeClientBuffer(
                    front.hardwareBuffer
            );

    if (clientBuffer == nullptr) {
        LOGE("Step 15D: eglGetNativeClientBufferANDROID failed");
        destroyStep15DFrontBuffer(display, front);
        return false;
    }

    const EGLint imageAttribs[] = {
            EGL_NONE
    };

    front.eglImage =
            createImage(
                    display,
                    EGL_NO_CONTEXT,
                    EGL_NATIVE_BUFFER_ANDROID,
                    clientBuffer,
                    imageAttribs
            );

    if (front.eglImage == EGL_NO_IMAGE_KHR) {
        LOGE(
                "Step 15D: eglCreateImageKHR failed EGL=0x%04X",
                eglGetError()
        );
        destroyStep15DFrontBuffer(display, front);
        return false;
    }

    glGenTextures(1, &front.texture);
    glBindTexture(GL_TEXTURE_2D, front.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    while (glGetError() != GL_NO_ERROR) {
        // Clear pre-existing errors before importing the EGLImage.
    }

    imageTargetTexture(
            GL_TEXTURE_2D,
            reinterpret_cast<void*>(front.eglImage)
    );

    const GLenum imageBindError = glGetError();

    if (imageBindError != GL_NO_ERROR) {
        LOGE(
                "Step 15D: glEGLImageTargetTexture2DOES failed GL=0x%04X",
                imageBindError
        );
        glBindTexture(GL_TEXTURE_2D, 0);
        destroyStep15DFrontBuffer(display, front);
        return false;
    }

    glGenFramebuffers(1, &front.framebuffer);
    glBindFramebuffer(
            GL_FRAMEBUFFER,
            front.framebuffer
    );
    glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            front.texture,
            0
    );

    const GLenum framebufferStatus =
            glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (framebufferStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOGE(
                "Step 15D: front FBO incomplete status=0x%04X",
                framebufferStatus
        );
        destroyStep15DFrontBuffer(display, front);
        return false;
    }

    front.layer =
            ASurfaceControl_createFromWindow_weak(
                    parentWindow,
                    "UvcFieldMonitorFrontBuffer"
            );

    if (front.layer == nullptr) {
        LOGE(
                "Step 15D: ASurfaceControl_createFromWindow failed; fallback"
        );
        destroyStep15DFrontBuffer(display, front);
        return false;
    }

    front.active = true;

    LOGI(
            "Step 15D: READY persistent FRONT_BUFFER "
            "%ux%u stride=%u usage=0x%016llX",
            actual.width,
            actual.height,
            actual.stride,
            static_cast<unsigned long long>(actual.usage)
    );

    return true;
}


static bool submitStep15DFrontBuffer(
        Step15DFrontBuffer& front,
        bool firstPublication)
{
    if (!front.active ||
        front.hardwareBuffer == nullptr ||
        front.layer == nullptr) {
        return false;
    }

    if (firstPublication) {
        // One-time synchronization only.  The layer is not visible yet, so a
        // blocking finish here does not affect steady-state camera latency.
        glFinish();
    }
    else {
        // FRONT_BUFFER semantics intentionally permit the consumer/display to
        // read while GLES updates the same allocation.  Match AndroidX's
        // visible-front-buffer strategy: submit commands but do not wait.
        glFlush();
    }

    ASurfaceTransaction* tx =
            ASurfaceTransaction_create_weak();

    if (tx == nullptr) {
        LOGE("Step 15D: ASurfaceTransaction_create failed");
        return false;
    }

    // Re-submit the same persistent AHardwareBuffer every frame.  This wakes
    // the SurfaceControl/SF path without returning to BufferQueue.
    ASurfaceTransaction_setBuffer_weak(
            tx,
            front.layer,
            front.hardwareBuffer,
            -1
    );

    if (firstPublication) {

        ASurfaceTransaction_setZOrder_weak(
                tx,
                front.layer,
                0x7fffffff
        );

        if (ASurfaceTransaction_setBufferTransparency_weak != nullptr) {
            ASurfaceTransaction_setBufferTransparency_weak(
                    tx,
                    front.layer,
                    STEP15D_TRANSPARENCY_OPAQUE
            );
        }

        if (ASurfaceTransaction_setEnableBackPressure_weak != nullptr) {
            ASurfaceTransaction_setEnableBackPressure_weak(
                    tx,
                    front.layer,
                    false
            );
        }

        ASurfaceTransaction_setVisibility_weak(
                tx,
                front.layer,
                STEP15D_VISIBILITY_SHOW
        );
    }

    ASurfaceTransaction_apply_weak(tx);
    ASurfaceTransaction_delete_weak(tx);

    if (firstPublication) {
        front.published = true;

        LOGI(
                "Step 15D: initial SurfaceControl front buffer published; "
                "steady-state = glFlush + same-AHB setBuffer transaction"
        );
    }

    return true;
}


struct PboSlot {
    GLuint id = 0;
    uint8_t* mapped = nullptr;
    GLsync fence = nullptr;
};


static bool tryReleasePboFence(
        PboSlot& slot)
{
    if (slot.fence == nullptr) {
        return true;
    }

    const GLenum result =
            glClientWaitSync(
                    slot.fence,
                    0,
                    0
            );

    if (result == GL_ALREADY_SIGNALED ||
        result == GL_CONDITION_SATISFIED) {

        glDeleteSync(
                slot.fence
        );

        slot.fence = nullptr;

        return true;
    }

    if (result == GL_WAIT_FAILED) {

        LOGE(
                "Step 12: "
                "glClientWaitSync failed"
        );
    }

    // GL_TIMEOUT_EXPIRED means the PBO is still owned by GPU.
    // Never wait and never overwrite it.
    return false;
}


static int findAvailablePbo(
        PboSlot (&pbo)[2],
        int preferred)
{
    for (int n = 0;
         n < 2;
         ++n) {

        const int index =
                (preferred + n) & 1;

        if (tryReleasePboFence(
                pbo[index])) {

            return index;
        }
    }

    return -1;
}


static PFNGLBUFFERSTORAGEEXTPROC_LOCAL p_glBufferStorageEXT = nullptr;

static PFNEGLGETNEXTFRAMEIDANDROIDPROC_LOCAL
        p_eglGetNextFrameIdANDROID = nullptr;

static PFNEGLGETFRAMETIMESTAMPSANDROIDPROC_LOCAL
        p_eglGetFrameTimestampsANDROID = nullptr;

static PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC_LOCAL
        p_eglGetFrameTimestampSupportedANDROID = nullptr;

static PFNEGLGETCOMPOSITORTIMINGANDROIDPROC_LOCAL
        p_eglGetCompositorTimingANDROID = nullptr;

static PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC_LOCAL
        p_eglGetCompositorTimingSupportedANDROID = nullptr;


static PFNEGLPRESENTATIONTIMEANDROIDPROC_LOCAL
        p_eglPresentationTimeANDROID = nullptr;

static GLuint compileShader(
        GLenum type,
        const char* source)
{
    GLuint shader = glCreateShader(type);

    glShaderSource(
            shader,
            1,
            &source,
            nullptr
    );

    glCompileShader(shader);


    GLint ok = GL_FALSE;

    glGetShaderiv(
            shader,
            GL_COMPILE_STATUS,
            &ok
    );


    if (!ok) {

        char log[1024] = {};

        glGetShaderInfoLog(
                shader,
                sizeof(log),
                nullptr,
                log
        );

        LOGE(
                "Shader compile error: %s",
                log
        );

        glDeleteShader(shader);

        return 0;
    }


    return shader;
}


static GLuint createPlanarMjpegYuvProgram()
{
    static const char* vertexSource = R"(#version 300 es

out vec2 vUv;

void main()
{
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );

    vec2 p = positions[gl_VertexID];
    gl_Position = vec4(p, 0.0, 1.0);

    vUv = vec2(
        (p.x + 1.0) * 0.5,
        1.0 - ((p.y + 1.0) * 0.5)
    );
}
)";

    static const char* fragmentSource = R"(#version 300 es

precision highp float;

in vec2 vUv;
uniform sampler2D uY;
uniform sampler2D uCb;
uniform sampler2D uCr;
uniform int uPqToSdr;
uniform int uCalibrationEnabled;
uniform vec3 uCalibrationRow0;
uniform vec3 uCalibrationRow1;
uniform vec3 uCalibrationRow2;
uniform vec3 uCalibrationOffset;
out vec4 outColor;

// SMPTE ST 2084 (PQ) EOTF. Input is normalized PQ code value.
// Output is absolute linear-light RGB in cd/m^2.
vec3 pqEotfNits(vec3 pq)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;

    pq = clamp(pq, 0.0, 1.0);
    vec3 p = pow(pq, vec3(1.0 / m2));
    vec3 num = max(p - vec3(c1), vec3(0.0));
    vec3 den = max(vec3(c2) - vec3(c3) * p, vec3(1.0e-6));

    return 10000.0 * pow(num / den, vec3(1.0 / m1));
}

// Preview-only HDR -> SDR mapping. Measurement/scopes stay on the original
// planar-MJPEG source-code path and never pass through this function.
// 203 nit reference white maps to SDR white; highlights are compressed above it.
vec3 toneMap203NitToSdrLinear(vec3 rgbNits)
{
    const vec3 yCoeff2020 = vec3(0.2627, 0.6780, 0.0593);
    const float referenceWhiteNits = 203.0;

    rgbNits = max(rgbNits, vec3(0.0));
    float yNits = max(dot(rgbNits, yCoeff2020), 0.0);

    // Linear through reference white, then Reinhard-like shoulder above it.
    float ySdr;
    if (yNits <= referenceWhiteNits) {
        ySdr = yNits / referenceWhiteNits;
    }
    else {
        float x = (yNits - referenceWhiteNits) / referenceWhiteNits;
        ySdr = 1.0 + (x / (1.0 + x)) * 0.25;
    }

    float scale = (yNits > 1.0e-6) ? (ySdr / yNits) : 0.0;
    return rgbNits * scale;
}

vec3 bt2020ToBt709Linear(vec3 c)
{
    return vec3(
         1.6604910 * c.r - 0.5876411 * c.g - 0.0728499 * c.b,
        -0.1245505 * c.r + 1.1328999 * c.g - 0.0083494 * c.b,
        -0.0181508 * c.r - 0.1005789 * c.g + 1.1187297 * c.b
    );
}

vec3 linearToSrgb(vec3 c)
{
    c = max(c, vec3(0.0));
    vec3 lo = 12.92 * c;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    vec3 useHi = step(vec3(0.0031308), c);
    return mix(lo, hi, useHi);
}

void main()
{
    // Planar-MJPEG diagnostics measured the decoded JPEG planes as nominal
    // BT.601 limited-range codes: Y=16..235, Cb/Cr centered at 128 with
    // nominal excursion 16..240. Recover nonlinear R'G'B' before optional PQ.
    float y8 = texture(uY, vUv).r * 255.0;
    float cb8 = texture(uCb, vUv).r * 255.0;
    float cr8 = texture(uCr, vUv).r * 255.0;

    float y = (y8 - 16.0) / 219.0;
    float cb = (cb8 - 128.0) / 224.0;
    float cr = (cr8 - 128.0) / 224.0;

    vec3 rgb;
    rgb.r = y + 1.402000 * cr;
    rgb.g = y - 0.344136 * cb - 0.714136 * cr;
    rgb.b = y + 1.772000 * cb;
    rgb = clamp(rgb, 0.0, 1.0);

    if (uCalibrationEnabled != 0) {
        rgb = vec3(
            dot(uCalibrationRow0, rgb),
            dot(uCalibrationRow1, rgb),
            dot(uCalibrationRow2, rgb)
        ) + uCalibrationOffset;
        rgb = clamp(rgb, 0.0, 1.0);
    }

    if (uPqToSdr != 0) {
        // Preserve the existing planar-MJPEG YCbCr -> RGB reconstruction above.
        // Only the preview interprets the recovered R'G'B' as PQ/BT.2020.
        vec3 rgb2020Nits = pqEotfNits(rgb);
        vec3 rgb2020SdrLinear = toneMap203NitToSdrLinear(rgb2020Nits);
        vec3 rgb709Linear = bt2020ToBt709Linear(rgb2020SdrLinear);
        rgb = linearToSrgb(clamp(rgb709Linear, 0.0, 1.0));
    }

    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);

    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Planar MJPEG YCbCr program link error: %s", log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (program != 0) {
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "uY"), 0);
        glUniform1i(glGetUniformLocation(program, "uCb"), 1);
        glUniform1i(glGetUniformLocation(program, "uCr"), 2);

        glUniform1i(glGetUniformLocation(program, "uPqToSdr"), 0);
        glUniform1i(glGetUniformLocation(program, "uCalibrationEnabled"), 0);
    }

    return program;
}

// ------------------------------------------------------------
// EGL error helper
// ------------------------------------------------------------

static void logEglError(const char* where)
{
    EGLint err = eglGetError();

    LOGE(
            "%s: EGL error = 0x%04x",
            where,
            err
    );
}

static bool hasGlExtension(const char* target)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);

    for (GLint i = 0; i < count; ++i) {

        const char* ext =
                reinterpret_cast<const char*>(
                        glGetStringi(GL_EXTENSIONS, i)
                );

        if (ext != nullptr &&
            std::strcmp(ext, target) == 0) {

            return true;
        }
    }

    return false;
}


static bool hasEglExtension(
        EGLDisplay display,
        const char* target)
{
    const char* extensions =
            eglQueryString(
                    display,
                    EGL_EXTENSIONS
            );

    if (extensions == nullptr) {
        return false;
    }

    const size_t targetLen =
            std::strlen(target);

    const char* p = extensions;

    while ((p = std::strstr(p, target)) != nullptr) {

        const bool startOk =
                (p == extensions) ||
                (*(p - 1) == ' ');

        const char next =
                p[targetLen];

        const bool endOk =
                (next == '\0') ||
                (next == ' ');

        if (startOk && endOk) {
            return true;
        }

        p += targetLen;
    }

    return false;
}

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


static double nsToMs(
        uint64_t deltaNs)
{
    return
            static_cast<double>(
                    deltaNs
            ) /
            1'000'000.0;
}


struct LatencySample {
    double t0ToT1Ms = 0.0;
    double t1ToT2Ms = 0.0;
    double t2ToT3Ms = 0.0;
    double t3ToT4Ms = 0.0;
    double t4ToT5Ms = 0.0;
    double totalMs = 0.0;
};


static constexpr size_t LATENCY_WINDOW_SAMPLES = 300;


struct LatencyWindow {
    std::array<
            LatencySample,
            LATENCY_WINDOW_SAMPLES
    > samples{};

    size_t count = 0;
};


static double percentile(
        std::vector<double> values,
        double fraction)
{
    if (values.empty()) {
        return 0.0;
    }

    std::sort(
            values.begin(),
            values.end()
    );

    const double position =
            fraction *
            static_cast<double>(
                    values.size() - 1
            );

    const size_t index =
            static_cast<size_t>(
                    position + 0.5
            );

    return
            values[
                    std::min(
                            index,
                            values.size() - 1
                    )
            ];
}


static void logLatencyWindow(
        LatencyWindow& window)
{
    if (window.count <
        LATENCY_WINDOW_SAMPLES) {

        return;
    }

    std::vector<double> total;
    std::vector<double> eofToPbo;
    std::vector<double> swap;

    total.reserve(
            window.count
    );

    eofToPbo.reserve(
            window.count
    );

    swap.reserve(
            window.count
    );

    double totalSum = 0.0;
    double eofToPboSum = 0.0;
    double uploadSum = 0.0;
    double drawSum = 0.0;
    double swapSum = 0.0;
    double totalMax = 0.0;

    for (size_t i = 0;
         i < window.count;
         ++i) {

        const LatencySample& sample =
                window.samples[i];

        total.push_back(
                sample.totalMs
        );

        eofToPbo.push_back(
                sample.t0ToT1Ms
        );

        swap.push_back(
                sample.t4ToT5Ms
        );

        totalSum +=
                sample.totalMs;

        eofToPboSum +=
                sample.t0ToT1Ms;

        uploadSum +=
                sample.t1ToT2Ms;

        drawSum +=
                sample.t2ToT3Ms;

        swapSum +=
                sample.t4ToT5Ms;

        totalMax =
                std::max(
                        totalMax,
                        sample.totalMs
                );
    }

    const double n =
            static_cast<double>(
                    window.count
            );

    LOGI(
            "Step 14 summary n=%zu | "
            "T0->T5 avg=%.3f ms "
            "p50=%.3f p95=%.3f max=%.3f | "
            "T0->T1 avg=%.3f p95=%.3f | "
            "T1->T2 avg=%.3f | "
            "T2->T3 avg=%.3f | "
            "T4->T5 swap avg=%.3f p95=%.3f",
            window.count,
            totalSum / n,
            percentile(
                    total,
                    0.50
            ),
            percentile(
                    total,
                    0.95
            ),
            totalMax,
            eofToPboSum / n,
            percentile(
                    eofToPbo,
                    0.95
            ),
            uploadSum / n,
            drawSum / n,
            swapSum / n,
            percentile(
                    swap,
                    0.95
            )
    );

    window.count = 0;
}


struct PresentationSample {
    double t0ToPresentMs = 0.0;
    double t5ToPresentMs = 0.0;

    // For planar MJPEG, T0 is B2 (decoded frame publication).  preT0Ms stores
    // the already-measured B0->B2 duration so B0->PRESENT can be reported
    // without mixing CLOCK_MONOTONIC with the BULK steady-clock timestamps.
    bool planarMjpeg = false;
    double preT0Ms = 0.0;
    double b0ToPresentMs = 0.0;

    bool jitTimingValid = false;

    // Positive: T4 happened before the predicted composite deadline.
    // Negative: T4 was already past that deadline.
    double t4ToDeadlineMs = 0.0;
    double t5ToDeadlineMs = 0.0;

    double t4ToLatchMs = 0.0;
    double deadlineToLatchMs = 0.0;
    double latchToPresentMs = 0.0;

    double compositorIntervalMs = 0.0;
    double compositorToPresentMs = 0.0;

    // actualPresent - (predictedDeadline + compositorToPresentLatency)
    double predictedPresentErrorMs = 0.0;
};


static constexpr size_t PRESENTATION_WINDOW_SAMPLES = 300;
static constexpr size_t PRESENTATION_PENDING_SLOTS = 64;


struct PresentationWindow {
    std::array<
            PresentationSample,
            PRESENTATION_WINDOW_SAMPLES
    > samples{};

    size_t count = 0;
};


struct PendingPresentation {
    bool valid = false;

    uint64_t eglFrameId = 0;
    uint64_t sequence = 0;

    bool planarMjpeg = false;
    double preT0Ms = 0.0;

    uint64_t frameReadyMonotonicNs = 0;
    uint64_t swapStartMonotonicNs = 0;
    uint64_t swapReturnMonotonicNs = 0;

    bool compositorTimingValid = false;
    int64_t compositeDeadlineNs = 0;
    int64_t compositeIntervalNs = 0;
    int64_t compositeToPresentLatencyNs = 0;
};


struct PresentationTracker {
    bool enabled = false;

    bool latchSupported = false;
    bool compositorTimingEnabled = false;

    std::array<
            PendingPresentation,
            PRESENTATION_PENDING_SLOTS
    > pending{};

    PresentationWindow window{};

    uint64_t resolved = 0;
    uint64_t pendingPolls = 0;
    uint64_t invalidTimestamps = 0;

    // EGL_BAD_ACCESS means the requested frame ID is no longer
    // queryable (typically dropped/expired from timestamp history).
    uint64_t badAccessFrames = 0;

    uint64_t queryErrors = 0;
    uint64_t queueOverwrites = 0;

    uint64_t compositorTimingQueries = 0;
    uint64_t compositorTimingErrors = 0;
    uint64_t jitTimingResolved = 0;
};


static void logPresentationWindow(
        PresentationWindow& window)
{
    if (window.count <
        PRESENTATION_WINDOW_SAMPLES) {

        return;
    }

    if (!ENABLE_VERBOSE_TIMING_LOG) {
        window.count = 0;
        return;
    }

    std::vector<double> total;
    std::vector<double> afterSwap;

    std::vector<double> t4ToDeadline;
    std::vector<double> t5ToDeadline;
    std::vector<double> t4ToLatch;
    std::vector<double> deadlineToLatch;
    std::vector<double> latchToPresent;
    std::vector<double> presentPredictionError;

    total.reserve(
            window.count
    );

    afterSwap.reserve(
            window.count
    );

    double totalSum = 0.0;
    double afterSwapSum = 0.0;
    double totalMax = 0.0;

    double intervalSum = 0.0;
    double compositorToPresentSum = 0.0;
    size_t jitCount = 0;


    for (size_t i = 0;
         i < window.count;
         ++i) {

        const PresentationSample& sample =
                window.samples[i];

        total.push_back(
                sample.t0ToPresentMs
        );

        afterSwap.push_back(
                sample.t5ToPresentMs
        );

        totalSum +=
                sample.t0ToPresentMs;

        afterSwapSum +=
                sample.t5ToPresentMs;

        totalMax =
                std::max(
                        totalMax,
                        sample.t0ToPresentMs
                );


        if (sample.jitTimingValid) {

            t4ToDeadline.push_back(
                    sample.t4ToDeadlineMs
            );

            t5ToDeadline.push_back(
                    sample.t5ToDeadlineMs
            );

            t4ToLatch.push_back(
                    sample.t4ToLatchMs
            );

            deadlineToLatch.push_back(
                    sample.deadlineToLatchMs
            );

            latchToPresent.push_back(
                    sample.latchToPresentMs
            );

            presentPredictionError.push_back(
                    sample.predictedPresentErrorMs
            );

            intervalSum +=
                    sample.compositorIntervalMs;

            compositorToPresentSum +=
                    sample.compositorToPresentMs;

            ++jitCount;
        }
    }


    const double n =
            static_cast<double>(
                    window.count
            );


    LOGI(
            "Step 15C.0 PRESENT summary n=%zu | "
            "T0->PRESENT avg=%.3f ms "
            "p50=%.3f p95=%.3f max=%.3f | "
            "T5->PRESENT avg=%.3f "
            "p50=%.3f p95=%.3f",
            window.count,
            totalSum / n,
            percentile(
                    total,
                    0.50
            ),
            percentile(
                    total,
                    0.95
            ),
            totalMax,
            afterSwapSum / n,
            percentile(
                    afterSwap,
                    0.50
            ),
            percentile(
                    afterSwap,
                    0.95
            )
    );


    std::vector<double> planarMjpegB0ToPresent;
    std::vector<double> planarMjpegB2ToPresent;

    for (size_t i = 0; i < window.count; ++i) {
        const PresentationSample& sample = window.samples[i];
        if (!sample.planarMjpeg) {
            continue;
        }
        planarMjpegB0ToPresent.push_back(sample.b0ToPresentMs);
        planarMjpegB2ToPresent.push_back(sample.t0ToPresentMs);
    }

    if (!planarMjpegB0ToPresent.empty()) {
        const double msN = static_cast<double>(planarMjpegB0ToPresent.size());
        LOGI(
                "PLANAR_MJPEG PRESENT summary n=%zu | "
                "B0->PRESENT avg=%.3f ms p50=%.3f p95=%.3f max=%.3f | "
                "B2->PRESENT avg=%.3f p50=%.3f p95=%.3f",
                planarMjpegB0ToPresent.size(),
                std::accumulate(planarMjpegB0ToPresent.begin(), planarMjpegB0ToPresent.end(), 0.0) / msN,
                percentile(planarMjpegB0ToPresent, 0.50),
                percentile(planarMjpegB0ToPresent, 0.95),
                percentile(planarMjpegB0ToPresent, 1.00),
                std::accumulate(planarMjpegB2ToPresent.begin(), planarMjpegB2ToPresent.end(), 0.0) / msN,
                percentile(planarMjpegB2ToPresent, 0.50),
                percentile(planarMjpegB2ToPresent, 0.95)
        );
    }


    if (jitCount > 0) {

        const double jitN =
                static_cast<double>(
                        jitCount
                );

        LOGI(
                "Step 15C.0 JIT timing n=%zu | "
                "T4->DEADLINE avg=%.3f ms "
                "p50=%.3f p95=%.3f | "
                "T5->DEADLINE avg=%.3f | "
                "T4->LATCH avg=%.3f | "
                "DEADLINE->LATCH avg=%.3f "
                "p50=%.3f p95=%.3f | "
                "LATCH->PRESENT avg=%.3f | "
                "INTERVAL avg=%.3f | "
                "COMPOSITE->PRESENT avg=%.3f | "
                "PRESENT prediction error avg=%.3f",
                jitCount,
                std::accumulate(
                        t4ToDeadline.begin(),
                        t4ToDeadline.end(),
                        0.0
                ) / jitN,
                percentile(
                        t4ToDeadline,
                        0.50
                ),
                percentile(
                        t4ToDeadline,
                        0.95
                ),
                std::accumulate(
                        t5ToDeadline.begin(),
                        t5ToDeadline.end(),
                        0.0
                ) / jitN,
                std::accumulate(
                        t4ToLatch.begin(),
                        t4ToLatch.end(),
                        0.0
                ) / jitN,
                std::accumulate(
                        deadlineToLatch.begin(),
                        deadlineToLatch.end(),
                        0.0
                ) / jitN,
                percentile(
                        deadlineToLatch,
                        0.50
                ),
                percentile(
                        deadlineToLatch,
                        0.95
                ),
                std::accumulate(
                        latchToPresent.begin(),
                        latchToPresent.end(),
                        0.0
                ) / jitN,
                intervalSum / jitN,
                compositorToPresentSum / jitN,
                std::accumulate(
                        presentPredictionError.begin(),
                        presentPredictionError.end(),
                        0.0
                ) / jitN
        );
    }


    window.count = 0;
}


struct CompositorTimingSnapshot {
    bool valid = false;

    int64_t deadlineNs = 0;
    int64_t intervalNs = 0;
    int64_t toPresentLatencyNs = 0;
};


static CompositorTimingSnapshot queryCompositorTiming(
        PresentationTracker& tracker,
        EGLDisplay display,
        EGLSurface surface)
{
    CompositorTimingSnapshot snapshot{};

    if (!tracker.compositorTimingEnabled ||
        p_eglGetCompositorTimingANDROID ==
            nullptr) {

        return snapshot;
    }


    const EGLint names[] = {
            EGL_COMPOSITE_DEADLINE_ANDROID,
            EGL_COMPOSITE_INTERVAL_ANDROID,
            EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID
    };

    int64_t values[] = {
            0,
            0,
            0
    };


    const EGLBoolean ok =
            p_eglGetCompositorTimingANDROID(
                    display,
                    surface,
                    3,
                    names,
                    values
            );


    if (ok != EGL_TRUE) {

        ++tracker.compositorTimingErrors;

        if (tracker.compositorTimingErrors <= 5) {

            LOGE(
                    "Step 15C.0: "
                    "eglGetCompositorTimingANDROID "
                    "failed EGL=0x%04X",
                    eglGetError()
            );
        }

        return snapshot;
    }


    ++tracker.compositorTimingQueries;

    snapshot.deadlineNs =
            values[0];

    snapshot.intervalNs =
            values[1];

    snapshot.toPresentLatencyNs =
            values[2];

    snapshot.valid =
            snapshot.deadlineNs > 0 &&
            snapshot.intervalNs > 0 &&
            snapshot.toPresentLatencyNs >= 0;


    return snapshot;
}


static void queuePresentationFrame(
        PresentationTracker& tracker,
        uint64_t eglFrameId,
        uint64_t sequence,
        uint64_t frameReadyMonotonicNs,
        uint64_t swapStartMonotonicNs,
        uint64_t swapReturnMonotonicNs,
        const CompositorTimingSnapshot& compositorTiming,
        bool planarMjpeg = false,
        double preT0Ms = 0.0)
{
    if (!tracker.enabled ||
        frameReadyMonotonicNs == 0 ||
        swapReturnMonotonicNs == 0) {

        return;
    }

    PendingPresentation* target = nullptr;

    for (PendingPresentation& item :
         tracker.pending) {

        if (!item.valid) {

            target = &item;
            break;
        }
    }

    if (target == nullptr) {

        target =
                &tracker.pending[0];

        for (PendingPresentation& item :
             tracker.pending) {

            if (item.eglFrameId <
                target->eglFrameId) {

                target = &item;
            }
        }

        ++tracker.queueOverwrites;
    }

    target->valid = true;
    target->eglFrameId = eglFrameId;
    target->sequence = sequence;
    target->planarMjpeg = planarMjpeg;
    target->preT0Ms = preT0Ms;
    target->frameReadyMonotonicNs =
            frameReadyMonotonicNs;
    target->swapStartMonotonicNs =
            swapStartMonotonicNs;
    target->swapReturnMonotonicNs =
            swapReturnMonotonicNs;

    target->compositorTimingValid =
            compositorTiming.valid;

    target->compositeDeadlineNs =
            compositorTiming.deadlineNs;

    target->compositeIntervalNs =
            compositorTiming.intervalNs;

    target->compositeToPresentLatencyNs =
            compositorTiming.toPresentLatencyNs;
}


static void pollPresentationTimestamps(
        PresentationTracker& tracker,
        EGLDisplay display,
        EGLSurface surface)
{
    if (!tracker.enabled ||
        p_eglGetFrameTimestampsANDROID ==
            nullptr) {

        return;
    }


    const EGLint timestampNames[] = {
            EGL_COMPOSITION_LATCH_TIME_ANDROID,
            EGL_DISPLAY_PRESENT_TIME_ANDROID
    };


    for (PendingPresentation& item :
         tracker.pending) {

        if (!item.valid) {
            continue;
        }


        int64_t values[] = {
                EGL_TIMESTAMP_INVALID_ANDROID,
                EGL_TIMESTAMP_INVALID_ANDROID
        };


        const EGLint queryCount =
                tracker.latchSupported
                ? 2
                : 1;

        const EGLint* names =
                tracker.latchSupported
                ? timestampNames
                : &timestampNames[1];

        int64_t* queryValues =
                tracker.latchSupported
                ? values
                : &values[1];


        const EGLBoolean ok =
                p_eglGetFrameTimestampsANDROID(
                        display,
                        surface,
                        item.eglFrameId,
                        queryCount,
                        names,
                        queryValues
                );


        if (ok != EGL_TRUE) {

            const EGLint error =
                    eglGetError();


            if (error ==
                EGL_BAD_ACCESS) {

                ++tracker.badAccessFrames;

                if (tracker.badAccessFrames <= 3 ||
                    (tracker.badAccessFrames % 300) == 0) {

                    LOGI(
                            "Step 15C.0: timestamp "
                            "frame expired/dropped "
                            "frameId=%llu seq=%llu "
                            "count=%llu",
                            static_cast<unsigned long long>(
                                    item.eglFrameId
                            ),
                            static_cast<unsigned long long>(
                                    item.sequence
                            ),
                            static_cast<unsigned long long>(
                                    tracker.badAccessFrames
                            )
                    );
                }
            }
            else {

                ++tracker.queryErrors;

                LOGE(
                        "Step 15C.0: timestamp query "
                        "failed frameId=%llu "
                        "seq=%llu EGL=0x%04X",
                        static_cast<unsigned long long>(
                                item.eglFrameId
                        ),
                        static_cast<unsigned long long>(
                                item.sequence
                        ),
                        error
                );
            }

            item.valid = false;

            continue;
        }


        const int64_t latchValue =
                values[0];

        const int64_t presentValue =
                values[1];


        if (presentValue ==
            EGL_TIMESTAMP_PENDING_ANDROID) {

            ++tracker.pendingPolls;

            continue;
        }


        if (presentValue ==
                EGL_TIMESTAMP_INVALID_ANDROID ||
            presentValue < 0) {

            ++tracker.invalidTimestamps;

            item.valid = false;

            continue;
        }


        const uint64_t presentNs =
                static_cast<uint64_t>(
                        presentValue
                );


        if (presentNs <
            item.frameReadyMonotonicNs) {

            ++tracker.invalidTimestamps;

            LOGE(
                    "Step 15C.0: present timestamp "
                    "before T0 frameId=%llu seq=%llu",
                    static_cast<unsigned long long>(
                            item.eglFrameId
                    ),
                    static_cast<unsigned long long>(
                            item.sequence
                    )
            );

            item.valid = false;

            continue;
        }


        PresentationSample sample{};

        sample.t0ToPresentMs =
                nsToMs(
                        presentNs -
                        item.frameReadyMonotonicNs
                );

        sample.planarMjpeg = item.planarMjpeg;
        sample.preT0Ms = item.preT0Ms;
        sample.b0ToPresentMs =
                item.planarMjpeg
                ? item.preT0Ms + sample.t0ToPresentMs
                : 0.0;


        const int64_t afterSwapNs =
                static_cast<int64_t>(
                        presentNs
                ) -
                static_cast<int64_t>(
                        item.swapReturnMonotonicNs
                );

        sample.t5ToPresentMs =
                static_cast<double>(
                        afterSwapNs
                ) /
                1'000'000.0;


        const bool latchValid =
                tracker.latchSupported &&
                latchValue !=
                    EGL_TIMESTAMP_PENDING_ANDROID &&
                latchValue !=
                    EGL_TIMESTAMP_INVALID_ANDROID &&
                latchValue > 0;


        if (latchValid &&
            item.compositorTimingValid &&
            item.swapStartMonotonicNs > 0) {

            const int64_t t4Ns =
                    static_cast<int64_t>(
                            item.swapStartMonotonicNs
                    );

            const int64_t predictedPresentNs =
                    item.compositeDeadlineNs +
                    item.compositeToPresentLatencyNs;


            sample.jitTimingValid = true;

            sample.t4ToDeadlineMs =
                    static_cast<double>(
                            item.compositeDeadlineNs -
                            t4Ns
                    ) /
                    1'000'000.0;

            sample.t5ToDeadlineMs =
                    static_cast<double>(
                            item.compositeDeadlineNs -
                            static_cast<int64_t>(
                                    item.swapReturnMonotonicNs
                            )
                    ) /
                    1'000'000.0;

            sample.t4ToLatchMs =
                    static_cast<double>(
                            latchValue -
                            t4Ns
                    ) /
                    1'000'000.0;

            sample.deadlineToLatchMs =
                    static_cast<double>(
                            latchValue -
                            item.compositeDeadlineNs
                    ) /
                    1'000'000.0;

            sample.latchToPresentMs =
                    static_cast<double>(
                            presentValue -
                            latchValue
                    ) /
                    1'000'000.0;

            sample.compositorIntervalMs =
                    static_cast<double>(
                            item.compositeIntervalNs
                    ) /
                    1'000'000.0;

            sample.compositorToPresentMs =
                    static_cast<double>(
                            item.compositeToPresentLatencyNs
                    ) /
                    1'000'000.0;

            sample.predictedPresentErrorMs =
                    static_cast<double>(
                            presentValue -
                            predictedPresentNs
                    ) /
                    1'000'000.0;

            ++tracker.jitTimingResolved;
        }


        ++tracker.resolved;


        if (tracker.window.count <
            PRESENTATION_WINDOW_SAMPLES) {

            tracker.window.samples[
                    tracker.window.count
            ] = sample;

            ++tracker.window.count;
        }


        if (ENABLE_VERBOSE_TIMING_LOG &&
            (tracker.resolved <= 5 ||
             (tracker.resolved % 30) == 0)) {

            if (sample.jitTimingValid) {

                LOGI(
                        "Step 15C.0 frame "
                        "seq=%llu frameId=%llu | "
                        "T0->PRESENT=%.3f ms "
                        "T5->PRESENT=%.3f | "
                        "T4->DEADLINE=%.3f "
                        "T5->DEADLINE=%.3f "
                        "T4->LATCH=%.3f "
                        "DEADLINE->LATCH=%.3f "
                        "LATCH->PRESENT=%.3f | "
                        "interval=%.3f "
                        "comp->present=%.3f "
                        "predictionErr=%.3f",
                        static_cast<unsigned long long>(
                                item.sequence
                        ),
                        static_cast<unsigned long long>(
                                item.eglFrameId
                        ),
                        sample.t0ToPresentMs,
                        sample.t5ToPresentMs,
                        sample.t4ToDeadlineMs,
                        sample.t5ToDeadlineMs,
                        sample.t4ToLatchMs,
                        sample.deadlineToLatchMs,
                        sample.latchToPresentMs,
                        sample.compositorIntervalMs,
                        sample.compositorToPresentMs,
                        sample.predictedPresentErrorMs
                );
            }
            else {

                LOGI(
                        "Step 15C.0 frame "
                        "seq=%llu frameId=%llu | "
                        "T0->PRESENT=%.3f ms "
                        "T5->PRESENT=%.3f ms "
                        "(JIT timing unavailable)",
                        static_cast<unsigned long long>(
                                item.sequence
                        ),
                        static_cast<unsigned long long>(
                                item.eglFrameId
                        ),
                        sample.t0ToPresentMs,
                        sample.t5ToPresentMs
                );
            }
        }


        if (ENABLE_VERBOSE_TIMING_LOG &&
            sample.planarMjpeg &&
            (tracker.resolved <= 5 ||
             (tracker.resolved % 30) == 0)) {

            LOGI(
                    "PLANAR_MJPEG PRESENT seq=%llu frameId=%llu | "
                    "B0->B2=%.3f ms B2->PRESENT=%.3f ms "
                    "B0->PRESENT=%.3f ms T5->PRESENT=%.3f ms",
                    static_cast<unsigned long long>(item.sequence),
                    static_cast<unsigned long long>(item.eglFrameId),
                    sample.preT0Ms,
                    sample.t0ToPresentMs,
                    sample.b0ToPresentMs,
                    sample.t5ToPresentMs
            );
        }


        logPresentationWindow(
                tracker.window
        );

        item.valid = false;
    }
}


// ------------------------------------------------------------
// Render thread
// ------------------------------------------------------------

static void renderLoop(ANativeWindow* window)
{
    LOGI("Render thread started");

    // --------------------------------------------------------
    // EGL Display
    // --------------------------------------------------------

    EGLDisplay display =
            eglGetDisplay(EGL_DEFAULT_DISPLAY);

    if (display == EGL_NO_DISPLAY) {
        logEglError("eglGetDisplay");
        gRunning = false;
        return;
    }


    EGLint major = 0;
    EGLint minor = 0;

    if (!eglInitialize(
            display,
            &major,
            &minor)) {

        logEglError("eglInitialize");
        gRunning = false;
        return;
    }

    LOGI(
            "EGL version %d.%d",
            major,
            minor
    );


    // --------------------------------------------------------
    // GLES API
    // --------------------------------------------------------

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        logEglError("eglBindAPI");

        eglTerminate(display);
        gRunning = false;
        return;
    }


    // --------------------------------------------------------
    // EGL Config
    // GLES 3.x
    // RGB888
    // --------------------------------------------------------

    const EGLint configAttribs[] = {

            EGL_SURFACE_TYPE,
            EGL_WINDOW_BIT,

            EGL_RENDERABLE_TYPE,
            EGL_OPENGL_ES3_BIT_KHR,

            EGL_RED_SIZE,   8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE,  8,

            EGL_NONE
    };


    EGLConfig config = nullptr;
    EGLint numConfigs = 0;

    if (!eglChooseConfig(
            display,
            configAttribs,
            &config,
            1,
            &numConfigs)
        || numConfigs < 1) {

        logEglError("eglChooseConfig");

        eglTerminate(display);
        gRunning = false;
        return;
    }


    // --------------------------------------------------------
    // Step 15C.1:
    // inspect the selected EGLConfig's swap interval range.
    // --------------------------------------------------------

    EGLint minSwapInterval = -1;
    EGLint maxSwapInterval = -1;

    const EGLBoolean minSwapOk =
            eglGetConfigAttrib(
                    display,
                    config,
                    EGL_MIN_SWAP_INTERVAL,
                    &minSwapInterval
            );

    const EGLBoolean maxSwapOk =
            eglGetConfigAttrib(
                    display,
                    config,
                    EGL_MAX_SWAP_INTERVAL,
                    &maxSwapInterval
            );

    LOGI(
            "Step 15C.1: EGL swap interval range "
            "min=%d max=%d queryOK=%s/%s",
            minSwapInterval,
            maxSwapInterval,
            minSwapOk == EGL_TRUE ? "YES" : "NO",
            maxSwapOk == EGL_TRUE ? "YES" : "NO"
    );


    // --------------------------------------------------------
    // GLES 3 context
    // --------------------------------------------------------

    const EGLint contextAttribs[] = {

            EGL_CONTEXT_CLIENT_VERSION,
            3,

            EGL_NONE
    };


    EGLContext context =
            eglCreateContext(
                    display,
                    config,
                    EGL_NO_CONTEXT,
                    contextAttribs
            );

    if (context == EGL_NO_CONTEXT) {

        logEglError("eglCreateContext");

        eglTerminate(display);
        gRunning = false;
        return;
    }


    // --------------------------------------------------------
    // SurfaceView -> EGLSurface
    // --------------------------------------------------------

    EGLSurface surface =
            eglCreateWindowSurface(
                    display,
                    config,
                    window,
                    nullptr
            );

    if (surface == EGL_NO_SURFACE) {

        logEglError("eglCreateWindowSurface");

        eglDestroyContext(
                display,
                context
        );

        eglTerminate(display);

        gRunning = false;
        return;
    }


    // --------------------------------------------------------
    // Context bind
    // --------------------------------------------------------

    if (!eglMakeCurrent(
            display,
            surface,
            surface,
            context)) {

        logEglError("eglMakeCurrent");

        eglDestroySurface(
                display,
                surface
        );

        eglDestroyContext(
                display,
                context
        );

        eglTerminate(display);

        gRunning = false;
        return;
    }


    // --------------------------------------------------------
    // Step 15C.1:
    // direct unsynchronized-swap experiment.
    //
    // Keep all Step 15C.0 timing instrumentation unchanged so the
    // before/after comparison is directly measurable.
    // --------------------------------------------------------

    const EGLBoolean swapIntervalZeroOk =
            eglSwapInterval(
                    display,
                    0
            );

    if (swapIntervalZeroOk == EGL_TRUE) {

        LOGI(
                "Step 15C.1: eglSwapInterval(0) = EGL_TRUE "
                "(config min=%d max=%d)",
                minSwapInterval,
                maxSwapInterval
        );
    }
    else {

        const EGLint swapIntervalError =
                eglGetError();

        LOGE(
                "Step 15C.1: eglSwapInterval(0) = EGL_FALSE "
                "EGL=0x%04X "
                "(config min=%d max=%d)",
                swapIntervalError,
                minSwapInterval,
                maxSwapInterval
        );
    }


    // --------------------------------------------------------
    // Surface dimensions
    // --------------------------------------------------------

    EGLint width = 0;
    EGLint height = 0;

    eglQuerySurface(
            display,
            surface,
            EGL_WIDTH,
            &width
    );

    eglQuerySurface(
            display,
            surface,
            EGL_HEIGHT,
            &height
    );


    const UiLayout uiLayout =
            selectUiLayout(
                    width,
                    height
            );

    const UiCanvasViewport uiCanvas =
            calculateUiCanvasViewport(
                    width,
                    height,
                    uiLayout
            );

    const Viewport videoViewport =
            uiLogicalRectToViewport(
                    uiCanvas,
                    uiLayout.preview
            );

    const Viewport waveformViewport =
            uiLogicalRectToViewport(
                    uiCanvas,
                    uiLayout.waveform
            );

    const Viewport paradeViewport =
            uiLogicalRectToViewport(
                    uiCanvas,
                    uiLayout.parade
            );

    const Viewport histogramViewport =
            uiLogicalRectToViewport(
                    uiCanvas,
                    uiLayout.histogram
            );

    // Preserve the cropped 239:200 vectorscope shader geometry.
    // This keeps the measured circle circular in the wider portrait panel.
    const field_monitor::RectI vectorscopeContentRect =
            aspectFitRect(
                    uiLayout.vectorscope,
                    239,
                    200
            );

    const Viewport vectorscopeViewport =
            uiLogicalRectToViewport(
                    uiCanvas,
                    vectorscopeContentRect
            );


    LOGI(
            "Step 15.7 layout: mode=%s surface=%dx%d "
            "logical=%dx%d canvas=%dx%d+%d+%d scale=%.4f "
            "preview=%dx%d+%d+%d waveform=%dx%d+%d+%d parade=%dx%d+%d+%d "
            "histogram=%dx%d+%d+%d vectorData=%dx%d+%d+%d",
            uiLayout.mode == UiLayoutMode::Portrait ? "PORTRAIT" : "LANDSCAPE",
            width,
            height,
            uiLayout.canvasWidth,
            uiLayout.canvasHeight,
            uiCanvas.width,
            uiCanvas.height,
            uiCanvas.x,
            uiCanvas.y,
            uiCanvas.scale,
            videoViewport.width,
            videoViewport.height,
            videoViewport.x,
            videoViewport.y,
            waveformViewport.width,
            waveformViewport.height,
            waveformViewport.x,
            waveformViewport.y,
            paradeViewport.width,
            paradeViewport.height,
            paradeViewport.x,
            paradeViewport.y,
            histogramViewport.width,
            histogramViewport.height,
            histogramViewport.x,
            histogramViewport.y,
            vectorscopeViewport.width,
            vectorscopeViewport.height,
            vectorscopeViewport.x,
            vectorscopeViewport.y
    );

    // --------------------------------------------------------
    // Log GPU information
    // --------------------------------------------------------

    const GLubyte* version =
            glGetString(GL_VERSION);

    const GLubyte* vendor =
            glGetString(GL_VENDOR);

    const GLubyte* renderer =
            glGetString(GL_RENDERER);


    LOGI(
            "Surface = %d x %d",
            width,
            height
    );

    LOGI(
            "GL_VERSION = %s",
            version ? reinterpret_cast<const char*>(version) : "null"
    );

    LOGI(
            "GL_VENDOR = %s",
            vendor ? reinterpret_cast<const char*>(vendor) : "null"
    );

    LOGI(
            "GL_RENDERER = %s",
            renderer ? reinterpret_cast<const char*>(renderer) : "null"
    );

// --------------------------------------------------------
// OpenGL ES capabilities
// --------------------------------------------------------

    GLint numExtensions = 0;

    glGetIntegerv(
            GL_NUM_EXTENSIONS,
            &numExtensions
    );

    LOGI(
            "GL_NUM_EXTENSIONS = %d",
            numExtensions
    );


    const char* glTargets[] = {

            "GL_EXT_buffer_storage",

            "GL_EXT_disjoint_timer_query",

            "GL_EXT_disjoint_timer_query_webgl2",

            "GL_OES_EGL_image",

            "GL_OES_EGL_image_external",

            "GL_EXT_YUV_target"
    };


    for (const char* ext : glTargets) {

        LOGI(
                "%s = %s",
                ext,
                hasGlExtension(ext)
                ? "YES"
                : "NO"
        );
    }


// --------------------------------------------------------
// EGL capabilities
// --------------------------------------------------------

    const char* eglVendor =
            eglQueryString(
                    display,
                    EGL_VENDOR
            );

    const char* eglVersion =
            eglQueryString(
                    display,
                    EGL_VERSION
            );


    LOGI(
            "EGL_VENDOR = %s",
            eglVendor
            ? eglVendor
            : "null"
    );

    LOGI(
            "EGL_VERSION_STRING = %s",
            eglVersion
            ? eglVersion
            : "null"
    );


    const char* eglTargets[] = {

            "EGL_KHR_fence_sync",

            "EGL_ANDROID_native_fence_sync",

            "EGL_ANDROID_get_frame_timestamps",

            "EGL_ANDROID_presentation_time",

            "EGL_KHR_image",

            "EGL_KHR_image_base",

            "EGL_ANDROID_image_native_buffer",

            "EGL_ANDROID_get_native_client_buffer",

            "EGL_ANDROID_front_buffer_auto_refresh",

            "EGL_EXT_buffer_age",

            "EGL_KHR_swap_buffers_with_damage"
    };


    for (const char* ext : eglTargets) {

        LOGI(
                "%s = %s",
                ext,
                hasEglExtension(
                        display,
                        ext
                )
                ? "YES"
                : "NO"
        );
    }

// --------------------------------------------------------
// Step 15A front-buffer probe retired.
//
// A202ZT/Gralloc4 rejected AHARDWAREBUFFER_USAGE_FRONT_BUFFER.
// Do not run that probe on startup anymore.
// --------------------------------------------------------


// --------------------------------------------------------
// EGL_ANDROID_get_frame_timestamps
// --------------------------------------------------------

    PresentationTracker presentationTracker{};

    const bool frameTimestampsExtension =
            UVCFM_DIAGNOSTICS_ENABLED &&
            hasEglExtension(
                    display,
                    "EGL_ANDROID_get_frame_timestamps"
            );

    if (frameTimestampsExtension) {

        p_eglGetNextFrameIdANDROID =
                reinterpret_cast<
                    PFNEGLGETNEXTFRAMEIDANDROIDPROC_LOCAL
                >(
                    eglGetProcAddress(
                            "eglGetNextFrameIdANDROID"
                    )
                );

        p_eglGetFrameTimestampsANDROID =
                reinterpret_cast<
                    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC_LOCAL
                >(
                    eglGetProcAddress(
                            "eglGetFrameTimestampsANDROID"
                    )
                );

        p_eglGetFrameTimestampSupportedANDROID =
                reinterpret_cast<
                    PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC_LOCAL
                >(
                    eglGetProcAddress(
                            "eglGetFrameTimestampSupportedANDROID"
                    )
                );

        p_eglGetCompositorTimingANDROID =
                reinterpret_cast<
                    PFNEGLGETCOMPOSITORTIMINGANDROIDPROC_LOCAL
                >(
                    eglGetProcAddress(
                            "eglGetCompositorTimingANDROID"
                    )
                );

        p_eglGetCompositorTimingSupportedANDROID =
                reinterpret_cast<
                    PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC_LOCAL
                >(
                    eglGetProcAddress(
                            "eglGetCompositorTimingSupportedANDROID"
                    )
                );

        const bool entryPointsReady =
                p_eglGetNextFrameIdANDROID != nullptr &&
                p_eglGetFrameTimestampsANDROID != nullptr &&
                p_eglGetFrameTimestampSupportedANDROID != nullptr;

        if (!entryPointsReady) {

            LOGE(
                    "Step 13.1: frame timestamp "
                    "entry point missing"
            );
        }
        else {

            const EGLBoolean enableOk =
                    eglSurfaceAttrib(
                            display,
                            surface,
                            EGL_TIMESTAMPS_ANDROID,
                            EGL_TRUE
                    );

            if (enableOk != EGL_TRUE) {

                LOGE(
                        "Step 13.1: enabling "
                        "EGL_TIMESTAMPS_ANDROID failed "
                        "EGL=0x%04X",
                        eglGetError()
                );
            }
            else {

                const EGLBoolean presentSupported =
                        p_eglGetFrameTimestampSupportedANDROID(
                                display,
                                surface,
                                EGL_DISPLAY_PRESENT_TIME_ANDROID
                        );

                const EGLBoolean latchSupported =
                        p_eglGetFrameTimestampSupportedANDROID(
                                display,
                                surface,
                                EGL_COMPOSITION_LATCH_TIME_ANDROID
                        );

                presentationTracker.enabled =
                        presentSupported ==
                        EGL_TRUE;

                presentationTracker.latchSupported =
                        latchSupported ==
                        EGL_TRUE;


                bool deadlineSupported = false;
                bool intervalSupported = false;
                bool toPresentSupported = false;


                if (p_eglGetCompositorTimingANDROID != nullptr &&
                    p_eglGetCompositorTimingSupportedANDROID != nullptr) {

                    deadlineSupported =
                            p_eglGetCompositorTimingSupportedANDROID(
                                    display,
                                    surface,
                                    EGL_COMPOSITE_DEADLINE_ANDROID
                            ) == EGL_TRUE;

                    intervalSupported =
                            p_eglGetCompositorTimingSupportedANDROID(
                                    display,
                                    surface,
                                    EGL_COMPOSITE_INTERVAL_ANDROID
                            ) == EGL_TRUE;

                    toPresentSupported =
                            p_eglGetCompositorTimingSupportedANDROID(
                                    display,
                                    surface,
                                    EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID
                            ) == EGL_TRUE;
                }


                presentationTracker.compositorTimingEnabled =
                        deadlineSupported &&
                        intervalSupported &&
                        toPresentSupported;


                LOGI(
                        "Step 15C.0 timing support: "
                        "DEADLINE=%s INTERVAL=%s "
                        "COMPOSITE_TO_PRESENT=%s "
                        "LATCH=%s PRESENT=%s",
                        deadlineSupported
                        ? "YES"
                        : "NO",
                        intervalSupported
                        ? "YES"
                        : "NO",
                        toPresentSupported
                        ? "YES"
                        : "NO",
                        presentationTracker.latchSupported
                        ? "YES"
                        : "NO",
                        presentationTracker.enabled
                        ? "YES"
                        : "NO"
                );
            }
        }
    }


// --------------------------------------------------------
// Step 13.2 A/B:
// EGL_ANDROID_presentation_time desired-present hint
// --------------------------------------------------------

    bool presentationTimeHintEnabled = false;

    uint64_t presentationTimeHintCalls = 0;
    uint64_t presentationTimeHintErrors = 0;


    if (UVCFM_DIAGNOSTICS_ENABLED &&
        STEP13_2_PRESENTATION_TIME_HINT_NOW) {

        const bool presentationTimeExtension =
                hasEglExtension(
                        display,
                        "EGL_ANDROID_presentation_time"
                );

        if (!presentationTimeExtension) {

            LOGE(
                    "Step 13.2 B: "
                    "EGL_ANDROID_presentation_time "
                    "not available"
            );
        }
        else {

            p_eglPresentationTimeANDROID =
                    reinterpret_cast<
                        PFNEGLPRESENTATIONTIMEANDROIDPROC_LOCAL
                    >(
                        eglGetProcAddress(
                                "eglPresentationTimeANDROID"
                        )
                    );

            presentationTimeHintEnabled =
                    p_eglPresentationTimeANDROID !=
                    nullptr;

            if (!presentationTimeHintEnabled) {

                LOGE(
                        "Step 13.2 B: "
                        "eglPresentationTimeANDROID "
                        "entry point missing"
                );
            }
        }
    }


    if (UVCFM_DIAGNOSTICS_ENABLED) {
        LOGI(
                "Step 13.2 A/B mode: %s",
                presentationTimeHintEnabled
                ? "B = eglPresentationTimeANDROID(now)"
                : "A = Step 13.1 baseline / hint disabled"
        );
    }


// --------------------------------------------------------
// GL_EXT_buffer_storage entry point
// --------------------------------------------------------

    p_glBufferStorageEXT =
            reinterpret_cast<PFNGLBUFFERSTORAGEEXTPROC_LOCAL>(
                    eglGetProcAddress(
                            "glBufferStorageEXT"
                    )
            );


    if (p_glBufferStorageEXT == nullptr) {

        LOGE(
                "glBufferStorageEXT not found"
        );

        gRunning = false;
    }


// --------------------------------------------------------
// Double persistent PBO for decoded planar MJPEG YCbCr 4:2:2.
// --------------------------------------------------------

    const GLbitfield storageFlags =
            GL_MAP_WRITE_BIT |
            GL_MAP_PERSISTENT_BIT_EXT |
            GL_MAP_COHERENT_BIT_EXT;

    PboSlot planarMjpegPbo[2];

    for (auto& slot : planarMjpegPbo) {
        glGenBuffers(1, &slot.id);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot.id);

        p_glBufferStorageEXT(
                GL_PIXEL_UNPACK_BUFFER,
                PLANAR_MJPEG_YUV422_FRAME_BYTES,
                nullptr,
                storageFlags
        );

        slot.mapped =
                reinterpret_cast<uint8_t*>(
                        glMapBufferRange(
                                GL_PIXEL_UNPACK_BUFFER,
                                0,
                                PLANAR_MJPEG_YUV422_FRAME_BYTES,
                                storageFlags
                        )
                );

        if (slot.mapped == nullptr) {
            LOGE(
                    "Planar MJPEG persistent YUV422 PBO mapping failed"
            );
            gRunning = false;
            break;
        }

        LOGI(
                "Planar MJPEG persistent YUV422 PBO mapped: "
                "id=%u ptr=%p bytes=%zu",
                slot.id,
                slot.mapped,
                PLANAR_MJPEG_YUV422_FRAME_BYTES
        );
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);


    GLuint planarMjpegYTexture = 0;
    GLuint planarMjpegCbTexture = 0;
    GLuint planarMjpegCrTexture = 0;

    const auto createPlaneTexture = [](
            GLuint& outTexture,
            GLsizei planeWidth,
            GLsizei planeHeight) {

        glGenTextures(1, &outTexture);
        glBindTexture(GL_TEXTURE_2D, outTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(
                GL_TEXTURE_2D,
                1,
                GL_R8,
                planeWidth,
                planeHeight
        );
    };

    createPlaneTexture(
            planarMjpegYTexture,
            PLANAR_MJPEG_FRAME_W,
            PLANAR_MJPEG_FRAME_H
    );

    createPlaneTexture(
            planarMjpegCbTexture,
            PLANAR_MJPEG_CHROMA_W,
            PLANAR_MJPEG_FRAME_H
    );

    createPlaneTexture(
            planarMjpegCrTexture,
            PLANAR_MJPEG_CHROMA_W,
            PLANAR_MJPEG_FRAME_H
    );


    GLuint planarMjpegProgram =
            createPlanarMjpegYuvProgram();

    if (!planarMjpegProgram) {
        LOGE(
                "Planar MJPEG YCbCr shader program creation failed"
        );
        gRunning = false;
    }


    GLuint vao = 0;

    glGenVertexArrays(
            1,
            &vao
    );

    glBindVertexArray(
            vao
    );


    // --------------------------------------------------------
    // Step 15.4 modules: CM4 UI + shared GPU scope backend.
    //
    // native-lib.cpp owns orchestration only. The already-uploaded planar
    // Y/Cb/Cr textures feed Waveform + RGB Parade + Histogram + Vectorscope.
    // --------------------------------------------------------

    field_monitor::ScopeUi scopeUi;
    field_monitor::MonitorUiController& monitorUi =
            field_monitor::monitorUiController();

    if (!scopeUi.initialize()) {
        gRunning = false;
    }

    field_monitor::ScopeGpu scopeGpu;
    scopeGpu.initialize();

    // --------------------------------------------------------
    // Step 15D: try the persistent front-buffer path on Android 16.
    // Unsupported devices continue through the existing EGL/BufferQueue path.
    // --------------------------------------------------------

    Step15DFrontBuffer frontBuffer{};

    createStep15DFrontBuffer(
            display,
            window,
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            frontBuffer
    );


    auto drawStartupUiShell = [&]() {
        const calibration_profile::Profile startupCalibration =
                calibration_profile::snapshot();
        monitorUi.setHdrNitsAvailable(
                startupCalibration.enabled && startupCalibration.pqInput);
        const field_monitor::MonitorUiSnapshot uiSnapshot =
                monitorUi.snapshot();
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        scopeUi.drawOverlay(
                0.0,
                width,
                height,
                uiLayout,
                uiCanvas,
                startupCalibration.enabled,
                startupCalibration.width,
                startupCalibration.height,
                startupCalibration.limitedInput,
                startupCalibration.enabled
                        ? startupCalibration.colorimetry
                        : 0,
                uiSnapshot,
                vao
        );
        scopeUi.drawMonitorForeground(width, height, vao);
    };


    // Step 15.7.1: publish one non-camera UI shell immediately after EGL/UI
    // initialization.  With Step 15D active this is the initial
    // SurfaceControl buffer publication; later frames re-submit the same AHB.
    if (frontBuffer.active) {

        glBindFramebuffer(
                GL_FRAMEBUFFER,
                frontBuffer.framebuffer
        );

        drawStartupUiShell();

        if (!submitStep15DFrontBuffer(frontBuffer, true)) {

            LOGE(
                    "Step 15D: initial publish failed; "
                    "falling back to EGL/BufferQueue"
            );

            destroyStep15DFrontBuffer(
                    display,
                    frontBuffer
            );
        }
    }


    if (!frontBuffer.active) {

        glBindFramebuffer(
                GL_FRAMEBUFFER,
                0
        );

        drawStartupUiShell();

        const EGLBoolean primingSwapOk =
                eglSwapBuffers(display, surface);

        LOGI(
                "Step 15.7.1: startup UI shell swap=%s",
                primingSwapOk == EGL_TRUE ? "OK" : "FAILED"
        );
    }

    // --------------------------------------------------------
    // Step 14: event-driven render loop
    //
    // UVC FRAME READY
    //   -> copy newest frame only
    //   -> upload / draw / present once (SurfaceControl FRONT or EGL swap)
    // UI STATE DIRTY
    //   -> reuse the latest uploaded textures
    //   -> draw / present once without camera upload or scope compute
    // Both sources wake the same condition-variable wait.
    //
    // No duplicate redraw/swap of the same camera frame.
    // No video FIFO.
    // No PBO wait.
    // --------------------------------------------------------

    uint64_t frame = 0;
    uint64_t latestPlanarMjpegSequence = 0;
    uint64_t lastPlanarMjpegWakeSequence = 0;
    uint64_t liveUploads = 0;
    uint64_t wakeTimeouts = 0;
    uint64_t planarMjpegPboBusySkips = 0;
    uint64_t planarMjpegCopyRaceSkips = 0;

    int preferredPlanarMjpegPbo = 1;


    LOGI(
            "Step 14: event-driven renderer active; "
            "present on new UVC frame or UI state change"
    );

    LOGI(
            "Step 15D: present path = %s",
            frontBuffer.active
            ? "PERSISTENT_FRONT_BUFFER"
            : "EGL_BUFFERQUEUE"
    );

    LOGI(
            "Step 15C.1: swapInterval(0) experiment active=%s",
            swapIntervalZeroOk == EGL_TRUE
            ? "YES"
            : "NO"
    );


    // Step 15.1 UI FPS state remains in the render loop; drawing itself
    // is delegated to ScopeUi.
    double uiFps = 0.0;
    uint64_t uiFpsFrames = 0;
    uint64_t uiFpsWindowStartNs =
            nowMonotonicRawNs();

    bool haveUploadedFrame = false;
    uint64_t lastRenderedUiRevision =
            monitorUi.snapshot().revision;
    uint64_t lastRenderWakeSequence =
            uvc_mjpeg_decoder::currentRenderWakeSequence();

    const auto drawComposedFrame =
            [&](double currentUiFps,
                const calibration_profile::Profile& calibration,
                const field_monitor::MonitorUiSnapshot& uiSnapshot,
                bool drawVideo) {
        glBindFramebuffer(
                GL_FRAMEBUFFER,
                frontBuffer.active
                ? frontBuffer.framebuffer
                : 0);

        glViewport(
                videoViewport.x,
                videoViewport.y,
                videoViewport.width,
                videoViewport.height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (drawVideo) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, planarMjpegYTexture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, planarMjpegCbTexture);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, planarMjpegCrTexture);
            glUseProgram(planarMjpegProgram);

            glUniform1i(
                    glGetUniformLocation(
                            planarMjpegProgram,
                            "uCalibrationEnabled"),
                    calibration.enabled ? 1 : 0);
            glUniform3fv(
                    glGetUniformLocation(
                            planarMjpegProgram,
                            "uCalibrationRow0"),
                    1,
                    calibration.matrix.data());
            glUniform3fv(
                    glGetUniformLocation(
                            planarMjpegProgram,
                            "uCalibrationRow1"),
                    1,
                    calibration.matrix.data() + 3);
            glUniform3fv(
                    glGetUniformLocation(
                            planarMjpegProgram,
                            "uCalibrationRow2"),
                    1,
                    calibration.matrix.data() + 6);
            const GLfloat normalizedOffset[3] = {
                    calibration.offsetCode[0] / 255.0f,
                    calibration.offsetCode[1] / 255.0f,
                    calibration.offsetCode[2] / 255.0f,
            };
            glUniform3fv(
                    glGetUniformLocation(
                            planarMjpegProgram,
                            "uCalibrationOffset"),
                    1,
                    normalizedOffset);
            glUniform1i(
                    glGetUniformLocation(
                            planarMjpegProgram,
                            "uPqToSdr"),
                    calibration.pqInput ? 1 : 0);

            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        scopeUi.drawHistogram(
                scopeGpu.frontHistogramSsbo(),
                scopeGpu.frontMaximaSsbo(),
                scopeGpu.frontValid(),
                histogramViewport,
                vao);

        scopeUi.drawOverlay(
                currentUiFps,
                width,
                height,
                uiLayout,
                uiCanvas,
                calibration.enabled,
                calibration.width,
                calibration.height,
                calibration.limitedInput,
                calibration.enabled ? calibration.colorimetry : 0,
                uiSnapshot,
                vao);

        scopeUi.drawVectorscope(
                scopeGpu.frontVectorscopeSsbo(),
                scopeGpu.frontMaximaSsbo(),
                scopeGpu.frontValid(),
                vectorscopeViewport,
                vao);
        scopeUi.drawParade(
                scopeGpu.frontParadeSsbo(),
                scopeGpu.frontMaximaSsbo(),
                scopeGpu.frontValid(),
                paradeViewport,
                vao);
        scopeUi.drawWaveform(
                scopeGpu.frontWaveformSsbo(),
                scopeGpu.frontValid(),
                waveformViewport,
                vao);

        scopeUi.drawMonitorForeground(
                width,
                height,
                vao);
    };

    const auto presentUiRefresh =
            [&](const calibration_profile::Profile& calibration,
                const field_monitor::MonitorUiSnapshot& uiSnapshot) {
        drawComposedFrame(
                uiFps,
                calibration,
                uiSnapshot,
                haveUploadedFrame);

        if (frontBuffer.active) {
            return submitStep15DFrontBuffer(frontBuffer, false);
        }
        return eglSwapBuffers(display, surface) == EGL_TRUE;
    };


    while (gRunning.load(
            std::memory_order_acquire)) {

        const calibration_profile::Profile loopCalibration =
                calibration_profile::snapshot();
        monitorUi.setHdrNitsAvailable(
                loopCalibration.enabled && loopCalibration.pqInput);
        processPendingSurfaceTaps(
                height,
                uiLayout,
                uiCanvas,
                monitorUi);

        const field_monitor::MonitorUiSnapshot loopUiSnapshot =
                monitorUi.snapshot();
        if (loopUiSnapshot.revision != lastRenderedUiRevision) {
            if (!presentUiRefresh(loopCalibration, loopUiSnapshot)) {
                LOGE("UI-only refresh present failed");
                break;
            }
            lastRenderedUiRevision = loopUiSnapshot.revision;
        }


        // Poll already-submitted frame IDs without generating
        // any additional BufferQueue traffic.
        if (!frontBuffer.active) {
            pollPresentationTimestamps(
                    presentationTracker,
                    display,
                    surface
            );
        }


        // Step 15.0.1: top-of-loop non-blocking completion poll.
        scopeGpu.poll(false);


        if (!uvc_mjpeg_decoder::isRunning()) {
            ++wakeTimeouts;
            latestPlanarMjpegSequence = 0;
            lastPlanarMjpegWakeSequence = 0;

            if (wakeTimeouts <= 5 ||
                (wakeTimeouts % 20u) == 0u) {

                LOGW(
                        "Step 15.7.1: waiting for planar MJPEG decoder "
                        "timeouts=%llu isoRunning=%s",
                        static_cast<unsigned long long>(wakeTimeouts),
                        uvc_stream::isRunning() ? "YES" : "NO"
                );
            }

            std::this_thread::sleep_for(
                    std::chrono::milliseconds(10)
            );
            continue;
        }

        uint64_t readySequence =
                lastPlanarMjpegWakeSequence;
        uint64_t readyRenderWakeSequence =
                lastRenderWakeSequence;

        const bool frameReady =
                uvc_mjpeg_decoder::waitForDecodedFrameOrRenderWake(
                        lastPlanarMjpegWakeSequence,
                        lastRenderWakeSequence,
                        readySequence,
                        readyRenderWakeSequence,
                        100
                );

        if (!gRunning.load(
                std::memory_order_acquire)) {
            break;
        }

        const bool renderWake =
                readyRenderWakeSequence > lastRenderWakeSequence;
        lastRenderWakeSequence = readyRenderWakeSequence;

        if (renderWake) {
            processPendingSurfaceTaps(
                    height,
                    uiLayout,
                    uiCanvas,
                    monitorUi);
        }

        if (!frameReady) {
            if (renderWake) {
                continue;
            }
            ++wakeTimeouts;

            if (wakeTimeouts <= 5 ||
                (wakeTimeouts % 20u) == 0u) {

                LOGW(
                        "Step 15.7.1: waiting for frame "
                        "timeouts=%llu source=PLANAR_MJPEG "
                        "isoRunning=%s decoderRunning=%s "
                        "lastPlanarMjpegSeq=%llu",
                        static_cast<unsigned long long>(wakeTimeouts),
                        uvc_stream::isRunning() ? "YES" : "NO",
                        uvc_mjpeg_decoder::isRunning() ? "YES" : "NO",
                        static_cast<unsigned long long>(
                                lastPlanarMjpegWakeSequence
                        )
                );
            }

            if (!uvc_mjpeg_decoder::isRunning()) {
                latestPlanarMjpegSequence = 0;
                lastPlanarMjpegWakeSequence = 0;
            }

            continue;
        }

        lastPlanarMjpegWakeSequence = readySequence;

        uvc_mjpeg_decoder::DecodedFrameTiming planarMjpegFrameTiming{};

        CompositorTimingSnapshot compositorTiming{};

        if (!frontBuffer.active) {
            compositorTiming =
                    queryCompositorTiming(
                            presentationTracker,
                            display,
                            surface
                    );
        }

        uint64_t t2TextureUploadDoneNs = 0;
        uint64_t t3DrawIssuedNs = 0;
        uint64_t t4SwapStartNs = 0;
        uint64_t t5SwapReturnNs = 0;

        uint64_t t4SwapStartMonotonicNs = 0;

        uint64_t eglFrameId = 0;
        bool haveEglFrameId = false;

        uint64_t t5SwapReturnMonotonicNs = 0;


        // ----------------------------------------------------
        // Acquire one persistent mapped planar-MJPEG PBO without waiting.
        // ----------------------------------------------------

        const int pboIndex =
                findAvailablePbo(
                        planarMjpegPbo,
                        preferredPlanarMjpegPbo
                );

        if (pboIndex < 0) {
            ++planarMjpegPboBusySkips;
            continue;
        }

        PboSlot& slot = planarMjpegPbo[pboIndex];


        // ----------------------------------------------------
        // Consume newest decoded planar frame.
        // ----------------------------------------------------

        if (!uvc_mjpeg_decoder::copyLatestFrame(
                slot.mapped,
                PLANAR_MJPEG_YUV422_FRAME_BYTES,
                latestPlanarMjpegSequence,
                planarMjpegFrameTiming)) {

            ++planarMjpegCopyRaceSkips;
            continue;
        }

        lastPlanarMjpegWakeSequence =
                std::max(
                        lastPlanarMjpegWakeSequence,
                        latestPlanarMjpegSequence
                );

        if (planarMjpegFrameTiming.width != PLANAR_MJPEG_FRAME_W ||
            planarMjpegFrameTiming.height != PLANAR_MJPEG_FRAME_H ||
            planarMjpegFrameTiming.yStride !=
                static_cast<size_t>(PLANAR_MJPEG_FRAME_W) ||
            planarMjpegFrameTiming.cbStride !=
                static_cast<size_t>(PLANAR_MJPEG_CHROMA_W) ||
            planarMjpegFrameTiming.crStride !=
                static_cast<size_t>(PLANAR_MJPEG_CHROMA_W) ||
            planarMjpegFrameTiming.yBytes != PLANAR_MJPEG_Y_BYTES ||
            planarMjpegFrameTiming.cbBytes != PLANAR_MJPEG_C_BYTES ||
            planarMjpegFrameTiming.crBytes != PLANAR_MJPEG_C_BYTES ||
            planarMjpegFrameTiming.frameBytes != PLANAR_MJPEG_YUV422_FRAME_BYTES) {

            LOGE(
                    "Planar MJPEG renderer: unexpected planar geometry "
                    "%dx%d strides=%zu/%zu/%zu bytes=%zu/%zu/%zu total=%zu",
                    planarMjpegFrameTiming.width,
                    planarMjpegFrameTiming.height,
                    planarMjpegFrameTiming.yStride,
                    planarMjpegFrameTiming.cbStride,
                    planarMjpegFrameTiming.crStride,
                    planarMjpegFrameTiming.yBytes,
                    planarMjpegFrameTiming.cbBytes,
                    planarMjpegFrameTiming.crBytes,
                    planarMjpegFrameTiming.frameBytes
            );
            continue;
        }


        // ----------------------------------------------------
        // Persistent PBO -> planar source textures.
        // ----------------------------------------------------

        glBindBuffer(
                GL_PIXEL_UNPACK_BUFFER,
                slot.id
        );

        // One contiguous PBO contains all three planes. With a PBO bound,
        // glTexSubImage2D() interprets the last argument as a byte offset.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, planarMjpegYTexture);
        glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                PLANAR_MJPEG_FRAME_W,
                PLANAR_MJPEG_FRAME_H,
                GL_RED,
                GL_UNSIGNED_BYTE,
                reinterpret_cast<const void*>(0)
        );

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, planarMjpegCbTexture);
        glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                PLANAR_MJPEG_CHROMA_W,
                PLANAR_MJPEG_FRAME_H,
                GL_RED,
                GL_UNSIGNED_BYTE,
                reinterpret_cast<const void*>(PLANAR_MJPEG_CB_OFFSET)
        );

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, planarMjpegCrTexture);
        glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                PLANAR_MJPEG_CHROMA_W,
                PLANAR_MJPEG_FRAME_H,
                GL_RED,
                GL_UNSIGNED_BYTE,
                reinterpret_cast<const void*>(PLANAR_MJPEG_CR_OFFSET)
        );

        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        // B3: planar texture upload commands issued.
        t2TextureUploadDoneNs = nowMonotonicRawNs();

        slot.fence =
                glFenceSync(
                        GL_SYNC_GPU_COMMANDS_COMPLETE,
                        0
                );

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        preferredPlanarMjpegPbo = pboIndex ^ 1;

        ++liveUploads;

        if (ENABLE_VERBOSE_TIMING_LOG &&
            (liveUploads <= 5 ||
             (liveUploads % 300) == 0)) {

            LOGI(
                    "Step 14: live frame uploaded #%llu "
                    "source=PLANAR_MJPEG seq=%llu PBO=%d",
                    static_cast<unsigned long long>(liveUploads),
                    static_cast<unsigned long long>(latestPlanarMjpegSequence),
                    pboIndex
            );
        }


        // ----------------------------------------------------
        // Draw exactly once for this new UVC frame.
        //
        // Step 15.7 keeps status telemetry outside the clean 16:9 source
        // in both orientations while retaining a single BufferQueue submission:
        //   clean preview -> completed scopes -> UI/status overlay.
        // ----------------------------------------------------

        ++uiFpsFrames;

        const uint64_t uiFpsNowNs =
                nowMonotonicRawNs();

        const uint64_t uiFpsElapsedNs =
                uiFpsNowNs -
                uiFpsWindowStartNs;

        if (uiFpsElapsedNs >=
            500000000ULL) {

            uiFps =
                    static_cast<double>(
                            uiFpsFrames
                    ) *
                    1000000000.0 /
                    static_cast<double>(
                            uiFpsElapsedNs
                    );

            uiFpsFrames = 0;
            uiFpsWindowStartNs =
                    uiFpsNowNs;
        }


        const calibration_profile::Profile calibration =
                calibration_profile::snapshot();
        monitorUi.setHdrNitsAvailable(
                calibration.enabled && calibration.pqInput);
        const field_monitor::MonitorUiSnapshot uiSnapshot =
                monitorUi.snapshot();
        haveUploadedFrame = true;
        drawComposedFrame(
                uiFps,
                calibration,
                uiSnapshot,
                true);
        lastRenderedUiRevision = uiSnapshot.revision;


        // T3: complete frame composition commands issued.
        t3DrawIssuedNs =
                nowMonotonicRawNs();


        // ----------------------------------------------------
        // Step 13.2 result retained:
        // desired presentation time = CLOCK_MONOTONIC "now".
        // One call only for this new UVC frame / swap.
        // ----------------------------------------------------

        if (!frontBuffer.active &&
            presentationTimeHintEnabled) {

            const int64_t desiredPresentTimeNs =
                    static_cast<int64_t>(
                            nowMonotonicNs()
                    );

            const EGLBoolean hintOk =
                    p_eglPresentationTimeANDROID(
                            display,
                            surface,
                            desiredPresentTimeNs
                    );

            if (hintOk == EGL_TRUE) {

                ++presentationTimeHintCalls;
            }
            else {

                ++presentationTimeHintErrors;

                if (presentationTimeHintErrors <= 5) {

                    LOGE(
                            "Step 14: "
                            "eglPresentationTimeANDROID "
                            "failed EGL=0x%04X",
                            eglGetError()
                    );
                }
            }
        }


        // ----------------------------------------------------
        // T4: immediately before present submission.  EGL frame ID exists
        // only on the fallback window-surface swap path.
        // ----------------------------------------------------

        t4SwapStartNs =
                nowMonotonicRawNs();

        t4SwapStartMonotonicNs =
                nowMonotonicNs();


        if (!frontBuffer.active &&
            presentationTracker.enabled &&
            p_eglGetNextFrameIdANDROID !=
                nullptr) {

            haveEglFrameId =
                    p_eglGetNextFrameIdANDROID(
                            display,
                            surface,
                            &eglFrameId
                    ) ==
                    EGL_TRUE;

            if (!haveEglFrameId) {

                LOGE(
                        "Step 14: "
                        "eglGetNextFrameIdANDROID "
                        "failed EGL=0x%04X",
                        eglGetError()
                );
            }
        }


        if (frontBuffer.active) {

            // Persistent FRONT_BUFFER: submit GLES commands, then notify the
            // compositor with the SAME AHardwareBuffer.  No BufferQueue swap
            // and no acquire fence are used in steady state.
            if (!submitStep15DFrontBuffer(
                    frontBuffer,
                    false
            )) {

                LOGE(
                        "Step 15D: steady-state SurfaceControl submit failed"
                );
                break;
            }

            t5SwapReturnNs =
                    nowMonotonicRawNs();

            t5SwapReturnMonotonicNs =
                    nowMonotonicNs();

            if (liveUploads <= 5 ||
                (liveUploads % 30) == 0) {

                LOGI(
                        "Step 15D FRONT submit seq=%llu "
                        "draw->transaction=%.3f ms",
                        static_cast<unsigned long long>(
                                planarMjpegFrameTiming.sequence
                        ),
                        nsToMs(
                                t5SwapReturnNs -
                                t3DrawIssuedNs
                        )
                );
            }
        }
        else {

            // Exactly one BufferQueue submission for this camera frame.
            if (!eglSwapBuffers(
                    display,
                    surface)) {

                logEglError(
                        "eglSwapBuffers"
                );

                break;
            }

            // T5: eglSwapBuffers() returned.
            t5SwapReturnNs =
                    nowMonotonicRawNs();

            t5SwapReturnMonotonicNs =
                    nowMonotonicNs();
        }


        if (!frontBuffer.active &&
            haveEglFrameId &&
            planarMjpegFrameTiming.decodeDoneMonotonicNs != 0) {

            queuePresentationFrame(
                    presentationTracker,
                    eglFrameId,
                    planarMjpegFrameTiming.sequence,
                    planarMjpegFrameTiming.decodeDoneMonotonicNs,
                    t4SwapStartMonotonicNs,
                    t5SwapReturnMonotonicNs,
                    compositorTiming,
                    true,
                    planarMjpegFrameTiming.b0ToB2Ms
            );
        }


        // Step 15.0.1: poll again after the UVC wait and present.
        // The previous scope job may have completed while the render
        // thread was sleeping for this camera frame. Timeout remains 0.
        scopeGpu.poll(true);


        // ----------------------------------------------------
        // Step 15.4: queue scope analysis AFTER present.
        // ScopeGpu preserves the no-wait / latest-frame policy.
        // ----------------------------------------------------

        scopeGpu.queueFromYuv422Textures(
                planarMjpegYTexture,
                planarMjpegCbTexture,
                planarMjpegCrTexture,
                planarMjpegFrameTiming.sequence
        );


        if (frontBuffer.active) {
            // No eglSwapBuffers() follows in this mode.  Explicitly flush the
            // post-present scope compute commands so they can complete while
            // the render thread sleeps for the next UVC frame.
            glFlush();
        }


            if (planarMjpegFrameTiming.decodeDoneRawNs != 0 &&
                planarMjpegFrameTiming.copyDoneRawNs >=
                    planarMjpegFrameTiming.decodeDoneRawNs &&
                t2TextureUploadDoneNs >=
                    planarMjpegFrameTiming.copyDoneRawNs &&
                t3DrawIssuedNs >= t2TextureUploadDoneNs &&
                t4SwapStartNs >= t3DrawIssuedNs &&
                t5SwapReturnNs >= t4SwapStartNs) {

                const double b2ToCopyMs = nsToMs(
                        planarMjpegFrameTiming.copyDoneRawNs -
                        planarMjpegFrameTiming.decodeDoneRawNs
                );

                const double copyToB3Ms = nsToMs(
                        t2TextureUploadDoneNs -
                        planarMjpegFrameTiming.copyDoneRawNs
                );

                const double b3ToDrawMs = nsToMs(
                        t3DrawIssuedNs - t2TextureUploadDoneNs
                );

                const double drawToB4Ms = nsToMs(
                        t4SwapStartNs - t3DrawIssuedNs
                );

                const double b4ToB5Ms = nsToMs(
                        t5SwapReturnNs - t4SwapStartNs
                );

                const double b2ToB5Ms = nsToMs(
                        t5SwapReturnNs -
                        planarMjpegFrameTiming.decodeDoneRawNs
                );

                const double b0ToB5Ms =
                        planarMjpegFrameTiming.b0ToB2Ms +
                        b2ToB5Ms;

                if (ENABLE_VERBOSE_TIMING_LOG &&
                    (liveUploads <= 5 ||
                     (liveUploads % 30) == 0)) {

                    LOGI(
                            "PLANAR_MJPEG render latency seq=%llu path=%s | "
                            "B0->B2=%.3f ms B2->COPY=%.3f COPY->B3=%.3f "
                            "B3->DRAW=%.3f DRAW->B4=%.3f B4->B5=%.3f | "
                            "B2->B5=%.3f B0->B5=%.3f ms",
                            static_cast<unsigned long long>(
                                    planarMjpegFrameTiming.sequence
                            ),
                            frontBuffer.active
                            ? "FRONT_TX"
                            : "EGL_SWAP",
                            planarMjpegFrameTiming.b0ToB2Ms,
                            b2ToCopyMs,
                            copyToB3Ms,
                            b3ToDrawMs,
                            drawToB4Ms,
                            b4ToB5Ms,
                            b2ToB5Ms,
                            b0ToB5Ms
                    );
                }
            }

        ++frame;


        // Usually the just-submitted EGL frame is still PENDING here.
        // FRONT_BUFFER mode has no EGL frame ID because there is no swap.
        if (!frontBuffer.active) {
            pollPresentationTimestamps(
                    presentationTracker,
                    display,
                    surface
            );
        }


        if (ENABLE_VERBOSE_TIMING_LOG &&
            (frame % 300) == 0) {

            LOGI(
                    "Step 14 render stats: "
                    "renderFrames=%llu presentPath=%s liveUploads=%llu "
                    "wakeTimeouts=%llu planarMjpegPboBusySkips=%llu "
                    "planarMjpegCopyRaceSkips=%llu planarMjpegLatestSeq=%llu",
                    static_cast<unsigned long long>(frame),
                    frontBuffer.active ? "FRONT_BUFFER" : "EGL_SWAP",
                    static_cast<unsigned long long>(liveUploads),
                    static_cast<unsigned long long>(wakeTimeouts),
                    static_cast<unsigned long long>(planarMjpegPboBusySkips),
                    static_cast<unsigned long long>(planarMjpegCopyRaceSkips),
                    static_cast<unsigned long long>(latestPlanarMjpegSequence)
            );
        }
    }

    if (presentationTracker.enabled) {

        LOGI(
                "Step 15C.0 final: "
                "resolved=%llu jitResolved=%llu "
                "pendingPolls=%llu invalid=%llu "
                "badAccess=%llu queryErrors=%llu "
                "queueOverwrites=%llu "
                "compQueries=%llu compErrors=%llu",
                static_cast<unsigned long long>(
                        presentationTracker.resolved
                ),
                static_cast<unsigned long long>(
                        presentationTracker.jitTimingResolved
                ),
                static_cast<unsigned long long>(
                        presentationTracker.pendingPolls
                ),
                static_cast<unsigned long long>(
                        presentationTracker.invalidTimestamps
                ),
                static_cast<unsigned long long>(
                        presentationTracker.badAccessFrames
                ),
                static_cast<unsigned long long>(
                        presentationTracker.queryErrors
                ),
                static_cast<unsigned long long>(
                        presentationTracker.queueOverwrites
                ),
                static_cast<unsigned long long>(
                        presentationTracker.compositorTimingQueries
                ),
                static_cast<unsigned long long>(
                        presentationTracker.compositorTimingErrors
                )
        );
    }


    if (UVCFM_DIAGNOSTICS_ENABLED) {
        LOGI(
                "Step 14 presentation hint final: mode=%s "
                "hintCalls=%llu hintErrors=%llu",
                presentationTimeHintEnabled ? "B" : "A",
                static_cast<unsigned long long>(presentationTimeHintCalls),
                static_cast<unsigned long long>(presentationTimeHintErrors)
        );
    }


    for (auto& slot : planarMjpegPbo) {

        if (slot.fence != nullptr) {
            glDeleteSync(slot.fence);
            slot.fence = nullptr;
        }

        if (slot.id != 0) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot.id);

            if (slot.mapped != nullptr) {
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                slot.mapped = nullptr;
            }

            glDeleteBuffers(1, &slot.id);
            slot.id = 0;
        }
    }


    glBindBuffer(
            GL_PIXEL_UNPACK_BUFFER,
            0
    );


    destroyStep15DFrontBuffer(
            display,
            frontBuffer
    );

    glBindFramebuffer(
            GL_FRAMEBUFFER,
            0
    );

    scopeGpu.shutdown();
    scopeUi.shutdown();


    glDeleteTextures(1, &planarMjpegYTexture);
    glDeleteTextures(1, &planarMjpegCbTexture);
    glDeleteTextures(1, &planarMjpegCrTexture);


    glDeleteVertexArrays(
            1,
            &vao
    );


    glDeleteProgram(
            planarMjpegProgram
    );

    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    eglMakeCurrent(
            display,
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            EGL_NO_CONTEXT
    );

    eglDestroySurface(
            display,
            surface
    );

    eglDestroyContext(
            display,
            context
    );

    eglTerminate(display);


    LOGI(
            "Render thread stopped, frames=%llu",
            static_cast<unsigned long long>(frame)
    );
}


// ------------------------------------------------------------
// Native USB JNI bridge
//
// USB/UVC implementation lives in uvc_device.cpp.
// ------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL
Java_com_hev_uvcfieldmonitor_MainActivity_nativeOnSurfaceTap(
        JNIEnv* /* env */,
        jobject /* thiz */,
        jfloat x,
        jfloat y)
{
    enqueueSurfaceTap(
            static_cast<float>(x),
            static_cast<float>(y));
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hev_uvcfieldmonitor_MainActivity_nativeOpenUsb(
        JNIEnv* /* env */,
        jobject /* thiz */,
        jint fd)
{
    LOGI(
            "nativeOpenUsb(fd=%d)",
            fd
    );

    return
            uvc_device::openFromAndroidFd(
                    fd
            )
            ? JNI_TRUE
            : JNI_FALSE;
}


extern "C"
JNIEXPORT void JNICALL
Java_com_hev_uvcfieldmonitor_MainActivity_nativeCloseUsb(
        JNIEnv* /* env */,
        jobject /* thiz */)
{
    uvc_device::close();
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_hev_uvcfieldmonitor_MainActivity_nativeConfigureCalibrationProfile(
        JNIEnv* env,
        jobject /* thiz */,
        jfloatArray matrixArray,
        jfloatArray offsetArray,
        jintArray puArray,
        jboolean pqInput,
        jint colorimetry,
        jint width,
        jint height,
        jboolean limitedInput)
{
    if (matrixArray == nullptr || offsetArray == nullptr || puArray == nullptr ||
        env->GetArrayLength(matrixArray) != 9 ||
        env->GetArrayLength(offsetArray) != 3 ||
        env->GetArrayLength(puArray) != 4) {
        calibration_profile::clear();
        return JNI_FALSE;
    }

    calibration_profile::Profile profile;
    env->GetFloatArrayRegion(matrixArray, 0, 9, profile.matrix.data());
    env->GetFloatArrayRegion(offsetArray, 0, 3, profile.offsetCode.data());
    env->GetIntArrayRegion(puArray, 0, 4, profile.pu.data());
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        calibration_profile::clear();
        return JNI_FALSE;
    }
    profile.enabled = true;
    profile.width = static_cast<int>(width);
    profile.height = static_cast<int>(height);
    profile.limitedInput = limitedInput == JNI_TRUE;
    profile.pqInput = pqInput == JNI_TRUE;
    profile.colorimetry = static_cast<int>(colorimetry);
    calibration_profile::configure(profile);
    LOGI("Phase 8: calibration profile configured (%s)",
         profile.pqInput ? "PQ" : "SDR");
    return JNI_TRUE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hev_uvcfieldmonitor_MainActivity_nativeDisableCalibrationProfile(
        JNIEnv* /* env */, jobject /* thiz */)
{
    calibration_profile::clear();
    LOGI("Phase 8: calibration disabled");
}


// ------------------------------------------------------------
// Stop current renderer
//
// gMutexを取得した状態で呼ぶ。
// ------------------------------------------------------------

static void stopRendererLocked()
{
    gRunning.store(
            false,
            std::memory_order_release
    );

    if (gRenderThread.joinable()) {
        gRenderThread.join();
    }

    if (gWindow != nullptr) {

        ANativeWindow_release(gWindow);

        gWindow = nullptr;
    }

    {
        std::lock_guard<std::mutex> inputLock(gUiInputMutex);
        gPendingSurfaceTaps.clear();
    }
}


// ------------------------------------------------------------
// JNI
//
// Kotlin Surface
//       ↓
// ANativeWindow
// ------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL

Java_com_hev_uvcfieldmonitor_MainActivity_nativeSetSurface(
        JNIEnv* env,
        jobject /* thiz */,
        jobject surface)
{
    ANativeWindow* newWindow = nullptr;


    if (surface != nullptr) {

        newWindow =
                ANativeWindow_fromSurface(
                        env,
                        surface
                );

        if (newWindow == nullptr) {

            LOGE(
                    "ANativeWindow_fromSurface failed"
            );

            return;
        }
    }


    std::lock_guard<std::mutex> lock(
            gMutex
    );


    // 古いSurfaceがあれば停止
    stopRendererLocked();


    if (newWindow == nullptr) {

        LOGI(
                "Surface destroyed"
        );

        return;
    }


    gWindow = newWindow;


    LOGI(
            "Native window created: %dx%d",
            ANativeWindow_getWidth(
                    gWindow
            ),
            ANativeWindow_getHeight(
                    gWindow
            )
    );


    gRunning.store(
            true,
            std::memory_order_release
    );


    gRenderThread =
            std::thread(
                    renderLoop,
                    gWindow
            );
}
