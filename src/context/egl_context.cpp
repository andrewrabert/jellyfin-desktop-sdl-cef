#include "context/egl_context.h"
#include <iostream>
#include <cstring>

#ifdef SDL_PLATFORM_LINUX
#include <wayland-egl.h>
#endif

EGLContext_::EGLContext_() = default;

EGLContext_::~EGLContext_() {
    cleanup();
}

bool EGLContext_::init(SDL_Window* window) {
    // Get native Wayland display from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    struct wl_display* wl_display = static_cast<struct wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    struct wl_surface* wl_surface = static_cast<struct wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

    if (!wl_display || !wl_surface) {
        std::cerr << "[EGL] Failed to get Wayland display/surface from SDL" << std::endl;
        return false;
    }

    // Get EGL display
    display_ = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, nullptr);
    if (display_ == EGL_NO_DISPLAY) {
        std::cerr << "[EGL] Failed to get EGL display" << std::endl;
        return false;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(display_, &major, &minor)) {
        std::cerr << "[EGL] Failed to initialize EGL" << std::endl;
        return false;
    }
    std::cerr << "[EGL] Initialized EGL " << major << "." << minor << std::endl;

    // Check extensions
    const char* extensions = eglQueryString(display_, EGL_EXTENSIONS);
    if (extensions) {
        has_dmabuf_import_ = strstr(extensions, "EGL_EXT_image_dma_buf_import") != nullptr;
        std::cerr << "[EGL] DMA-BUF import: " << (has_dmabuf_import_ ? "yes" : "no") << std::endl;
    }

    // Load extension functions
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    // Bind OpenGL ES API
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        std::cerr << "[EGL] Failed to bind OpenGL ES API" << std::endl;
        return false;
    }

    // Choose config
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(display_, config_attribs, &config_, 1, &num_configs) || num_configs == 0) {
        std::cerr << "[EGL] Failed to choose config" << std::endl;
        return false;
    }

    // Create context
    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
    if (context_ == EGL_NO_CONTEXT) {
        std::cerr << "[EGL] Failed to create context" << std::endl;
        return false;
    }

    // Get window size in pixels (for HiDPI support)
    SDL_GetWindowSizeInPixels(window, &width_, &height_);

    // Create wayland-egl window and EGL surface at pixel size
    egl_window_ = wl_egl_window_create(wl_surface, width_, height_);
    if (!egl_window_) {
        std::cerr << "[EGL] Failed to create wayland-egl window" << std::endl;
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config_, (EGLNativeWindowType)egl_window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        std::cerr << "[EGL] Failed to create window surface" << std::endl;
        wl_egl_window_destroy(egl_window_);
        egl_window_ = nullptr;
        return false;
    }

    // Make context current
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        std::cerr << "[EGL] Failed to make context current" << std::endl;
        return false;
    }

    // Enable vsync
    eglSwapInterval(display_, 1);

    std::cerr << "[EGL] Context created successfully" << std::endl;
    std::cerr << "[EGL] GL_VERSION: " << glGetString(GL_VERSION) << std::endl;
    std::cerr << "[EGL] GL_RENDERER: " << glGetString(GL_RENDERER) << std::endl;

    return true;
}

void EGLContext_::cleanup() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    if (egl_window_) {
        wl_egl_window_destroy(egl_window_);
        egl_window_ = nullptr;
    }
}

void EGLContext_::swapBuffers() {
    eglSwapBuffers(display_, surface_);
}

bool EGLContext_::resize(int width, int height) {
    if (width == width_ && height == height_) {
        return true;
    }
    width_ = width;
    height_ = height;
    if (egl_window_) {
        wl_egl_window_resize(egl_window_, width, height, 0, 0);
    }
    return true;
}
