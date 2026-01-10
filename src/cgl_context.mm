#ifdef __APPLE__

#import "cgl_context.h"
#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

bool CGLContext::init(SDL_Window* window) {
    window_ = window;

    // Get NSWindow from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* ns_window = (__bridge NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!ns_window) {
        NSLog(@"Failed to get NSWindow from SDL");
        return false;
    }

    // Create OpenGL pixel format
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        0
    };

    pixel_format_ = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!pixel_format_) {
        NSLog(@"Failed to create NSOpenGLPixelFormat");
        return false;
    }

    // Create OpenGL context
    ns_context_ = [[NSOpenGLContext alloc] initWithFormat:pixel_format_ shareContext:nil];
    if (!ns_context_) {
        NSLog(@"Failed to create NSOpenGLContext");
        return false;
    }

    // Attach to window's content view
    [ns_context_ setView:[ns_window contentView]];
    [ns_context_ makeCurrentContext];

    // Get CGL context for IOSurface operations
    cgl_context_ = [ns_context_ CGLContextObj];

    // Enable vsync
    GLint swapInterval = 1;
    [ns_context_ setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];

    SDL_GetWindowSize(window, &width_, &height_);

    NSLog(@"CGL context initialized: %dx%d", width_, height_);
    return true;
}

void CGLContext::cleanup() {
    if (ns_context_) {
        [NSOpenGLContext clearCurrentContext];
        ns_context_ = nil;
    }
    pixel_format_ = nil;
    cgl_context_ = nullptr;
}

void CGLContext::swapBuffers() {
    if (ns_context_) {
        [ns_context_ flushBuffer];
    }
}

bool CGLContext::resize(int width, int height) {
    if (width == width_ && height == height_) {
        return true;
    }

    width_ = width;
    height_ = height;

    if (ns_context_) {
        [ns_context_ update];
    }

    return true;
}

#endif // __APPLE__
