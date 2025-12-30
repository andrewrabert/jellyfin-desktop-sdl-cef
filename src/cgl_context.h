#pragma once
#ifdef __APPLE__

#include <SDL3/SDL.h>

// Forward declarations for Objective-C types
#ifdef __OBJC__
@class NSOpenGLContext;
@class NSOpenGLPixelFormat;
#else
typedef void NSOpenGLContext;
typedef void NSOpenGLPixelFormat;
#endif

typedef struct _CGLContextObject *CGLContextObj;

class CGLContext {
public:
    bool init(SDL_Window* window);
    void cleanup();
    void swapBuffers();
    bool resize(int width, int height);

    CGLContextObj cglContext() const { return cgl_context_; }

private:
    SDL_Window* window_ = nullptr;
    NSOpenGLContext* ns_context_ = nullptr;
    NSOpenGLPixelFormat* pixel_format_ = nullptr;
    CGLContextObj cgl_context_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

#endif // __APPLE__
