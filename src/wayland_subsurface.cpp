#include "wayland_subsurface.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <iostream>
#include <algorithm>
#include <cstring>

// Image description listener callbacks
struct ImageDescContext {
    bool ready = false;
};

static void image_desc_failed(void*, struct wp_image_description_v1*, uint32_t, const char* msg) {
    std::cerr << "Image description failed: " << msg << std::endl;
}

static void image_desc_ready(void* data, struct wp_image_description_v1*, uint32_t) {
    auto* ctx = static_cast<ImageDescContext*>(data);
    ctx->ready = true;
}

static void image_desc_ready2(void* data, struct wp_image_description_v1*, uint32_t, uint32_t) {
    auto* ctx = static_cast<ImageDescContext*>(data);
    ctx->ready = true;
}

static const struct wp_image_description_v1_listener s_imageDescListener = {
    .failed = image_desc_failed,
    .ready = image_desc_ready,
    .ready2 = image_desc_ready2,
};

static const wl_registry_listener s_registryListener = {
    .global = WaylandSubsurface::registryGlobal,
    .global_remove = WaylandSubsurface::registryGlobalRemove,
};

WaylandSubsurface::WaylandSubsurface() = default;

WaylandSubsurface::~WaylandSubsurface() {
    cleanup();
}

void WaylandSubsurface::registryGlobal(void* data, wl_registry* registry,
                                        uint32_t name, const char* interface, uint32_t version) {
    auto* self = static_cast<WaylandSubsurface*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        self->wl_compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        self->wl_subcompositor_ = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
        self->color_manager_ = static_cast<wp_color_manager_v1*>(
            wl_registry_bind(registry, name, &wp_color_manager_v1_interface, std::min(version, 1u)));
        std::cout << "Bound wp_color_manager_v1" << std::endl;
    }
}

void WaylandSubsurface::registryGlobalRemove(void*, wl_registry*, uint32_t) {}

bool WaylandSubsurface::initWayland(SDL_Window* window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) {
        std::cerr << "Failed to get window info: " << SDL_GetError() << std::endl;
        return false;
    }

    if (info.subsystem != SDL_SYSWM_WAYLAND) {
        std::cerr << "Not running on Wayland" << std::endl;
        return false;
    }

    wl_display_ = info.info.wl.display;
    wl_surface* parent_surface = info.info.wl.surface;

    // Get compositor and subcompositor
    wl_registry* registry = wl_display_get_registry(wl_display_);
    wl_registry_add_listener(registry, &s_registryListener, this);
    wl_display_roundtrip(wl_display_);
    wl_registry_destroy(registry);

    if (!wl_compositor_ || !wl_subcompositor_) {
        std::cerr << "Missing Wayland globals" << std::endl;
        return false;
    }

    return createSubsurface(parent_surface);
}

bool WaylandSubsurface::createSubsurface(wl_surface* parentSurface) {
    mpv_surface_ = wl_compositor_create_surface(wl_compositor_);
    if (!mpv_surface_) {
        std::cerr << "Failed to create mpv surface" << std::endl;
        return false;
    }

    mpv_subsurface_ = wl_subcompositor_get_subsurface(wl_subcompositor_, mpv_surface_, parentSurface);
    if (!mpv_subsurface_) {
        std::cerr << "Failed to create subsurface" << std::endl;
        return false;
    }

    // Position at origin, place below parent (so CEF renders on top)
    wl_subsurface_set_position(mpv_subsurface_, 0, 0);
    wl_subsurface_place_below(mpv_subsurface_, parentSurface);
    wl_subsurface_set_desync(mpv_subsurface_);

    wl_surface_commit(mpv_surface_);
    wl_display_roundtrip(wl_display_);

    std::cout << "Created mpv subsurface below main window" << std::endl;
    return true;
}

bool WaylandSubsurface::createVkSurface() {
    VkWaylandSurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info.display = wl_display_;
    create_info.surface = mpv_surface_;

    auto vkCreateWaylandSurfaceKHR = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
    if (!vkCreateWaylandSurfaceKHR) {
        std::cerr << "vkCreateWaylandSurfaceKHR not available" << std::endl;
        return false;
    }

    VkResult result = vkCreateWaylandSurfaceKHR(instance_, &create_info, nullptr, &vk_surface_);
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan Wayland surface: " << result << std::endl;
        return false;
    }

    return true;
}

bool WaylandSubsurface::init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
                              VkDevice device, uint32_t queueFamily) {
    instance_ = instance;
    physical_device_ = physicalDevice;
    device_ = device;
    queue_family_ = queueFamily;

    if (!initWayland(window)) return false;

    // Initialize color management BEFORE Vulkan surface (like mpv does)
    initColorManagement();

    if (!createVkSurface()) return false;

    return true;
}

bool WaylandSubsurface::createSwapchain(int width, int height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, vk_surface_, &caps);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &format_count, nullptr);
    if (format_count == 0) {
        std::cerr << "No subsurface formats available" << std::endl;
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &format_count, formats.data());

    // Debug: print available formats
    std::cout << "Subsurface available formats:" << std::endl;
    for (const auto& fmt : formats) {
        std::cout << "  format=" << fmt.format << " colorSpace=" << fmt.colorSpace << std::endl;
    }

    // Use PASS_THROUGH colorspace - actual colorspace signaled via Wayland color management
    // This prevents Mesa WSI from creating its own color surface (like mpv does)
    swapchain_format_ = formats[0].format;
    swapchain_color_space_ = formats[0].colorSpace;
    is_hdr_ = false;

    for (const auto& fmt : formats) {
        if (fmt.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT) {
            // Prefer high bit depth for HDR content
            if (fmt.format == VK_FORMAT_R16G16B16A16_UNORM ||
                fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                fmt.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
                swapchain_format_ = fmt.format;
                swapchain_color_space_ = fmt.colorSpace;
                is_hdr_ = true;
                std::cout << "Subsurface HDR: format=" << fmt.format << " PASS_THROUGH" << std::endl;
                break;
            }
        }
    }

    // Fallback: any passthrough
    if (!is_hdr_) {
        for (const auto& fmt : formats) {
            if (fmt.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT) {
                swapchain_format_ = fmt.format;
                swapchain_color_space_ = fmt.colorSpace;
                is_hdr_ = true;
                std::cout << "Subsurface HDR: format=" << fmt.format << " PASS_THROUGH (fallback)" << std::endl;
                break;
            }
        }
    }

    if (!is_hdr_) {
        std::cerr << "PASS_THROUGH colorspace not available for subsurface" << std::endl;
    }

    swapchain_extent_ = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    swapchain_extent_.width = std::clamp(swapchain_extent_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    swapchain_extent_.height = std::clamp(swapchain_extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchain_info{};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = vk_surface_;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = swapchain_format_;
    swapchain_info.imageColorSpace = swapchain_color_space_;
    swapchain_info.imageExtent = swapchain_extent_;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = caps.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &swapchain_);
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create subsurface swapchain: " << result << std::endl;
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());

    swapchain_views_.resize(image_count);
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = swapchain_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain_format_;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &view_info, nullptr, &swapchain_views_[i]);
    }

    std::cout << "Subsurface swapchain created: " << swapchain_extent_.width << "x" << swapchain_extent_.height
              << " (HDR: " << (is_hdr_ ? "yes" : "no") << ")" << std::endl;

    if (is_hdr_) {
        setHdrMetadata();
        // Note: setColorspace() must be called later when video playback starts
        // Calling it at swapchain creation causes protocol errors
    }

    return true;
}

void WaylandSubsurface::setHdrMetadata() {
    auto vkSetHdrMetadataEXT = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
        vkGetDeviceProcAddr(device_, "vkSetHdrMetadataEXT"));
    if (!vkSetHdrMetadataEXT) {
        std::cerr << "vkSetHdrMetadataEXT not available" << std::endl;
        return;
    }

    VkHdrMetadataEXT hdr_metadata{};
    hdr_metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;

    // BT.2020 primaries
    hdr_metadata.displayPrimaryRed = {0.708f, 0.292f};
    hdr_metadata.displayPrimaryGreen = {0.170f, 0.797f};
    hdr_metadata.displayPrimaryBlue = {0.131f, 0.046f};
    hdr_metadata.whitePoint = {0.3127f, 0.3290f};  // D65

    // Luminance range
    hdr_metadata.maxLuminance = 1000.0f;
    hdr_metadata.minLuminance = 0.001f;

    // Content light level
    hdr_metadata.maxContentLightLevel = 1000.0f;
    hdr_metadata.maxFrameAverageLightLevel = 200.0f;

    vkSetHdrMetadataEXT(device_, 1, &swapchain_, &hdr_metadata);
    std::cout << "Subsurface HDR metadata set" << std::endl;
}

bool WaylandSubsurface::initColorManagement() {
    if (!color_manager_) {
        std::cout << "Color manager not available (compositor may not support wp-color-management-v1)" << std::endl;
        return false;
    }

    // Create color management surface BEFORE Vulkan surface
    color_surface_ = wp_color_manager_v1_get_surface(color_manager_, mpv_surface_);
    if (!color_surface_) {
        std::cerr << "Failed to create color management surface" << std::endl;
        return false;
    }

    std::cout << "Created color management surface" << std::endl;
    return true;
}

void WaylandSubsurface::setColorspace() {
    if (!color_surface_ || !color_manager_ || !is_hdr_) {
        return;
    }

    // Destroy old image description if any
    if (hdr_image_desc_) {
        wp_image_description_v1_destroy(hdr_image_desc_);
        hdr_image_desc_ = nullptr;
    }

    // Create parametric image description for HDR (PQ/BT.2020)
    wp_image_description_creator_params_v1* creator =
        wp_color_manager_v1_create_parametric_creator(color_manager_);
    if (!creator) {
        std::cerr << "Failed to create parametric image description creator" << std::endl;
        return;
    }

    // Set BT.2020 primaries and PQ transfer function
    wp_image_description_creator_params_v1_set_primaries_named(
        creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    wp_image_description_creator_params_v1_set_tf_named(
        creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);

    // Set luminance metadata
    // min_lum is in 0.0001 cd/m² units, max/ref in cd/m²
    uint32_t min_lum = 1;      // 0.0001 cd/m²
    uint32_t max_lum = 1000;   // 1000 cd/m²
    uint32_t ref_lum = 203;    // Reference white per ITU-R BT.2408
    wp_image_description_creator_params_v1_set_luminances(creator, min_lum, max_lum, ref_lum);
    wp_image_description_creator_params_v1_set_mastering_luminance(creator, 1, 1000);

    hdr_image_desc_ = wp_image_description_creator_params_v1_create(creator);
    if (!hdr_image_desc_) {
        std::cerr << "Failed to create HDR image description" << std::endl;
        return;
    }

    // Add listener and wait for ready event before using
    ImageDescContext ctx{};
    wp_image_description_v1_add_listener(hdr_image_desc_, &s_imageDescListener, &ctx);
    wl_display_roundtrip(wl_display_);

    if (!ctx.ready) {
        std::cerr << "Image description not ready" << std::endl;
        wp_image_description_v1_destroy(hdr_image_desc_);
        hdr_image_desc_ = nullptr;
        return;
    }

    wp_color_management_surface_v1_set_image_description(
        color_surface_, hdr_image_desc_,
        WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wl_surface_commit(mpv_surface_);
    wl_display_flush(wl_display_);
    std::cout << "Set Wayland surface colorspace to PQ/BT.2020" << std::endl;
}

bool WaylandSubsurface::recreateSwapchain(int width, int height) {
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    return createSwapchain(width, height);
}

void WaylandSubsurface::destroySwapchain() {
    for (auto view : swapchain_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear();

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void WaylandSubsurface::commit() {
    wl_surface_commit(mpv_surface_);
    wl_display_flush(wl_display_);
}

void WaylandSubsurface::cleanup() {
    if (!device_) return;  // Already cleaned up

    vkDeviceWaitIdle(device_);
    destroySwapchain();

    if (vk_surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, vk_surface_, nullptr);
        vk_surface_ = VK_NULL_HANDLE;
    }

    // Clean up color management
    if (hdr_image_desc_) {
        wp_image_description_v1_destroy(hdr_image_desc_);
        hdr_image_desc_ = nullptr;
    }
    if (color_surface_) {
        wp_color_management_surface_v1_destroy(color_surface_);
        color_surface_ = nullptr;
    }
    if (color_manager_) {
        wp_color_manager_v1_destroy(color_manager_);
        color_manager_ = nullptr;
    }

    if (mpv_subsurface_) {
        wl_subsurface_destroy(mpv_subsurface_);
        mpv_subsurface_ = nullptr;
    }

    if (mpv_surface_) {
        wl_surface_destroy(mpv_surface_);
        mpv_surface_ = nullptr;
    }

    // Don't destroy compositor/subcompositor - they're global
    wl_compositor_ = nullptr;
    wl_subcompositor_ = nullptr;
    wl_display_ = nullptr;
    device_ = nullptr;  // Mark as cleaned up
}
