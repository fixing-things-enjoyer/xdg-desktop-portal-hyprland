#pragma once

#ifdef HAS_VULKAN_ROTATION

#include <cstdint>
#include <memory>
#include <wayland-client.h>

struct gbm_bo;

class CVulkanTransform {
  public:
    static CVulkanTransform& instance();

    bool                     init();
    bool                     good() const;
    void                     destroy();

    struct SRotationResult {
        bool     success   = false;
        gbm_bo*  outputBo  = nullptr;
        uint32_t outWidth  = 0;
        uint32_t outHeight = 0;
        int      outFd     = -1;
    };

    SRotationResult rotateBuffer(gbm_bo* inputBo, uint32_t width, uint32_t height, uint32_t format, wl_output_transform transform);

    static bool     needsRotation(wl_output_transform transform);
    static void     getRotatedDimensions(uint32_t inW, uint32_t inH, wl_output_transform transform, uint32_t& outW, uint32_t& outH);

  private:
    CVulkanTransform()  = default;
    ~CVulkanTransform() = default;

    CVulkanTransform(const CVulkanTransform&)            = delete;
    CVulkanTransform& operator=(const CVulkanTransform&) = delete;

    struct SVulkanState;
    std::unique_ptr<SVulkanState> m_pState;

    bool                          m_bInitialized = false;
    bool                          m_bGood        = false;
};

inline CVulkanTransform& g_VulkanTransform = CVulkanTransform::instance();

#else

#include <cstdint>
#include <wayland-client.h>

class CVulkanTransform {
  public:
    static CVulkanTransform& instance() {
        static CVulkanTransform inst;
        return inst;
    }

    bool init() {
        return false;
    }
    bool good() const {
        return false;
    }
    void destroy() {}

    struct SRotationResult {
        bool     success   = false;
        void*    outputBo  = nullptr;
        uint32_t outWidth  = 0;
        uint32_t outHeight = 0;
        int      outFd     = -1;
    };

    SRotationResult rotateBuffer(void*, uint32_t, uint32_t, uint32_t, wl_output_transform) {
        return {};
    }

    static bool needsRotation(wl_output_transform transform) {
        return transform != WL_OUTPUT_TRANSFORM_NORMAL;
    }

    static void getRotatedDimensions(uint32_t inW, uint32_t inH, wl_output_transform transform, uint32_t& outW, uint32_t& outH) {
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

  private:
    CVulkanTransform()  = default;
    ~CVulkanTransform() = default;
};

inline CVulkanTransform& g_VulkanTransform = CVulkanTransform::instance();

#endif
