#pragma once

#include <EGL/egl.h>
#include <GLES3/gl31.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace field_monitor {

// Scope raster resolution is intentionally independent of the 1280x720
// source geometry. Horizontal source samples are reduced to 720 waveform bins.
static constexpr int WAVEFORM_W = 720;
static constexpr int WAVEFORM_H = 256;
static constexpr size_t WAVEFORM_ITEMS =
        static_cast<size_t>(WAVEFORM_W) * WAVEFORM_H;
static constexpr size_t WAVEFORM_BYTES =
        WAVEFORM_ITEMS * sizeof(uint32_t);

struct RectI {
    int x = 0;
    int y = 0;  // logical top-origin
    int width = 0;
    int height = 0;
};

enum class UiLayoutMode {
    Landscape,
    Portrait,
};

struct UiLayout {
    UiLayoutMode mode = UiLayoutMode::Landscape;
    int canvasWidth = 0;
    int canvasHeight = 0;

    RectI inputStatus;
    RectI preview;
    RectI runtimeStatus;
    RectI waveform;
    RectI parade;
    RectI histogram;
    RectI vectorscope;
};

// Step 15.6 landscape geometry retained exactly.
inline UiLayout makeLandscapeUiLayout()
{
    UiLayout out{};
    out.mode = UiLayoutMode::Landscape;
    out.canvasWidth = 1280;
    out.canvasHeight = 454;

    out.inputStatus = {0, 0, 711, 28};
    out.preview = {0, 28, 711, 400};
    out.runtimeStatus = {0, 428, 711, 26};

    out.waveform = {711, 28, 330, 200};
    out.parade = {711, 228, 330, 200};
    out.histogram = {1041, 228, 239, 200};
    out.vectorscope = {1041, 28, 239, 200};
    return out;
}

// Step 15.7 portrait geometry.
//
// The draft image is treated as a 710x796 logical canvas.  The whole canvas
// is aspect-fitted and centered on the Android Surface, so a 1080x2400 panel
// becomes approximately 1080x1211 with ~594 px letterbox above and below.
//
// Telemetry never overlaps the source image:
//   input status  : y=0..27
//   clean preview : 638x359 (16:9), x=36
//   runtime status: y=387..408
//   scopes        : y=409..795, 2x2
inline UiLayout makePortraitUiLayout()
{
    UiLayout out{};
    out.mode = UiLayoutMode::Portrait;
    out.canvasWidth = 710;
    out.canvasHeight = 796;

    out.inputStatus = {0, 0, 710, 28};
    out.preview = {36, 28, 638, 359};
    out.runtimeStatus = {0, 387, 710, 22};

    // Keep Waveform and RGB Parade in one equal-width column. Vectorscope
    // and Histogram share the right column.
    out.waveform = {0, 409, 413, 193};
    out.parade = {0, 602, 413, 194};
    out.histogram = {413, 602, 297, 194};
    out.vectorscope = {413, 409, 297, 193};
    return out;
}

inline UiLayout selectUiLayout(EGLint surfaceWidth, EGLint surfaceHeight)
{
    if (surfaceHeight > surfaceWidth) {
        return makePortraitUiLayout();
    }
    return makeLandscapeUiLayout();
}

struct Viewport {
    GLint x = 0;
    GLint y = 0;
    GLsizei width = 0;
    GLsizei height = 0;
};

struct UiCanvasViewport {
    GLint x = 0;
    GLint y = 0;  // OpenGL bottom-origin
    GLsizei width = 0;
    GLsizei height = 0;
    float scale = 1.0f;
};

inline UiCanvasViewport calculateUiCanvasViewport(
        EGLint surfaceWidth,
        EGLint surfaceHeight,
        const UiLayout& layout)
{
    UiCanvasViewport out{};

    if (surfaceWidth <= 0 || surfaceHeight <= 0 ||
        layout.canvasWidth <= 0 || layout.canvasHeight <= 0) {
        return out;
    }

    const float sx =
            static_cast<float>(surfaceWidth) /
            static_cast<float>(layout.canvasWidth);

    const float sy =
            static_cast<float>(surfaceHeight) /
            static_cast<float>(layout.canvasHeight);

    out.scale = std::min(sx, sy);

    out.width =
            static_cast<GLsizei>(
                    std::lround(
                            static_cast<float>(layout.canvasWidth) *
                            out.scale));

    out.height =
            static_cast<GLsizei>(
                    std::lround(
                            static_cast<float>(layout.canvasHeight) *
                            out.scale));

    out.x = (surfaceWidth - out.width) / 2;
    out.y = (surfaceHeight - out.height) / 2;

    return out;
}

inline Viewport uiLogicalRectToViewport(
        const UiCanvasViewport& canvas,
        float x,
        float y,
        float w,
        float h)
{
    Viewport out{};

    const int left =
            canvas.x +
            static_cast<int>(std::lround(x * canvas.scale));

    const int right =
            canvas.x +
            static_cast<int>(std::lround((x + w) * canvas.scale));

    // Logical UI coordinates are top-origin; OpenGL viewport is bottom-origin.
    const int bottom =
            canvas.y +
            canvas.height -
            static_cast<int>(std::lround((y + h) * canvas.scale));

    const int top =
            canvas.y +
            canvas.height -
            static_cast<int>(std::lround(y * canvas.scale));

    out.x = left;
    out.y = bottom;
    out.width = std::max(0, right - left);
    out.height = std::max(0, top - bottom);

    return out;
}

inline Viewport uiLogicalRectToViewport(
        const UiCanvasViewport& canvas,
        const RectI& rect)
{
    return uiLogicalRectToViewport(
            canvas,
            static_cast<float>(rect.x),
            static_cast<float>(rect.y),
            static_cast<float>(rect.width),
            static_cast<float>(rect.height));
}

// Vectorscope data shader is authored for a 239x200 panel.  In portrait the
// panel is wider relative to its height; aspect-fit the shader viewport inside
// the panel so the measured scope stays circular instead of becoming an oval.
inline RectI aspectFitRect(
        const RectI& outer,
        int contentWidth,
        int contentHeight)
{
    if (outer.width <= 0 || outer.height <= 0 ||
        contentWidth <= 0 || contentHeight <= 0) {
        return {};
    }

    const float scale = std::min(
            static_cast<float>(outer.width) /
                    static_cast<float>(contentWidth),
            static_cast<float>(outer.height) /
                    static_cast<float>(contentHeight));

    const int width = static_cast<int>(std::lround(contentWidth * scale));
    const int height = static_cast<int>(std::lround(contentHeight * scale));

    return {
            outer.x + (outer.width - width) / 2,
            outer.y + (outer.height - height) / 2,
            width,
            height,
    };
}

}  // namespace field_monitor
