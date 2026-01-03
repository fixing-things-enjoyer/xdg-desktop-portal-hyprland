#ifdef HAS_VULKAN_ROTATION

#include "VulkanTransform.hpp"
#include "../helpers/Log.hpp"
#include "../core/PortalManager.hpp"

#include <vulkan/vulkan.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <array>

struct CVulkanTransform::SVulkanState {
    VkInstance                                    instance            = VK_NULL_HANDLE;
    VkPhysicalDevice                              physicalDevice      = VK_NULL_HANDLE;
    VkDevice                                      device              = VK_NULL_HANDLE;
    VkQueue                                       computeQueue        = VK_NULL_HANDLE;
    uint32_t                                      queueFamily         = 0;
    VkCommandPool                                 commandPool         = VK_NULL_HANDLE;
    VkDescriptorPool                              descriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSetLayout                         descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout                              pipelineLayout      = VK_NULL_HANDLE;
    VkPipeline                                    computePipeline     = VK_NULL_HANDLE;
    VkShaderModule                                shaderModule        = VK_NULL_HANDLE;

    PFN_vkGetMemoryFdKHR                          vkGetMemoryFdKHR                          = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 vkGetPhysicalDeviceImageFormatProperties2 = nullptr;
};

static const uint32_t rotateShaderSpv[] = {
#include "rotate_shader.inc"
};

CVulkanTransform& CVulkanTransform::instance() {
    static CVulkanTransform inst;
    return inst;
}

bool CVulkanTransform::init() {
    if (m_bInitialized)
        return m_bGood;

    m_bInitialized = true;
    m_pState       = std::make_unique<SVulkanState>();

    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "xdg-desktop-portal-hyprland";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName        = "xdph";
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    std::vector<const char*> extensions = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&createInfo, nullptr, &m_pState->instance) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create Vulkan instance");
        return false;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_pState->instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        Debug::log(ERR, "[vulkan] No Vulkan devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_pState->instance, &deviceCount, devices.data());
    m_pState->physicalDevice = devices[0];

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_pState->physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_pState->physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            m_pState->queueFamily = i;
            break;
        }
    }

    float                   queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_pState->queueFamily;
    queueCreateInfo.queueCount       = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExtensions = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    };

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo       deviceCreateInfo{};
    deviceCreateInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount    = 1;
    deviceCreateInfo.pQueueCreateInfos       = &queueCreateInfo;
    deviceCreateInfo.pEnabledFeatures        = &deviceFeatures;
    deviceCreateInfo.enabledExtensionCount   = deviceExtensions.size();
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(m_pState->physicalDevice, &deviceCreateInfo, nullptr, &m_pState->device) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create logical device");
        return false;
    }

    vkGetDeviceQueue(m_pState->device, m_pState->queueFamily, 0, &m_pState->computeQueue);

    m_pState->vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(m_pState->device, "vkGetMemoryFdKHR");
    if (!m_pState->vkGetMemoryFdKHR) {
        Debug::log(ERR, "[vulkan] Failed to get vkGetMemoryFdKHR");
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_pState->queueFamily;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(m_pState->device, &poolInfo, nullptr, &m_pState->commandPool) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create command pool");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 4;

    VkDescriptorPoolCreateInfo descPoolInfo{};
    descPoolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes    = &poolSize;
    descPoolInfo.maxSets       = 2;

    if (vkCreateDescriptorPool(m_pState->device, &descPoolInfo, nullptr, &m_pState->descriptorPool) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create descriptor pool");
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bindings.size();
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(m_pState->device, &layoutInfo, nullptr, &m_pState->descriptorSetLayout) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create descriptor set layout");
        return false;
    }

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset     = 0;
    pushConstant.size       = sizeof(int32_t);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount         = 1;
    pipelineLayoutInfo.pSetLayouts            = &m_pState->descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges    = &pushConstant;

    if (vkCreatePipelineLayout(m_pState->device, &pipelineLayoutInfo, nullptr, &m_pState->pipelineLayout) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create pipeline layout");
        return false;
    }

    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(rotateShaderSpv);
    shaderInfo.pCode    = rotateShaderSpv;

    if (vkCreateShaderModule(m_pState->device, &shaderInfo, nullptr, &m_pState->shaderModule) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create shader module");
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = m_pState->shaderModule;
    shaderStage.pName  = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage  = shaderStage;
    pipelineInfo.layout = m_pState->pipelineLayout;

    if (vkCreateComputePipelines(m_pState->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pState->computePipeline) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create compute pipeline");
        return false;
    }

    m_bGood = true;
    Debug::log(LOG, "[vulkan] GPU rotation initialized successfully");
    return true;
}

bool CVulkanTransform::good() const {
    return m_bGood;
}

void CVulkanTransform::destroy() {
    if (!m_pState)
        return;

    if (m_pState->device) {
        vkDeviceWaitIdle(m_pState->device);

        if (m_pState->computePipeline)
            vkDestroyPipeline(m_pState->device, m_pState->computePipeline, nullptr);
        if (m_pState->shaderModule)
            vkDestroyShaderModule(m_pState->device, m_pState->shaderModule, nullptr);
        if (m_pState->pipelineLayout)
            vkDestroyPipelineLayout(m_pState->device, m_pState->pipelineLayout, nullptr);
        if (m_pState->descriptorSetLayout)
            vkDestroyDescriptorSetLayout(m_pState->device, m_pState->descriptorSetLayout, nullptr);
        if (m_pState->descriptorPool)
            vkDestroyDescriptorPool(m_pState->device, m_pState->descriptorPool, nullptr);
        if (m_pState->commandPool)
            vkDestroyCommandPool(m_pState->device, m_pState->commandPool, nullptr);

        vkDestroyDevice(m_pState->device, nullptr);
    }

    if (m_pState->instance)
        vkDestroyInstance(m_pState->instance, nullptr);

    m_pState.reset();
    m_bGood = false;
}

bool CVulkanTransform::needsRotation(wl_output_transform transform) {
    return transform != WL_OUTPUT_TRANSFORM_NORMAL;
}

void CVulkanTransform::getRotatedDimensions(uint32_t inW, uint32_t inH, wl_output_transform transform, uint32_t& outW, uint32_t& outH) {
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_90:
        case WL_OUTPUT_TRANSFORM_270:
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            outW = inH;
            outH = inW;
            break;
        default:
            outW = inW;
            outH = inH;
            break;
    }
}

static int32_t transformToRotationMode(wl_output_transform transform) {
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_90:
        case WL_OUTPUT_TRANSFORM_FLIPPED_90: return 1;
        case WL_OUTPUT_TRANSFORM_180:
        case WL_OUTPUT_TRANSFORM_FLIPPED_180: return 2;
        case WL_OUTPUT_TRANSFORM_270:
        case WL_OUTPUT_TRANSFORM_FLIPPED_270: return 3;
        default: return 0;
    }
}

static VkFormat drmToVkFormat(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
        default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

CVulkanTransform::SRotationResult CVulkanTransform::rotateBuffer(gbm_bo* inputBo, uint32_t width, uint32_t height, uint32_t format, wl_output_transform transform) {
    SRotationResult result{};

    if (!m_bGood || !inputBo) {
        Debug::log(ERR, "[vulkan] rotateBuffer called with invalid state");
        return result;
    }

    uint32_t outW, outH;
    getRotatedDimensions(width, height, transform, outW, outH);

    int32_t rotationMode = transformToRotationMode(transform);
    if (rotationMode == 0) {
        Debug::log(TRACE, "[vulkan] No rotation needed");
        return result;
    }

    Debug::log(TRACE, "[vulkan] Rotating {}x{} -> {}x{} (mode {})", width, height, outW, outH, rotationMode);

    VkFormat vkFormat = drmToVkFormat(format);

    int      inputFd = gbm_bo_get_fd(inputBo);
    if (inputFd < 0) {
        Debug::log(ERR, "[vulkan] Failed to get fd from input gbm_bo");
        return result;
    }

    uint32_t            inputStride   = gbm_bo_get_stride(inputBo);
    uint64_t            inputModifier = gbm_bo_get_modifier(inputBo);

    VkSubresourceLayout inputLayout{};
    inputLayout.offset   = 0;
    inputLayout.rowPitch = inputStride;
    inputLayout.size     = inputStride * height;

    VkImageDrmFormatModifierExplicitCreateInfoEXT inputDrmInfo{};
    inputDrmInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    inputDrmInfo.drmFormatModifier           = inputModifier;
    inputDrmInfo.drmFormatModifierPlaneCount = 1;
    inputDrmInfo.pPlaneLayouts               = &inputLayout;

    VkExternalMemoryImageCreateInfo externalInfo{};
    externalInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalInfo.pNext       = &inputDrmInfo;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo inputImageInfo{};
    inputImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    inputImageInfo.pNext         = &externalInfo;
    inputImageInfo.imageType     = VK_IMAGE_TYPE_2D;
    inputImageInfo.format        = vkFormat;
    inputImageInfo.extent        = {width, height, 1};
    inputImageInfo.mipLevels     = 1;
    inputImageInfo.arrayLayers   = 1;
    inputImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    inputImageInfo.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    inputImageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT;
    inputImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    inputImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage inputImage = VK_NULL_HANDLE;
    if (vkCreateImage(m_pState->device, &inputImageInfo, nullptr, &inputImage) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create input VkImage");
        close(inputFd);
        return result;
    }

    VkMemoryRequirements inputMemReqs{};
    vkGetImageMemoryRequirements(m_pState->device, inputImage, &inputMemReqs);

    VkImportMemoryFdInfoKHR importInfo{};
    importInfo.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd         = inputFd;

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = inputImage;

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_pState->physicalDevice, &memProps);

    uint32_t inputMemType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((inputMemReqs.memoryTypeBits & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            inputMemType = i;
            break;
        }
    }
    if (inputMemType == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if (inputMemReqs.memoryTypeBits & (1 << i)) {
                inputMemType = i;
                break;
            }
        }
    }

    VkMemoryAllocateInfo inputAllocInfo{};
    inputAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    inputAllocInfo.pNext           = &dedicatedInfo;
    inputAllocInfo.allocationSize  = inputMemReqs.size;
    inputAllocInfo.memoryTypeIndex = inputMemType;

    VkDeviceMemory inputMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(m_pState->device, &inputAllocInfo, nullptr, &inputMemory) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to allocate input memory");
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    vkBindImageMemory(m_pState->device, inputImage, inputMemory, 0);

    gbm_bo* outputBo = gbm_bo_create(g_pPortalManager->m_sWaylandConnection.gbmDevice, outW, outH, format, GBM_BO_USE_RENDERING);
    if (!outputBo) {
        Debug::log(ERR, "[vulkan] Failed to create output gbm_bo");
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    int outputFd = gbm_bo_get_fd(outputBo);
    if (outputFd < 0) {
        Debug::log(ERR, "[vulkan] Failed to get fd from output gbm_bo");
        gbm_bo_destroy(outputBo);
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    uint32_t            outputStride   = gbm_bo_get_stride(outputBo);
    uint64_t            outputModifier = gbm_bo_get_modifier(outputBo);

    VkSubresourceLayout outputLayout{};
    outputLayout.offset   = 0;
    outputLayout.rowPitch = outputStride;
    outputLayout.size     = outputStride * outH;

    VkImageDrmFormatModifierExplicitCreateInfoEXT outputDrmInfo{};
    outputDrmInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    outputDrmInfo.drmFormatModifier           = outputModifier;
    outputDrmInfo.drmFormatModifierPlaneCount = 1;
    outputDrmInfo.pPlaneLayouts               = &outputLayout;

    VkExternalMemoryImageCreateInfo outputExternalInfo{};
    outputExternalInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    outputExternalInfo.pNext       = &outputDrmInfo;
    outputExternalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo outputImageInfo{};
    outputImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    outputImageInfo.pNext         = &outputExternalInfo;
    outputImageInfo.imageType     = VK_IMAGE_TYPE_2D;
    outputImageInfo.format        = vkFormat;
    outputImageInfo.extent        = {outW, outH, 1};
    outputImageInfo.mipLevels     = 1;
    outputImageInfo.arrayLayers   = 1;
    outputImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    outputImageInfo.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    outputImageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT;
    outputImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    outputImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage outputImage = VK_NULL_HANDLE;
    if (vkCreateImage(m_pState->device, &outputImageInfo, nullptr, &outputImage) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create output VkImage");
        close(outputFd);
        gbm_bo_destroy(outputBo);
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    VkMemoryRequirements outputMemReqs{};
    vkGetImageMemoryRequirements(m_pState->device, outputImage, &outputMemReqs);

    VkImportMemoryFdInfoKHR outputImportInfo{};
    outputImportInfo.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    outputImportInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    outputImportInfo.fd         = outputFd;

    VkMemoryDedicatedAllocateInfo outputDedicatedInfo{};
    outputDedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    outputDedicatedInfo.pNext = &outputImportInfo;
    outputDedicatedInfo.image = outputImage;

    uint32_t outputMemType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((outputMemReqs.memoryTypeBits & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            outputMemType = i;
            break;
        }
    }
    if (outputMemType == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if (outputMemReqs.memoryTypeBits & (1 << i)) {
                outputMemType = i;
                break;
            }
        }
    }

    VkMemoryAllocateInfo outputAllocInfo{};
    outputAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    outputAllocInfo.pNext           = &outputDedicatedInfo;
    outputAllocInfo.allocationSize  = outputMemReqs.size;
    outputAllocInfo.memoryTypeIndex = outputMemType;

    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(m_pState->device, &outputAllocInfo, nullptr, &outputMemory) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to allocate output memory");
        vkDestroyImage(m_pState->device, outputImage, nullptr);
        gbm_bo_destroy(outputBo);
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    vkBindImageMemory(m_pState->device, outputImage, outputMemory, 0);

    VkImageViewCreateInfo inputViewInfo{};
    inputViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    inputViewInfo.image                           = inputImage;
    inputViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    inputViewInfo.format                          = vkFormat;
    inputViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    inputViewInfo.subresourceRange.baseMipLevel   = 0;
    inputViewInfo.subresourceRange.levelCount     = 1;
    inputViewInfo.subresourceRange.baseArrayLayer = 0;
    inputViewInfo.subresourceRange.layerCount     = 1;

    VkImageView inputView = VK_NULL_HANDLE;
    if (vkCreateImageView(m_pState->device, &inputViewInfo, nullptr, &inputView) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create input image view");
        vkFreeMemory(m_pState->device, outputMemory, nullptr);
        vkDestroyImage(m_pState->device, outputImage, nullptr);
        gbm_bo_destroy(outputBo);
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    VkImageViewCreateInfo outputViewInfo{};
    outputViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    outputViewInfo.image                           = outputImage;
    outputViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    outputViewInfo.format                          = vkFormat;
    outputViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    outputViewInfo.subresourceRange.baseMipLevel   = 0;
    outputViewInfo.subresourceRange.levelCount     = 1;
    outputViewInfo.subresourceRange.baseArrayLayer = 0;
    outputViewInfo.subresourceRange.layerCount     = 1;

    VkImageView outputView = VK_NULL_HANDLE;
    if (vkCreateImageView(m_pState->device, &outputViewInfo, nullptr, &outputView) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to create output image view");
        vkDestroyImageView(m_pState->device, inputView, nullptr);
        vkFreeMemory(m_pState->device, outputMemory, nullptr);
        vkDestroyImage(m_pState->device, outputImage, nullptr);
        gbm_bo_destroy(outputBo);
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    VkDescriptorSetAllocateInfo descAllocInfo{};
    descAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descAllocInfo.descriptorPool     = m_pState->descriptorPool;
    descAllocInfo.descriptorSetCount = 1;
    descAllocInfo.pSetLayouts        = &m_pState->descriptorSetLayout;

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_pState->device, &descAllocInfo, &descriptorSet) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to allocate descriptor set");
        vkDestroyImageView(m_pState->device, outputView, nullptr);
        vkDestroyImageView(m_pState->device, inputView, nullptr);
        vkFreeMemory(m_pState->device, outputMemory, nullptr);
        vkDestroyImage(m_pState->device, outputImage, nullptr);
        gbm_bo_destroy(outputBo);
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    std::array<VkDescriptorImageInfo, 2> imageInfos{};
    imageInfos[0].imageView   = inputView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfos[1].imageView   = outputView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &imageInfos[0];

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &imageInfos[1];

    vkUpdateDescriptorSets(m_pState->device, writes.size(), writes.data(), 0, nullptr);

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool        = m_pState->commandPool;
    cmdAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_pState->device, &cmdAllocInfo, &cmdBuffer) != VK_SUCCESS) {
        Debug::log(ERR, "[vulkan] Failed to allocate command buffer");
        vkFreeDescriptorSets(m_pState->device, m_pState->descriptorPool, 1, &descriptorSet);
        vkDestroyImageView(m_pState->device, outputView, nullptr);
        vkDestroyImageView(m_pState->device, inputView, nullptr);
        vkFreeMemory(m_pState->device, outputMemory, nullptr);
        vkDestroyImage(m_pState->device, outputImage, nullptr);
        gbm_bo_destroy(outputBo);
        vkFreeMemory(m_pState->device, inputMemory, nullptr);
        vkDestroyImage(m_pState->device, inputImage, nullptr);
        return result;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmdBuffer, &beginInfo);

    std::array<VkImageMemoryBarrier, 2> barriers{};
    barriers[0].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask                   = 0;
    barriers[0].dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image                           = inputImage;
    barriers[0].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel   = 0;
    barriers[0].subresourceRange.levelCount     = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount     = 1;

    barriers[1].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].srcAccessMask                   = 0;
    barriers[1].dstAccessMask                   = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image                           = outputImage;
    barriers[1].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel   = 0;
    barriers[1].subresourceRange.levelCount     = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount     = 1;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());

    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pState->computePipeline);
    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pState->pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmdBuffer, m_pState->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t), &rotationMode);
    vkCmdDispatch(cmdBuffer, (width + 15) / 16, (height + 15) / 16, 1);

    VkImageMemoryBarrier outputBarrier{};
    outputBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputBarrier.srcAccessMask                   = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.dstAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
    outputBarrier.oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.image                           = outputImage;
    outputBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    outputBarrier.subresourceRange.baseMipLevel   = 0;
    outputBarrier.subresourceRange.levelCount     = 1;
    outputBarrier.subresourceRange.baseArrayLayer = 0;
    outputBarrier.subresourceRange.layerCount     = 1;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &outputBarrier);

    vkEndCommandBuffer(cmdBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmdBuffer;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(m_pState->device, &fenceInfo, nullptr, &fence);

    vkQueueSubmit(m_pState->computeQueue, 1, &submitInfo, fence);
    vkWaitForFences(m_pState->device, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(m_pState->device, fence, nullptr);
    vkFreeCommandBuffers(m_pState->device, m_pState->commandPool, 1, &cmdBuffer);
    vkFreeDescriptorSets(m_pState->device, m_pState->descriptorPool, 1, &descriptorSet);
    vkDestroyImageView(m_pState->device, outputView, nullptr);
    vkDestroyImageView(m_pState->device, inputView, nullptr);
    vkFreeMemory(m_pState->device, outputMemory, nullptr);
    vkDestroyImage(m_pState->device, outputImage, nullptr);
    vkFreeMemory(m_pState->device, inputMemory, nullptr);
    vkDestroyImage(m_pState->device, inputImage, nullptr);

    result.success   = true;
    result.outputBo  = outputBo;
    result.outWidth  = outW;
    result.outHeight = outH;
    result.outFd     = gbm_bo_get_fd(outputBo);

    Debug::log(LOG, "[vulkan] Rotation complete: {}x{}", outW, outH);
    return result;
}

#endif
