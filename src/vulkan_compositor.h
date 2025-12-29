#pragma once

#include "vulkan_context.h"
#include "cef_client.h"  // For AcceleratedPaintInfo

class VulkanCompositor {
public:
    VulkanCompositor();
    ~VulkanCompositor();

    bool init(VulkanContext* vk, uint32_t width, uint32_t height);
    void cleanup();

    // Update overlay texture from CEF buffer (BGRA) - software path
    void updateOverlay(const void* data, int width, int height);

    // Update overlay from DMA-BUF - hardware accelerated path
    bool updateOverlayFromDmaBuf(const AcceleratedPaintInfo& info);

    // Composite overlay onto swapchain image
    // Call after mpv has rendered to the image
    void composite(VkCommandBuffer cmd, VkImage target, VkImageView targetView,
                   uint32_t width, uint32_t height, float alpha);

    // Resize resources
    void resize(uint32_t width, uint32_t height);

    // Check if we have valid content to composite
    bool hasValidOverlay() const { return using_dmabuf_ || has_software_content_; }

    // Mark that we have software content
    void setSoftwareContentReady() { has_software_content_ = true; }

private:
    bool createOverlayResources();
    bool createPipeline();
    bool createDescriptorSets();
    void destroyOverlayResources();
    void destroyDmaBufImage();

    VulkanContext* vk_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Overlay texture (software path)
    VkImage overlay_image_ = VK_NULL_HANDLE;
    VkDeviceMemory overlay_memory_ = VK_NULL_HANDLE;
    VkImageView overlay_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // DMA-BUF imported texture (hardware path)
    // Use more slots than CEF's buffer pool to avoid needing to destroy during normal operation
    static constexpr int DMABUF_BUFFER_COUNT = 6;
    struct DmaBufResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t buffer_id = 0;  // Identifies the underlying DMA-BUF (dev << 32 | ino)
    };
    DmaBufResource dmabuf_[DMABUF_BUFFER_COUNT];
    int dmabuf_current_ = 0;
    bool using_dmabuf_ = false;
    bool dmabuf_supported_ = true;  // Set false if import fails
    bool has_software_content_ = false;

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
