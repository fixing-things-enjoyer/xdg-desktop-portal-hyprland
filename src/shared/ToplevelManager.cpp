#include "ToplevelManager.hpp"
#include "../helpers/Log.hpp"
#include "../core/PortalManager.hpp"
#include "../shared/ToplevelMappingManager.hpp" // Added for CToplevelMappingManager

SToplevelHandle::SToplevelHandle(SP<CCZwlrForeignToplevelHandleV1> handle_) : handle(handle_) {
    handle->setTitle([this](CCZwlrForeignToplevelHandleV1* r, const char* title) { windowTitle = title; });
    handle->setAppId([this](CCZwlrForeignToplevelHandleV1* r, const char* appid) { windowClass = appid; });
    handle->setClosed([this](CCZwlrForeignToplevelHandleV1* r) {
        Debug::log(LOG, "[toplevel] Toplevel {} closed", windowTitle);
        std::erase_if(mgr->m_vToplevels, [&](const auto& other) { return other->handle == handle; });
        if (g_pPortalManager->m_sHelpers.toplevelMapping)
            g_pPortalManager->m_sHelpers.toplevelMapping->m_muAddresses.erase(this->handle);
    });
}

CToplevelManager::CToplevelManager(uint32_t name, uint32_t version) {
    m_pManager = makeShared<CCZwlrForeignToplevelManagerV1>(
        (wl_proxy*)wl_registry_bind((wl_registry*)g_pPortalManager->m_sWaylandConnection.registry->resource(), name, &zwlr_foreign_toplevel_manager_v1_interface, version));

    m_pManager->setToplevel([this](CCZwlrForeignToplevelManagerV1* r, wl_proxy* toplevel) { // Corrected wl_resource* to wl_proxy*
        const auto HANDLE = m_vToplevels.emplace_back(makeShared<SToplevelHandle>(makeShared<CCZwlrForeignToplevelHandleV1>(toplevel))); // Corrected
        HANDLE->mgr       = this;
        if (g_pPortalManager->m_sHelpers.toplevelMapping)
            g_pPortalManager->m_sHelpers.toplevelMapping->fetchWindowForToplevel(HANDLE->handle);
    });

    m_pManager->setFinished([this](CCZwlrForeignToplevelManagerV1* r) {
        Debug::log(LOG, "[toplevel] Toplevel manager finished");
        m_vToplevels.clear();
        if (g_pPortalManager->m_sHelpers.toplevelMapping)
            g_pPortalManager->m_sHelpers.toplevelMapping->m_muAddresses.clear();
    });

    m_sWaylandConnection.name    = name;
    m_sWaylandConnection.version = version;
}

void CToplevelManager::activate() {
    if (m_iActivateLocks++ == 0) {
        Debug::log(LOG, "[toplevel] Toplevel manager activated");
        // no-op
    }
}

void CToplevelManager::deactivate() {
    if (--m_iActivateLocks == 0) {
        Debug::log(LOG, "[toplevel] Toplevel manager deactivated");
        m_vToplevels.clear();
        if (g_pPortalManager->m_sHelpers.toplevelMapping)
            g_pPortalManager->m_sHelpers.toplevelMapping->m_muAddresses.clear();
    }
}

SP<SToplevelHandle> CToplevelManager::handleFromClass(const std::string& windowClass) {
    for (auto& t : m_vToplevels) {
        if (t->windowClass == windowClass)
            return t;
    }

    return nullptr;
}

SP<SToplevelHandle> CToplevelManager::handleFromHandleLower(uint32_t handle) {
    for (auto& t : m_vToplevels) {
        if (((uint64_t)t->handle->resource() & 0xFFFFFFFF) == handle)
            return t;
    }

    return nullptr;
}

SP<SToplevelHandle> CToplevelManager::handleFromHandleFull(uint64_t handle) {
    for (auto& t : m_vToplevels) {
        if (((uint64_t)t->handle->resource()) == handle)
            return t;
    }

    return nullptr;
}