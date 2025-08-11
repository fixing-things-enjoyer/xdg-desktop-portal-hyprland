#include "ScreencopyPicker.hpp"
#include "../helpers/MiscFunctions.hpp"
#include <wayland-client.h>
#include "../helpers/Log.hpp"
#include <libdrm/drm_fourcc.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <hyprutils/os/Process.hpp>
using namespace Hyprutils::OS;

#include "../shared/ScreencopyShared.hpp" // Added for SSelectionData definition
#include "../portals/Screencopy.hpp" // Added for CScreencopyPortal definition

CScreencopyPicker::CScreencopyPicker(CPortalManager* pPortalManager) : m_pPortalManager(pPortalManager) {
    // Constructor logic if any
}

std::string CScreencopyPicker::sanitizeNameForWindowList(const std::string& name) {
    std::string result = name;
    std::replace(result.begin(), result.end(), '\'', ' ');
    std::replace(result.begin(), result.end(), '"', ' ');
    std::replace(result.begin(), result.end(), '$', ' ');
    std::replace(result.begin(), result.end(), '`', ' ');
    for (size_t i = 1; i < result.size(); ++i) {
        if (result[i - 1] == '>' && result[i] == ']')
            result[i] = ' ';
    }
    return result;
}

std::string CScreencopyPicker::buildWindowList() {
    std::string result = "";
    if (!m_pPortalManager->m_sPortals.screencopy->hasToplevelCapabilities())
        return result;

    for (auto& e : m_pPortalManager->m_sHelpers.toplevel->m_vToplevels) {
        result += std::format("{}[HC>]{}[HT>]{}[HE>]{}[HA>]", (uint32_t)(((uint64_t)e->handle->resource()) & 0xFFFFFFFF), sanitizeNameForWindowList(e->windowClass),
                              sanitizeNameForWindowList(e->windowTitle),
                              m_pPortalManager->m_sHelpers.toplevelMapping ? m_pPortalManager->m_sHelpers.toplevelMapping->getWindowForToplevel(e->handle) : 0);
    }

    return result;
}

SSelectionData CScreencopyPicker::promptForScreencopySelection() {
    SSelectionData      data;

    const char*         WAYLAND_DISPLAY             = getenv("WAYLAND_DISPLAY");
    const char*         XCURSOR_SIZE                = getenv("XCURSOR_SIZE");
    const char*         HYPRLAND_INSTANCE_SIGNATURE = getenv("HYPRLAND_INSTANCE_SIGNATURE");

    static auto* const* PALLOWTOKENBYDEFAULT =
        (Hyprlang::INT* const*)m_pPortalManager->m_sConfig.config->getConfigValuePtr("screencopy:allow_token_by_default")->getDataStaticPtr();
    static auto* const*      PCUSTOMPICKER = (Hyprlang::STRING* const)m_pPortalManager->m_sConfig.config->getConfigValuePtr("screencopy:custom_picker_binary")->getDataStaticPtr();

    std::vector<std::string> args;
    if (**PALLOWTOKENBYDEFAULT)
        args.emplace_back("--allow-token");

    CProcess proc(std::string{*PCUSTOMPICKER}.empty() ? "hyprland-share-picker" : *PCUSTOMPICKER, args);
    proc.addEnv("WAYLAND_DISPLAY", WAYLAND_DISPLAY ? WAYLAND_DISPLAY : "");
    proc.addEnv("QT_QPA_PLATFORM", "wayland");
    proc.addEnv("XCURSOR_SIZE", XCURSOR_SIZE ? XCURSOR_SIZE : "24");
    proc.addEnv("HYPRLAND_INSTANCE_SIGNATURE", HYPRLAND_INSTANCE_SIGNATURE ? HYPRLAND_INSTANCE_SIGNATURE : "0");
    proc.addEnv("XDPH_WINDOW_SHARING_LIST", buildWindowList()); // buildWindowList will sanitize any shell stuff in case the picker (qt) does something funky? It shouldn't.

    if (!proc.runSync())
        return data;

    const auto RETVAL    = proc.stdOut();
    const auto RETVALERR = proc.stdErr();

    if (!RETVAL.contains("[SELECTION]")) {
        // failed
        constexpr const char* QPA_ERR = "qt.qpa.plugin: Could not find the Qt platform plugin";

        if (RETVAL.contains(QPA_ERR) || RETVALERR.contains(QPA_ERR)) {
            // prompt the user to install qt5-wayland and qt6-wayland
            addHyprlandNotification("3", 7000, "0", "[xdph] Could not open the picker: qt5-wayland or qt6-wayland doesn't seem to be installed.");
        }

        return data;
    }

    const auto SELECTION = RETVAL.substr(RETVAL.find("[SELECTION]") + 11);

    Debug::log(LOG, "[sc] Selection: {}", SELECTION);

    const auto FLAGS = SELECTION.substr(0, SELECTION.find_first_of('/'));
    const auto SEL   = SELECTION.substr(SELECTION.find_first_of('/') + 1);

    for (auto& flag : FLAGS) {
        if (flag == 'r') {
            data.allowToken = true;
        } else if (flag == 't') {
            data.needsTransform = true;
        } else
            Debug::log(LOG, "[screencopy] unknown flag from share-picker: {}", flag);
    }

    if (SEL.find("screen:") == 0) {
        data.type   = TYPE_OUTPUT;
        data.output = SEL.substr(7);

        data.output.pop_back();
    } else if (SEL.find("window:") == 0) {
        data.type         = TYPE_WINDOW;
        uint32_t handleLo = std::stoull(SEL.substr(7));
        data.windowHandle = nullptr;

        const auto HANDLE = m_pPortalManager->m_sHelpers.toplevel->handleFromHandleLower(handleLo);
        if (HANDLE) {
            data.windowHandle = HANDLE->handle;
            data.windowClass  = HANDLE->windowClass;
        }

        if (data.needsTransform)
            Debug::log(WARN, "[screencopy] transform forced on a window. This is not supported and will be ignored.");
    } else if (SEL.find("region:") == 0) {
        std::string running = SEL;
        running             = running.substr(7);
        data.type           = TYPE_GEOMETRY;
        data.output         = running.substr(0, running.find_first_of('@'));
        running             = running.substr(running.find_first_of('@') + 1);

        data.x  = std::stoi(running.substr(0, running.find_first_of(',')));
        running = running.substr(running.find_first_of(',') + 1);
        data.y  = std::stoi(running.substr(0, running.find_first_of(',')));
        running = running.substr(running.find_first_of(',') + 1);
        data.w  = std::stoi(running.substr(0, running.find_first_of(',')));
        running = running.substr(running.find_first_of(',') + 1);
        data.h  = std::stoi(running);
    }

    return data;
}
