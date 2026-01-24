#include "compositor/opengl_compositor.h"
#include <cstring>
#include "logging.h"

#ifdef __APPLE__
// macOS gl3.h defines GL_BGRA, not GL_BGRA_EXT
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT GL_BGRA
#endif
#elif defined(_WIN32)
// Windows: Load OpenGL extension function pointers
static PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
static PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
static PFNGLBUFFERDATAPROC glBufferData = nullptr;
static PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
static PFNGLMAPBUFFERRANGEPROC glMapBufferRange = nullptr;
static PFNGLUNMAPBUFFERPROC glUnmapBuffer = nullptr;
static PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
static PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
static PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
static PFNGLCREATESHADERPROC glCreateShader = nullptr;
static PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
static PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
static PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
static PFNGLDELETESHADERPROC glDeleteShader = nullptr;
static PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
static PFNGLATTACHSHADERPROC glAttachShader = nullptr;
static PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
static PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
static PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
static PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
static PFNGLUNIFORM1FPROC glUniform1f = nullptr;
static PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;

static bool s_wglExtensionsLoaded = false;

static void loadWGLExtensions() {
    if (s_wglExtensionsLoaded) return;
    glGenBuffers = (PFNGLGENBUFFERSPROC)wglGetProcAddress("glGenBuffers");
    glBindBuffer = (PFNGLBINDBUFFERPROC)wglGetProcAddress("glBindBuffer");
    glBufferData = (PFNGLBUFFERDATAPROC)wglGetProcAddress("glBufferData");
    glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)wglGetProcAddress("glDeleteBuffers");
    glMapBufferRange = (PFNGLMAPBUFFERRANGEPROC)wglGetProcAddress("glMapBufferRange");
    glUnmapBuffer = (PFNGLUNMAPBUFFERPROC)wglGetProcAddress("glUnmapBuffer");
    glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)wglGetProcAddress("glGenVertexArrays");
    glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)wglGetProcAddress("glBindVertexArray");
    glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)wglGetProcAddress("glDeleteVertexArrays");
    glCreateShader = (PFNGLCREATESHADERPROC)wglGetProcAddress("glCreateShader");
    glShaderSource = (PFNGLSHADERSOURCEPROC)wglGetProcAddress("glShaderSource");
    glCompileShader = (PFNGLCOMPILESHADERPROC)wglGetProcAddress("glCompileShader");
    glGetShaderiv = (PFNGLGETSHADERIVPROC)wglGetProcAddress("glGetShaderiv");
    glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)wglGetProcAddress("glGetShaderInfoLog");
    glDeleteShader = (PFNGLDELETESHADERPROC)wglGetProcAddress("glDeleteShader");
    glCreateProgram = (PFNGLCREATEPROGRAMPROC)wglGetProcAddress("glCreateProgram");
    glAttachShader = (PFNGLATTACHSHADERPROC)wglGetProcAddress("glAttachShader");
    glLinkProgram = (PFNGLLINKPROGRAMPROC)wglGetProcAddress("glLinkProgram");
    glGetProgramiv = (PFNGLGETPROGRAMIVPROC)wglGetProcAddress("glGetProgramiv");
    glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)wglGetProcAddress("glGetProgramInfoLog");
    glDeleteProgram = (PFNGLDELETEPROGRAMPROC)wglGetProcAddress("glDeleteProgram");
    glUseProgram = (PFNGLUSEPROGRAMPROC)wglGetProcAddress("glUseProgram");
    glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)wglGetProcAddress("glGetUniformLocation");
    glUniform1f = (PFNGLUNIFORM1FPROC)wglGetProcAddress("glUniform1f");
    glActiveTexture = (PFNGLACTIVETEXTUREPROC)wglGetProcAddress("glActiveTexture");
    s_wglExtensionsLoaded = true;
}
#endif

static auto _log_start = std::chrono::steady_clock::now();
static long _comp_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _log_start).count(); }

#ifdef __APPLE__
// macOS: Desktop OpenGL 3.2 Core with GL_TEXTURE_2D (software path)
static const char* vert_src = R"(#version 150
out vec2 texCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 150
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    fragColor = color * alpha;
}
)";
#elif defined(_WIN32)
// Windows: Desktop OpenGL 2.1+ with GL_TEXTURE_2D
static const char* vert_src = R"(#version 130
out vec2 texCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 130
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    // CEF provides BGRA - swizzle to RGBA
    fragColor = color.bgra * alpha;
}
)";
#else
// Linux: OpenGL ES 3.0
static const char* vert_src = R"(#version 300 es
out vec2 texCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 300 es
precision mediump float;
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    // CEF provides BGRA, uploaded as RGBA - swizzle back
    fragColor = color.bgra * alpha;
}
)";
#endif

OpenGLCompositor::OpenGLCompositor() = default;

OpenGLCompositor::~OpenGLCompositor() {
    cleanup();
}

bool OpenGLCompositor::init(GLContext* ctx, uint32_t width, uint32_t height) {
    ctx_ = ctx;
    width_ = width;
    height_ = height;

#ifdef _WIN32
    loadWGLExtensions();
#endif

    if (!createTexture()) return false;
    if (!createShader()) return false;

    // Create VAO (required for GLES 3.0 / OpenGL core)
    glGenVertexArrays(1, &vao_);

    return true;
}

bool OpenGLCompositor::createTexture() {
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifdef __APPLE__
    // Software path: allocate texture storage and PBOs
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
#elif defined(_WIN32)
    // Windows: Software path only
    // Use GL_RGBA (universally supported) - shader swizzles BGRA->RGBA
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLenum texErr = glGetError();
    if (texErr != GL_NO_ERROR) {
        LOG_ERROR(LOG_COMPOSITOR, "glTexImage2D failed: %d", texErr);
        return false;
    }
#else
    // Software path: allocate texture storage and PBOs
    // Use GL_RGBA (universally supported) - shader swizzles BGRA->RGBA
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLenum texErr = glGetError();
    if (texErr != GL_NO_ERROR) {
        LOG_ERROR(LOG_COMPOSITOR, "glTexImage2D failed: %d", texErr);
        return false;
    }
#endif

    // Create double-buffered PBOs for async upload
    size_t pbo_size = width_ * height_ * 4;
    glGenBuffers(2, pbos_);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[i]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, pbo_size, nullptr, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    GLenum pboErr = glGetError();
    if (pboErr != GL_NO_ERROR) {
        LOG_ERROR(LOG_COMPOSITOR, "PBO creation failed: %d", pboErr);
        return false;
    }

    // Map the first PBO for writing
    current_pbo_ = 0;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
    pbo_mapped_ = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, pbo_size,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOG_ERROR(LOG_COMPOSITOR, "glMapBufferRange failed: %d", err);
        return false;
    }

    return true;
}

bool OpenGLCompositor::createShader() {
    // Compile vertex shader
    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert, 1, &vert_src, nullptr);
    glCompileShader(vert);

    GLint status;
    glGetShaderiv(vert, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vert, 512, nullptr, log);
        LOG_ERROR(LOG_COMPOSITOR, "Vertex shader error: %s", log);
        glDeleteShader(vert);
        return false;
    }

    // Compile fragment shader
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag, 1, &frag_src, nullptr);
    glCompileShader(frag);

    glGetShaderiv(frag, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(frag, 512, nullptr, log);
        LOG_ERROR(LOG_COMPOSITOR, "Fragment shader error: %s", log);
        glDeleteShader(vert);
        glDeleteShader(frag);
        return false;
    }

    // Link program
    program_ = glCreateProgram();
    glAttachShader(program_, vert);
    glAttachShader(program_, frag);
    glLinkProgram(program_);

    glGetProgramiv(program_, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(program_, 512, nullptr, log);
        LOG_ERROR(LOG_COMPOSITOR, "Program link error: %s", log);
        glDeleteShader(vert);
        glDeleteShader(frag);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    // Get uniform locations
    alpha_loc_ = glGetUniformLocation(program_, "alpha");

    return true;
}

void OpenGLCompositor::updateOverlay(const void* data, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (width != static_cast<int>(width_) || height != static_cast<int>(height_)) {
        return;
    }

    if (pbo_mapped_) {
        std::memcpy(pbo_mapped_, data, width * height * 4);
        staging_pending_ = true;
    }
}

void* OpenGLCompositor::getStagingBuffer(int width, int height) {
    if (width != static_cast<int>(width_) || height != static_cast<int>(height_)) {
        return nullptr;
    }
    return pbo_mapped_;
}

bool OpenGLCompositor::flushOverlay() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!staging_pending_ || !texture_) {
        return false;
    }

    size_t pbo_size = width_ * height_ * 4;

    // Unmap current PBO and start async DMA transfer to texture
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

    glBindTexture(GL_TEXTURE_2D, texture_);
    // With PBO bound, last arg is offset into PBO, not pointer
    // Upload as RGBA - shader swizzles BGRA->RGBA
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Swap to next PBO and map it for next frame's writes
    current_pbo_ = 1 - current_pbo_;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
    pbo_mapped_ = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, pbo_size,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    staging_pending_ = false;
    has_content_ = true;
    return true;
}

void OpenGLCompositor::composite(uint32_t width, uint32_t height, float alpha) {
    if (!has_content_ || !program_) {
        return;
    }

    glViewport(0, 0, width, height);

    // Enable blending with premultiplied alpha
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program_);
    glUniform1f(alpha_loc_, alpha);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

void OpenGLCompositor::resize(uint32_t width, uint32_t height) {
    LOG_DEBUG(LOG_COMPOSITOR, "[%ldms] resize: %ux%u (current: %ux%u)", _comp_ms(), width, height, width_, height_);

    if (width == width_ && height == height_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    destroyTexture();

    width_ = width;
    height_ = height;
    has_content_ = false;

    createTexture();
    LOG_DEBUG(LOG_COMPOSITOR, "[%ldms] resize: done", _comp_ms());
}

void OpenGLCompositor::destroyTexture() {
    // Unmap and delete PBOs
    if (pbo_mapped_) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        pbo_mapped_ = nullptr;
    }
    if (pbos_[0]) {
        glDeleteBuffers(2, pbos_);
        pbos_[0] = pbos_[1] = 0;
    }
    current_pbo_ = 0;

    if (texture_) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
}

void OpenGLCompositor::cleanup() {
    if (!ctx_) return;

    destroyTexture();

    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }

    ctx_ = nullptr;
}
