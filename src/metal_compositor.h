#pragma once
#ifdef __APPLE__

#include <SDL3/SDL.h>
#include <cstdint>
#include <mutex>

// Forward declarations for ObjC types
#ifdef __OBJC__
@class CAMetalLayer;
@class NSView;
@class NSWindow;
#else
typedef void CAMetalLayer;
typedef void NSView;
typedef void NSWindow;
#endif

class MetalCompositor {
public:
    bool init(SDL_Window* window, uint32_t width, uint32_t height);
    void cleanup();

    // Update overlay from CEF paint callback
    void updateOverlay(const void* data, int width, int height);

    // Get staging buffer for direct copy
    void* getStagingBuffer(int width, int height);
    void markStagingDirty();

    // Render frame
    void composite(uint32_t width, uint32_t height, float alpha);

    // Resize
    void resize(uint32_t width, uint32_t height);

    // Visibility
    void setVisible(bool visible);

    bool hasValidOverlay() const { return has_content_; }
    bool hasPendingContent() const { return staging_dirty_; }

    // For video layer to position itself
    NSWindow* parentWindow() const { return parent_window_; }
    CAMetalLayer* layer() const { return metal_layer_; }

private:
    bool createPipeline();
    bool createTexture(uint32_t width, uint32_t height);

    SDL_Window* window_ = nullptr;
    NSWindow* parent_window_ = nullptr;
    NSView* metal_view_ = nullptr;
    CAMetalLayer* metal_layer_ = nullptr;

    void* device_ = nullptr;
    void* command_queue_ = nullptr;
    void* texture_ = nullptr;
    void* pipeline_state_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::mutex mutex_;
    void* staging_buffer_ = nullptr;
    size_t staging_size_ = 0;
    bool staging_dirty_ = false;
    bool has_content_ = false;
};

#endif // __APPLE__
