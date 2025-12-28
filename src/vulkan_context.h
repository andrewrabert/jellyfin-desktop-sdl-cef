#pragma once

#include <vulkan/vulkan.h>
#include <SDL2/SDL.h>
#include <vector>

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    bool init(SDL_Window* window);
    bool createSwapchain(int width, int height);
    bool recreateSwapchain(int width, int height);
    void cleanup();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_device_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queue_family_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkFormat swapchainFormat() const { return swapchain_format_; }
    VkExtent2D swapchainExtent() const { return swapchain_extent_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchain_images_; }
    const std::vector<VkImageView>& swapchainViews() const { return swapchain_views_; }

    VkCommandPool commandPool() const { return command_pool_; }

    VkPhysicalDeviceFeatures2* features() { return &features2_; }
    const char* const* deviceExtensions() const { return device_extensions_; }
    int deviceExtensionCount() const { return device_extension_count_; }

    // Helpers
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer cmd);

private:
    bool createInstance(SDL_Window* window);
    bool createSurface(SDL_Window* window);
    bool selectPhysicalDevice();
    bool createDevice();
    bool createCommandPool();
    void destroySwapchain();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchain_extent_ = {0, 0};
    VkCommandPool command_pool_ = VK_NULL_HANDLE;

    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;

    VkPhysicalDeviceVulkan11Features vk11_features_{};
    VkPhysicalDeviceVulkan12Features vk12_features_{};
    VkPhysicalDeviceFeatures2 features2_{};

    static const char* device_extensions_[];
    static const int device_extension_count_;
};
