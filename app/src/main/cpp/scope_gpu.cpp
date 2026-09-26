#include "scope_gpu.h"

#include "field_monitor_layout.h"

#include <android/log.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#define LOG_TAG "UvcFieldMonitor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace field_monitor {
namespace {

static constexpr int PARADE_PLOT_W = 224;
static constexpr int PARADE_PLOT_H = 161;
static constexpr size_t PARADE_ITEMS =
        static_cast<size_t>(PARADE_PLOT_W) * PARADE_PLOT_H;
static constexpr size_t PARADE_BYTES = PARADE_ITEMS * sizeof(uint32_t);
static constexpr int HIST_BINS = 256;
static constexpr size_t HIST_ITEMS = static_cast<size_t>(HIST_BINS) * 3u;
static constexpr size_t HIST_BYTES = HIST_ITEMS * sizeof(uint32_t);
static constexpr int VECTOR_D = 141;
static constexpr int VECTOR_RADIUS = 70;
static constexpr size_t VECTOR_ITEMS = static_cast<size_t>(VECTOR_D) * VECTOR_D;
static constexpr size_t VECTOR_BYTES = VECTOR_ITEMS * sizeof(uint32_t);
static constexpr size_t MAXIMA_ITEMS = 6;
static constexpr size_t MAXIMA_BYTES = MAXIMA_ITEMS * sizeof(uint32_t);

GLuint compileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Scope GPU shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint createComputeProgram(const char* source, const char* label)
{
    GLuint cs = compileShader(GL_COMPUTE_SHADER, source);

    if (!cs) {
        LOGE("Step 15.4: compute shader compile failed (%s)", label);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, cs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);

    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Step 15.4: compute program link error (%s): %s", label, log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(cs);
    return program;
}

GLuint createScopeClearProgram()
{
    static const char* source = R"(#version 310 es

layout(local_size_x = 256) in;

layout(std430, binding = 0) buffer WaveformBuffer {
    uint wave[];
};

layout(std430, binding = 1) buffer ParadeBuffer {
    uint parade[];
};

layout(std430, binding = 2) buffer HistogramBuffer {
    uint histogram[];
};

layout(std430, binding = 3) buffer VectorscopeBuffer {
    uint vectorscope[];
};

layout(std430, binding = 4) buffer MaximaBuffer {
    uint maxima[];
};

void main()
{
    uint i = gl_GlobalInvocationID.x;

    if (i < 184320u) {
        wave[i] = 0u;
    }

    if (i < 36064u) {
        parade[i] = 0u;
    }

    if (i < 768u) {
        histogram[i] = 0u;
    }

    if (i < 19881u) {
        vectorscope[i] = 0u;
    }

    if (i < 6u) {
        maxima[i] = 0u;
    }
}
)";

    return createComputeProgram(source, "scope-clear-wave-parade-hist-vector");
}

GLuint createScopeAccumulateProgram()
{
    static const char* source = R"(#version 310 es

precision highp float;
precision highp int;

layout(local_size_x = 16, local_size_y = 16) in;

uniform sampler2D uPacked;

layout(std430, binding = 0) buffer WaveformBuffer {
    uint wave[];
};

layout(std430, binding = 1) buffer ParadeBuffer {
    uint parade[];
};

layout(std430, binding = 2) buffer HistogramBuffer {
    uint histogram[];
};

layout(std430, binding = 3) buffer VectorscopeBuffer {
    uint vectorscope[];
};

layout(std430, binding = 4) buffer MaximaBuffer {
    uint maxima[];
};

void bumpWave(uint x, uint y)
{
    atomicAdd(wave[y * 720u + x], 1u);
}

void bumpParade(int index, int maxIndex)
{
    uint v = atomicAdd(parade[index], 1u) + 1u;
    atomicMax(maxima[maxIndex], v);
}

void bumpHistogram(int index)
{
    uint v = atomicAdd(histogram[index], 1u) + 1u;
    atomicMax(maxima[4], v);
}

void bumpVector(int index)
{
    uint v = atomicAdd(vectorscope[index], 1u) + 1u;
    atomicMax(maxima[5], v);
}

vec3 rgb601Limited(float y8, float u8, float v8)
{
    float yn = (y8 - 16.0) / 219.0;
    float cb = (u8 - 128.0) / 224.0;
    float cr = (v8 - 128.0) / 224.0;

    vec3 rgb;
    rgb.r = yn + 1.402000 * cr;
    rgb.g = yn - 0.344136 * cb - 0.714136 * cr;
    rgb.b = yn + 1.772000 * cb;
    return clamp(rgb, 0.0, 1.0);
}

void accumulatePixel(int x, uint yCode, float u8, float v8)
{
    bumpWave(uint(x), yCode);

    vec3 rgb = rgb601Limited(float(yCode), u8, v8);

    int pyR = clamp(160 - int(round(rgb.r * 160.0)), 0, 160);
    int pyG = clamp(160 - int(round(rgb.g * 160.0)), 0, 160);
    int pyB = clamp(160 - int(round(rgb.b * 160.0)), 0, 160);

    int pxR = min(73,  (x * 74) / 720);
    int pxG = min(73,  (x * 74) / 720) + 74;
    int pxB = min(75,  (x * 76) / 720) + 148;

    bumpParade(pyR * 224 + pxR, 1);
    bumpParade(pyG * 224 + pxG, 2);
    bumpParade(pyB * 224 + pxB, 3);

    ivec3 bins = ivec3(round(rgb * 255.0));
    bumpHistogram(0 * 256 + clamp(bins.r, 0, 255));
    bumpHistogram(1 * 256 + clamp(bins.g, 0, 255));
    bumpHistogram(2 * 256 + clamp(bins.b, 0, 255));
}

void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);

    // Android packed texture: 360x480 RGBA8, one texel per YUYV pair.
    // R=Y0, G=U, B=Y1, A=V.
    if (p.x >= 360 || p.y >= 480) {
        return;
    }

    vec4 packed = texelFetch(uPacked, p, 0);

    uint y0 = uint(round(packed.r * 255.0));
    float u8 = round(packed.g * 255.0);
    uint y1 = uint(round(packed.b * 255.0));
    float v8 = round(packed.a * 255.0);

    int x0 = p.x * 2;
    int x1 = x0 + 1;

    accumulatePixel(x0, y0, u8, v8);
    accumulatePixel(x1, y1, u8, v8);

    // YUYV 4:2:2 has one Cb/Cr sample per packed pair. Keep that
    // measurement domain directly; do not derive vectorscope chroma from RGB.
    float cb = (u8 - 128.0) / 224.0;
    float cr = (v8 - 128.0) / 224.0;

    int gx = int(round(cb * 128.0 + 70.0));
    int gy = int(round(-cr * 128.0 + 70.0));

    if (gx >= 0 && gx < 141 && gy >= 0 && gy < 141) {
        bumpVector(gy * 141 + gx);
    }
}
)";

    return createComputeProgram(source, "scope-accumulate-wave-parade-hist-vector");
}

GLuint createYuv422ScopeAccumulateProgram()
{
    static const char* source = R"(#version 310 es

precision highp float;
precision highp int;

layout(local_size_x = 16, local_size_y = 16) in;

uniform sampler2D uY;
uniform sampler2D uCb;
uniform sampler2D uCr;

layout(std430, binding = 0) buffer WaveformBuffer {
    uint wave[];
};

layout(std430, binding = 1) buffer ParadeBuffer {
    uint parade[];
};

layout(std430, binding = 2) buffer HistogramBuffer {
    uint histogram[];
};

layout(std430, binding = 3) buffer VectorscopeBuffer {
    uint vectorscope[];
};

layout(std430, binding = 4) buffer MaximaBuffer {
    uint maxima[];
};

void bumpWave(uint x, uint y)
{
    atomicAdd(wave[y * 720u + x], 1u);
}

void bumpParade(int index, int maxIndex)
{
    uint v = atomicAdd(parade[index], 1u) + 1u;
    atomicMax(maxima[maxIndex], v);
}

void bumpHistogram(int index)
{
    uint v = atomicAdd(histogram[index], 1u) + 1u;
    atomicMax(maxima[4], v);
}

void bumpVector(int index)
{
    uint v = atomicAdd(vectorscope[index], 1u) + 1u;
    atomicMax(maxima[5], v);
}

vec3 rgb601LimitedCodes(float y8, float cb8, float cr8)
{
    // MS2130 decoded planes are treated as nominal BT.601 limited-range codes.
    float y = (y8 - 16.0) / 219.0;
    float cb = (cb8 - 128.0) / 224.0;
    float cr = (cr8 - 128.0) / 224.0;

    vec3 rgb;
    rgb.r = y + 1.402000 * cr;
    rgb.g = y - 0.344136 * cb - 0.714136 * cr;
    rgb.b = y + 1.772000 * cb;
    return clamp(rgb, 0.0, 1.0);
}

void accumulatePixel(int sourceX, uint yCode, float cb8, float cr8)
{
    // Preserve the decoded 8-bit luma code exactly. The waveform buffer already
    // spans 0..255; its studio-range IRE graticule remains anchored at 16..235.
    // This keeps PQ code positions and below-black/above-white excursions intact.
    uint waveX = uint(min(719, (sourceX * 720) / 1280));
    bumpWave(waveX, min(yCode, 255u));

    vec3 rgb = rgb601LimitedCodes(float(yCode), cb8, cr8);

    int pyR = clamp(160 - int(round(rgb.r * 160.0)), 0, 160);
    int pyG = clamp(160 - int(round(rgb.g * 160.0)), 0, 160);
    int pyB = clamp(160 - int(round(rgb.b * 160.0)), 0, 160);

    int pxR = min(73,  (sourceX * 74) / 1280);
    int pxG = min(73,  (sourceX * 74) / 1280) + 74;
    int pxB = min(75,  (sourceX * 76) / 1280) + 148;

    bumpParade(pyR * 224 + pxR, 1);
    bumpParade(pyG * 224 + pxG, 2);
    bumpParade(pyB * 224 + pxB, 3);

    ivec3 bins = ivec3(round(rgb * 255.0));
    bumpHistogram(0 * 256 + clamp(bins.r, 0, 255));
    bumpHistogram(1 * 256 + clamp(bins.g, 0, 255));
    bumpHistogram(2 * 256 + clamp(bins.b, 0, 255));
}

void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);

    // One invocation per JPEG 4:2:2 chroma sample / horizontal pixel pair.
    if (p.x >= 640 || p.y >= 720) {
        return;
    }

    int x0 = p.x * 2;
    int x1 = x0 + 1;

    uint y0 = uint(round(texelFetch(uY, ivec2(x0, p.y), 0).r * 255.0));
    uint y1 = uint(round(texelFetch(uY, ivec2(x1, p.y), 0).r * 255.0));
    float cb8 = round(texelFetch(uCb, p, 0).r * 255.0);
    float cr8 = round(texelFetch(uCr, p, 0).r * 255.0);

    accumulatePixel(x0, y0, cb8, cr8);
    accumulatePixel(x1, y1, cb8, cr8);

    // Keep vectorscope in the source YCbCr domain. Nominal BT.601 limited
    // chroma uses 224 code values peak-to-peak around code 128.
    float cb = (cb8 - 128.0) / 224.0;
    float cr = (cr8 - 128.0) / 224.0;

    int gx = int(round(cb * 128.0 + 70.0));
    int gy = int(round(-cr * 128.0 + 70.0));

    if (gx >= 0 && gx < 141 && gy >= 0 && gy < 141) {
        bumpVector(gy * 141 + gx);
    }
}
)";

    return createComputeProgram(source, "scope-accumulate-ms2130-yuv422");
}

template <typename T>
uint64_t sumMappedBuffer(GLuint buffer, size_t itemCount, size_t bytes)
{
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);

    const T* data = reinterpret_cast<const T*>(
            glMapBufferRange(
                    GL_SHADER_STORAGE_BUFFER,
                    0,
                    static_cast<GLsizeiptr>(bytes),
                    GL_MAP_READ_BIT));

    if (data == nullptr) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        return UINT64_MAX;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < itemCount; ++i) {
        sum += static_cast<uint64_t>(data[i]);
    }

    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return sum;
}

}  // namespace

void ScopeGpu::initialize()
{
    GLint glMajor = 0;
    GLint glMinor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
    glGetIntegerv(GL_MINOR_VERSION, &glMinor);

    const bool computeSupported =
            (glMajor > 3) ||
            (glMajor == 3 && glMinor >= 1);

    enabled_ = computeSupported;

    if (!computeSupported) {
        LOGE(
                "Step 15.4: GLES %d.%d has no compute shader support; "
                "scope backend disabled",
                glMajor,
                glMinor);
        return;
    }

    clearProgram_ = createScopeClearProgram();
    accumulateProgram_ = createScopeAccumulateProgram();
    yuv422AccumulateProgram_ = createYuv422ScopeAccumulateProgram();

    if (!clearProgram_ || !accumulateProgram_ || !yuv422AccumulateProgram_) {
        LOGE(
                "Step 15.4: scope compute program creation failed; "
                "preview remains enabled");
        enabled_ = false;
        return;
    }

    packedLocation_ = glGetUniformLocation(accumulateProgram_, "uPacked");
    yLocation_ = glGetUniformLocation(yuv422AccumulateProgram_, "uY");
    cbLocation_ = glGetUniformLocation(yuv422AccumulateProgram_, "uCb");
    crLocation_ = glGetUniformLocation(yuv422AccumulateProgram_, "uCr");

    if (packedLocation_ < 0 || yLocation_ < 0 || cbLocation_ < 0 || crLocation_ < 0) {
        LOGE("Step 16.0: scope sampler uniform not found; scope backend disabled");
        enabled_ = false;
        return;
    }

    glGenTextures(1, &scopeTexture_);
    glBindTexture(GL_TEXTURE_2D, scopeTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, PACKED_W, FRAME_H);

    for (auto& slot : slots_) {
        glGenBuffers(1, &slot.waveformSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, slot.waveformSsbo);
        glBufferData(
                GL_SHADER_STORAGE_BUFFER,
                static_cast<GLsizeiptr>(WAVEFORM_BYTES),
                nullptr,
                GL_DYNAMIC_COPY);

        glGenBuffers(1, &slot.paradeSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, slot.paradeSsbo);
        glBufferData(
                GL_SHADER_STORAGE_BUFFER,
                static_cast<GLsizeiptr>(PARADE_BYTES),
                nullptr,
                GL_DYNAMIC_COPY);

        glGenBuffers(1, &slot.histogramSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, slot.histogramSsbo);
        glBufferData(
                GL_SHADER_STORAGE_BUFFER,
                static_cast<GLsizeiptr>(HIST_BYTES),
                nullptr,
                GL_DYNAMIC_COPY);

        glGenBuffers(1, &slot.vectorscopeSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, slot.vectorscopeSsbo);
        glBufferData(
                GL_SHADER_STORAGE_BUFFER,
                static_cast<GLsizeiptr>(VECTOR_BYTES),
                nullptr,
                GL_DYNAMIC_COPY);

        glGenBuffers(1, &slot.maximaSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, slot.maximaSsbo);
        glBufferData(
                GL_SHADER_STORAGE_BUFFER,
                static_cast<GLsizeiptr>(MAXIMA_BYTES),
                nullptr,
                GL_DYNAMIC_COPY);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    LOGI(
            "Step 15.4: GPU scope backend enabled: packed=%dx%d "
            "wave=%dx%d parade=%dx%d histogram=%dx%d vector=%dx%d GLES=%d.%d",
            PACKED_W,
            FRAME_H,
            WAVEFORM_W,
            WAVEFORM_H,
            PARADE_PLOT_W,
            PARADE_PLOT_H,
            3,
            HIST_BINS,
            VECTOR_D,
            VECTOR_D,
            glMajor,
            glMinor);
}

void ScopeGpu::poll(bool preDispatchPoll)
{
    if (!enabled_) {
        return;
    }

    Slot& back = slots_[back_];

    if (back.fence == nullptr) {
        return;
    }

    const GLenum result = glClientWaitSync(back.fence, 0, 0);

    if (result == GL_ALREADY_SIGNALED ||
        result == GL_CONDITION_SATISFIED) {

        glDeleteSync(back.fence);
        back.fence = nullptr;

        std::swap(front_, back_);
        frontValid_ = true;
        ++promotions_;

        if (preDispatchPoll) {
            ++preDispatchPromotions_;
        }

        if (validationPasses_ + validationFailures_ < 3) {
            const Slot& front = slots_[front_];

            const uint64_t waveformSum =
                    sumMappedBuffer<uint32_t>(
                            front.waveformSsbo,
                            WAVEFORM_ITEMS,
                            WAVEFORM_BYTES);

            const uint64_t paradeSum =
                    sumMappedBuffer<uint32_t>(
                            front.paradeSsbo,
                            PARADE_ITEMS,
                            PARADE_BYTES);

            const uint64_t histogramSum =
                    sumMappedBuffer<uint32_t>(
                            front.histogramSsbo,
                            HIST_ITEMS,
                            HIST_BYTES);

            const uint64_t vectorSum =
                    sumMappedBuffer<uint32_t>(
                            front.vectorscopeSsbo,
                            VECTOR_ITEMS,
                            VECTOR_BYTES);

            const uint64_t expectedWave = front.expectedPixels;
            const uint64_t expectedParade = expectedWave * 3u;
            const uint64_t expectedHistogram = expectedWave * 3u;
            const uint64_t vectorSampleMax = front.expectedVectorSamples;

            // Vector bins intentionally reject chroma outside the 141x141
            // plotted domain, so the accepted total may be below 360x480.
            const bool vectorCountValid = vectorSum <= vectorSampleMax;

            if (waveformSum == expectedWave &&
                paradeSum == expectedParade &&
                histogramSum == expectedHistogram &&
                vectorCountValid) {
                ++validationPasses_;
                LOGI(
                        "Step 15.4 VALIDATION PASS: seq=%llu "
                        "waveform_sum=%llu expected=%llu "
                        "parade_sum=%llu expected=%llu "
                        "histogram_sum=%llu expected=%llu "
                        "vector_sum=%llu max=%llu",
                        static_cast<unsigned long long>(front.sequence),
                        static_cast<unsigned long long>(waveformSum),
                        static_cast<unsigned long long>(expectedWave),
                        static_cast<unsigned long long>(paradeSum),
                        static_cast<unsigned long long>(expectedParade),
                        static_cast<unsigned long long>(histogramSum),
                        static_cast<unsigned long long>(expectedHistogram),
                        static_cast<unsigned long long>(vectorSum),
                        static_cast<unsigned long long>(vectorSampleMax));
            }
            else {
                ++validationFailures_;
                LOGE(
                        "Step 15.4 VALIDATION FAIL: seq=%llu "
                        "waveform_sum=%llu expected=%llu "
                        "parade_sum=%llu expected=%llu "
                        "histogram_sum=%llu expected=%llu "
                        "vector_sum=%llu max=%llu",
                        static_cast<unsigned long long>(front.sequence),
                        static_cast<unsigned long long>(waveformSum),
                        static_cast<unsigned long long>(expectedWave),
                        static_cast<unsigned long long>(paradeSum),
                        static_cast<unsigned long long>(expectedParade),
                        static_cast<unsigned long long>(histogramSum),
                        static_cast<unsigned long long>(expectedHistogram),
                        static_cast<unsigned long long>(vectorSum),
                        static_cast<unsigned long long>(vectorSampleMax));
            }
        }
    }
    else if (result == GL_WAIT_FAILED) {
        LOGE("Step 15.4: scope fence wait failed");
    }
}

void ScopeGpu::queueFromPbo(
        GLuint pboId,
        GLsync& pboFence,
        uint64_t sequence)
{
    if (!enabled_) {
        return;
    }

    Slot& back = slots_[back_];

    if (back.fence != nullptr) {
        ++busySkips_;
        return;
    }

    // One post-present upload feeds all scope compute work. The preview texture
    // stays independent so the next camera frame never races a scope read.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scopeTexture_);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboId);

    glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            PACKED_W,
            FRAME_H,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr);

    if (pboFence != nullptr) {
        glDeleteSync(pboFence);
        pboFence = nullptr;
    }

    pboFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, back.waveformSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, back.paradeSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, back.histogramSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, back.vectorscopeSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, back.maximaSsbo);

    glUseProgram(clearProgram_);
    glDispatchCompute(
            static_cast<GLuint>((WAVEFORM_ITEMS + 255u) / 256u),
            1u,
            1u);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(accumulateProgram_);
    glUniform1i(packedLocation_, 0);
    glDispatchCompute(
            static_cast<GLuint>((PACKED_W + 15) / 16),
            static_cast<GLuint>((FRAME_H + 15) / 16),
            1u);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    back.sequence = sequence;
    back.expectedPixels = static_cast<uint64_t>(FRAME_W) * FRAME_H;
    back.expectedVectorSamples = static_cast<uint64_t>(PACKED_W) * FRAME_H;
    back.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    glFlush();
    ++dispatches_;

    if (dispatches_ <= 5 || (dispatches_ % 300) == 0) {
        LOGI(
                "Step 15.4 scope queued #%llu seq=%llu "
                "busySkip=%llu preDispatchPromote=%llu",
                static_cast<unsigned long long>(dispatches_),
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(busySkips_),
                static_cast<unsigned long long>(preDispatchPromotions_));
    }
}

void ScopeGpu::queueFromYuv422Textures(
        GLuint yTexture,
        GLuint cbTexture,
        GLuint crTexture,
        uint64_t sequence)
{
    if (!enabled_ ||
        yTexture == 0 ||
        cbTexture == 0 ||
        crTexture == 0) {
        return;
    }

    Slot& back = slots_[back_];

    if (back.fence != nullptr) {
        ++busySkips_;
        return;
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, back.waveformSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, back.paradeSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, back.histogramSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, back.vectorscopeSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, back.maximaSsbo);

    glUseProgram(clearProgram_);
    glDispatchCompute(
            static_cast<GLuint>((WAVEFORM_ITEMS + 255u) / 256u),
            1u,
            1u);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Reuse the three preview textures. Commands are submitted on the same GL
    // context after present, so the compute read is ordered before the next
    // frame's glTexSubImage2D updates of these texture objects.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, cbTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, crTexture);

    glUseProgram(yuv422AccumulateProgram_);
    glUniform1i(yLocation_, 0);
    glUniform1i(cbLocation_, 1);
    glUniform1i(crLocation_, 2);
    glDispatchCompute(
            static_cast<GLuint>((640 + 15) / 16),
            static_cast<GLuint>((720 + 15) / 16),
            1u);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    back.sequence = sequence;
    back.expectedPixels = 1280u * 720u;
    back.expectedVectorSamples = 640u * 720u;
    back.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    glFlush();
    ++dispatches_;

    if (dispatches_ <= 5 || (dispatches_ % 300) == 0) {
        LOGI(
                "Step 16.0 MS2130 scope queued #%llu seq=%llu "
                "source=YUV422_1280x720 busySkip=%llu preDispatchPromote=%llu",
                static_cast<unsigned long long>(dispatches_),
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(busySkips_),
                static_cast<unsigned long long>(preDispatchPromotions_));
    }
}

GLuint ScopeGpu::frontWaveformSsbo() const
{
    return frontValid_ ? slots_[front_].waveformSsbo : 0;
}

GLuint ScopeGpu::frontParadeSsbo() const
{
    return frontValid_ ? slots_[front_].paradeSsbo : 0;
}

GLuint ScopeGpu::frontHistogramSsbo() const
{
    return frontValid_ ? slots_[front_].histogramSsbo : 0;
}

GLuint ScopeGpu::frontVectorscopeSsbo() const
{
    return frontValid_ ? slots_[front_].vectorscopeSsbo : 0;
}

GLuint ScopeGpu::frontMaximaSsbo() const
{
    return frontValid_ ? slots_[front_].maximaSsbo : 0;
}

void ScopeGpu::shutdown()
{
    if (clearProgram_ != 0 ||
        accumulateProgram_ != 0 ||
        yuv422AccumulateProgram_ != 0 ||
        scopeTexture_ != 0 ||
        slots_[0].waveformSsbo != 0 ||
        slots_[1].waveformSsbo != 0) {

        LOGI(
                "Step 15.4 scope stats: enabled=%s dispatch=%llu promote=%llu "
                "preDispatchPromote=%llu busySkip=%llu validationPass=%llu "
                "validationFail=%llu frontValid=%s",
                enabled_ ? "YES" : "NO",
                static_cast<unsigned long long>(dispatches_),
                static_cast<unsigned long long>(promotions_),
                static_cast<unsigned long long>(preDispatchPromotions_),
                static_cast<unsigned long long>(busySkips_),
                static_cast<unsigned long long>(validationPasses_),
                static_cast<unsigned long long>(validationFailures_),
                frontValid_ ? "YES" : "NO");
    }

    for (auto& slot : slots_) {
        if (slot.fence != nullptr) {
            glDeleteSync(slot.fence);
            slot.fence = nullptr;
        }

        if (slot.waveformSsbo != 0) {
            glDeleteBuffers(1, &slot.waveformSsbo);
            slot.waveformSsbo = 0;
        }

        if (slot.paradeSsbo != 0) {
            glDeleteBuffers(1, &slot.paradeSsbo);
            slot.paradeSsbo = 0;
        }

        if (slot.histogramSsbo != 0) {
            glDeleteBuffers(1, &slot.histogramSsbo);
            slot.histogramSsbo = 0;
        }

        if (slot.vectorscopeSsbo != 0) {
            glDeleteBuffers(1, &slot.vectorscopeSsbo);
            slot.vectorscopeSsbo = 0;
        }

        if (slot.maximaSsbo != 0) {
            glDeleteBuffers(1, &slot.maximaSsbo);
            slot.maximaSsbo = 0;
        }
    }

    if (scopeTexture_ != 0) {
        glDeleteTextures(1, &scopeTexture_);
        scopeTexture_ = 0;
    }

    if (clearProgram_ != 0) {
        glDeleteProgram(clearProgram_);
        clearProgram_ = 0;
    }

    if (accumulateProgram_ != 0) {
        glDeleteProgram(accumulateProgram_);
        accumulateProgram_ = 0;
    }

    if (yuv422AccumulateProgram_ != 0) {
        glDeleteProgram(yuv422AccumulateProgram_);
        yuv422AccumulateProgram_ = 0;
    }

    packedLocation_ = -1;
    yLocation_ = -1;
    cbLocation_ = -1;
    crLocation_ = -1;
    enabled_ = false;
    frontValid_ = false;
}

}  // namespace field_monitor
