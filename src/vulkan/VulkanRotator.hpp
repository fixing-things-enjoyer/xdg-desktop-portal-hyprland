#pragma once

#include <vulkan/vulkan.h>
#include <wayland-client-protocol.h>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <future>
#include <string>
#include <cstdint>

#include "CoordTransform.hpp"

namespace xdph::vulkan {

    struct FrameInput {
        int                 dmaBufFd;
        uint32_t            width;
        uint32_t            height;
        uint32_t            stride;
        uint32_t            format;
        uint64_t            modifier;
        wl_output_transform transform;
        Region              region;
        const void*         hostSrcData = nullptr;
        uint32_t            hostSrcStride = 0;
        uint32_t            hostSrcSize = 0;
        void*               hostDstData = nullptr;
        uint32_t            hostDstStride = 0;
        uint32_t            hostDstSize = 0;
    };

    struct FrameOutput {
        int         dmaBufFd;
        uint32_t    width;
        uint32_t    height;
        uint32_t    stride;
        uint32_t    size;
        bool        success;
        std::string error;
    };

    class VulkanRotator {
      public:
        static std::unique_ptr<VulkanRotator> create();
        ~VulkanRotator();

        VulkanRotator(const VulkanRotator&)            = delete;
        VulkanRotator& operator=(const VulkanRotator&) = delete;

        FrameOutput    process(const FrameInput& input);

      private:
        VulkanRotator();
        bool                                                         initialize();
        void                                                         workerLoop();
        bool                                                         selectPhysicalDevice();
        bool                                                         createLogicalDevice();
        bool                                                         createCommandPool();
        bool                                                         createDescriptorPool();
        bool                                                         createDescriptorSetLayout();
        bool                                                         createPipelineLayout();
        bool                                                         createComputePipeline();
        bool                                                         createCommandBuffersAndFences();

        FrameOutput                                                  processFrame(const FrameInput& input);

        struct ReusableBuffer {
            VkBuffer       buffer          = VK_NULL_HANDLE;
            VkDeviceMemory memory          = VK_NULL_HANDLE;
            VkDeviceSize   capacity        = 0;
            uint32_t       memoryTypeIndex = UINT32_MAX;
            bool           hostCoherent    = false;
            void*          mapped          = nullptr;
        };

        struct SlotResources {
            ReusableBuffer srcHost;
            ReusableBuffer srcDevice;
            ReusableBuffer dstHost;
            VkImage         importedSrcImage       = VK_NULL_HANDLE;
            VkDeviceMemory  importedSrcImageMemory = VK_NULL_HANDLE;
            int             importedSrcFd          = -1;
            uint32_t        importedSrcWidth       = 0;
            uint32_t        importedSrcHeight      = 0;
            uint32_t        importedSrcStride      = 0;
            uint32_t        importedSrcFormat      = 0;
            uint64_t        importedSrcModifier    = 0;
            VkDescriptorSet descriptorSet          = VK_NULL_HANDLE;
        };

        VkInstance                                                   m_instance            = VK_NULL_HANDLE;
        VkPhysicalDevice                                             m_physicalDevice      = VK_NULL_HANDLE;
        VkDevice                                                     m_device              = VK_NULL_HANDLE;
        VkQueue                                                      m_computeQueue        = VK_NULL_HANDLE;
        uint32_t                                                     m_computeQueueFamily  = 0;
        VkCommandPool                                                m_commandPool         = VK_NULL_HANDLE;
        VkDescriptorPool                                             m_descriptorPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout                                        m_descriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout                                             m_pipelineLayout      = VK_NULL_HANDLE;
        VkPipeline                                                   m_computePipeline     = VK_NULL_HANDLE;
        VkShaderModule                                               m_shaderModule        = VK_NULL_HANDLE;

        // Keep one Vulkan slot per rotated capture buffer so imported DMA-BUF images
        // and other per-slot resources can be reused instead of recreated every frame.
        static constexpr size_t                                      POOL_SIZE                   = 12;
        VkCommandBuffer                                              m_commandBuffers[POOL_SIZE] = {};
        VkFence                                                      m_fences[POOL_SIZE]         = {};
        SlotResources                                                m_slotResources[POOL_SIZE]  = {};
        size_t                                                       m_currentPoolIndex          = 0;

        std::thread                                                  m_workerThread;
        std::atomic<bool>                                            m_running{true};
        std::mutex                                                   m_queueMutex;
        std::condition_variable                                      m_queueCV;
        std::queue<std::pair<FrameInput, std::promise<FrameOutput>>> m_frameQueue;

        bool                                                         m_initialized = false;

#ifdef XDPH_DEBUG
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
#endif
    };

}
