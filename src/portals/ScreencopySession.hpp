#pragma once

#include <sdbus-c++/sdbus-c++.h>
#include <mutex>
#include <condition_variable>
#include "../shared/ScreencopyShared.hpp"
#include "../dbusDefines.hpp"
#include <chrono>
#include "wlr-screencopy-unstable-v1.hpp"
#include "hyprland-toplevel-export-v1.hpp"
#include "../shared/Session.hpp" // Explicitly include Session.hpp for SDBusRequest and SDBusSession

// Forward declaration for CScreencopyPortal if needed within SSession
class CScreencopyPortal;

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

struct SSession {
    std::string                   appid;
    sdbus::ObjectPath             requestHandle, sessionHandle;
    uint32_t                      cursorMode  = HIDDEN;
    uint32_t                      persistMode = 0;

    std::unique_ptr<SDBusRequest> request;
    std::unique_ptr<SDBusSession> session;
    SSelectionData                selection;

    std::mutex              start_reply_mutex;
     
    
    bool                    stream_ready = false;
    bool                    m_bStreamActive = true;

    void                          startCopy();
    void                          initCallbacks();
    void                          getLogicalDimensions(int& width, int& height);
    void                          getTargetDimensions(int& width, int& height);

    ~SSession(); // Destructor declaration

    struct {
        bool                                  active              = false;
        SP<CCZwlrScreencopyFrameV1>           frameCallback       = nullptr;
        SP<CCHyprlandToplevelExportFrameV1>   windowFrameCallback = nullptr;
        frameStatus                           status              = FRAME_NONE;
        uint64_t                              tvSec               = 0;
        uint32_t                              tvNsec              = 0;
        uint64_t                              tvTimestampNs       = 0;
        uint32_t                              nodeID              = 0;
        uint32_t                              framerate           = 60;
        wl_output_transform                   transform           = WL_OUTPUT_TRANSFORM_NORMAL;
        std::chrono::system_clock::time_point begunFrame          = std::chrono::system_clock::now();
        uint32_t                              copyRetries         = 0;

        // This is our private buffer for the compositor
        SP<CCWlBuffer>  compositor_wl_buffer = nullptr;
        gbm_bo*         compositor_gbm_bo    = nullptr;

        struct {
            uint32_t w = 0, h = 0, size = 0, stride = 0, fmt = 0;
        } frameInfoSHM;

        struct {
            uint32_t w = 0, h = 0, fmt = 0;
        } frameInfoDMA;

        struct {
            uint32_t x = 0, y = 0, w = 0, h = 0;
        } damage[4];
        uint32_t damageCount = 0;
    } sharingData;

    void onCloseRequest(sdbus::MethodCall&);
    void onCloseSession(sdbus::MethodCall&);
};
