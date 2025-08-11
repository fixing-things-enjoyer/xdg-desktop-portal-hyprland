#pragma once

#include "wlr-screencopy-unstable-v1.hpp"
#include "hyprland-toplevel-export-v1.hpp"
#include <sdbus-c++/sdbus-c++.h>
#include "../shared/ScreencopyShared.hpp"
#include "../shared/Session.hpp"
#include "../dbusDefines.hpp"
#include <chrono>
#include "../pipewire/PipewireConnection.hpp"
#include "ScreencopySession.hpp" // Include the new SSession header

class CScreencopyPortal {
  public:
    CScreencopyPortal(SP<CCZwlrScreencopyManagerV1>);

    void   appendToplevelExport(SP<CCHyprlandToplevelExportManagerV1>);

    dbUasv onCreateSession(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts);
    dbUasv onSelectSources(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts);
    dbUasv onStart(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow,
                   std::unordered_map<std::string, sdbus::Variant> opts);

    void                                 startFrameCopy(SSession* pSession);
    void                                 queueNextShareFrame(SSession* pSession);
    bool                                 hasToplevelCapabilities();

    std::unique_ptr<CPipewireConnection> m_pPipewire;

    // Changed from private to public to allow SSession to access it
    struct {
        SP<CCZwlrScreencopyManagerV1>         screencopy = nullptr;
        SP<CCHyprlandToplevelExportManagerV1> toplevel   = nullptr;
    } m_sState;

  private:
    std::unique_ptr<sdbus::IObject>        m_pObject;

    std::vector<std::unique_ptr<SSession>> m_vSessions;

    SSession*                              getSession(sdbus::ObjectPath& path);
    void                                   startSharing(SSession* pSession);

    const sdbus::InterfaceName INTERFACE_NAME = sdbus::InterfaceName{"org.freedesktop.impl.portal.ScreenCast"};
    const sdbus::ObjectPath    OBJECT_PATH    = sdbus::ObjectPath{"/org/freedesktop/portal/desktop"};
};