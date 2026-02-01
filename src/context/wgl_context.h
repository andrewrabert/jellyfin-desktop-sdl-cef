#pragma once
#ifdef _WIN32

#include <SDL3/SDL.h>
#include <windows.h>
#include <GL/gl.h>

class WGLContext {
public:
    WGLContext();
    ~WGLContext();

    bool init(SDL_Window* window);
    void cleanup();
    void makeCurrent();
    void swapBuffers();
    bool resize(int width, int height);

    // Create a shared context for use on another thread
    HGLRC createSharedContext() const;
    void destroyContext(HGLRC ctx) const;
    bool makeCurrent(HGLRC ctx) const;
    bool makeCurrentMain() const;

    HDC hdc() const { return hdc_; }
    HGLRC hglrc() const { return hglrc_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Get OpenGL function pointer (for mpv render context)
    void* getProcAddress(const char* name);

private:
    SDL_Window* window_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
    HGLRC hglrc_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

#endif // _WIN32
