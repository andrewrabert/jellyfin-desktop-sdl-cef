#ifdef _WIN32

#include "context/wgl_context.h"
#include "context/gl_loader.h"
#include "logging.h"

WGLContext::WGLContext() = default;

WGLContext::~WGLContext() {
    cleanup();
}

bool WGLContext::init(SDL_Window* window) {
    window_ = window;

    // Get HWND from SDL3
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd_) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to get HWND from SDL window");
        return false;
    }

    hdc_ = GetDC(hwnd_);
    if (!hdc_) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to get DC");
        return false;
    }

    // Set pixel format
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 0;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hdc_, &pfd);
    if (!pixelFormat || !SetPixelFormat(hdc_, pixelFormat, &pfd)) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to set pixel format");
        return false;
    }

    // Create OpenGL context
    hglrc_ = wglCreateContext(hdc_);
    if (!hglrc_) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to create WGL context");
        return false;
    }

    makeCurrent();

    // Load GL extension functions
    if (!gl::initGLLoader()) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to load GL extensions");
        return false;
    }

    // Get window size
    SDL_GetWindowSize(window, &width_, &height_);

    LOG_INFO(LOG_GL, "[WGL] Context created successfully");
    LOG_INFO(LOG_GL, "[WGL] GL_VERSION: %s", glGetString(GL_VERSION));
    LOG_INFO(LOG_GL, "[WGL] GL_RENDERER: %s", glGetString(GL_RENDERER));

    return true;
}

void WGLContext::cleanup() {
    if (hglrc_) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hglrc_);
        hglrc_ = nullptr;
    }
    if (hdc_ && hwnd_) {
        ReleaseDC(hwnd_, hdc_);
        hdc_ = nullptr;
    }
}

void WGLContext::makeCurrent() {
    if (hdc_ && hglrc_) {
        wglMakeCurrent(hdc_, hglrc_);
    }
}

void WGLContext::swapBuffers() {
    if (hdc_) {
        SwapBuffers(hdc_);
    }
}

bool WGLContext::resize(int width, int height) {
    if (width == width_ && height == height_) {
        return true;
    }
    width_ = width;
    height_ = height;
    // WGL doesn't need explicit resize handling - the DC is tied to the HWND
    return true;
}

void* WGLContext::getProcAddress(const char* name) {
    // Try wglGetProcAddress first (for extensions)
    void* proc = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (proc) {
        return proc;
    }
    // Fall back to GetProcAddress from opengl32.dll (for core GL functions)
    static HMODULE opengl32 = LoadLibraryA("opengl32.dll");
    if (opengl32) {
        return reinterpret_cast<void*>(GetProcAddress(opengl32, name));
    }
    return nullptr;
}

HGLRC WGLContext::createSharedContext() const {
    if (!hdc_ || !hglrc_) {
        return nullptr;
    }

    // Create a new context and share lists with the main context
    HGLRC shared = wglCreateContext(hdc_);
    if (!shared) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to create shared context");
        return nullptr;
    }

    // Share display lists (textures, VBOs, etc.) between contexts
    if (!wglShareLists(hglrc_, shared)) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to share lists between contexts");
        wglDeleteContext(shared);
        return nullptr;
    }

    LOG_INFO(LOG_GL, "[WGL] Created shared context");
    return shared;
}

void WGLContext::destroyContext(HGLRC ctx) const {
    if (ctx) {
        wglDeleteContext(ctx);
    }
}

bool WGLContext::makeCurrent(HGLRC ctx) const {
    if (!hdc_) return false;
    if (!ctx) {
        return wglMakeCurrent(nullptr, nullptr) == TRUE;
    }
    return wglMakeCurrent(hdc_, ctx) == TRUE;
}

bool WGLContext::makeCurrentMain() const {
    if (!hdc_ || !hglrc_) return false;
    return wglMakeCurrent(hdc_, hglrc_) == TRUE;
}

#endif // _WIN32
