#include "scope_ui.h"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "UvcFieldMonitor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace field_monitor {
namespace {

struct UiColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct UiVertex {
    float x = 0.0f;
    float y = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

static GLuint compileShader(
        GLenum type,
        const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint createWaveformRenderProgram()
{
    static const char* vertexSource = R"(#version 310 es

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

    // Top-origin panel UV, matching the CM4 logical UI.
    vUv = vec2(
        (p.x + 1.0) * 0.5,
        1.0 - ((p.y + 1.0) * 0.5)
    );
}
)";


    static const char* fragmentSource = R"(#version 310 es

precision highp float;
precision highp int;

in vec2 vUv;

layout(std430, binding = 0) readonly buffer WaveformBuffer {
    uint wave[];
};

out vec4 outColor;

void main()
{
    const int PANEL_W = 330;
    const int PANEL_H = 200;

    const int LEFT = 32;
    const int TOP = 28;
    const int PLOT_W = 291;
    const int PLOT_H = 161;

    ivec2 p = ivec2(
        floor(
            clamp(
                vUv,
                vec2(0.0),
                vec2(0.999999)
            ) * vec2(
                float(PANEL_W),
                float(PANEL_H)
            )
        )
    );

    float intensity = 0.0;

    if (p.x >= LEFT &&
        p.x < LEFT + PLOT_W &&
        p.y >= TOP &&
        p.y < TOP + PLOT_H) {

        int plotX = p.x - LEFT;
        int plotY = p.y - TOP;

        // 720 source columns -> 291 CM4 display columns.
        int x0 = (plotX * 720) / PLOT_W;
        int x1 = (((plotX + 1) * 720) / PLOT_W) - 1;

        x0 = clamp(x0, 0, 719);
        x1 = clamp(max(x0, x1), 0, 719);

        // Full 8-bit luma domain. The display is top-origin, so reverse
        // the 161-row plot before mapping it to Y code 0..255.
        int rowFromBottom = (PLOT_H - 1) - plotY;

        int y0 = (rowFromBottom * 256) / PLOT_H;
        int y1 = (((rowFromBottom + 1) * 256) / PLOT_H) - 1;

        y0 = clamp(y0, 0, 255);
        y1 = clamp(max(y0, y1), 0, 255);

        uint sum = 0u;

        for (int yy = y0; yy <= y1; ++yy) {
            for (int xx = x0; xx <= x1; ++xx) {
                sum += wave[yy * 720 + xx];
            }
        }

        float xCount = float(x1 - x0 + 1);

        // Horizontal downsampling combines 2-3 source columns. Normalize
        // that grouping so the density scale remains approximately 0..480
        // samples per original column.
        float count = float(sum) / max(xCount, 1.0);

        const float gain = 1.3;
        const float maxCount = 480.0;

        if (count > 0.0) {
            intensity =
                log(1.0 + count * gain) /
                log(1.0 + maxCount * gain);
        }
    }

    if (intensity <= 0.0) {
        discard;
    }

    // The waveform is composited after the graticule.  Use the measured
    // density as alpha so only waveform samples cover the grid beneath them.
    outColor = vec4(
        vec3(1.0),
        clamp(intensity, 0.0, 1.0)
    );
}
)";


    GLuint vs =
            compileShader(
                    GL_VERTEX_SHADER,
                    vertexSource
            );

    GLuint fs =
            compileShader(
                    GL_FRAGMENT_SHADER,
                    fragmentSource
            );

    if (!vs || !fs) {

        if (vs) {
            glDeleteShader(vs);
        }

        if (fs) {
            glDeleteShader(fs);
        }

        return 0;
    }

    GLuint program =
            glCreateProgram();

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;

    glGetProgramiv(
            program,
            GL_LINK_STATUS,
            &ok
    );

    if (!ok) {

        char log[2048] = {};

        glGetProgramInfoLog(
                program,
                sizeof(log),
                nullptr,
                log
        );

        LOGE(
                "Step 15.1: waveform render program link error: %s",
                log
        );

        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return program;
}


static GLuint createParadeRenderProgram()
{
    static const char* vertexSource = R"(#version 310 es

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

    static const char* fragmentSource = R"(#version 310 es

precision highp float;
precision highp int;

in vec2 vUv;

layout(std430, binding = 1) readonly buffer ParadeBuffer {
    uint parade[];
};

layout(std430, binding = 4) readonly buffer MaximaBuffer {
    uint maxima[];
};

out vec4 outColor;

float density(uint v, uint vmax, float gain)
{
    if (v == 0u || vmax == 0u) {
        return 0.0;
    }

    return log(1.0 + float(v) * gain) /
           log(1.0 + float(vmax) * gain);
}

void main()
{
    const int PANEL_W = 239;
    const int PANEL_H = 200;
    const int LEFT = 8;
    const int TOP = 28;
    const int PLOT_W = 224;
    const int PLOT_H = 161;

    ivec2 p = ivec2(
        floor(
            clamp(vUv, vec2(0.0), vec2(0.999999)) *
            vec2(float(PANEL_W), float(PANEL_H))
        )
    );

    vec3 color = vec3(0.0);
    float intensity = 0.0;

    if (p.x >= LEFT &&
        p.x < LEFT + PLOT_W &&
        p.y >= TOP &&
        p.y < TOP + PLOT_H) {

        int ax = p.x - LEFT;
        int y = p.y - TOP;
        int channel = (ax < 74) ? 0 : ((ax < 148) ? 1 : 2);

        uint d = parade[y * PLOT_W + ax];

        // Mali-G57 GLSL ES compiler rejects passing a readonly SSBO
        // element directly as a function argument (S0001: discards
        // 'readonly' access qualifier). Copy it to a local value first.
        uint vmax = maxima[1 + channel];
        float v = density(d, vmax, 1.3);
        intensity = v;

        if (channel == 0) {
            color = vec3(1.0, 0.0, 0.0);
        }
        else if (channel == 1) {
            color = vec3(0.0, 1.0, 0.0);
        }
        else {
            color = vec3(0.0, 0.0, 1.0);
        }
    }

    if (intensity <= 0.0) {
        discard;
    }

    outColor = vec4(color, clamp(intensity, 0.0, 1.0));
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
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Step 15.2: parade render program link error: %s", log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}


static GLuint createHistogramRenderProgram()
{
    static const char* vertexSource = R"(#version 310 es

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

    static const char* fragmentSource = R"(#version 310 es

precision highp float;
precision highp int;

in vec2 vUv;

layout(std430, binding = 2) readonly buffer HistogramBuffer {
    uint histogram[];
};

layout(std430, binding = 4) readonly buffer MaximaBuffer {
    uint maxima[];
};

out vec4 outColor;

void main()
{
    const int PANEL_W = 285;
    const int PANEL_H = 200;
    const int LEFT = 10;
    const int RIGHT = 278;
    const int TOP = 32;
    const int BOTTOM = 188;

    ivec2 p = ivec2(
        floor(
            clamp(vUv, vec2(0.0), vec2(0.999999)) *
            vec2(float(PANEL_W), float(PANEL_H))
        )
    );

    vec3 color = vec3(0.0);

    if (p.x >= LEFT && p.x < RIGHT &&
        p.y >= TOP && p.y <= BOTTOM) {

        int ax = p.x - LEFT;
        int bin = clamp(
            int(round(float(ax) * 255.0 / 267.0)),
            0,
            255
        );

        // Mali-G57 GLSL ES compiler can reject passing a readonly SSBO
        // element directly to a built-in function. Copy to a local first.
        uint hmax = maxima[4];
        uint hm = max(hmax, 1u);

        for (int channel = 0; channel < 3; ++channel) {
            uint count = histogram[channel * 256 + bin];

            if (count == 0u) {
                continue;
            }

            float n = float(count) / float(hm);
            int yy = BOTTOM - int(round(n * float(BOTTOM - TOP)));

            if (p.y >= yy && p.y <= BOTTOM) {
                if (channel == 0) {
                    color = max(color, vec3(1.0, 0.0, 0.0));
                }
                else if (channel == 1) {
                    color = max(color, vec3(0.0, 1.0, 0.0));
                }
                else {
                    color = max(color, vec3(0.0, 80.0 / 255.0, 1.0));
                }
            }
        }
    }

    outColor = vec4(color, 1.0);
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
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Step 15.3: histogram render program link error: %s", log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}


static GLuint createVectorscopeRenderProgram()
{
    static const char* vertexSource = R"(#version 310 es

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

    static const char* fragmentSource = R"(#version 310 es

precision highp float;
precision highp int;

in vec2 vUv;

layout(std430, binding = 3) readonly buffer VectorscopeBuffer {
    uint vectorscope[];
};

layout(std430, binding = 4) readonly buffer MaximaBuffer {
    uint maxima[];
};

out vec4 outColor;

float density(uint v, uint vmax, float gain)
{
    if (v == 0u || vmax == 0u) {
        return 0.0;
    }

    return log(1.0 + float(v) * gain) /
           log(1.0 + float(vmax) * gain);
}

void main()
{
    const int PANEL_W = 284;
    const int PANEL_H = 200;
    const int CX = 142;
    const int CY = 111;
    const int RADIUS = 70;
    const int VECTOR_D = 141;

    ivec2 p = ivec2(
        floor(
            clamp(vUv, vec2(0.0), vec2(0.999999)) *
            vec2(float(PANEL_W), float(PANEL_H))
        )
    );

    int dx = p.x - CX;
    int dy = p.y - CY;
    int rr = dx * dx + dy * dy;
    float intensity = 0.0;

    if (abs(dx) <= RADIUS &&
        abs(dy) <= RADIUS &&
        rr <= RADIUS * RADIUS) {

        int vx = dx + RADIUS;
        int vy = dy + RADIUS;
        uint d = 0u;

        // Display-only 3x3 expansion. The accumulator itself remains one
        // exact Cb/Cr bin, matching the CM4 implementation.
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                int sx = vx + ox;
                int sy = vy + oy;

                if (sx >= 0 && sx < VECTOR_D &&
                    sy >= 0 && sy < VECTOR_D) {
                    // Mali-G57: do not pass a readonly SSBO element directly
                    // to max(); copy the value to a local first.
                    uint binValue = vectorscope[sy * VECTOR_D + sx];
                    d = max(d, binValue);
                }
            }
        }

        // Mali-G57: same readonly-SSBO qualifier workaround as Parade
        // and Histogram. Pass a plain local value to density().
        uint vmax = maxima[5];
        intensity = density(d, vmax, 1.4);
    }

    outColor = vec4(vec3(clamp(intensity, 0.0, 1.0)), 1.0);
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
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Step 15.4: vectorscope render program link error: %s", log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}


static GLuint createUiProgram()
{
    static const char* vertexSource = R"(#version 300 es

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec4 aColor;

out vec4 vColor;

void main()
{
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vColor = aColor;
}
)";


    static const char* fragmentSource = R"(#version 300 es

precision mediump float;

in vec4 vColor;

out vec4 outColor;

void main()
{
    outColor = vColor;
}
)";


    GLuint vs =
            compileShader(
                    GL_VERTEX_SHADER,
                    vertexSource
            );

    GLuint fs =
            compileShader(
                    GL_FRAGMENT_SHADER,
                    fragmentSource
            );

    if (!vs || !fs) {

        if (vs) {
            glDeleteShader(vs);
        }

        if (fs) {
            glDeleteShader(fs);
        }

        return 0;
    }

    GLuint program =
            glCreateProgram();

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;

    glGetProgramiv(
            program,
            GL_LINK_STATUS,
            &ok
    );

    if (!ok) {

        char log[2048] = {};

        glGetProgramInfoLog(
                program,
                sizeof(log),
                nullptr,
                log
        );

        LOGE(
                "Step 15.1: UI program link error: %s",
                log
        );

        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return program;
}


using UiGlyph = std::array<uint8_t, 7>;


static const std::unordered_map<char, UiGlyph>& uiFont()
{
    static const std::unordered_map<char, UiGlyph> f = {
        {'A',{14,17,17,31,17,17,17}},
        {'B',{30,17,17,30,17,17,30}},
        {'C',{15,16,16,16,16,16,15}},
        {'D',{30,17,17,17,17,17,30}},
        {'E',{31,16,16,30,16,16,31}},
        {'F',{31,16,16,30,16,16,16}},
        {'G',{15,16,16,19,17,17,15}},
        {'H',{17,17,17,31,17,17,17}},
        {'I',{31,4,4,4,4,4,31}},
        {'J',{7,2,2,2,18,18,12}},
        {'K',{17,18,20,24,20,18,17}},
        {'L',{16,16,16,16,16,16,31}},
        {'M',{17,27,21,21,17,17,17}},
        {'N',{17,25,21,19,17,17,17}},
        {'O',{14,17,17,17,17,17,14}},
        {'P',{30,17,17,30,16,16,16}},
        {'Q',{14,17,17,17,21,18,13}},
        {'R',{30,17,17,30,20,18,17}},
        {'S',{15,16,16,14,1,1,30}},
        {'T',{31,4,4,4,4,4,4}},
        {'U',{17,17,17,17,17,17,14}},
        {'V',{17,17,17,17,17,10,4}},
        {'W',{17,17,17,21,21,27,17}},
        {'X',{17,17,10,4,10,17,17}},
        {'Y',{17,17,10,4,4,4,4}},
        {'Z',{31,1,2,4,8,16,31}},
        {'0',{14,17,19,21,25,17,14}},
        {'1',{4,12,4,4,4,4,14}},
        {'2',{14,17,1,2,4,8,31}},
        {'3',{30,1,1,14,1,1,30}},
        {'4',{2,6,10,18,31,2,2}},
        {'5',{31,16,16,30,1,1,30}},
        {'6',{14,16,16,30,17,17,14}},
        {'7',{31,1,2,4,8,8,8}},
        {'8',{14,17,17,14,17,17,14}},
        {'9',{14,17,17,15,1,1,14}},
        {'.',{0,0,0,0,0,12,12}},
        {':',{0,12,12,0,12,12,0}},
        {'/',{1,2,2,4,8,8,16}},
        {'-',{0,0,0,31,0,0,0}},
        {'_',{0,0,0,0,0,0,31}},
        {'%',{25,26,2,4,8,11,19}},
        {'x',{0,0,17,10,4,10,17}},
        {' ',{0,0,0,0,0,0,0}}
    };

    return f;
}


struct VectorTargetUi {
    const char* label;
    float cb;
    float cr;
    UiColor color;
    int labelDx;
    int labelDy;
};


static constexpr VectorTargetUi kVectorTargets601Ui[] = {
    {"R",  -0.1687359f,  0.5000000f, {1.0f, 80.0f/255.0f, 80.0f/255.0f, 1.0f}, -3, -11},
    {"M",   0.3312641f,  0.4186876f, {1.0f, 80.0f/255.0f, 1.0f, 1.0f}, 5, -8},
    {"B",   0.5000000f, -0.0813124f, {80.0f/255.0f, 120.0f/255.0f, 1.0f, 1.0f}, 6, -3},
    {"CY",  0.1687359f, -0.5000000f, {80.0f/255.0f, 1.0f, 1.0f, 1.0f}, -5, 6},
    {"G",  -0.3312641f, -0.4186876f, {80.0f/255.0f, 1.0f, 80.0f/255.0f, 1.0f}, -12, 5},
    {"Y",  -0.5000000f,  0.0813124f, {1.0f, 1.0f, 80.0f/255.0f, 1.0f}, -12, -3},
};

}  // namespace

bool ScopeUi::initialize()
{
    waveformRenderProgram_ = createWaveformRenderProgram();
    paradeRenderProgram_ = createParadeRenderProgram();
    histogramRenderProgram_ = createHistogramRenderProgram();
    vectorscopeRenderProgram_ = createVectorscopeRenderProgram();
    uiProgram_ = createUiProgram();

    glGenBuffers(1, &uiVbo_);

    if (!waveformRenderProgram_ || !paradeRenderProgram_ ||
        !histogramRenderProgram_ || !vectorscopeRenderProgram_ ||
        !uiProgram_ || uiVbo_ == 0) {
        LOGE(
                "Step 15.4: UI renderer initialization failed "
                "waveProgram=%u paradeProgram=%u histProgram=%u "
                "vectorProgram=%u uiProgram=%u uiVbo=%u",
                waveformRenderProgram_,
                paradeRenderProgram_,
                histogramRenderProgram_,
                vectorscopeRenderProgram_,
                uiProgram_,
                uiVbo_);
        return false;
    }

    LOGI(
            "Step 15.7: orientation-aware clean UI enabled "
            "(landscape + centered portrait 710x796)");

    return true;
}

void ScopeUi::drawWaveform(
        GLuint waveformSsbo,
        bool frontValid,
        const Viewport& viewport,
        GLuint vao) const
{
    if (!frontValid || waveformSsbo == 0 || waveformRenderProgram_ == 0) {
        return;
    }

    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, waveformSsbo);
    glUseProgram(waveformRenderProgram_);
    glBindVertexArray(vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

void ScopeUi::drawParade(
        GLuint paradeSsbo,
        GLuint maximaSsbo,
        bool frontValid,
        const Viewport& viewport,
        GLuint vao) const
{
    if (!frontValid ||
        paradeSsbo == 0 ||
        maximaSsbo == 0 ||
        paradeRenderProgram_ == 0) {
        return;
    }

    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, paradeSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, maximaSsbo);
    glUseProgram(paradeRenderProgram_);
    glBindVertexArray(vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

void ScopeUi::drawHistogram(
        GLuint histogramSsbo,
        GLuint maximaSsbo,
        bool frontValid,
        const Viewport& viewport,
        GLuint vao) const
{
    if (!frontValid ||
        histogramSsbo == 0 ||
        maximaSsbo == 0 ||
        histogramRenderProgram_ == 0) {
        return;
    }

    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, histogramSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, maximaSsbo);
    glUseProgram(histogramRenderProgram_);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void ScopeUi::drawVectorscope(
        GLuint vectorscopeSsbo,
        GLuint maximaSsbo,
        bool frontValid,
        const Viewport& viewport,
        GLuint vao) const
{
    if (!frontValid ||
        vectorscopeSsbo == 0 ||
        maximaSsbo == 0 ||
        vectorscopeRenderProgram_ == 0) {
        return;
    }

    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, vectorscopeSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, maximaSsbo);
    glUseProgram(vectorscopeRenderProgram_);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void ScopeUi::drawOverlay(
        double currentUiFps,
        EGLint surfaceWidth,
        EGLint surfaceHeight,
        const UiLayout& layout,
        const UiCanvasViewport& canvas,
        GLuint vao)
{
    std::vector<UiVertex> vertices;
    vertices.reserve(12000);

        vertices.clear();

        auto logicalToClip =
                [&](float x, float y) -> std::array<float, 2> {

            const float px =
                    static_cast<float>(canvas.x) +
                    x * canvas.scale;

            const float py =
                    static_cast<float>(canvas.y) +
                    static_cast<float>(canvas.height) -
                    y * canvas.scale;

            const float nx =
                    (px / static_cast<float>(surfaceWidth)) *
                    2.0f - 1.0f;

            const float ny =
                    (py / static_cast<float>(surfaceHeight)) *
                    2.0f - 1.0f;

            return {nx, ny};
        };


        auto pushVertex =
                [&](const std::array<float, 2>& p,
                    const UiColor& c) {

            vertices.push_back(
                    {
                        p[0],
                        p[1],
                        c.r,
                        c.g,
                        c.b,
                        c.a
                    }
            );
        };


        auto addQuad =
                [&](float x0, float y0,
                    float x1, float y1,
                    float x2, float y2,
                    float x3, float y3,
                    const UiColor& c) {

            const auto p0 = logicalToClip(x0, y0);
            const auto p1 = logicalToClip(x1, y1);
            const auto p2 = logicalToClip(x2, y2);
            const auto p3 = logicalToClip(x3, y3);

            pushVertex(p0, c);
            pushVertex(p1, c);
            pushVertex(p2, c);

            pushVertex(p0, c);
            pushVertex(p2, c);
            pushVertex(p3, c);
        };


        auto addRect =
                [&](float x, float y,
                    float w, float h,
                    const UiColor& c) {

            addQuad(
                    x,     y,
                    x + w, y,
                    x + w, y + h,
                    x,     y + h,
                    c
            );
        };


        auto addLine =
                [&](float x0, float y0,
                    float x1, float y1,
                    float thickness,
                    const UiColor& c) {

            const float dx = x1 - x0;
            const float dy = y1 - y0;

            const float len =
                    std::sqrt(
                            dx * dx +
                            dy * dy
                    );

            if (len <= 0.0001f) {
                return;
            }

            const float half =
                    thickness * 0.5f;

            const float px =
                    -dy / len * half;

            const float py =
                    dx / len * half;

            addQuad(
                    x0 + px, y0 + py,
                    x1 + px, y1 + py,
                    x1 - px, y1 - py,
                    x0 - px, y0 - py,
                    c
            );
        };


        auto addText =
                [&](int x,
                    int y,
                    const std::string& text,
                    const UiColor& c,
                    int scale) {

            int cursorX = x;

            for (char raw : text) {

                char ch = raw;

                if (ch >= 'a' &&
                    ch <= 'z' &&
                    ch != 'x') {

                    ch =
                            static_cast<char>(
                                    std::toupper(
                                            static_cast<unsigned char>(
                                                    ch
                                            )
                                    )
                            );
                }

                const auto it =
                        uiFont().find(ch);

                if (it != uiFont().end()) {

                    for (int row = 0;
                         row < 7;
                         ++row) {

                        for (int col = 0;
                             col < 5;
                             ++col) {

                            if (
                                (
                                    it->second[row] >>
                                    (4 - col)
                                ) & 1u
                            ) {

                                addRect(
                                        static_cast<float>(
                                                cursorX +
                                                col * scale
                                        ),
                                        static_cast<float>(
                                                y +
                                                row * scale
                                        ),
                                        static_cast<float>(
                                                scale
                                        ),
                                        static_cast<float>(
                                                scale
                                        ),
                                        c
                                );
                            }
                        }
                    }
                }

                cursorX +=
                        6 * scale;
            }
        };


        const UiColor border{
                65.0f / 255.0f,
                70.0f / 255.0f,
                74.0f / 255.0f,
                1.0f
        };

        const UiColor separator{
                70.0f / 255.0f,
                75.0f / 255.0f,
                79.0f / 255.0f,
                1.0f
        };

        const UiColor grid{
                38.0f / 255.0f,
                42.0f / 255.0f,
                45.0f / 255.0f,
                1.0f
        };

        const UiColor label{
                220.0f / 255.0f,
                224.0f / 255.0f,
                228.0f / 255.0f,
                1.0f
        };

        const UiColor axis{
                140.0f / 255.0f,
                145.0f / 255.0f,
                150.0f / 255.0f,
                1.0f
        };

        const UiColor previewText{
                235.0f / 255.0f,
                238.0f / 255.0f,
                240.0f / 255.0f,
                1.0f
        };

        const UiColor guide{
                235.0f / 255.0f,
                235.0f / 255.0f,
                235.0f / 255.0f,
                190.0f / 255.0f
        };

        const UiColor bar{
                0.0f,
                0.0f,
                0.0f,
                1.0f
        };


        // Step 15.7: one renderer, two logical layouts.  Status telemetry
        // remains outside the clean source image in both orientations.
        const RectI& inputStatus = layout.inputStatus;
        const RectI& preview = layout.preview;
        const RectI& runtimeStatus = layout.runtimeStatus;
        const RectI& wavePanel = layout.waveform;
        const RectI& paradePanel = layout.parade;
        const RectI& histPanel = layout.histogram;
        const RectI& vectorPanel = layout.vectorscope;

        addRect(
                static_cast<float>(inputStatus.x),
                static_cast<float>(inputStatus.y),
                static_cast<float>(inputStatus.width),
                static_cast<float>(inputStatus.height),
                bar
        );

        addRect(
                static_cast<float>(runtimeStatus.x),
                static_cast<float>(runtimeStatus.y),
                static_cast<float>(runtimeStatus.width),
                static_cast<float>(runtimeStatus.height),
                bar
        );

        addText(
                inputStatus.x + 12,
                inputStatus.y + 8,
                "IN  UVC MJPEG  1280x720    60.00P    BT.2020    10-BIT",
                previewText,
                1
        );

        addText(
                runtimeStatus.x + 12,
                runtimeStatus.y + 7,
                "CLEAN",
                previewText,
                1
        );

        char fpsText[48] = {};

        std::snprintf(
                fpsText,
                sizeof(fpsText),
                "UI %.1f FPS",
                currentUiFps
        );

        addText(
                runtimeStatus.x + runtimeStatus.width - 130,
                runtimeStatus.y + 7,
                fpsText,
                previewText,
                1
        );


        // Optical overlays remain inside the source image only.
        const int mx = preview.x + preview.width / 2;
        const int my = preview.y + preview.height / 2;

        addRect(
                static_cast<float>(mx - 12),
                static_cast<float>(my),
                25.0f,
                1.0f,
                guide
        );

        addRect(
                static_cast<float>(mx),
                static_cast<float>(my - 12),
                1.0f,
                25.0f,
                guide
        );

        const int safeX =
                preview.x +
                static_cast<int>(
                        std::lround(preview.width * 0.05)
                );

        const int safeY =
                preview.y +
                static_cast<int>(
                        std::lround(preview.height * 0.05)
                );

        const int safeW =
                static_cast<int>(
                        std::lround(preview.width * 0.90)
                );

        const int safeH =
                static_cast<int>(
                        std::lround(preview.height * 0.90)
                );

        addRect(
                static_cast<float>(safeX),
                static_cast<float>(safeY),
                static_cast<float>(safeW),
                1.0f,
                guide
        );

        addRect(
                static_cast<float>(safeX),
                static_cast<float>(safeY + safeH - 1),
                static_cast<float>(safeW),
                1.0f,
                guide
        );

        addRect(
                static_cast<float>(safeX),
                static_cast<float>(safeY),
                1.0f,
                static_cast<float>(safeH),
                guide
        );

        addRect(
                static_cast<float>(safeX + safeW - 1),
                static_cast<float>(safeY),
                1.0f,
                static_cast<float>(safeH),
                guide
        );


        // Scope panel borders.
        const auto addPanelBorder =
                [&](const RectI& r) {

            addRect(
                    static_cast<float>(r.x),
                    static_cast<float>(r.y),
                    static_cast<float>(r.width),
                    1.0f,
                    border);

            addRect(
                    static_cast<float>(r.x),
                    static_cast<float>(r.y + r.height - 1),
                    static_cast<float>(r.width),
                    1.0f,
                    border);

            addRect(
                    static_cast<float>(r.x),
                    static_cast<float>(r.y),
                    1.0f,
                    static_cast<float>(r.height),
                    border);

            addRect(
                    static_cast<float>(r.x + r.width - 1),
                    static_cast<float>(r.y),
                    1.0f,
                    static_cast<float>(r.height),
                    border);
        };

        addPanelBorder(wavePanel);
        addPanelBorder(paradePanel);
        addPanelBorder(histPanel);
        addPanelBorder(vectorPanel);


        // Landscape keeps the original vertical preview/scope separator.
        // Portrait stacks the scope matrix below the runtime status row.
        if (layout.mode == UiLayoutMode::Landscape) {
            addRect(
                    static_cast<float>(wavePanel.x),
                    static_cast<float>(preview.y),
                    1.0f,
                    static_cast<float>(preview.height),
                    separator
            );
        }


        // Scope labels.
        addText(
                wavePanel.x + 9,
                wavePanel.y + 7,
                "WAVEFORM  LUMA / % + CODE",
                label,
                1
        );

        addText(
                paradePanel.x + 9,
                paradePanel.y + 7,
                "RGB PARADE",
                label,
                1
        );

        addText(
                histPanel.x + 9,
                histPanel.y + 7,
                "HISTOGRAM",
                label,
                1
        );

        addText(
                vectorPanel.x + 9,
                vectorPanel.y + 7,
                "VECTORSCOPE",
                label,
                1
        );


        // Waveform plot grid + full-range percentage/code labels.  Geometry
        // is expressed as the
        // original 330x200 panel ratios so the data shader and overlay remain
        // aligned when the portrait panel becomes 413x193.
        static constexpr int percentValues[] = {
                0, 25, 50, 75, 100
        };
        static constexpr int codeValues[] = {
                0, 64, 128, 191, 255
        };

        const int waveLeft =
                wavePanel.x +
                static_cast<int>(std::lround(
                        wavePanel.width * (32.0 / 330.0)));

        const int waveTop =
                wavePanel.y +
                static_cast<int>(std::lround(
                        wavePanel.height * (28.0 / 200.0)));

        const int wavePlotW =
                std::max(1,
                        static_cast<int>(std::lround(
                                wavePanel.width * (291.0 / 330.0))));

        const int wavePlotH =
                std::max(1,
                        static_cast<int>(std::lround(
                                wavePanel.height * (161.0 / 200.0))));

        for (size_t i = 0; i < std::size(percentValues); ++i) {

            const int percent = percentValues[i];
            const int code = codeValues[i];

            const int waveRow =
                    wavePlotH - 1 -
                    static_cast<int>(
                            std::lround(
                                    (
                                        static_cast<double>(code) /
                                        255.0
                                    ) *
                                    static_cast<double>(
                                            wavePlotH - 1
                                    )
                            )
                    );

            const int y =
                    waveTop +
                    waveRow;

            const int labelY =
                    percent == 100 ? y + 2 :
                    percent == 0 ? y - 8 :
                    y - 3;

            addRect(
                    static_cast<float>(waveLeft),
                    static_cast<float>(y),
                    static_cast<float>(wavePlotW),
                    1.0f,
                    grid
            );

            addText(
                    wavePanel.x + 3,
                    labelY,
                    std::to_string(percent),
                    axis,
                    1
            );

            const std::string codeText = std::to_string(code);
            addText(
                    waveLeft + wavePlotW -
                            static_cast<int>(codeText.size()) * 6 - 4,
                    labelY,
                    codeText,
                    axis,
                    1
            );
        }


        // RGB Parade grid/separators.  Keep the original 239x200 normalized
        // geometry so it remains aligned with the parade render shader.
        const int paradeLeft =
                paradePanel.x +
                static_cast<int>(std::lround(
                        paradePanel.width * (8.0 / 239.0)));

        const int paradeTop =
                paradePanel.y +
                static_cast<int>(std::lround(
                        paradePanel.height * (28.0 / 200.0)));

        const int paradePlotW =
                std::max(1,
                        static_cast<int>(std::lround(
                                paradePanel.width * (224.0 / 239.0))));

        const int paradeMidY =
                paradePanel.y +
                static_cast<int>(std::lround(
                        paradePanel.height * (108.0 / 200.0)));

        const int paradeBottom =
                paradePanel.y +
                static_cast<int>(std::lround(
                        paradePanel.height * (188.0 / 200.0)));

        addRect(
                static_cast<float>(paradeLeft),
                static_cast<float>(paradeTop),
                static_cast<float>(paradePlotW),
                1.0f,
                grid
        );

        addRect(
                static_cast<float>(paradeLeft),
                static_cast<float>(paradeMidY),
                static_cast<float>(paradePlotW),
                1.0f,
                grid
        );

        addRect(
                static_cast<float>(paradeLeft),
                static_cast<float>(paradeBottom),
                static_cast<float>(paradePlotW),
                1.0f,
                grid
        );

        const int paradeSep0 =
                paradePanel.x +
                static_cast<int>(std::lround(
                        paradePanel.width * (82.0 / 239.0)));

        const int paradeSep1 =
                paradePanel.x +
                static_cast<int>(std::lround(
                        paradePanel.width * (156.0 / 239.0)));

        addRect(
                static_cast<float>(paradeSep0),
                static_cast<float>(paradeTop),
                1.0f,
                static_cast<float>(paradeBottom - paradeTop),
                separator
        );

        addRect(
                static_cast<float>(paradeSep1),
                static_cast<float>(paradeTop),
                1.0f,
                static_cast<float>(paradeBottom - paradeTop),
                separator
        );


        // Vectorscope: aspect-fit the original 284x200 scope geometry inside
        // the current panel.  This is especially important in portrait,
        // where the panel itself is wider; the measured vector circle must
        // remain a circle, not an ellipse.
        const RectI vectorContent =
                aspectFitRect(
                        vectorPanel,
                        284,
                        200
                );

        const float vectorScaleToPanel =
                static_cast<float>(vectorContent.height) /
                200.0f;

        const float vectorCx =
                static_cast<float>(vectorContent.x) +
                142.0f * vectorScaleToPanel;

        const float vectorCy =
                static_cast<float>(vectorContent.y) +
                111.0f * vectorScaleToPanel;

        const float vectorRadius =
                70.0f * vectorScaleToPanel;

        static constexpr int circleSegments =
                96;

        for (int i = 0;
             i < circleSegments;
             ++i) {

            const float a0 =
                    static_cast<float>(
                            2.0 *
                            3.14159265358979323846 *
                            static_cast<double>(i) /
                            static_cast<double>(circleSegments)
                    );

            const float a1 =
                    static_cast<float>(
                            2.0 *
                            3.14159265358979323846 *
                            static_cast<double>(i + 1) /
                            static_cast<double>(circleSegments)
                    );

            addLine(
                    vectorCx +
                    std::cos(a0) *
                    vectorRadius,
                    vectorCy +
                    std::sin(a0) *
                    vectorRadius,
                    vectorCx +
                    std::cos(a1) *
                    vectorRadius,
                    vectorCy +
                    std::sin(a1) *
                    vectorRadius,
                    1.0f,
                    separator
            );
        }

        addRect(
                vectorCx,
                vectorCy - vectorRadius,
                1.0f,
                vectorRadius * 2.0f,
                grid
        );

        addRect(
                vectorCx - vectorRadius,
                vectorCy,
                vectorRadius * 2.0f,
                1.0f,
                grid
        );

        const float vectorScale =
                128.0f *
                vectorScaleToPanel;

        for (const auto& target :
             kVectorTargets601Ui) {

            const int tx =
                    static_cast<int>(
                            std::lround(
                                    vectorCx +
                                    target.cb *
                                    vectorScale
                            )
                    );

            const int ty =
                    static_cast<int>(
                            std::lround(
                                    vectorCy -
                                    target.cr *
                                    vectorScale
                            )
                    );

            addRect(
                    static_cast<float>(tx - 3),
                    static_cast<float>(ty - 3),
                    7.0f,
                    1.0f,
                    target.color
            );

            addRect(
                    static_cast<float>(tx - 3),
                    static_cast<float>(ty + 3),
                    7.0f,
                    1.0f,
                    target.color
            );

            addRect(
                    static_cast<float>(tx - 3),
                    static_cast<float>(ty - 3),
                    1.0f,
                    7.0f,
                    target.color
            );

            addRect(
                    static_cast<float>(tx + 3),
                    static_cast<float>(ty - 3),
                    1.0f,
                    7.0f,
                    target.color
            );

            addText(
                    tx + target.labelDx,
                    ty + target.labelDy,
                    target.label,
                    target.color,
                    1
            );
        }

        if (vertices.empty()) {
            return;
        }


        glViewport(
                0,
                0,
                surfaceWidth,
                surfaceHeight
        );

        glEnable(GL_BLEND);

        glBlendFunc(
                GL_SRC_ALPHA,
                GL_ONE_MINUS_SRC_ALPHA
        );

        glUseProgram(
                uiProgram_
        );

        glBindVertexArray(
                vao
        );

        glBindBuffer(
                GL_ARRAY_BUFFER,
                uiVbo_
        );

        glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(
                        vertices.size() *
                        sizeof(UiVertex)
                ),
                vertices.data(),
                GL_STREAM_DRAW
        );

        glEnableVertexAttribArray(0);

        glVertexAttribPointer(
                0,
                2,
                GL_FLOAT,
                GL_FALSE,
                sizeof(UiVertex),
                reinterpret_cast<void*>(0)
        );

        glEnableVertexAttribArray(1);

        glVertexAttribPointer(
                1,
                4,
                GL_FLOAT,
                GL_FALSE,
                sizeof(UiVertex),
                reinterpret_cast<void*>(
                        sizeof(float) * 2
                )
        );

        glDrawArrays(
                GL_TRIANGLES,
                0,
                static_cast<GLsizei>(
                        vertices.size()
                )
        );

        glDisable(GL_BLEND);

        glBindBuffer(
                GL_ARRAY_BUFFER,
                0
        );
}

void ScopeUi::shutdown()
{
    if (waveformRenderProgram_ != 0) {
        glDeleteProgram(waveformRenderProgram_);
        waveformRenderProgram_ = 0;
    }

    if (paradeRenderProgram_ != 0) {
        glDeleteProgram(paradeRenderProgram_);
        paradeRenderProgram_ = 0;
    }

    if (histogramRenderProgram_ != 0) {
        glDeleteProgram(histogramRenderProgram_);
        histogramRenderProgram_ = 0;
    }

    if (vectorscopeRenderProgram_ != 0) {
        glDeleteProgram(vectorscopeRenderProgram_);
        vectorscopeRenderProgram_ = 0;
    }

    if (uiProgram_ != 0) {
        glDeleteProgram(uiProgram_);
        uiProgram_ = 0;
    }

    if (uiVbo_ != 0) {
        glDeleteBuffers(1, &uiVbo_);
        uiVbo_ = 0;
    }
}

}  // namespace field_monitor
