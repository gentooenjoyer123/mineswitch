#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

static FILE *network_log;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static EGLSurface surface = EGL_NO_SURFACE;
static bool display_initialized;
static GLuint program, vao, vbo;
static constexpr int width = 1280, height = 720;

static void report(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void report(const char *format, ...) {
    if (!network_log) return;
    va_list args;
    va_start(args, format);
    vfprintf(network_log, format, args);
    va_end(args);
    fflush(network_log);
}

static void report_memory(const char *stage) {
    u64 total = 0, used = 0;
    Result a = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    Result b = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    report("memory.%s total_result=0x%08" PRIx32 " used_result=0x%08" PRIx32
           " total_bytes=%" PRIu64 " committed_bytes=%" PRIu64 "\n",
           stage, static_cast<uint32_t>(a), static_cast<uint32_t>(b),
           static_cast<uint64_t>(total), static_cast<uint64_t>(used));
    struct mallinfo heap = mallinfo();
    report("malloc.%s arena_bytes=%zu allocated_bytes=%zu free_in_arena_bytes=%zu\n",
           stage, heap.arena, heap.uordblks, heap.fordblks);
}

static bool egl_failure(const char *operation) {
    report("%s FAIL EGL_error=0x%04x\n", operation, eglGetError());
    return false;
}

static bool check_surface_dimensions(const char *stage) {
    EGLint actual_width = 0, actual_height = 0;
    if (!eglQuerySurface(display, surface, EGL_WIDTH, &actual_width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &actual_height))
        return egl_failure("egl.query_surface");
    u32 native_width = 0, native_height = 0;
    Result rc = nwindowGetDimensions(nwindowGetDefault(), &native_width, &native_height);
    report("surface.%s EGL_width=%d EGL_height=%d native_width=%u native_height=%u"
           " native_result=0x%08" PRIx32 "\n", stage, actual_width, actual_height,
           native_width, native_height, static_cast<uint32_t>(rc));
    if (R_FAILED(rc) || native_width != width || native_height != height) {
        report("surface.%s FAIL unexpected native dimensions\n", stage);
        return false;
    }
    if (actual_width == 0 && actual_height == 0) {
        // Mesa's Switch backend sizes its buffers from NWindow but leaves the
        // generic EGL surface Width/Height fields zero (see graphics/README.md).
        // This is not a passed rendering check: center AND far-corner readback
        // must still succeed after drawing before the overall probe can pass.
        report("surface.%s EGL_dimensions=unreported native_dimensions=1280x720\n", stage);
        return true;
    }
    if (actual_width != width || actual_height != height) {
        report("surface.%s FAIL unexpected EGL dimensions\n", stage);
        return false;
    }
    return true;
}

static bool init_egl() {
    NWindow *window = nwindowGetDefault();
    Result dimensions = nwindowSetDimensions(window, width, height);
    report("window.dimensions %dx%d result=0x%08" PRIx32 "\n",
           width, height, static_cast<uint32_t>(dimensions));
    if (R_FAILED(dimensions)) return false;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return egl_failure("egl.get_display");
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) return egl_failure("egl.initialize");
    display_initialized = true;
    report("EGL_VERSION=%d.%d EGL_VENDOR=%s\n", major, minor,
           eglQueryString(display, EGL_VENDOR));
    if (!eglBindAPI(EGL_OPENGL_API)) return egl_failure("egl.bind_opengl");

    const EGLint framebuffer_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_NONE
    };
    EGLConfig config;
    EGLint config_count = 0;
    if (!eglChooseConfig(display, framebuffer_attributes, &config, 1, &config_count))
        return egl_failure("egl.choose_config");
    if (config_count != 1) {
        report("egl.choose_config FAIL no matching framebuffer\n");
        return false;
    }
    surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) return egl_failure("egl.create_surface");
    const EGLint context_attributes[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
        EGL_CONTEXT_MINOR_VERSION_KHR, 2,
        EGL_NONE
    };
    report("egl.request OpenGL=3.2 profile=core\n");
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) return egl_failure("egl.create_context");
    if (!eglMakeCurrent(display, surface, surface, context))
        return egl_failure("egl.make_current");
    if (!check_surface_dimensions("before_draw")) return false;
    if (!eglSwapInterval(display, 1)) return egl_failure("egl.swap_interval");
    return true;
}

static void exit_egl() {
    if (display_initialized) {
        if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
            egl_failure("egl.release_context");
        if (context != EGL_NO_CONTEXT && !eglDestroyContext(display, context))
            egl_failure("egl.destroy_context");
        if (surface != EGL_NO_SURFACE && !eglDestroySurface(display, surface))
            egl_failure("egl.destroy_surface");
        if (!eglTerminate(display)) egl_failure("egl.terminate");
    }
    eglReleaseThread();
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
    display = EGL_NO_DISPLAY;
    display_initialized = false;
}

static bool check_gl(const char *stage) {
    bool okay = true;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        report("gl.error stage=%s code=0x%04x\n", stage, error);
        okay = false;
    }
    return okay;
}

static bool describe_gl() {
    if (!gladLoadGL()) {
        report("glad.load FAIL\n");
        return false;
    }
    report("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\nGL_SHADING_LANGUAGE_VERSION=%s\n",
           glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION),
           glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint major = 0, minor = 0, profile = 0, max_texture_size = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    report("gl.context actual=%d.%d profile_mask=0x%x GL_MAX_TEXTURE_SIZE=%d\n",
           major, minor, profile, max_texture_size);
    bool okay = (major > 3 || (major == 3 && minor >= 2)) &&
                (profile & GL_CONTEXT_CORE_PROFILE_BIT) && GLAD_GL_VERSION_3_2;
    okay = check_gl("context_capabilities") && okay;
    report("gl.core_3_2 %s\n", okay ? "PASS" : "FAIL");
    return okay;
}

static GLuint compile_shader(GLenum type, const char *source, const char *label) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        report("shader.%s.create FAIL\n", label);
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    char log[2048] = {};
    GLsizei log_length = 0;
    glGetShaderInfoLog(shader, sizeof(log), &log_length, log);
    report("shader.%s GLSL=150 %s\n", label, success ? "PASS" : "FAIL");
    if (log_length) report("shader.%s.log %s\n", label, log);
    if (!success) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool init_scene() {
    static const char vertex_source[] =
        "#version 150 core\n"
        "in vec2 position;\n"
        "in vec3 color;\n"
        "out vec3 vertex_color;\n"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); vertex_color = color; }\n";
    static const char fragment_source[] =
        "#version 150 core\n"
        "in vec3 vertex_color;\n"
        "out vec4 fragment_color;\n"
        "void main() { fragment_color = vec4(vertex_color, 1.0); }\n";
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source, "vertex");
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source, "fragment");
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        // GLSL 150 uses API bindings instead of GLSL 330 layout qualifiers.
        glBindAttribLocation(program, 0, "position");
        glBindAttribLocation(program, 1, "color");
        glBindFragDataLocation(program, 0, "fragment_color");
        glLinkProgram(program);
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!program) return false;
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    char log[2048] = {};
    GLsizei log_length = 0;
    glGetProgramInfoLog(program, sizeof(log), &log_length, log);
    report("shader.link %s\n", linked ? "PASS" : "FAIL");
    if (log_length) report("shader.link.log %s\n", log);
    if (!linked) return false;

    struct Vertex { float position[2], color[3]; };
    static const Vertex vertices[] = {
        {{-0.75f, -0.75f}, {1.0f, 0.0f, 0.0f}},
        {{ 0.75f, -0.75f}, {0.0f, 1.0f, 0.0f}},
        {{ 0.00f,  0.75f}, {0.0f, 0.0f, 1.0f}}
    };
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, color)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glViewport(0, 0, width, height);
    glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glClearColor(8.0f / 255.0f, 16.0f / 255.0f, 32.0f / 255.0f, 1.0f);
    return vao && vbo && check_gl("scene_setup");
}

static bool render() {
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return check_gl("draw");
}

static bool check_pixel(const char *label, int x, int y, const int expected[4]) {
    GLubyte pixel[4] = {};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    // Reading the current back buffer waits for the preceding GPU work.
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    bool okay = check_gl("readback");
    for (int i = 0; i < 4; ++i)
        okay = okay && std::abs(static_cast<int>(pixel[i]) - expected[i]) <= 3;
    report("readback.%s x=%d y=%d rgba=(%u,%u,%u,%u) expected=(%d,%d,%d,%d) tolerance=3 %s\n",
           label, x, y, pixel[0], pixel[1], pixel[2], pixel[3],
           expected[0], expected[1], expected[2], expected[3], okay ? "PASS" : "FAIL");
    return okay;
}

static void exit_scene() {
    glFinish();
    glBindVertexArray(0);
    glUseProgram(0);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    vbo = vao = program = 0;
    check_gl("scene_cleanup");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Result socket_rc = socketInitializeDefault();
    if (R_SUCCEEDED(socket_rc)) {
        int network_fd = nxlinkConnectToHost(false, false);
        if (network_fd >= 0) {
            network_log = fdopen(network_fd, "w");
            if (!network_log) close(network_fd);
        }
    }
    report("MINESWITCH_GL_BEGIN version=0.1.1\n");
    report("applet.type=%d (%s)\n", static_cast<int>(appletGetAppletType()),
           appletGetAppletType() == AppletType_Application ? "application" : "applet/other");
    report_memory("before_egl");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    bool graphics_ready = init_egl() && describe_gl();
    bool scene_ready = graphics_ready && init_scene();
    bool passed = false;
    if (scene_ready) {
        report_memory("after_scene_init");
        bool drawn = render();
        const int center[4] = {64, 64, 128, 255};
        const int background[4] = {8, 16, 32, 255};
        bool center_okay = check_pixel("triangle_center", width / 2, height / 2, center);
        bool background_okay = check_pixel("background", 8, 8, background);
        // A far-corner pixel must be written by glReadPixels, not left as the
        // all-zero sentinel for a position outside the actual back buffer.
        bool extent_okay = check_pixel("far_corner", width - 1, height - 1, background);
        bool dimensions_okay = check_surface_dimensions("after_first_draw");
        passed = drawn && center_okay && background_okay && extent_okay && dimensions_okay;
        report_memory("after_first_draw");
        report("MINESWITCH_GL_CHECKS_COMPLETE %s\n", passed ? "PASS" : "FAIL");
        report("Press + to exit. Automatic return in 20 seconds.\n");

        u64 start = armGetSystemTick();
        uint64_t frames = 0;
        unsigned last_heartbeat = 0;
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
                report("exit.reason=plus\n");
                break;
            }
            u64 elapsed_ns = armTicksToNs(armGetSystemTick() - start);
            unsigned seconds = static_cast<unsigned>(elapsed_ns / UINT64_C(1000000000));
            if (seconds >= 20) {
                report("exit.reason=automatic\n");
                break;
            }
            if (!render() || !eglSwapBuffers(display, surface)) {
                report("frame FAIL EGL_error=0x%04x\n", eglGetError());
                passed = false;
                break;
            }
            ++frames;
            if (seconds / 5 > last_heartbeat) {
                last_heartbeat = seconds / 5;
                report("heartbeat seconds=%u frames=%" PRIu64 "\n", seconds, frames);
            }
        }
        report("presentation.frames=%" PRIu64 " (vsync enabled; not a game benchmark)\n", frames);
    } else {
        report("MINESWITCH_GL_CHECKS_COMPLETE FAIL initialization\n");
    }
    if (graphics_ready) exit_scene();
    exit_egl();
    report_memory("after_egl_cleanup");
    report("MINESWITCH_GL_END %s\n", passed ? "PASS" : "FAIL");
    if (network_log) {
        FILE *finished_log = network_log;
        network_log = nullptr;
        fclose(finished_log);
    }
    // Every socket-backed FILE is closed, while stdout/stderr were never redirected.
    if (R_SUCCEEDED(socket_rc)) socketExit();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
