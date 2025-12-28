#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "wayland-protocols/color-management-v1-client.h"
#include <vector>

struct SDL_Window;

class WaylandSubsurface {
public:
    WaylandSubsurface();
    ~WaylandSubsurface();

    bool init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
              VkDevice device, uint32_t queueFamily);
    bool createSwapchain(int width, int height);
    bool recreateSwapchain(int width, int height);
    void cleanup();

    // Accessors
    wl_display* display() const { return wl_display_; }
    wl_surface* surface() const { return mpv_surface_; }
    VkSurfaceKHR vkSurface() const { return vk_surface_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkFormat swapchainFormat() const { return swapchain_format_; }
    VkExtent2D swapchainExtent() const { return swapchain_extent_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchain_images_; }
    const std::vector<VkImageView>& swapchainViews() const { return swapchain_views_; }
    bool isHdr() const { return is_hdr_; }
    uint32_t width() const { return swapchain_extent_.width; }
    uint32_t height() const { return swapchain_extent_.height; }

    void commit();
    void setColorspace();  // Call when video playback starts

    // Wayland registry callbacks (public for C callback struct)
    static void registryGlobal(void* data, wl_registry* registry,
                               uint32_t name, const char* interface, uint32_t version);
    static void registryGlobalRemove(void* data, wl_registry* registry, uint32_t name);

private:
    bool initWayland(SDL_Window* window);
    bool createSubsurface(wl_surface* parentSurface);
    bool createVkSurface();
    bool initColorManagement();
    void destroySwapchain();
    void setHdrMetadata();

    // Wayland
    wl_display* wl_display_ = nullptr;
    wl_compositor* wl_compositor_ = nullptr;
    wl_subcompositor* wl_subcompositor_ = nullptr;
    wl_surface* mpv_surface_ = nullptr;
    wl_subsurface* mpv_subsurface_ = nullptr;

    // Color management
    wp_color_manager_v1* color_manager_ = nullptr;
    wp_color_management_surface_v1* color_surface_ = nullptr;
    wp_image_description_v1* hdr_image_desc_ = nullptr;

    // Vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_R16G16B16A16_UNORM;
    VkColorSpaceKHR swapchain_color_space_ = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    VkExtent2D swapchain_extent_ = {0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    bool is_hdr_ = false;
};
