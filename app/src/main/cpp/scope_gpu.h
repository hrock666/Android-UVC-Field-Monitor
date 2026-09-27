#pragma once

#include <GLES3/gl31.h>

#include <cstdint>

namespace field_monitor {

class ScopeGpu {
public:
    ScopeGpu() = default;
    ~ScopeGpu() = default;

    ScopeGpu(const ScopeGpu&) = delete;
    ScopeGpu& operator=(const ScopeGpu&) = delete;

    // Compute backend failure is non-fatal: initialize() leaves enabled()==false.
    void initialize();

    // Zero-timeout fence poll. Never waits for the GPU.
    void poll(bool preDispatchPoll);

    // Analyze the already-uploaded planar JPEG YCbCr 4:2:2 textures
    // directly. No extra PBO copy/upload is performed for scopes.
    void queueFromYuv422Textures(
            GLuint yTexture,
            GLuint cbTexture,
            GLuint crTexture,
            uint64_t sequence);

    bool enabled() const { return enabled_; }
    bool frontValid() const { return frontValid_; }

    GLuint frontWaveformSsbo() const;
    GLuint frontParadeSsbo() const;
    GLuint frontHistogramSsbo() const;
    GLuint frontVectorscopeSsbo() const;
    GLuint frontMaximaSsbo() const;

    void shutdown();

private:
    struct Slot {
        GLuint waveformSsbo = 0;
        GLuint paradeSsbo = 0;
        GLuint histogramSsbo = 0;
        GLuint vectorscopeSsbo = 0;
        GLuint maximaSsbo = 0;
        GLsync fence = nullptr;
        uint64_t sequence = 0;
        uint64_t expectedPixels = 0;
        uint64_t expectedVectorSamples = 0;
    };

    GLuint clearProgram_ = 0;
    GLuint yuv422AccumulateProgram_ = 0;
    GLint yLocation_ = -1;
    GLint cbLocation_ = -1;
    GLint crLocation_ = -1;

    Slot slots_[2];
    int front_ = 0;
    int back_ = 1;
    bool frontValid_ = false;
    bool enabled_ = false;

    uint64_t dispatches_ = 0;
    uint64_t busySkips_ = 0;
    uint64_t promotions_ = 0;
    uint64_t preDispatchPromotions_ = 0;
    uint64_t validationPasses_ = 0;
    uint64_t validationFailures_ = 0;
};

}  // namespace field_monitor
