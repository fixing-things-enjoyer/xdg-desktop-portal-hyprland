#pragma once

#include "../includes.hpp" // Moved to top

#include <sdbus-c++/sdbus-c++.h>
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

#include "../helpers/Log.hpp"
#include "../helpers/Timer.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../dbusDefines.hpp"

// Removed: #include "../portals/Screencopy.hpp"
// Removed: #include "../portals/GlobalShortcuts.hpp"
// Removed: #include "../portals/Screenshot.hpp"
// Removed: #include "../portals/ScreencopyPicker.hpp"

#include "../protocols/linux-dmabuf-v1.hpp" // Added for CCZwpLinuxDmabufV1 and CCZwpLinuxDmabufFeedbackV1
#include "../protocols/hyprland-toplevel-mapping-v1.hpp" // Added for CCHyprlandToplevelMappingManagerV1

#include <hyprlang.hpp>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-util.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <pipewire/pipewire.h>

#include "wayland.hpp" // Explicitly included for CCWlOutput, CCWlRegistry, CCWlShm

// Forward declaration for CEventLoopManager
class CEventLoopManager;

// Forward declarations for Toplevel Managers
class CToplevelManager;
class CToplevelMappingManager;

// Forward declarations for Portals
class CScreencopyPortal;
class CGlobalShortcutsPortal;
class CScreenshotPortal;
class CScreencopyPicker;

struct SOutput {
    SOutput(SP<CCWlOutput> output_);

    SP<CCWlOutput>    output;
    std::string       name;
    uint32_t          id = 0;
    int               refreshRate = 0;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
};

// Definition for SDMABUFModifier
struct SDMABUFModifier {
    uint32_t fourcc;
    uint64_t mod;
};

class CPortalManager {
  public:
    CPortalManager();
    ~CPortalManager(); // Declared destructor

    void init();
    void terminate();

    sdbus::IConnection* getConnection();
    SOutput* getOutputFromName(const std::string& name);
    gbm_device* createGBMDevice(drmDevice* dev);
    void addTimer(const CTimer& timer);

    // Public members for CEventLoopManager access
    std::unique_ptr<sdbus::IConnection> m_pConnection;
    struct {
        SP<CCWlRegistry> registry;
        SP<CCWlShm> shm;
        SP<CCZwpLinuxDmabufV1> linuxDmabuf;
        SP<CCZwpLinuxDmabufFeedbackV1> linuxDmabufFeedback;
        SP<CCHyprlandToplevelExportManagerV1> hyprlandToplevelMgr;
        wl_display* display;
        gbm_device* gbmDevice = nullptr;
        struct {
            void* formatTable = nullptr;
            uint32_t formatTableSize = 0;
            bool done = false;
            bool deviceUsed = false;
        } dma;
    } m_sWaylandConnection;

    struct {
        pw_loop* loop = nullptr;
    } m_sPipewire;

    struct {
        std::unique_ptr<CScreencopyPortal> screencopy;
        std::unique_ptr<CGlobalShortcutsPortal> globalShortcuts;
        std::unique_ptr<CScreenshotPortal> screenshot;
        std::unique_ptr<CScreencopyPicker> screencopyPicker; // Added for CScreencopyPicker
    } m_sPortals;

    struct {
        std::unique_ptr<CToplevelManager> toplevel;
        std::unique_ptr<CToplevelMappingManager> toplevelMapping;
    } m_sHelpers;

    struct {
        std::unique_ptr<Hyprlang::CConfig> config;
    } m_sConfig;

    std::vector<std::unique_ptr<SOutput>> m_vOutputs;
    std::vector<SDMABUFModifier> m_vDMABUFMods;

    bool m_bTerminate = false;
    pid_t m_iPID = 0;

  private:
    void onGlobal(uint32_t name, const char* interface, uint32_t version);
    void onGlobalRemoved(uint32_t name);

    std::unique_ptr<CEventLoopManager> m_pEventLoopManager;
};

inline std::unique_ptr<CPortalManager> g_pPortalManager;