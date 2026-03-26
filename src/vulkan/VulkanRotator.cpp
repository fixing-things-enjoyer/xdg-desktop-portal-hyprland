#include "VulkanRotator.hpp"
#include "../helpers/Log.hpp"

#include <xf86drm.h>
#include <libdrm/drm_fourcc.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <vector>

#include "rotate_shader.h"

namespace xdph::vulkan {

#ifdef XDPH_DEBUG
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            Debug::log(ERR, "[vulkan] Validation: {}", pCallbackData->pMessage);
        return VK_FALSE;
    }
#endif

    std::unique_ptr<VulkanRotator> VulkanRotator::create() {
        auto rotator = std::unique_ptr<VulkanRotator>(new VulkanRotator());
        if (!rotator->initialize()) {
            Debug::log(ERR, "[vulkan] Failed to initialize VulkanRotator");
            return nullptr;
        }
        return rotator;
    }

    VulkanRotator::VulkanRotator() = default;

    VulkanRotator::~VulkanRotator() {
        m_running = false;
        m_queueCV.notify_all();

        if (m_workerThread.joinable())
            m_workerThread.join();

        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);

            auto destroyReusableBuffer = [&](ReusableBuffer& buffer) {
                if (buffer.mapped) {
                    vkUnmapMemory(m_device, buffer.memory);
                    buffer.mapped = nullptr;
                }
                if (buffer.buffer != VK_NULL_HANDLE)
                    vkDestroyBuffer(m_device, buffer.buffer, nullptr);
                if (buffer.memory != VK_NULL_HANDLE)
                    vkFreeMemory(m_device, buffer.memory, nullptr);
                buffer = {};
            };

            for (auto& slot : m_slotResources) {
                destroyReusableBuffer(slot.srcHost);
                destroyReusableBuffer(slot.srcDevice);
                destroyReusableBuffer(slot.dstHost);
                if (slot.importedSrcImage != VK_NULL_HANDLE)
                    vkDestroyImage(m_device, slot.importedSrcImage, nullptr);
                if (slot.importedSrcImageMemory != VK_NULL_HANDLE)
                    vkFreeMemory(m_device, slot.importedSrcImageMemory, nullptr);
            }

            for (size_t i = 0; i < POOL_SIZE; i++) {
                if (m_fences[i] != VK_NULL_HANDLE)
                    vkDestroyFence(m_device, m_fences[i], nullptr);
            }

            if (m_computePipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_device, m_computePipeline, nullptr);
            if (m_pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            if (m_descriptorSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
            if (m_descriptorPool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            if (m_commandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            if (m_shaderModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(m_device, m_shaderModule, nullptr);

            vkDestroyDevice(m_device, nullptr);
        }

#ifdef XDPH_DEBUG
        if (m_debugMessenger != VK_NULL_HANDLE) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
            if (func)
                func(m_instance, m_debugMessenger, nullptr);
        }
#endif

        if (m_instance != VK_NULL_HANDLE)
            vkDestroyInstance(m_instance, nullptr);
    }

    bool VulkanRotator::initialize() {
        VkApplicationInfo appInfo{};
        appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName   = "xdg-desktop-portal-hyprland";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName        = "xdph-vulkan";
        appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion         = VK_API_VERSION_1_1;

        std::vector<const char*> extensions = {
            VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        };

        std::vector<const char*> layers;

#ifdef XDPH_DEBUG
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

        VkInstanceCreateInfo createInfo{};
        createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo        = &appInfo;
        createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount       = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames     = layers.data();

        if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create Vulkan instance");
            return false;
        }

#ifdef XDPH_DEBUG
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (func)
            func(m_instance, &debugCreateInfo, nullptr, &m_debugMessenger);
#endif

        if (!selectPhysicalDevice())
            return false;
        if (!createLogicalDevice())
            return false;
        if (!createCommandPool())
            return false;
        if (!createDescriptorPool())
            return false;
        if (!createDescriptorSetLayout())
            return false;
        if (!createPipelineLayout())
            return false;
        if (!createComputePipeline())
            return false;
        if (!createCommandBuffersAndFences())
            return false;

        m_workerThread = std::thread(&VulkanRotator::workerLoop, this);
        m_initialized  = true;

        Debug::log(LOG, "[vulkan] VulkanRotator initialized successfully");
        return true;
    }

    bool VulkanRotator::selectPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);

        if (deviceCount == 0) {
            Debug::log(ERR, "[vulkan] No Vulkan-capable devices found");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        for (const auto& device : devices) {
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

            for (uint32_t i = 0; i < queueFamilyCount; i++) {
                if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    m_physicalDevice     = device;
                    m_computeQueueFamily = i;

                    VkPhysicalDeviceProperties props;
                    vkGetPhysicalDeviceProperties(device, &props);
                    Debug::log(LOG, "[vulkan] Selected device: {}", props.deviceName);
                    return true;
                }
            }
        }

        Debug::log(ERR, "[vulkan] No device with compute capability found");
        return false;
    }

    bool VulkanRotator::createLogicalDevice() {
        float                   queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = m_computeQueueFamily;
        queueCreateInfo.queueCount       = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        std::vector<const char*> deviceExtensions = {
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        };

        VkPhysicalDeviceFeatures deviceFeatures{};

        VkDeviceCreateInfo       createInfo{};
        createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount    = 1;
        createInfo.pQueueCreateInfos       = &queueCreateInfo;
        createInfo.enabledExtensionCount   = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        createInfo.pEnabledFeatures        = &deviceFeatures;

        if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create logical device");
            return false;
        }

        vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);
        return true;
    }

    bool VulkanRotator::createCommandPool() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_computeQueueFamily;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create command pool");
            return false;
        }
        return true;
    }

    bool VulkanRotator::createDescriptorPool() {
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = POOL_SIZE * 2;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = POOL_SIZE;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create descriptor pool");
            return false;
        }
        return true;
    }

    bool VulkanRotator::createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding                      = 0;
        bindings[0].descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount              = 1;
        bindings[0].stageFlags                   = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings    = bindings;

        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create descriptor set layout");
            return false;
        }
        return true;
    }

    bool VulkanRotator::createPipelineLayout() {
        struct PushConstants {
            int32_t transform;
            int32_t srcWidth;
            int32_t srcHeight;
            int32_t srcStridePixels;
            int32_t dstStridePixels;
            int32_t srcRegionX;
            int32_t srcRegionY;
            int32_t copyWidth;
            int32_t copyHeight;
            int32_t logicalRegionX;
            int32_t logicalRegionY;
            int32_t dstWidth;
            int32_t dstHeight;
        };

        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushConstant.offset     = 0;
        pushConstant.size       = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = 1;
        layoutInfo.pSetLayouts            = &m_descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pushConstant;

        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create pipeline layout");
            return false;
        }
        return true;
    }

    bool VulkanRotator::createComputePipeline() {
        VkShaderModuleCreateInfo shaderInfo{};
        shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderInfo.codeSize = sizeof(rotate_shader_spv);
        shaderInfo.pCode    = reinterpret_cast<const uint32_t*>(rotate_shader_spv);

        if (vkCreateShaderModule(m_device, &shaderInfo, nullptr, &m_shaderModule) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create shader module");
            return false;
        }

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = m_shaderModule;
        stageInfo.pName  = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage  = stageInfo;
        pipelineInfo.layout = m_pipelineLayout;

        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_computePipeline) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to create compute pipeline");
            return false;
        }
        return true;
    }

    bool VulkanRotator::createCommandBuffersAndFences() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = m_commandPool;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = POOL_SIZE;

        if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers) != VK_SUCCESS) {
            Debug::log(ERR, "[vulkan] Failed to allocate command buffers");
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < POOL_SIZE; i++) {
            if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_fences[i]) != VK_SUCCESS) {
                Debug::log(ERR, "[vulkan] Failed to create fence");
                return false;
            }
        }
        return true;
    }

    void VulkanRotator::workerLoop() {
        while (m_running) {
            std::pair<FrameInput, std::promise<FrameOutput>> work;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCV.wait(lock, [this] { return !m_frameQueue.empty() || !m_running; });
                if (!m_running && m_frameQueue.empty())
                    return;
                work = std::move(m_frameQueue.front());
                m_frameQueue.pop();
            }

            FrameOutput result = processFrame(work.first);
            work.second.set_value(std::move(result));
        }
    }

    FrameOutput VulkanRotator::process(const FrameInput& input) {
        std::promise<FrameOutput> promise;
        std::future<FrameOutput>  future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_frameQueue.push({input, std::move(promise)});
        }
        m_queueCV.notify_one();

        return future.get();
    }

    static VkFormat drmFormatToVkFormat(uint32_t drmFormat) {
        switch (drmFormat) {
            case DRM_FORMAT_ARGB8888:
            case DRM_FORMAT_XRGB8888:
            case DRM_FORMAT_BGRA8888:
            case DRM_FORMAT_BGRX8888: return VK_FORMAT_B8G8R8A8_UNORM;
            case DRM_FORMAT_ABGR8888:
            case DRM_FORMAT_XBGR8888:
            case DRM_FORMAT_RGBA8888:
            case DRM_FORMAT_RGBX8888: return VK_FORMAT_R8G8B8A8_UNORM;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    FrameOutput VulkanRotator::processFrame(const FrameInput& input) {
        struct PushConstants {
            int32_t transform;
            int32_t srcWidth;
            int32_t srcHeight;
            int32_t srcStridePixels;
            int32_t dstStridePixels;
            int32_t srcRegionX;
            int32_t srcRegionY;
            int32_t copyWidth;
            int32_t copyHeight;
            int32_t logicalRegionX;
            int32_t logicalRegionY;
            int32_t dstWidth;
            int32_t dstHeight;
        };

        FrameOutput output{};
        output.dmaBufFd = -1;
        output.success  = false;
        output.size     = 0;

        Debug::log(LOG, "[vkrot] process transform {}, dims {}x{}, stride {}", (int)input.transform, input.width, input.height, input.stride);

        if (input.transform == WL_OUTPUT_TRANSFORM_NORMAL) {
            output.error = "No rotation needed";
            Debug::log(TRACE, "[vkrot] transform normal, skipping");
            return output;
        }

        VkFormat vkFormat = drmFormatToVkFormat(input.format);
        if (vkFormat == VK_FORMAT_UNDEFINED) {
            output.error = "Unsupported DRM format";
            return output;
        }

        Dimensions fullSrcDims    = {input.width, input.height};
        Dimensions logicalSrcDims = getLogicalDimensions(fullSrcDims, input.transform);

        Region logicalRegion = input.region;
        if (logicalRegion.width <= 0 || logicalRegion.height <= 0) {
            logicalRegion = {0, 0, (int32_t)logicalSrcDims.width, (int32_t)logicalSrcDims.height};
        } else {
            logicalRegion.x      = std::clamp<int32_t>(logicalRegion.x, 0, logicalSrcDims.width > 0 ? (int32_t)logicalSrcDims.width - 1 : 0);
            logicalRegion.y      = std::clamp<int32_t>(logicalRegion.y, 0, logicalSrcDims.height > 0 ? (int32_t)logicalSrcDims.height - 1 : 0);
            logicalRegion.width  = std::clamp<int32_t>(logicalRegion.width, 1, (int32_t)logicalSrcDims.width - logicalRegion.x);
            logicalRegion.height = std::clamp<int32_t>(logicalRegion.height, 1, (int32_t)logicalSrcDims.height - logicalRegion.y);
        }

        Region     physicalRegion = logicalToPhysical(logicalRegion, logicalSrcDims, input.transform);
        Dimensions srcDims        = {(uint32_t)physicalRegion.width, (uint32_t)physicalRegion.height};
        Dimensions destDims       = {(uint32_t)logicalRegion.width, (uint32_t)logicalRegion.height};
        const uint32_t srcStride  = srcDims.width * 4;
        const uint32_t dstStride  = destDims.width * 4;
        const VkDeviceSize srcSize = static_cast<VkDeviceSize>(srcStride) * srcDims.height;
        const VkDeviceSize dstSize = static_cast<VkDeviceSize>(dstStride) * destDims.height;

        size_t     poolIdx = m_currentPoolIndex;
        m_currentPoolIndex = (m_currentPoolIndex + 1) % POOL_SIZE;
        auto& slot = m_slotResources[poolIdx];

        vkWaitForFences(m_device, 1, &m_fences[poolIdx], VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_fences[poolIdx]);

        const bool      useHostSource      = input.hostSrcData != nullptr;
        const bool      useHostDestination = input.hostDstData != nullptr;
        VkImage         srcImage           = VK_NULL_HANDLE;
        VkBuffer        srcBuffer          = VK_NULL_HANDLE;
        VkDeviceMemory  srcMemory          = VK_NULL_HANDLE;
        VkBuffer        dstBuffer          = VK_NULL_HANDLE;
        VkDeviceMemory  dstMemory          = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet      = VK_NULL_HANDLE;
        bool            ownsDstBuffer      = false;

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
        auto findMemoryType = [&](uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) -> uint32_t {
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((bits & (1u << i)) == 0)
                    continue;
                const auto flags = memProps.memoryTypes[i].propertyFlags;
                if ((flags & required) != required)
                    continue;
                if (preferred == 0 || (flags & preferred) == preferred)
                    return i;
            }

            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((bits & (1u << i)) == 0)
                    continue;
                const auto flags = memProps.memoryTypes[i].propertyFlags;
                if ((flags & required) == required)
                    return i;
            }

            return UINT32_MAX;
        };

        auto destroyReusableBuffer = [&](ReusableBuffer& buffer) {
            if (buffer.mapped) {
                vkUnmapMemory(m_device, buffer.memory);
                buffer.mapped = nullptr;
            }
            if (buffer.buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(m_device, buffer.buffer, nullptr);
            if (buffer.memory != VK_NULL_HANDLE)
                vkFreeMemory(m_device, buffer.memory, nullptr);
            buffer = {};
        };

        auto destroyImportedSrcImage = [&](SlotResources& resources) {
            if (resources.importedSrcImage != VK_NULL_HANDLE)
                vkDestroyImage(m_device, resources.importedSrcImage, nullptr);
            if (resources.importedSrcImageMemory != VK_NULL_HANDLE)
                vkFreeMemory(m_device, resources.importedSrcImageMemory, nullptr);

            resources.importedSrcImage       = VK_NULL_HANDLE;
            resources.importedSrcImageMemory = VK_NULL_HANDLE;
            resources.importedSrcFd          = -1;
            resources.importedSrcWidth       = 0;
            resources.importedSrcHeight      = 0;
            resources.importedSrcStride      = 0;
            resources.importedSrcFormat      = 0;
            resources.importedSrcModifier    = 0;
        };

        auto cleanup = [&]() {
            if (ownsDstBuffer && dstBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(m_device, dstBuffer, nullptr);
            if (ownsDstBuffer && dstMemory != VK_NULL_HANDLE)
                vkFreeMemory(m_device, dstMemory, nullptr);
        };

        auto ensureReusableBuffer = [&](ReusableBuffer& buffer, VkDeviceSize requiredSize, VkBufferUsageFlags usage, VkMemoryPropertyFlags requiredProps,
                                        VkMemoryPropertyFlags preferredProps) -> bool {
            if (buffer.buffer != VK_NULL_HANDLE && buffer.capacity >= requiredSize)
                return true;

            destroyReusableBuffer(buffer);

            VkBufferCreateInfo info{};
            info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size        = requiredSize;
            info.usage       = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(m_device, &info, nullptr, &buffer.buffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements reqs;
            vkGetBufferMemoryRequirements(m_device, buffer.buffer, &reqs);

            buffer.memoryTypeIndex = findMemoryType(reqs.memoryTypeBits, requiredProps, preferredProps);
            if (buffer.memoryTypeIndex == UINT32_MAX)
                return false;

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize  = reqs.size;
            allocInfo.memoryTypeIndex = buffer.memoryTypeIndex;

            if (vkAllocateMemory(m_device, &allocInfo, nullptr, &buffer.memory) != VK_SUCCESS)
                return false;

            if (vkBindBufferMemory(m_device, buffer.buffer, buffer.memory, 0) != VK_SUCCESS)
                return false;

            buffer.capacity     = reqs.size;
            buffer.hostCoherent = (memProps.memoryTypes[buffer.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

            if ((memProps.memoryTypes[buffer.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
                if (vkMapMemory(m_device, buffer.memory, 0, buffer.capacity, 0, &buffer.mapped) != VK_SUCCESS)
                    return false;
            }

            return true;
        };

        ReusableBuffer& srcReusable = useHostSource ? slot.srcHost : slot.srcDevice;
        const VkBufferUsageFlags srcUsage = (useHostSource ? 0 : VK_BUFFER_USAGE_TRANSFER_DST_BIT) | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        const VkMemoryPropertyFlags srcRequired = useHostSource ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0;
        const VkMemoryPropertyFlags srcPreferred = useHostSource ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        if (!ensureReusableBuffer(srcReusable, srcSize, srcUsage, srcRequired, srcPreferred)) {
            output.error = "Failed to allocate reusable source buffer";
            Debug::log(WARN, "[vkrot] alloc reusable src buffer failed");
            cleanup();
            return output;
        }

        srcBuffer = srcReusable.buffer;
        srcMemory = srcReusable.memory;

        if (useHostSource) {
            const size_t rowBytes = std::min<size_t>(srcStride, std::max<int32_t>(physicalRegion.width, 0) * 4);
            auto* srcBytes        = static_cast<const std::byte*>(input.hostSrcData);
            auto* dstBytes        = static_cast<std::byte*>(srcReusable.mapped);
            for (uint32_t row = 0; row < srcDims.height; row++) {
                const size_t srcOffset = static_cast<size_t>(physicalRegion.y + (int32_t)row) * input.hostSrcStride + static_cast<size_t>(physicalRegion.x) * 4;
                std::memcpy(dstBytes + row * srcStride, srcBytes + srcOffset, rowBytes);
            }

            if (!srcReusable.hostCoherent) {
                VkMappedMemoryRange range{};
                range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = srcMemory;
                range.offset = 0;
                range.size   = srcSize;
                vkFlushMappedMemoryRanges(m_device, 1, &range);
            }
        } else {
            const uint64_t srcModifier = input.modifier == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR : input.modifier;
            const bool canReuseImportedSrcImage = slot.importedSrcImage != VK_NULL_HANDLE && slot.importedSrcFd == input.dmaBufFd && slot.importedSrcWidth == input.width &&
                slot.importedSrcHeight == input.height && slot.importedSrcStride == input.stride && slot.importedSrcFormat == input.format && slot.importedSrcModifier == srcModifier;

            if (!canReuseImportedSrcImage) {
                destroyImportedSrcImage(slot);

                VkSubresourceLayout planeLayout{};
                planeLayout.offset     = 0;
                planeLayout.size       = srcSize;
                planeLayout.rowPitch   = input.stride;
                planeLayout.arrayPitch = srcSize;
                planeLayout.depthPitch = srcSize;

                VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{};
                modifierInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
                modifierInfo.drmFormatModifier           = srcModifier;
                modifierInfo.drmFormatModifierPlaneCount = 1;
                modifierInfo.pPlaneLayouts               = &planeLayout;

                VkExternalMemoryImageCreateInfo srcExternalImageInfo{};
                srcExternalImageInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
                srcExternalImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                srcExternalImageInfo.pNext       = &modifierInfo;

                VkImageCreateInfo srcImageInfo{};
                srcImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                srcImageInfo.pNext         = &srcExternalImageInfo;
                srcImageInfo.imageType     = VK_IMAGE_TYPE_2D;
                srcImageInfo.format        = vkFormat;
                srcImageInfo.extent        = {input.width, input.height, 1};
                srcImageInfo.mipLevels     = 1;
                srcImageInfo.arrayLayers   = 1;
                srcImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
                srcImageInfo.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                srcImageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                srcImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
                srcImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

                if (vkCreateImage(m_device, &srcImageInfo, nullptr, &slot.importedSrcImage) != VK_SUCCESS) {
                    output.error = "Failed to create source image";
                    Debug::log(WARN, "[vkrot] create src image failed");
                    cleanup();
                    return output;
                }

                VkMemoryRequirements srcImageReqs;
                vkGetImageMemoryRequirements(m_device, slot.importedSrcImage, &srcImageReqs);

                VkImportMemoryFdInfoKHR importInfo{};
                importInfo.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
                importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                importInfo.fd         = dup(input.dmaBufFd);

                VkMemoryDedicatedAllocateInfo dedicatedInfo{};
                dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
                dedicatedInfo.image = slot.importedSrcImage;
                dedicatedInfo.pNext = &importInfo;

                VkMemoryAllocateInfo srcImageAllocInfo{};
                srcImageAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                srcImageAllocInfo.pNext           = &dedicatedInfo;
                srcImageAllocInfo.allocationSize  = srcImageReqs.size;
                srcImageAllocInfo.memoryTypeIndex = findMemoryType(srcImageReqs.memoryTypeBits, 0);

                if (srcImageAllocInfo.memoryTypeIndex == UINT32_MAX ||
                    vkAllocateMemory(m_device, &srcImageAllocInfo, nullptr, &slot.importedSrcImageMemory) != VK_SUCCESS) {
                    if (importInfo.fd >= 0)
                        close(importInfo.fd);
                    destroyImportedSrcImage(slot);
                    output.error = "Failed to import source DMA-BUF";
                    Debug::log(WARN, "[vkrot] import src dma-buf failed");
                    cleanup();
                    return output;
                }

                if (vkBindImageMemory(m_device, slot.importedSrcImage, slot.importedSrcImageMemory, 0) != VK_SUCCESS) {
                    destroyImportedSrcImage(slot);
                    output.error = "Failed to bind source image memory";
                    Debug::log(WARN, "[vkrot] bind src image failed");
                    cleanup();
                    return output;
                }

                slot.importedSrcFd       = input.dmaBufFd;
                slot.importedSrcWidth    = input.width;
                slot.importedSrcHeight   = input.height;
                slot.importedSrcStride   = input.stride;
                slot.importedSrcFormat   = input.format;
                slot.importedSrcModifier = srcModifier;
            }

            srcImage = slot.importedSrcImage;
        }

        bool  dstHostCoherent = false;
        void* dstMapped       = nullptr;

        if (useHostDestination) {
            if (!ensureReusableBuffer(slot.dstHost, dstSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                output.error = "Failed to allocate reusable destination buffer";
                Debug::log(WARN, "[vkrot] alloc reusable dst buffer failed");
                cleanup();
                return output;
            }

            dstBuffer       = slot.dstHost.buffer;
            dstMemory       = slot.dstHost.memory;
            dstHostCoherent = slot.dstHost.hostCoherent;
            dstMapped       = slot.dstHost.mapped;
        } else {
            VkExternalMemoryBufferCreateInfo dstExtBufferInfo{};
            dstExtBufferInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            dstExtBufferInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

            VkBufferCreateInfo dstBufferInfo{};
            dstBufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            dstBufferInfo.pNext       = &dstExtBufferInfo;
            dstBufferInfo.size        = dstSize;
            dstBufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            dstBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(m_device, &dstBufferInfo, nullptr, &dstBuffer) != VK_SUCCESS) {
                output.error = "Failed to create destination buffer";
                Debug::log(WARN, "[vkrot] create dst buffer failed");
                cleanup();
                return output;
            }

            ownsDstBuffer = true;

            VkMemoryRequirements dstMemReqs;
            vkGetBufferMemoryRequirements(m_device, dstBuffer, &dstMemReqs);

            VkMemoryAllocateInfo dstAllocInfo{};
            dstAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            VkExportMemoryAllocateInfo exportInfo{};
            exportInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            dstAllocInfo.pNext     = &exportInfo;
            dstAllocInfo.allocationSize  = dstMemReqs.size;
            dstAllocInfo.memoryTypeIndex = findMemoryType(dstMemReqs.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            if (dstAllocInfo.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &dstAllocInfo, nullptr, &dstMemory) != VK_SUCCESS) {
                output.error = "Failed to allocate destination memory";
                Debug::log(WARN, "[vkrot] alloc dst memory failed");
                cleanup();
                return output;
            }

            if (vkBindBufferMemory(m_device, dstBuffer, dstMemory, 0) != VK_SUCCESS) {
                output.error = "Failed to bind destination buffer memory";
                Debug::log(WARN, "[vkrot] bind dst buffer failed");
                cleanup();
                return output;
            }
        }

        if (slot.descriptorSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo dsAllocInfo{};
            dsAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsAllocInfo.descriptorPool     = m_descriptorPool;
            dsAllocInfo.descriptorSetCount = 1;
            dsAllocInfo.pSetLayouts        = &m_descriptorSetLayout;

            if (vkAllocateDescriptorSets(m_device, &dsAllocInfo, &slot.descriptorSet) != VK_SUCCESS) {
                output.error = "Failed to allocate descriptor set";
                Debug::log(WARN, "[vkrot] alloc descriptor set failed");
                cleanup();
                return output;
            }
        }

        descriptorSet = slot.descriptorSet;

        VkDescriptorBufferInfo srcDescInfo{};
        srcDescInfo.buffer = srcBuffer;
        srcDescInfo.offset = 0;
        srcDescInfo.range  = srcSize;

        VkDescriptorBufferInfo dstDescInfo{};
        dstDescInfo.buffer = dstBuffer;
        dstDescInfo.offset = 0;
        dstDescInfo.range  = dstSize;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet               = descriptorSet;
        writes[0].dstBinding           = 0;
        writes[0].descriptorType       = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount      = 1;
        writes[0].pBufferInfo          = &srcDescInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &dstDescInfo;

        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

        VkCommandBuffer cmd = m_commandBuffers[poolIdx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            output.error = "Failed to begin command buffer";
            Debug::log(WARN, "[vkrot] begin cmd buffer failed");
            cleanup();
            return output;
        }

        VkBufferMemoryBarrier copyDstBarrier{};
        copyDstBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        copyDstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyDstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copyDstBarrier.buffer              = srcBuffer;
        copyDstBarrier.offset              = 0;
        copyDstBarrier.size                = srcSize;
        copyDstBarrier.srcAccessMask       = 0;
        copyDstBarrier.dstAccessMask       = useHostSource ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;

        if (useHostSource) {
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &copyDstBarrier, 0, nullptr);
        } else {
            VkImageMemoryBarrier srcImageBarrier{};
            srcImageBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            srcImageBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_EXTERNAL;
            srcImageBarrier.dstQueueFamilyIndex             = m_computeQueueFamily;
            srcImageBarrier.oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
            srcImageBarrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            srcImageBarrier.srcAccessMask                   = 0;
            srcImageBarrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;
            srcImageBarrier.image                           = srcImage;
            srcImageBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            srcImageBarrier.subresourceRange.baseMipLevel   = 0;
            srcImageBarrier.subresourceRange.levelCount     = 1;
            srcImageBarrier.subresourceRange.baseArrayLayer = 0;
            srcImageBarrier.subresourceRange.layerCount     = 1;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &copyDstBarrier, 1, &srcImageBarrier);

            VkBufferImageCopy copyRegion{};
            copyRegion.bufferOffset                    = 0;
            copyRegion.bufferRowLength                 = srcDims.width;
            copyRegion.bufferImageHeight               = 0;
            copyRegion.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel       = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount     = 1;
            copyRegion.imageOffset                     = {physicalRegion.x, physicalRegion.y, 0};
            copyRegion.imageExtent                     = {srcDims.width, srcDims.height, 1};

            vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcBuffer, 1, &copyRegion);

            VkImageMemoryBarrier releaseBarrier{};
            releaseBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            releaseBarrier.srcQueueFamilyIndex             = m_computeQueueFamily;
            releaseBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_EXTERNAL;
            releaseBarrier.oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            releaseBarrier.newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
            releaseBarrier.srcAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;
            releaseBarrier.dstAccessMask                   = 0;
            releaseBarrier.image                           = srcImage;
            releaseBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            releaseBarrier.subresourceRange.baseMipLevel   = 0;
            releaseBarrier.subresourceRange.levelCount     = 1;
            releaseBarrier.subresourceRange.baseArrayLayer = 0;
            releaseBarrier.subresourceRange.layerCount     = 1;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &releaseBarrier);
        }

        VkBufferMemoryBarrier srcBarrier{};
        srcBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.buffer              = srcBuffer;
        srcBarrier.offset              = 0;
        srcBarrier.size                = srcSize;
        srcBarrier.srcAccessMask       = useHostSource ? VK_ACCESS_HOST_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        srcBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;

        VkBufferMemoryBarrier dstBarrier{};
        dstBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.buffer              = dstBuffer;
        dstBarrier.offset              = 0;
        dstBarrier.size                = dstSize;
        dstBarrier.srcAccessMask       = 0;
        dstBarrier.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;

        VkBufferMemoryBarrier barriers[2] = {srcBarrier, dstBarrier};
        vkCmdPipelineBarrier(cmd, useHostSource ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                             barriers, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

        PushConstants pushConstants = {
            .transform       = static_cast<int32_t>(input.transform),
            .srcWidth        = static_cast<int32_t>(srcDims.width),
            .srcHeight       = static_cast<int32_t>(srcDims.height),
            .srcStridePixels = static_cast<int32_t>(srcStride / 4),
            .dstStridePixels = static_cast<int32_t>(dstStride / 4),
            .srcRegionX      = 0,
            .srcRegionY      = 0,
            .copyWidth       = static_cast<int32_t>(srcDims.width),
            .copyHeight      = static_cast<int32_t>(srcDims.height),
            .logicalRegionX  = 0,
            .logicalRegionY  = 0,
            .dstWidth        = static_cast<int32_t>(destDims.width),
            .dstHeight       = static_cast<int32_t>(destDims.height),
        };
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &pushConstants);

        Debug::log(LOG, "[vkrot] crop physical {}x{}+{},{} -> logical {}x{}+{},{}", physicalRegion.width, physicalRegion.height, physicalRegion.x, physicalRegion.y,
                   logicalRegion.width, logicalRegion.height, logicalRegion.x, logicalRegion.y);

        uint32_t groupCountX = (srcDims.width + 15) / 16;
        uint32_t groupCountY = (srcDims.height + 15) / 16;
        vkCmdDispatch(cmd, groupCountX, groupCountY, 1);

        VkBufferMemoryBarrier exportBarrier{};
        exportBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        exportBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        exportBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        exportBarrier.buffer              = dstBuffer;
        exportBarrier.offset              = 0;
        exportBarrier.size                = dstSize;
        exportBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        exportBarrier.dstAccessMask       = useHostDestination ? VK_ACCESS_HOST_READ_BIT : VK_ACCESS_MEMORY_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, useHostDestination ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
                             &exportBarrier, 0, nullptr);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            output.error = "Failed to record command buffer";
            Debug::log(WARN, "[vkrot] end cmd buffer failed");
            cleanup();
            return output;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &cmd;

        VkResult submitRes = vkQueueSubmit(m_computeQueue, 1, &submitInfo, m_fences[poolIdx]);
        if (submitRes != VK_SUCCESS) {
            output.error = "Failed to submit command buffer";
            Debug::log(WARN, "[vkrot] queue submit failed: {}", (int)submitRes);
            cleanup();
            return output;
        }

        vkWaitForFences(m_device, 1, &m_fences[poolIdx], VK_TRUE, UINT64_MAX);

        if (useHostDestination) {
            if (!dstMapped) {
                output.error = "Destination buffer not mapped";
                cleanup();
                return output;
            }

            if (!dstHostCoherent) {
                VkMappedMemoryRange range{};
                range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = dstMemory;
                range.offset = 0;
                range.size   = dstSize;
                vkInvalidateMappedMemoryRanges(m_device, 1, &range);
            }

            const size_t rowBytes = std::min<size_t>(dstStride, input.hostDstStride);
            auto* srcBytes        = static_cast<std::byte*>(dstMapped);
            auto* dstBytes        = static_cast<std::byte*>(input.hostDstData);
            for (uint32_t row = 0; row < destDims.height; row++) {
                std::memcpy(dstBytes + row * input.hostDstStride, srcBytes + row * dstStride, rowBytes);
                if (input.hostDstStride > rowBytes)
                    std::memset(dstBytes + row * input.hostDstStride + rowBytes, 0, input.hostDstStride - rowBytes);
            }

            output.width   = destDims.width;
            output.height  = destDims.height;
            output.stride  = input.hostDstStride;
            output.size    = std::min<uint32_t>(input.hostDstSize, static_cast<uint32_t>(dstStride * destDims.height));
            output.success = true;
        } else {
            auto vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR");
            if (!vkGetMemoryFdKHR) {
                output.error = "vkGetMemoryFdKHR not available";
                cleanup();
                return output;
            }

            VkMemoryGetFdInfoKHR getFdInfo{};
            getFdInfo.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            getFdInfo.memory     = dstMemory;
            getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

            int outputFd = -1;
            if (vkGetMemoryFdKHR(m_device, &getFdInfo, &outputFd) != VK_SUCCESS) {
                output.error = "Failed to export destination DMA-BUF";
                cleanup();
                return output;
            }

            output.dmaBufFd = outputFd;
            output.width    = destDims.width;
            output.height   = destDims.height;
            output.stride   = dstStride;
            output.size     = static_cast<uint32_t>(dstSize);
            output.success  = true;
        }

        cleanup();

        return output;
    }

}
