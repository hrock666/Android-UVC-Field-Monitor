#pragma once

#include "field_monitor_layout.h"
#include "monitor_ui_state.h"

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
            bool calibrationEnabled,
            int calibrationWidth,
            int calibrationHeight,
            bool calibrationLimited,
            int vectorColorimetry,
            const MonitorUiSnapshot& uiSnapshot,
            GLuint vao);

    // Draw only the MENU / Function Rail / Preset vertices prepared by the
    // latest drawOverlay() call. This keeps controls above scope samples
    // without moving the existing grid and label pass above those samples.
    void drawMonitorForeground(
            EGLint surfaceWidth,
            EGLint surfaceHeight,
            GLuint vao) const;

    void shutdown();

private:
    GLuint waveformRenderProgram_ = 0;
    GLuint paradeRenderProgram_ = 0;
    GLuint histogramRenderProgram_ = 0;
    GLuint vectorscopeRenderProgram_ = 0;
    GLuint uiProgram_ = 0;
    GLuint uiVbo_ = 0;
    GLint monitorForegroundFirst_ = 0;
    GLsizei monitorForegroundCount_ = 0;
};

}  // namespace field_monitor
