#ifdef _WIN32

#include "platform/windows_video_layer.h"
#include <SDL3/SDL.h>
#include "logging.h"
#include <algorithm>
#include <vector>

// Device extensions needed for mpv/libplacebo
static const char* s_deviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    VK_KHR_MAINTENANCE2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE3_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
};
static const int s_deviceExtensionCount = sizeof(s_deviceExtensions) / sizeof(s_deviceExtensions[0]);

static const wchar_t* VIDEO_WINDOW_CLASS = L"JellyfinVideoLayer";
static bool s_classRegistered = false;

LRESULT CALLBACK WindowsVideoLayer::VideoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // Prevent flicker
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

WindowsVideoLayer::WindowsVideoLayer() = default;

WindowsVideoLayer::~WindowsVideoLayer() {
    cleanup();
}

bool WindowsVideoLayer::init(SDL_Window* window, VkInstance, VkPhysicalDevice,
                              VkDevice, uint32_t,
                              const char* const*, uint32_t,
                              const char* const*) {
    // We ignore the passed-in Vulkan handles and create our own
    // This matches how WaylandSubsurface and MacOSVideoLayer work
    parent_window_ = window;

    // Get parent HWND from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    parent_hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!parent_hwnd_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to get parent HWND from SDL");
        return false;
    }

    // Register window class for video child
    if (!s_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = VideoWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = VIDEO_WINDOW_CLASS;
        if (!RegisterClassExW(&wc)) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to register video window class");
            return false;
        }
        s_classRegistered = true;
    }

    // Get parent window size
    RECT parentRect;
    GetClientRect(parent_hwnd_, &parentRect);
    int w = parentRect.right - parentRect.left;
    int h = parentRect.bottom - parentRect.top;

    // Create child window for video (positioned below parent's content)
    video_hwnd_ = CreateWindowExW(
        0,                          // No extended styles
        VIDEO_WINDOW_CLASS,
        L"Video",
        WS_CHILD | WS_VISIBLE,      // Child window, visible
        0, 0, w, h,                 // Fill parent
        parent_hwnd_,               // Parent
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (!video_hwnd_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create video child window: %lu", GetLastError());
        return false;
    }

    // Position video window at bottom of Z-order (below other content)
    SetWindowPos(video_hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Video child window created: %dx%d", w, h);

    // Create Vulkan instance
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_2;
    appInfo.pApplicationName = "Jellyfin Desktop CEF";

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 3;
    instanceInfo.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create Vulkan instance");
        return false;
    }

    // Select physical device
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] No Vulkan devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());
    physical_device_ = gpus[0];

    VkPhysicalDeviceProperties gpuProps;
    vkGetPhysicalDeviceProperties(physical_device_, &gpuProps);
    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Using GPU: %s", gpuProps.deviceName);

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

    // Create device with required features
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

    // Query what features are actually supported
    vkGetPhysicalDeviceFeatures2(physical_device_, &features2_);

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2_;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = s_deviceExtensionCount;
    deviceInfo.ppEnabledExtensionNames = s_deviceExtensions;

    if (vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create Vulkan device");
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    device_extensions_ = s_deviceExtensions;
    device_extension_count_ = s_deviceExtensionCount;

    // Create Vulkan surface for child window
    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = video_hwnd_;

    auto vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)
        vkGetInstanceProcAddr(instance_, "vkCreateWin32SurfaceKHR");
    if (!vkCreateWin32SurfaceKHR ||
        vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create Vulkan Win32 surface");
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Vulkan context initialized");
    return true;
}

bool WindowsVideoLayer::createSwapchain(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    // Query surface formats
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, formats.data());

    // Prefer HDR format if available
    format_ = VK_FORMAT_B8G8R8A8_UNORM;
    color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    is_hdr_ = false;

    // Try 10-bit HDR formats
    for (const auto& fmt : formats) {
        if (fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            if (fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                fmt.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
                format_ = fmt.format;
                color_space_ = fmt.colorSpace;
                is_hdr_ = true;
                LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Using HDR format %d", fmt.format);
                break;
            }
        }
    }

    if (!is_hdr_) {
        LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] HDR not available, using SDR");
    }

    // Get surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    // Create swapchain
    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = surface_;
    swapInfo.minImageCount = (std::max)(2u, caps.minImageCount);
    swapInfo.imageFormat = format_;
    swapInfo.imageColorSpace = color_space_;
    swapInfo.imageExtent = {width, height};
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = swapchain_;

    if (vkCreateSwapchainKHR(device_, &swapInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create swapchain");
        return false;
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr);
    image_count_ = (std::min)(image_count_, MAX_IMAGES);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_);

    // Create image views
    for (uint32_t i = 0; i < image_count_; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device_, &viewInfo, nullptr, &image_views_[i]) != VK_SUCCESS) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create image view %u", i);
            return false;
        }
    }

    // Create sync objects
    if (image_available_ == VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &semInfo, nullptr, &image_available_);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(device_, &fenceInfo, nullptr, &acquire_fence_);
    }

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Swapchain created: %ux%u format=%d HDR=%s",
             width, height, format_, is_hdr_ ? "yes" : "no");

    return true;
}

void WindowsVideoLayer::destroySwapchain() {
    if (!device_) return;

    vkDeviceWaitIdle(device_);

    for (uint32_t i = 0; i < image_count_; i++) {
        if (image_views_[i]) {
            vkDestroyImageView(device_, image_views_[i], nullptr);
            image_views_[i] = VK_NULL_HANDLE;
        }
    }
    image_count_ = 0;

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool WindowsVideoLayer::startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) {
    if (frame_active_ || !swapchain_) return false;

    vkResetFences(device_, 1, &acquire_fence_);
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                             VK_NULL_HANDLE, acquire_fence_, &current_image_idx_);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, UINT64_MAX);

    frame_active_ = true;
    *outImage = images_[current_image_idx_];
    *outView = image_views_[current_image_idx_];
    *outFormat = format_;
    return true;
}

void WindowsVideoLayer::submitFrame() {
    if (!frame_active_) return;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &current_image_idx_;

    vkQueuePresentKHR(queue_, &presentInfo);
    frame_active_ = false;
    visible_ = true;
}

void WindowsVideoLayer::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;

    // Resize child window
    if (video_hwnd_) {
        SetWindowPos(video_hwnd_, HWND_BOTTOM, 0, 0, width, height, SWP_NOACTIVATE);
    }

    vkDeviceWaitIdle(device_);
    destroySwapchain();
    createSwapchain(width, height);
}

void WindowsVideoLayer::setVisible(bool visible) {
    if (video_hwnd_) {
        ShowWindow(video_hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
    visible_ = visible;
}

void WindowsVideoLayer::setPosition(int x, int y) {
    if (video_hwnd_) {
        SetWindowPos(video_hwnd_, HWND_BOTTOM, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void WindowsVideoLayer::cleanup() {
    if (device_) {
        vkDeviceWaitIdle(device_);
    }

    destroySwapchain();

    if (acquire_fence_) {
        vkDestroyFence(device_, acquire_fence_, nullptr);
        acquire_fence_ = VK_NULL_HANDLE;
    }
    if (image_available_) {
        vkDestroySemaphore(device_, image_available_, nullptr);
        image_available_ = VK_NULL_HANDLE;
    }

    if (surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    if (video_hwnd_) {
        DestroyWindow(video_hwnd_);
        video_hwnd_ = nullptr;
    }
}

#endif // _WIN32
