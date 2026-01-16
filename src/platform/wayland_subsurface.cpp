#include "platform/wayland_subsurface.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <algorithm>
#include <cstring>

// Required device extensions for mpv/libplacebo
static const char* s_requiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

// Optional extensions (HDR support)
static const char* s_optionalDeviceExtensions[] = {
    VK_EXT_HDR_METADATA_EXTENSION_NAME,
};

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
        std::cerr << "Bound wp_color_manager_v1" << std::endl;
    }
}

void WaylandSubsurface::registryGlobalRemove(void*, wl_registry*, uint32_t) {}

bool WaylandSubsurface::initWayland(SDL_Window* window) {
    // SDL3 property-based Wayland access
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) {
        std::cerr << "Failed to get window properties" << std::endl;
        return false;
    }

    wl_display_ = static_cast<wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    wl_surface* parent_surface = static_cast<wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

    if (!wl_display_ || !parent_surface) {
        std::cerr << "Not running on Wayland or failed to get Wayland handles" << std::endl;
        return false;
    }

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

    std::cerr << "Created mpv subsurface below main window" << std::endl;
    return true;
}

bool WaylandSubsurface::init(SDL_Window* window, VkInstance, VkPhysicalDevice,
                              VkDevice, uint32_t,
                              const char* const*, int,
                              const VkPhysicalDeviceFeatures2*) {
    // We ignore the passed-in Vulkan handles and create our own
    // This matches how the old jellyfin-desktop works

    if (!initWayland(window)) return false;

    // CRITICAL: Create color management surface BEFORE Vulkan surface
    initColorManagement();

    // Create our own Vulkan instance (like old jellyfin-desktop)
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.pApplicationName = "Jellyfin Desktop CEF";

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 5;
    instanceInfo.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance" << std::endl;
        return false;
    }

    // Select physical device
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        std::cerr << "No Vulkan devices found" << std::endl;
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());
    physical_device_ = gpus[0];

    // Print selected GPU
    VkPhysicalDeviceProperties gpuProps;
    vkGetPhysicalDeviceProperties(physical_device_, &gpuProps);
    std::cerr << "WaylandSubsurface using GPU: " << gpuProps.deviceName << std::endl;

    // Get available extensions
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extCount, availableExts.data());

    auto hasExtension = [&](const char* name) {
        for (const auto& ext : availableExts) {
            if (strcmp(ext.extensionName, name) == 0) return true;
        }
        return false;
    };

    // Build extension list: required + available optional
    std::vector<const char*> enabledExtensions;
    constexpr int requiredCount = sizeof(s_requiredDeviceExtensions) / sizeof(s_requiredDeviceExtensions[0]);
    constexpr int optionalCount = sizeof(s_optionalDeviceExtensions) / sizeof(s_optionalDeviceExtensions[0]);

    for (int i = 0; i < requiredCount; i++) {
        if (!hasExtension(s_requiredDeviceExtensions[i])) {
            std::cerr << "Missing required extension: " << s_requiredDeviceExtensions[i] << std::endl;
            return false;
        }
        enabledExtensions.push_back(s_requiredDeviceExtensions[i]);
    }

    for (int i = 0; i < optionalCount; i++) {
        if (hasExtension(s_optionalDeviceExtensions[i])) {
            enabledExtensions.push_back(s_optionalDeviceExtensions[i]);
            std::cerr << "Enabled optional extension: " << s_optionalDeviceExtensions[i] << std::endl;
        }
    }

    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            break;
        }
    }

    // Create device with features needed for mpv/libplacebo
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queue_family_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    vk11_features_ = {};
    vk11_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vk11_features_.samplerYcbcrConversion = VK_TRUE;

    vk12_features_ = {};
    vk12_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12_features_.pNext = &vk11_features_;
    vk12_features_.timelineSemaphore = VK_TRUE;
    vk12_features_.hostQueryReset = VK_TRUE;

    features2_ = {};
    features2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2_.pNext = &vk12_features_;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2_;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();

    VkResult deviceResult = vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_);
    if (deviceResult != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan device: VkResult=" << deviceResult << std::endl;
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Create VkSurface for our wl_surface
    VkWaylandSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.display = wl_display_;
    surfaceInfo.surface = mpv_surface_;

    auto vkCreateWaylandSurfaceKHR = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
    if (!vkCreateWaylandSurfaceKHR ||
        vkCreateWaylandSurfaceKHR(instance_, &surfaceInfo, nullptr, &vk_surface_) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan surface" << std::endl;
        return false;
    }

    std::cerr << "Vulkan subsurface initialized (manual instance/device)" << std::endl;
    return true;
}

bool WaylandSubsurface::createSwapchain(int width, int height) {
    // Query surface formats
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &formatCount, formats.data());

    // Find PASS_THROUGH_EXT with R16G16B16A16_UNORM (like old jellyfin-desktop)
    swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    is_hdr_ = false;

    // First: try R16G16B16A16_UNORM with PASS_THROUGH (preferred)
    for (const auto &fmt : formats) {
        if (fmt.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT &&
            fmt.format == VK_FORMAT_R16G16B16A16_UNORM) {
            swapchain_format_ = fmt.format;
            color_space_ = fmt.colorSpace;
            is_hdr_ = true;
            std::cerr << "Using PASS_THROUGH with R16G16B16A16_UNORM (format 91)" << std::endl;
            break;
        }
    }

    // Fallback: 10-bit formats
    if (!is_hdr_) {
        for (const auto &fmt : formats) {
            if (fmt.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT) {
                if (fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                    fmt.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
                    swapchain_format_ = fmt.format;
                    color_space_ = fmt.colorSpace;
                    is_hdr_ = true;
                    std::cerr << "Using PASS_THROUGH with 10-bit format " << fmt.format << std::endl;
                    break;
                }
            }
        }
    }

    if (!is_hdr_) {
        std::cerr << "PASS_THROUGH not available, using SDR" << std::endl;
    }

    // Get surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, vk_surface_, &caps);

    swapchain_extent_ = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    // Create swapchain
    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = vk_surface_;
    swapInfo.minImageCount = caps.minImageCount + 1;
    swapInfo.imageFormat = swapchain_format_;
    swapInfo.imageColorSpace = color_space_;
    swapInfo.imageExtent = swapchain_extent_;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &swapInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        std::cerr << "Failed to create swapchain" << std::endl;
        return false;
    }

    // Get swapchain images
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchain_images_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchain_images_.data());

    // Create image views
    swapchain_views_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain_images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain_format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &viewInfo, nullptr, &swapchain_views_[i]);
    }

    // Create sync objects
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device_, &semInfo, nullptr, &image_available_);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device_, &fenceInfo, nullptr, &acquire_fence_);

    std::cerr << "Swapchain: " << width << "x" << height
              << " format=" << swapchain_format_
              << " colorSpace=" << color_space_
              << " HDR=" << (is_hdr_ ? "yes" : "no") << std::endl;

    return true;
}

bool WaylandSubsurface::initColorManagement() {
    if (!color_manager_) {
        std::cerr << "Color manager not available" << std::endl;
        return false;
    }

    color_surface_ = wp_color_manager_v1_get_surface(color_manager_, mpv_surface_);
    if (!color_surface_) {
        std::cerr << "Failed to create color management surface" << std::endl;
        return false;
    }

    std::cerr << "Created color management surface" << std::endl;
    return true;
}

void WaylandSubsurface::setColorspace() {
    if (!color_surface_ || !color_manager_ || !is_hdr_) {
        return;
    }

    if (hdr_image_desc_) {
        wp_image_description_v1_destroy(hdr_image_desc_);
        hdr_image_desc_ = nullptr;
    }

    wp_image_description_creator_params_v1* creator =
        wp_color_manager_v1_create_parametric_creator(color_manager_);
    if (!creator) {
        std::cerr << "Failed to create parametric image description creator" << std::endl;
        return;
    }

    wp_image_description_creator_params_v1_set_primaries_named(
        creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    wp_image_description_creator_params_v1_set_tf_named(
        creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);

    uint32_t min_lum = 1;
    uint32_t max_lum = 1000;
    uint32_t ref_lum = 203;
    wp_image_description_creator_params_v1_set_luminances(creator, min_lum, max_lum, ref_lum);
    wp_image_description_creator_params_v1_set_mastering_luminance(creator, 1, 1000);

    hdr_image_desc_ = wp_image_description_creator_params_v1_create(creator);
    if (!hdr_image_desc_) {
        std::cerr << "Failed to create HDR image description" << std::endl;
        return;
    }

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
    std::cerr << "Set Wayland surface colorspace to PQ/BT.2020" << std::endl;
}

bool WaylandSubsurface::startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) {
    if (!swapchain_) return false;

    // Acquire next image with fence
    vkResetFences(device_, 1, &acquire_fence_);
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, 100000000,
                                            VK_NULL_HANDLE, acquire_fence_, &current_image_idx_);
    if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, UINT64_MAX);

    frame_active_ = true;
    *outImage = swapchain_images_[current_image_idx_];
    *outView = swapchain_views_[current_image_idx_];
    *outFormat = swapchain_format_;
    return true;
}

void WaylandSubsurface::submitFrame() {
    if (!frame_active_ || !swapchain_) return;

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &current_image_idx_;

    vkQueuePresentKHR(queue_, &presentInfo);

    // Commit surface
    wl_surface_commit(mpv_surface_);
    wl_display_flush(wl_display_);

    frame_active_ = false;
}

bool WaylandSubsurface::recreateSwapchain(int width, int height) {
    destroySwapchain();
    return createSwapchain(width, height);
}

void WaylandSubsurface::destroySwapchain() {
    if (!device_) return;

    vkDeviceWaitIdle(device_);

    if (acquire_fence_) {
        vkDestroyFence(device_, acquire_fence_, nullptr);
        acquire_fence_ = VK_NULL_HANDLE;
    }
    if (image_available_) {
        vkDestroySemaphore(device_, image_available_, nullptr);
        image_available_ = VK_NULL_HANDLE;
    }

    for (auto view : swapchain_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear();

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    frame_active_ = false;
}

void WaylandSubsurface::commit() {
    wl_surface_commit(mpv_surface_);
    wl_display_flush(wl_display_);
}

void WaylandSubsurface::cleanup() {
    destroySwapchain();

    if (vk_surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, vk_surface_, nullptr);
        vk_surface_ = VK_NULL_HANDLE;
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

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

    wl_compositor_ = nullptr;
    wl_subcompositor_ = nullptr;
    wl_display_ = nullptr;
}

VkQueue WaylandSubsurface::vkQueue() const {
    return queue_;
}

uint32_t WaylandSubsurface::vkQueueFamily() const {
    return queue_family_;
}

const char* const* WaylandSubsurface::deviceExtensions() const {
    return s_requiredDeviceExtensions;
}

int WaylandSubsurface::deviceExtensionCount() const {
    return sizeof(s_requiredDeviceExtensions) / sizeof(s_requiredDeviceExtensions[0]);
}
