#pragma once

#include "field_monitor_layout.h"

#include <GLES3/gl31.h>

namespace field_monitor {

class ScopeUi {
public:
    ScopeUi() = default;
    ~ScopeUi() = default;

    ScopeUi(const ScopeUi&) = delete;
    ScopeUi& operator=(const ScopeUi&) = delete;

    bool initialize();

    // Draw the most recently completed waveform SSBO. Never waits.
    void drawWaveform(
            GLuint waveformSsbo,
            bool frontValid,
            const Viewport& viewport,
            GLuint vao) const;

    void drawParade(
            GLuint paradeSsbo,
            GLuint maximaSsbo,
            bool frontValid,
            const Viewport& viewport,
            GLuint vao) const;

    void drawHistogram(
            GLuint histogramSsbo,
            GLuint maximaSsbo,
            bool frontValid,
            const Viewport& viewport,
            GLuint vao) const;

    void drawVectorscope(
            GLuint vectorscopeSsbo,
            GLuint maximaSsbo,
            bool frontValid,
            const Viewport& viewport,
            GLuint vao) const;

    // Draw status rows outside the clean source image, plus grid, labels,
    // optical overlays and scope shells.
    void drawOverlay(
            double currentUiFps,
            EGLint surfaceWidth,
            EGLint surfaceHeight,
            const UiLayout& layout,
            const UiCanvasViewport& canvas,
            int vectorColorimetry,
            GLuint vao);

    void shutdown();

private:
    GLuint waveformRenderProgram_ = 0;
    GLuint paradeRenderProgram_ = 0;
    GLuint histogramRenderProgram_ = 0;
    GLuint vectorscopeRenderProgram_ = 0;
    GLuint uiProgram_ = 0;
    GLuint uiVbo_ = 0;
};

}  // namespace field_monitor
