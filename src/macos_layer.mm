#ifdef __APPLE__

#import "macos_layer.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <SDL3/SDL_syswm.h>

// Vulkan surface extension for macOS
#include <vulkan/vulkan_metal.h>

bool MacOSVideoLayer::init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
                           VkDevice device, uint32_t queueFamily,
                           const char* const* deviceExtensions, uint32_t deviceExtensionCount,
                           const char* const* instanceExtensions) {
    window_ = window;
    instance_ = instance;
    physical_device_ = physicalDevice;
    device_ = device;
    queue_family_ = queueFamily;
    vkGetDeviceQueue(device, queueFamily, 0, &queue_);

    // Get NSWindow from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* ns_window = (__bridge NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!ns_window) {
        NSLog(@"Failed to get NSWindow from SDL");
        return false;
    }

    // Create a subview for video (behind the main content view)
    NSView* content_view = [ns_window contentView];
    NSRect frame = [content_view bounds];

    video_view_ = [[NSView alloc] initWithFrame:frame];
    [video_view_ setWantsLayer:YES];
    [video_view_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    // Create CAMetalLayer with HDR support
    metal_layer_ = [CAMetalLayer layer];
    metal_layer_.device = MTLCreateSystemDefaultDevice();
    metal_layer_.pixelFormat = MTLPixelFormatRGBA16Float;  // HDR format
    metal_layer_.wantsExtendedDynamicRangeContent = YES;
    CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    metal_layer_.colorspace = colorspace;
    CGColorSpaceRelease(colorspace);
    metal_layer_.framebufferOnly = YES;
    metal_layer_.frame = frame;

    [video_view_ setLayer:metal_layer_];

    // Insert video view behind content view
    [content_view addSubview:video_view_ positioned:NSWindowBelow relativeTo:nil];

    is_hdr_ = YES;
    NSLog(@"MacOS video layer initialized with HDR (EDR) support");

    // Create Vulkan surface from Metal layer
    VkMetalSurfaceCreateInfoEXT surfaceCreateInfo = {};
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    surfaceCreateInfo.pLayer = metal_layer_;

    PFN_vkCreateMetalSurfaceEXT vkCreateMetalSurfaceEXT =
        (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT");

    if (!vkCreateMetalSurfaceEXT) {
        NSLog(@"vkCreateMetalSurfaceEXT not available");
        return false;
    }

    VkResult result = vkCreateMetalSurfaceEXT(instance, &surfaceCreateInfo, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        NSLog(@"Failed to create Vulkan Metal surface: %d", result);
        return false;
    }

    return true;
}

void MacOSVideoLayer::cleanup() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    destroySwapchain();

    if (image_available_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, image_available_, nullptr);
        image_available_ = VK_NULL_HANDLE;
    }
    if (render_finished_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, render_finished_, nullptr);
        render_finished_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (video_view_) {
        [video_view_ removeFromSuperview];
        video_view_ = nil;
    }

    metal_layer_ = nil;
}

bool MacOSVideoLayer::createSwapchain(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    // Update Metal layer size
    metal_layer_.drawableSize = CGSizeMake(width, height);

    // Query surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);

    // Choose HDR format if available
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, formats.data());

    // Prefer HDR formats
    format_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    color_space_ = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;

    // Check if our preferred format is supported
    bool found = false;
    for (const auto& fmt : formats) {
        if (fmt.format == format_ && fmt.colorSpace == color_space_) {
            found = true;
            break;
        }
    }

    if (!found && !formats.empty()) {
        // Fall back to first available
        format_ = formats[0].format;
        color_space_ = formats[0].colorSpace;
        is_hdr_ = false;
        NSLog(@"HDR format not available, falling back to SDR");
    }

    // Create swapchain
    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = std::max(2u, capabilities.minImageCount);
    createInfo.imageFormat = format_;
    createInfo.imageColorSpace = color_space_;
    createInfo.imageExtent = {width, height};
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = swapchain_;

    VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
    if (result != VK_SUCCESS) {
        NSLog(@"Failed to create swapchain: %d", result);
        return false;
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr);
    image_count_ = std::min(image_count_, MAX_IMAGES);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_);

    // Create image views
    for (uint32_t i = 0; i < image_count_; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &viewInfo, nullptr, &image_views_[i]) != VK_SUCCESS) {
            NSLog(@"Failed to create image view %d", i);
            return false;
        }
    }

    // Create semaphores for frame sync
    if (image_available_ == VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo semInfo = {};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &semInfo, nullptr, &image_available_);
        vkCreateSemaphore(device_, &semInfo, nullptr, &render_finished_);
    }

    NSLog(@"Swapchain created: %dx%d format=%d colorSpace=%d HDR=%s",
          width, height, format_, color_space_, is_hdr_ ? "yes" : "no");

    return true;
}

void MacOSVideoLayer::destroySwapchain() {
    for (uint32_t i = 0; i < image_count_; i++) {
        if (image_views_[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, image_views_[i], nullptr);
            image_views_[i] = VK_NULL_HANDLE;
        }
    }

    if (swapchain_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    image_count_ = 0;
}

bool MacOSVideoLayer::startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) {
    if (frame_active_ || swapchain_ == VK_NULL_HANDLE) {
        return false;
    }

    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                             image_available_, VK_NULL_HANDLE,
                                             &current_image_idx_);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (result != VK_SUCCESS) {
        return false;
    }

    frame_active_ = true;
    *outImage = images_[current_image_idx_];
    *outView = image_views_[current_image_idx_];
    *outFormat = format_;
    return true;
}

void MacOSVideoLayer::submitFrame() {
    if (!frame_active_) {
        return;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 0;  // mpv handles its own sync
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &current_image_idx_;

    vkQueuePresentKHR(queue_, &presentInfo);
    frame_active_ = false;
}

void MacOSVideoLayer::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) {
        return;
    }

    vkDeviceWaitIdle(device_);
    destroySwapchain();
    createSwapchain(width, height);

    // Update view frame
    if (video_view_) {
        NSRect frame = NSMakeRect(0, 0, width, height);
        [video_view_ setFrame:frame];
    }
}

void MacOSVideoLayer::setVisible(bool visible) {
    if (video_view_) {
        [video_view_ setHidden:!visible];
    }
}

void MacOSVideoLayer::setPosition(int x, int y) {
    if (video_view_) {
        NSRect frame = [video_view_ frame];
        frame.origin.x = x;
        frame.origin.y = y;
        [video_view_ setFrame:frame];
    }
}

#endif // __APPLE__
