#pragma once

#include "vulkan_context.h"

class VulkanCompositor {
public:
    VulkanCompositor();
    ~VulkanCompositor();

    bool init(VulkanContext* vk, uint32_t width, uint32_t height);
    void cleanup();

    // Update overlay texture from CEF buffer (BGRA)
    void updateOverlay(const void* data, int width, int height);

    // Composite overlay onto swapchain image
    // Call after mpv has rendered to the image
    void composite(VkCommandBuffer cmd, VkImage target, VkImageView targetView,
                   uint32_t width, uint32_t height, float alpha);

    // Resize resources
    void resize(uint32_t width, uint32_t height);

private:
    bool createOverlayResources();
    bool createPipeline();
    bool createDescriptorSets();
    void destroyOverlayResources();

    VulkanContext* vk_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Overlay texture
    VkImage overlay_image_ = VK_NULL_HANDLE;
    VkDeviceMemory overlay_memory_ = VK_NULL_HANDLE;
    VkImageView overlay_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // Staging buffer for texture upload
    VkBuffer staging_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    void* staging_mapped_ = nullptr;

    // Render pass and pipeline for compositing
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    // Descriptor set for overlay texture
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // Per-frame resources
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    // Push constants
    struct PushConstants {
        float alpha;
        float padding[3];
    };
};
