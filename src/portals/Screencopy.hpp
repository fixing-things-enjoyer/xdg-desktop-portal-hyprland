#pragma once

#include "ext-image-capture-source-v1.hpp"
#include "ext-image-copy-capture-v1.hpp"
#include "wlr-screencopy-unstable-v1.hpp"
#include "hyprland-toplevel-export-v1.hpp"
#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <sdbus-c++/sdbus-c++.h>
#include "../shared/ScreencopyShared.hpp"
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include "../shared/Session.hpp"
#include "../dbusDefines.hpp"
#include <chrono>
#include <array>
#include <vector>

namespace xdph::vulkan {
    class VulkanRotator;
}

enum cursorModes {
    HIDDEN   = 1,
    EMBEDDED = 2,
    METADATA = 4,
};

enum sourceTypes {
    MONITOR = 1,
    WINDOW  = 2,
    VIRTUAL = 4,
};

enum frameStatus {
    FRAME_NONE = 0,
    FRAME_QUEUED,
    FRAME_READY,
    FRAME_FAILED,
    FRAME_RENEG,
};

struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_buffer;

struct SBuffer {
    ~SBuffer();

    bool           isDMABUF = false;
    uint32_t       w = 0, h = 0, fmt = 0;
    int            planeCount = 0;

    int            fd[4] = {-1, -1, -1, -1};
    uint32_t       size[4] = {0, 0, 0, 0}, stride[4] = {0, 0, 0, 0}, offset[4] = {0, 0, 0, 0};
    uint64_t       modifier = DRM_FORMAT_MOD_INVALID;

    gbm_bo*        bo = nullptr;
    void*          mapped = nullptr;
    size_t         mappedSize = 0;
    bool           awaitingRelease = false;

    SP<CCWlBuffer> wlBuffer = nullptr;
    pw_buffer*     pwBuffer = nullptr;
};

class CPipewireConnection;

class CScreencopyPortal {
  public:
    static constexpr size_t ROTATION_CAPTURE_POOL_SIZE = 12;

    CScreencopyPortal(SP<CCZwlrScreencopyManagerV1>);
    ~CScreencopyPortal();

    void   appendToplevelExport(SP<CCHyprlandToplevelExportManagerV1>);

    dbUasv onCreateSession(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts);
    dbUasv onSelectSources(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts);
    dbUasv onStart(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow,
                   std::unordered_map<std::string, sdbus::Variant> opts);

    struct SSession {
        std::string                               appid;
        sdbus::ObjectPath                         requestHandle, sessionHandle;
        uint32_t                                  cursorMode  = HIDDEN;
        uint32_t                                  persistMode = 0;

        std::unique_ptr<SDBusRequest>             request;
        std::unique_ptr<SDBusSession>             session;
        SSelectionData                            selection;
        Hyprutils::Memory::CWeakPointer<SSession> self;

        void                                      startCopy();
        void                                      initCallbacks();

        struct {
            bool                                  active              = false;
            SP<CCZwlrScreencopyFrameV1>           frameCallback       = nullptr;
            SP<CCHyprlandToplevelExportFrameV1>   windowFrameCallback = nullptr;
            frameStatus                           status              = FRAME_NONE;
            bool                                  rotationReady       = false;
            bool                                  rotationRequested   = true;
            uint64_t                              tvSec               = 0;
            uint32_t                              tvNsec              = 0;
            uint64_t                              tvTimestampNs       = 0;
            uint32_t                              nodeID              = 0;
            uint32_t                              framerate           = 60;
            wl_output_transform                   transform           = WL_OUTPUT_TRANSFORM_NORMAL;
            wl_output_transform                   lastTransform       = WL_OUTPUT_TRANSFORM_NORMAL;
            std::chrono::system_clock::time_point begunFrame          = std::chrono::system_clock::now();
            uint32_t                              copyRetries         = 0;
            uint32_t                              idleFrameStreak     = 0;

            struct {
                uint32_t w = 0, h = 0, size = 0, stride = 0, fmt = 0;
            } frameInfoSHM;

            struct {
                uint32_t w = 0, h = 0, fmt = 0;
            } frameInfoDMA;

            struct {
                uint32_t x = 0, y = 0, w = 0, h = 0;
            } damage[4];
            uint32_t                              damageCount = 0;
            std::array<std::unique_ptr<SBuffer>, ROTATION_CAPTURE_POOL_SIZE> rotationCaptureBuffers = {};
            size_t                                rotationCaptureBufferIndex = 0;
            size_t                                rotationCaptureBufferActiveIndex = 0;
            SP<CCExtImageCaptureSourceV1>        extImageSource = nullptr;
            SP<CCExtImageCopyCaptureSessionV1>   extImageCopySession = nullptr;
            SP<CCExtImageCopyCaptureFrameV1>     extImageCopyFrame = nullptr;
            bool                                 extImageCopyConstraintsReady = false;
            uint32_t                             extImageCopyBufferW = 0;
            uint32_t                             extImageCopyBufferH = 0;
            uint32_t                             extImageCopyDMAFormat = DRM_FORMAT_INVALID;
            uint64_t                             extImageCopyDMAModifier = DRM_FORMAT_MOD_INVALID;
            std::vector<uint32_t>               extImageCopySHMFormats;
            std::vector<std::pair<uint32_t, uint64_t>> extImageCopyDMAFormats;
        } sharingData;

        void onCloseRequest(sdbus::MethodCall&);
        void onCloseSession(sdbus::MethodCall&);
    };

    void                                         startFrameCopy(SSession* pSession);
    void                                         queueNextShareFrame(SSession* pSession);
    bool                                         hasToplevelCapabilities();
    bool                                         shouldUseExtImageCopy(SSession* pSession);
    bool                                         shouldApplyGpuRotation(SSession* pSession);
    std::pair<uint32_t, uint32_t>                getStreamDimensions(SSession* pSession, uint32_t physicalW, uint32_t physicalH);

    std::unique_ptr<CPipewireConnection>         m_pPipewire;
    std::unique_ptr<xdph::vulkan::VulkanRotator> m_pRotator;

    xdph::vulkan::VulkanRotator*                 getRotator();

  private:
    std::unique_ptr<sdbus::IObject>                          m_pObject;

    std::vector<Hyprutils::Memory::CUniquePointer<SSession>> m_vSessions;

    SSession*                                                getSession(sdbus::ObjectPath& path);
    void                                                     startSharing(SSession* pSession);
    bool                                                     ensureExtImageCopySession(SSession* pSession);
    void                                                     destroyExtImageCopySession(SSession* pSession);

    struct {
        bool                                  valid      = false;
        std::string                           appid;
        uint32_t                              cursorMode = HIDDEN;
        SSelectionData                        selection;
        std::chrono::system_clock::time_point issuedAt   = std::chrono::system_clock::time_point{};
    } m_sRecentSelection;

    struct {
        SP<CCZwlrScreencopyManagerV1>         screencopy = nullptr;
        SP<CCHyprlandToplevelExportManagerV1> toplevel   = nullptr;
    } m_sState;

    const sdbus::InterfaceName INTERFACE_NAME = sdbus::InterfaceName{"org.freedesktop.impl.portal.ScreenCast"};
    const sdbus::ObjectPath    OBJECT_PATH    = sdbus::ObjectPath{"/org/freedesktop/portal/desktop"};

    friend struct SSession;
};

class CPipewireConnection {
  public:
    CPipewireConnection();
    ~CPipewireConnection();

    bool good();

    void createStream(CScreencopyPortal::SSession* pSession);
    void destroyStream(CScreencopyPortal::SSession* pSession);

    void enqueue(CScreencopyPortal::SSession* pSession);
    void dequeue(CScreencopyPortal::SSession* pSession);

    struct SPWStream {
        CScreencopyPortal::SSession*          pSession    = nullptr;
        pw_stream*                            stream      = nullptr;
        bool                                  streamState = false;
        spa_hook                              streamListener;
        SBuffer*                              currentPWBuffer = nullptr;
        spa_video_info_raw                    pwVideoInfo;
        uint32_t                              seq   = 0;
        bool                                  isDMA = false;

        std::vector<std::unique_ptr<SBuffer>> buffers;
    };

    std::unique_ptr<SBuffer> createBuffer(SPWStream* pStream, bool dmabuf, bool useStreamDimensions = true);
    SBuffer*                 ensureSessionCaptureBuffer(SPWStream* pStream);
    SPWStream*               streamFromSession(CScreencopyPortal::SSession* pSession);
    void                     removeSessionFrameCallbacks(CScreencopyPortal::SSession* pSession);
    uint32_t                 buildFormatsFor(spa_pod_builder* b[2], const spa_pod* params[2], SPWStream* stream);
    void                     updateStreamParam(SPWStream* pStream);

  private:
    std::vector<std::unique_ptr<SPWStream>> m_vStreams;

    bool                                    buildModListFor(SPWStream* stream, uint32_t drmFmt, uint64_t** mods, uint32_t* modCount);

    pw_context*                             m_pContext = nullptr;
    pw_core*                                m_pCore    = nullptr;
};
