#include "Screencopy.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "ScreencopySession.hpp" // Include the new SSession header
#include "../render/Renderer.hpp" // Include for CRenderer

#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include "linux-dmabuf-v1.hpp"
#include <unistd.h>

#include "../shared/ToplevelManager.hpp" // Added for CToplevelManager
#include "../shared/ToplevelMappingManager.hpp" // Added for CToplevelMappingManager
#include "../portals/ScreencopyPicker.hpp" // Added for CScreencopyPicker

constexpr static int MAX_RETRIES = 10;

//
sdbus::Struct<std::string, uint32_t, sdbus::Variant> CScreencopyPortal::getFullRestoreStruct(const SSelectionData& data, uint32_t cursor) {
    std::unordered_map<std::string, sdbus::Variant> mapData;

    switch (data.type) {
        case TYPE_GEOMETRY:
        case TYPE_OUTPUT: mapData["output"] = sdbus::Variant{data.output}; break;
        case TYPE_WINDOW:
            mapData["windowHandle"] = sdbus::Variant{(uint64_t)data.windowHandle->resource()};
            mapData["windowClass"]  = sdbus::Variant{data.windowClass};
            break;
        default: Debug::log(ERR, "[screencopy] wonk selection in token saving"); break;
    }
    mapData["timeIssued"] = sdbus::Variant{uint64_t(time(nullptr))};
    mapData["token"]      = sdbus::Variant{std::string("todo")};
    mapData["withCursor"] = sdbus::Variant{cursor};

    sdbus::Variant restoreData{mapData};

    return sdbus::Struct<std::string, uint32_t, sdbus::Variant>{"hyprland", 3, restoreData};
}

dbUasv CScreencopyPortal::onCreateSession(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                                          std::unordered_map<std::string, sdbus::Variant> opts) {
    g_pPortalManager->m_sHelpers.toplevel->activate();

    Debug::log(LOG, "[screencopy] New session:");
    Debug::log(LOG, "[screencopy]  | {}", requestHandle.c_str());
    Debug::log(LOG, "[screencopy]  | {}", sessionHandle.c_str());
    Debug::log(LOG, "[screencopy]  | appid: {}", appID);

    const auto PSESSION = m_vSessions.emplace_back(std::make_unique<SSession>(appID, requestHandle, sessionHandle)).get();

    // create objects
    PSESSION->session            = createDBusSession(sessionHandle);
    PSESSION->session->onDestroy = [PSESSION, this]() {
        if (PSESSION->sharingData.active) {
            m_pPipewire->destroyStream(PSESSION);
            Debug::log(LOG, "[screencopy] Stream destroyed");
        }
        if (PSESSION->sharingData.compositor_gbm_bo) {
            // The wl_buffer is a smart pointer and will be destroyed automatically.
            // We just need to destroy the gbm_bo.
            gbm_bo_destroy(PSESSION->sharingData.compositor_gbm_bo);
            PSESSION->sharingData.compositor_gbm_bo = nullptr;
        }
        PSESSION->session.release();
        Debug::log(LOG, "[screencopy] Session destroyed");

        // deactivate toplevel so it doesn't listen and waste battery
        g_pPortalManager->m_sHelpers.toplevel->deactivate();
    };
    PSESSION->request            = createDBusRequest(requestHandle);
    PSESSION->request->onDestroy = [PSESSION]() { PSESSION->request.release(); };

    return {0, {}};
}

dbUasv CScreencopyPortal::onSelectSources(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                                          std::unordered_map<std::string, sdbus::Variant> options) {
    Debug::log(LOG, "[screencopy] SelectSources:");
    Debug::log(LOG, "[screencopy]  | {}", requestHandle.c_str());
    Debug::log(LOG, "[screencopy]  | {}", sessionHandle.c_str());
    Debug::log(LOG, "[screencopy]  | appid: {}", appID);

    const auto PSESSION = getSession(sessionHandle);

    if (!PSESSION) {
        Debug::log(ERR, "[screencopy] SelectSources: no session found??");
        throw sdbus::Error{sdbus::Error::Name{"NOSESSION"}, "No session found"};
        return {1, {}};
    }

    struct {
        bool        exists = false;
        std::string token, output;
        uint64_t    windowHandle;
        bool        withCursor;
        uint64_t    timeIssued;
        std::string windowClass;
    } restoreData;

    for (auto& [key, val] : options) {

        if (key == "cursor_mode") {
            PSESSION->cursorMode = val.get<uint32_t>();
            Debug::log(LOG, "[screencopy] option cursor_mode to {}", PSESSION->cursorMode);
        } else if (key == "restore_data") {
            // suv
            // v -> r(susbt) -> v2
            // v -> a(sv) -> v3
            std::string issuer;
            uint32_t    version;
            auto        suv = val.get<sdbus::Struct<std::string, uint32_t, sdbus::Variant>>();
            issuer          = suv.get<0>();
            version         = suv.get<1>();

            sdbus::Variant data = suv.get<2>();

            if (issuer != "hyprland") {
                Debug::log(LOG, "[screencopy] Restore token from {}, ignoring", issuer);
                continue;
            }

            Debug::log(LOG, "[screencopy] Restore token from {} ver {}", issuer, version);

            if (version != 2 && version != 3) {
                Debug::log(LOG, "[screencopy] Restore token ver unsupported, skipping", issuer);
                continue;
            }

            if (version == 2) {
                auto susbt = data.get<sdbus::Struct<std::string, uint32_t, std::string, bool, uint64_t>>();

                restoreData.exists = true;

                restoreData.token        = susbt.get<0>();
                restoreData.windowHandle = susbt.get<1>();
                restoreData.output       = susbt.get<2>();
                restoreData.withCursor   = susbt.get<3>();
                restoreData.timeIssued   = susbt.get<4>();

                Debug::log(LOG, "[screencopy] Restore token v2 {} with data: {} {} {} {}", restoreData.token, restoreData.windowHandle, restoreData.output, restoreData.withCursor,
                           restoreData.timeIssued);
            } else {
                // ver 3
                auto sv = data.get<std::unordered_map<std::string, sdbus::Variant>>();

                restoreData.exists = true;

                for (auto& [tkkey, tkval] : sv) {
                    if (tkkey == "output")
                        restoreData.output = tkval.get<std::string>();
                    else if (tkkey == "windowHandle")
                        restoreData.windowHandle = tkval.get<uint64_t>();
                    else if (tkkey == "windowClass")
                        restoreData.windowClass = tkval.get<std::string>();
                    else if (tkkey == "withCursor")
                        restoreData.withCursor = (bool)tkval.get<uint32_t>();
                    else if (tkkey == "timeIssued")
                        restoreData.timeIssued = tkval.get<uint64_t>();
                    else if (tkkey == "token")
                        restoreData.token = tkval.get<std::string>();
                    else
                        Debug::log(LOG, "[screencopy] restore token v3, unknown prop {}", tkkey);
                }

                Debug::log(LOG, "[screencopy] Restore token v3 {} with data: {} {} {} {} {}", restoreData.token, restoreData.windowHandle, restoreData.windowClass,
                           restoreData.output, restoreData.withCursor, restoreData.timeIssued);
            }

        } else if (key == "persist_mode") {
            PSESSION->persistMode = val.get<uint32_t>();
            Debug::log(LOG, "[screencopy] option persist_mode to {}", PSESSION->persistMode);
        } else {
            Debug::log(LOG, "[screencopy] unused option {}", key);
        }
    }

    // clang-format off
    const bool     RESTOREDATAVALID = restoreData.exists &&
    (
        (!restoreData.output.empty() && g_pPortalManager->m_sHelpers.toplevel->handleFromClass(restoreData.output)) || // output exists
        (!restoreData.windowClass.empty() && g_pPortalManager->m_sHelpers.toplevel->handleFromClass(restoreData.windowClass)) // window exists
    );
    // clang-format on

    SSelectionData SHAREDATA;
    if (RESTOREDATAVALID) {
        Debug::log(LOG, "[screencopy] restore data valid, not prompting");

        const bool WINDOW      = !restoreData.windowClass.empty();
        const auto HANDLEMATCH = WINDOW && restoreData.windowHandle != 0 ? g_pPortalManager->m_sHelpers.toplevel->handleFromHandleFull(restoreData.windowHandle) : nullptr;

        SHAREDATA.output       = restoreData.output;
        SHAREDATA.type         = WINDOW ? TYPE_WINDOW : TYPE_OUTPUT;
        SHAREDATA.windowHandle = WINDOW ? (HANDLEMATCH ? HANDLEMATCH->handle : g_pPortalManager->m_sHelpers.toplevel->handleFromClass(restoreData.windowClass)->handle) : nullptr;
        SHAREDATA.windowClass  = restoreData.windowClass;
        SHAREDATA.allowToken   = true; // user allowed token before
        PSESSION->cursorMode   = restoreData.withCursor;
    } else {
        Debug::log(LOG, "[screencopy] restore data invalid / missing, prompting");

        SHAREDATA = g_pPortalManager->m_sPortals.screencopyPicker->promptForScreencopySelection(); // Corrected call
    }

    Debug::log(LOG, "[screencopy] SHAREDATA returned selection type: {}, needsTransform: {}", (int)SHAREDATA.type, SHAREDATA.needsTransform);

    if (SHAREDATA.type == TYPE_WINDOW && !m_sState.toplevel) {
        Debug::log(ERR, "[screencopy] Requested type window for no toplevel export protocol!");
        SHAREDATA.type = TYPE_INVALID;
    } else if (SHAREDATA.type == TYPE_OUTPUT || SHAREDATA.type == TYPE_GEOMETRY) {
        const auto POUTPUT = g_pPortalManager->getOutputFromName(SHAREDATA.output);

        if (POUTPUT) {
            static auto* const* PFPS = (Hyprlang::INT* const*)g_pPortalManager->m_sConfig.config->getConfigValuePtr("screencopy:max_fps")->getDataStaticPtr();

            if (**PFPS <= 0)
                PSESSION->sharingData.framerate = POUTPUT->refreshRate;
            else
                PSESSION->sharingData.framerate = std::clamp((float)POUTPUT->refreshRate, 1.F, (float)**PFPS);
        }
    }

    PSESSION->selection = SHAREDATA;

    return {SHAREDATA.type == TYPE_INVALID ? 1 : 0, {}};
}

dbUasv CScreencopyPortal::onStart(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow, std::unordered_map<std::string, sdbus::Variant> opts) {
    Debug::log(LOG, "[screencopy] Start:");
    Debug::log(LOG, "[screencopy]  | {}", requestHandle.c_str());
    Debug::log(LOG, "[screencopy]  | {}", sessionHandle.c_str());
    Debug::log(LOG, "[screencopy]  | appid: {}", appID);
    Debug::log(LOG, "[screencopy]  | parent_window: {}", parentWindow);

    const auto PSESSION = getSession(sessionHandle);

    if (!PSESSION) {
        Debug::log(ERR, "[screencopy] Start: no session found??");
        throw sdbus::Error{sdbus::Error::Name{"NOSESSION"}, "No session found"};
        return {1, {}};
    }

    startSharing(PSESSION);

    Debug::log(LOG, "[screencopy] onStart entering active wait for stream to be ready...");
    
    while (true) {
        {
            std::lock_guard lock(PSESSION->start_reply_mutex);
            if (PSESSION->stream_ready) {
                break; // Exit loop if flag is set
            }
        }

        // Manually drive the PipeWire event loop.
        // The timeout of 10ms means we check the flag roughly 100 times per second,
        // while yielding the CPU to avoid a spinlock.
        pw_loop_iterate(g_pPortalManager->m_sPipewire.loop, 10);
    }
    
    Debug::log(LOG, "[screencopy] onStart active wait complete, stream is ready.");
    
    // Build the final reply and return it
    std::unordered_map<std::string, sdbus::Variant> options;

    if (PSESSION->selection.allowToken) {
        options["restore_data"] = sdbus::Variant{getFullRestoreStruct(PSESSION->selection, PSESSION->cursorMode)};
        options["persist_mode"] = sdbus::Variant{uint32_t{2}};
        Debug::log(LOG, "[screencopy] Sent restore token to {}", PSESSION->sessionHandle.c_str());
    }

    uint32_t type = 0;
    switch (PSESSION->selection.type) {
        case TYPE_OUTPUT: type = 1 << MONITOR; break;
        case TYPE_WINDOW: type = 1 << WINDOW; break;
        case TYPE_GEOMETRY:
        case TYPE_WORKSPACE: type = 1 << VIRTUAL; break;
        default: type = 0; break;
    }
    options["source_type"] = sdbus::Variant{type};

    std::vector<sdbus::Struct<uint32_t, std::unordered_map<std::string, sdbus::Variant>>> streams;

    int targetWidth, targetHeight;
    PSESSION->getTargetDimensions(targetWidth, targetHeight);

    std::unordered_map<std::string, sdbus::Variant> streamData;
    streamData["position"]    = sdbus::Variant{sdbus::Struct<int32_t, int32_t>{0, 0}};
    streamData["size"]        = sdbus::Variant{sdbus::Struct<int32_t, int32_t>{targetWidth, targetHeight}};
    streamData["source_type"] = sdbus::Variant{uint32_t{type}};
    streams.emplace_back(sdbus::Struct<uint32_t, std::unordered_map<std::string, sdbus::Variant>>{PSESSION->sharingData.nodeID, streamData});

    options["streams"] = sdbus::Variant{streams};

    return {0, options};
}




void CScreencopyPortal::startSharing(SSession* pSession) {
    pSession->sharingData.active = true;

    startFrameCopy(pSession);

    wl_display_dispatch(g_pPortalManager->m_sWaylandConnection.display);
    wl_display_roundtrip(g_pPortalManager->m_sWaylandConnection.display);

    // This is now handled inside the compositor callback
    // if (pSession->sharingData.frameInfoDMA.fmt == DRM_FORMAT_INVALID) {
    //     Debug::log(ERR, "[screencopy] Couldn't obtain a format from dma");
    //     return;
    // }

    // m_pPipewire->createStream(pSession); // <-- COMMENT OUT OR DELETE THIS

    // The rest of this function will now be triggered by the stream creation itself.
    // while (pSession->sharingData.nodeID == SPA_ID_INVALID) {
    //     int ret = pw_loop_iterate(g_pPortalManager->m_sPipewire.loop, 0);
    //     if (ret < 0) {
    //         Debug::log(ERR, "[pipewire] pw_loop_iterate failed with {}", spa_strerror(ret));
    //         return;
    //     }
    // }

    // Debug::log(LOG, "[screencopy] Sharing initialized");

    // g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(pSession);

    Debug::log(TRACE, "[sc] queued frame in {}ms", 1000.0 / pSession->sharingData.framerate);
}

void CScreencopyPortal::startFrameCopy(SSession* pSession) {
    pSession->startCopy();

    Debug::log(TRACE, "[screencopy] frame callbacks initialized");
}

void SSession::startCopy() {
    if (!m_bStreamActive) {
        Debug::log(TRACE, "[sc] startCopy: stream not active, skipping frame copy.");
        return;
    }

    // --- START: New Guard Clause ---
    const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);
    if (PSTREAM && !PSTREAM->streamState) {
        Debug::log(TRACE, "[sc] startCopy: not copying, stream not active");
        return;
    }
    // --- END: New Guard Clause ---

    const auto POUTPUT = g_pPortalManager->getOutputFromName(selection.output);

    if (!sharingData.active) {
        Debug::log(TRACE, "[sc] startFrameCopy: not copying, inactive session");
        return;
    }

    if (!POUTPUT && (selection.type == TYPE_GEOMETRY || selection.type == TYPE_OUTPUT)) {
        Debug::log(ERR, "[screencopy] Output {} not found??", selection.output);
        return;
    }

    if ((sharingData.frameCallback && (selection.type == TYPE_GEOMETRY || selection.type == TYPE_OUTPUT)) || (sharingData.windowFrameCallback && selection.type == TYPE_WINDOW)) {
        Debug::log(ERR, "[screencopy] tried scheduling on already scheduled cb (type {})", (int)selection.type);
        return;
    }

    

    if (selection.type == TYPE_GEOMETRY && !selection.needsTransform) {
        // This is the original, non-compatibility path for region selection. It remains unchanged.
        sharingData.frameCallback = makeShared<CCZwlrScreencopyFrameV1>(g_pPortalManager->m_sPortals.screencopy->m_sState.screencopy->sendCaptureOutputRegion(
            cursorMode, POUTPUT->output->resource(), selection.x, selection.y, selection.w, selection.h));
        sharingData.transform     = POUTPUT->transform;
        Debug::log(LOG, "[screencopy] Session for output region {} using transform {}", POUTPUT->name, (int)POUTPUT->transform);
    } else if (selection.type == TYPE_OUTPUT || (selection.type == TYPE_GEOMETRY && selection.needsTransform)) {
        // This now handles both full-screen selection and region selection when compatibility mode is on.
        sharingData.frameCallback =
            makeShared<CCZwlrScreencopyFrameV1>(g_pPortalManager->m_sPortals.screencopy->m_sState.screencopy->sendCaptureOutput(cursorMode, POUTPUT->output->resource()));
        sharingData.transform = POUTPUT->transform;
        Debug::log(LOG, "[screencopy] Session for output {} (or region with transform) using transform {}", POUTPUT->name, (int)POUTPUT->transform);
    } else if (selection.type == TYPE_WINDOW) {
        if (!selection.windowHandle) {
            Debug::log(ERR, "[screencopy] selected invalid window?");
            return;
        }
        sharingData.windowFrameCallback = makeShared<CCHyprlandToplevelExportFrameV1>(
            g_pPortalManager->m_sPortals.screencopy->m_sState.toplevel->sendCaptureToplevelWithWlrToplevelHandle(cursorMode, selection.windowHandle->resource()));
        sharingData.transform = WL_OUTPUT_TRANSFORM_NORMAL;
        Debug::log(LOG, "[screencopy] Session for window using transform {}", (int)WL_OUTPUT_TRANSFORM_NORMAL);
    } else {
        Debug::log(ERR, "[screencopy] Unsupported selection {}", (int)selection.type);
        return;
    }

    sharingData.status = FRAME_QUEUED;

    initCallbacks();
}

void SSession::initCallbacks() {
    if (sharingData.frameCallback) {
        sharingData.frameCallback->setBuffer([this](CCZwlrScreencopyFrameV1* r, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
            Debug::log(TRACE, "[sc] wlrOnBuffer for {}", (void*)this);

            sharingData.frameInfoSHM.w      = width;
            sharingData.frameInfoSHM.h      = height;
            sharingData.frameInfoSHM.fmt    = drmFourccFromSHM((wl_shm_format)format);
            sharingData.frameInfoSHM.size   = stride * height;
            sharingData.frameInfoSHM.stride = stride;

            // todo: done if ver < 3
        });
        sharingData.frameCallback->setReady([this](CCZwlrScreencopyFrameV1* r, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
            // v-- ADD THIS LOG --v
            Debug::log(LOG, "[sc] wlrOnReady callback has fired.");
            Debug::log(TRACE, "[sc] wlrOnReady for {}", (void*)this);

            if (selection.needsTransform && g_pPortalManager->m_pRenderer) {
                const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);
                if (PSTREAM && PSTREAM->currentPWBuffer && sharingData.compositor_gbm_bo) {
                    SRenderBox cropBox;
                    getPhysicalCastingBox(&cropBox);
                    
                    Debug::log(LOG, "[render] Executing render pass with crop.");
                    if (!g_pPortalManager->m_pRenderer->render(PSTREAM->currentPWBuffer->bo, sharingData.compositor_gbm_bo, sharingData.transform, &cropBox)) {
                        Debug::log(ERR, "[screencopy] Render failed, skipping frame enqueue.");
                        sharingData.status = FRAME_NONE;
                        g_pPortalManager->addTimer({100.0, [this]() { g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this); }});
                        return;
                    }
                }
            }

            sharingData.status = FRAME_READY;

            sharingData.tvSec         = ((((uint64_t)tv_sec_hi) << 32) + (uint64_t)tv_sec_lo);
            sharingData.tvNsec        = tv_nsec;
            sharingData.tvTimestampNs = sharingData.tvSec * SPA_NSEC_PER_SEC + sharingData.tvNsec;

            Debug::log(TRACE, "[sc] frame timestamp sec: {} nsec: {} combined: {}ns", sharingData.tvSec, sharingData.tvNsec, sharingData.tvTimestampNs);

            // v-- ADD THIS LOG --v
            Debug::log(LOG, "[sc] Enqueuing frame to PipeWire.");
            g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);

            if (g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this))
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);

            sharingData.frameCallback.reset();
        });
        sharingData.frameCallback->setFailed([this](CCZwlrScreencopyFrameV1* r) {
            Debug::log(TRACE, "[sc] wlrOnFailed for {}", (void*)this);
            sharingData.status = FRAME_FAILED;
        });
        sharingData.frameCallback->setDamage([this](CCZwlrScreencopyFrameV1* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] wlrOnDamage for {}", (void*)this);

            if (sharingData.damageCount > 3) {
                sharingData.damage[0] = {0, 0, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h};
                return;
            }

            sharingData.damage[sharingData.damageCount++] = {x, y, width, height};

            Debug::log(TRACE, "[sc] wlr damage: {} {} {} {}", x, y, width, height);
        });
        sharingData.frameCallback->setLinuxDmabuf([this](CCZwlrScreencopyFrameV1* r, uint32_t format, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] wlrOnDmabuf for {}", (void*)this);

            sharingData.frameInfoDMA.w   = width;
            sharingData.frameInfoDMA.h   = height;
            sharingData.frameInfoDMA.fmt = format;
            
            Debug::log(LOG, "[sc-diag] Compositor reported DMA info: w={}, h={}, transform={}", sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h, (int)sharingData.transform);
        });
        sharingData.frameCallback->setBufferDone([this](CCZwlrScreencopyFrameV1* r) {
            Debug::log(TRACE, "[sc] wlrOnBufferDone for {}", (void*)this);

            // v-- ADD THIS BLOCK --v
            // Initialize renderer if it doesn't exist
            if (!g_pPortalManager->m_pRenderer) {
                if (g_pPortalManager->m_sWaylandConnection.gbmDevice) {
                    g_pPortalManager->m_pRenderer = std::make_unique<CRenderer>();
                    if (!g_pPortalManager->m_pRenderer->m_bGood) {
                        Debug::log(WARN, "[core] Failed to initialize renderer. Transform will not work.");
                        g_pPortalManager->m_pRenderer.reset();
                    }
                } else {
                    Debug::log(WARN, "[core] No GBM device, cannot initialize renderer. Transform will not work.");
                }
            }
            // ^-- ADD THIS BLOCK --^

            if (selection.needsTransform && !sharingData.compositor_gbm_bo) {
                // Use the compositor's reported native dimensions directly
                int sourceWidth = sharingData.frameInfoDMA.w;
                int sourceHeight = sharingData.frameInfoDMA.h;

                uint32_t sourceFormat = sharingData.frameInfoDMA.fmt;
                Debug::log(LOG, "[screencopy] Attempting to create NATIVE-sized GBM BO with: w={}, h={}, format={}", sourceWidth, sourceHeight, sourceFormat);

                sharingData.compositor_gbm_bo = gbm_bo_create(g_pPortalManager->m_sWaylandConnection.gbmDevice, sourceWidth, sourceHeight, sourceFormat, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
                if (!sharingData.compositor_gbm_bo) { Debug::log(ERR, "[screencopy] Failed to create dedicated compositor GBM buffer."); return; }
                
                if (!g_pPortalManager->m_sWaylandConnection.linuxDmabuf) { Debug::log(ERR, "[screencopy] zwp_linux_dmabuf_v1 protocol not available, cannot import buffer."); gbm_bo_destroy(sharingData.compositor_gbm_bo); sharingData.compositor_gbm_bo = nullptr; return; }

                auto params = makeShared<CCZwpLinuxBufferParamsV1>(g_pPortalManager->m_sWaylandConnection.linuxDmabuf->sendCreateParams());
                if (!params) { Debug::log(ERR, "[screencopy] zwp_linux_dmabuf_v1_create_params failed for compositor buffer."); gbm_bo_destroy(sharingData.compositor_gbm_bo); sharingData.compositor_gbm_bo = nullptr; return; }
                
                uint64_t modifier = gbm_bo_get_modifier(sharingData.compositor_gbm_bo);
                params->sendAdd(gbm_bo_get_fd(sharingData.compositor_gbm_bo), 0, 0, gbm_bo_get_stride(sharingData.compositor_gbm_bo), modifier >> 32, modifier & 0xffffffff);
                sharingData.compositor_wl_buffer = makeShared<CCWlBuffer>(params->sendCreateImmed(sourceWidth, sourceHeight, sourceFormat, (zwpLinuxBufferParamsV1Flags)0));
                if (!sharingData.compositor_wl_buffer) { Debug::log(ERR, "[screencopy] Failed to create dedicated compositor wl_buffer via dmabuf."); gbm_bo_destroy(sharingData.compositor_gbm_bo); sharingData.compositor_gbm_bo = nullptr; return; }
                
                Debug::log(LOG, "[screencopy] Dedicated compositor buffer created successfully.");
            }

            const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);

            if (!PSTREAM) {
                // Stream doesn't exist yet, create it now that we have frame info.
                Debug::log(LOG, "[screencopy] First frame info received, creating PipeWire stream now.");
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->createStream(this);
                
                // Now that the stream is being created, we must wait for it to be ready.
                // We will re-queue the frame copy from the PipeWire state change handler.
                // So, we simply return here. The next steps will happen asynchronously.
                return;
            }

            Debug::log(TRACE, "[sc] pw format {} size {}x{}", (int)PSTREAM->pwVideoInfo.format, PSTREAM->pwVideoInfo.size.width, PSTREAM->pwVideoInfo.size.height);
            Debug::log(TRACE, "[sc] wlr format {} size {}x{}", (int)sharingData.frameInfoSHM.fmt, sharingData.frameInfoSHM.w, sharingData.frameInfoSHM.h);
            Debug::log(TRACE, "[sc] wlr format dma {} size {}x{}", (int)sharingData.frameInfoDMA.fmt, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h);

            if (!selection.needsTransform) {
                const auto FMT = PSTREAM->isDMA ? sharingData.frameInfoDMA.fmt : sharingData.frameInfoSHM.fmt;
                if ((PSTREAM->pwVideoInfo.format != pwFromDrmFourcc(FMT) && PSTREAM->pwVideoInfo.format != pwStripAlpha(pwFromDrmFourcc(FMT))) ||
                    (PSTREAM->pwVideoInfo.size.width != sharingData.frameInfoDMA.w || PSTREAM->pwVideoInfo.size.height != sharingData.frameInfoDMA.h)) {
                    Debug::log(LOG, "[sc] Incompatible formats, renegotiate stream");
                    sharingData.status = FRAME_RENEG;
                    sharingData.frameCallback.reset();
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                    sharingData.status = FRAME_NONE;
                    return;
                }
            }

            

            if (!PSTREAM->currentPWBuffer) {
                Debug::log(LOG, "[sc] Dequeuing buffer for render pass.");
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->dequeue(this);
            }

            if (!PSTREAM->currentPWBuffer) {
                sharingData.frameCallback.reset();
                Debug::log(LOG, "[screencopy/pipewire] Out of buffers");
                sharingData.status = FRAME_NONE;
                if (sharingData.copyRetries++ < MAX_RETRIES) {
                    Debug::log(LOG, "[sc] Retrying screencopy ({}/{})", sharingData.copyRetries, MAX_RETRIES);
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                }
                return;
            }

            if (selection.needsTransform) {
                // Compositor copies to our dedicated buffer.
                sharingData.frameCallback->sendCopyWithDamage(sharingData.compositor_wl_buffer->resource());
            } else {
                // No transform, compositor copies directly to the PipeWire buffer.
                sharingData.frameCallback->sendCopyWithDamage(PSTREAM->currentPWBuffer->wlBuffer->resource());
            }

            sharingData.copyRetries = 0;

            Debug::log(TRACE, "[sc] wlr frame copied");
        });
    } else if (sharingData.windowFrameCallback) {
        sharingData.windowFrameCallback->setBuffer([this](CCHyprlandToplevelExportFrameV1* r, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
            Debug::log(TRACE, "[sc] hlOnBuffer for {}", (void*)this);

            sharingData.frameInfoSHM.w      = width;
            sharingData.frameInfoSHM.h      = height;
            sharingData.frameInfoSHM.fmt    = drmFourccFromSHM((wl_shm_format)format);
            sharingData.frameInfoSHM.size   = stride * height;
            sharingData.frameInfoSHM.stride = stride;

            // todo: done if ver < 3
        });
        sharingData.windowFrameCallback->setReady([this](CCHyprlandToplevelExportFrameV1* r, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
            // v-- ADD THIS LOG --v
            Debug::log(LOG, "[sc] hlOnReady callback has fired.");
            Debug::log(TRACE, "[sc] hlOnReady for {}", (void*)this);

            if (selection.needsTransform && g_pPortalManager->m_pRenderer) {
                const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);
                if (PSTREAM && PSTREAM->currentPWBuffer && sharingData.compositor_gbm_bo) {
                    SRenderBox cropBox;
                    getPhysicalCastingBox(&cropBox);
                    
                    Debug::log(LOG, "[render] Executing render pass with crop.");
                    if (!g_pPortalManager->m_pRenderer->render(PSTREAM->currentPWBuffer->bo, sharingData.compositor_gbm_bo, sharingData.transform, &cropBox)) {
                        Debug::log(ERR, "[screencopy] Render failed, skipping frame enqueue.");
                        sharingData.status = FRAME_NONE;
                        g_pPortalManager->addTimer({100.0, [this]() { g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this); }});
                        return;
                    }
                }
            }

            sharingData.status = FRAME_READY;

            sharingData.tvSec         = ((((uint64_t)tv_sec_hi) << 32) + (uint64_t)tv_sec_lo);
            sharingData.tvNsec        = tv_nsec;
            sharingData.tvTimestampNs = sharingData.tvSec * SPA_NSEC_PER_SEC + sharingData.tvNsec;

            Debug::log(TRACE, "[sc] frame timestamp sec: {} nsec: {} combined: {}ns", sharingData.tvSec, sharingData.tvNsec, sharingData.tvTimestampNs);

            // v-- ADD THIS LOG --v
            Debug::log(LOG, "[sc] Enqueuing frame to PipeWire.");
            g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);

            if (g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this))
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);

            sharingData.windowFrameCallback.reset();
        });
        sharingData.windowFrameCallback->setFailed([this](CCHyprlandToplevelExportFrameV1* r) {
            Debug::log(TRACE, "[sc] hlOnFailed for {}", (void*)this);
            sharingData.status = FRAME_FAILED;
        });
        sharingData.windowFrameCallback->setDamage([this](CCHyprlandToplevelExportFrameV1* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] hlOnDamage for {}", (void*)this);

            if (sharingData.damageCount > 3) {
                sharingData.damage[0] = {0, 0, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h};
                return;
            }

            sharingData.damage[sharingData.damageCount++] = {x, y, width, height};

            Debug::log(TRACE, "[sc] hl damage: {} {} {} {}", x, y, width, height);
        });
        sharingData.windowFrameCallback->setLinuxDmabuf([this](CCHyprlandToplevelExportFrameV1* r, uint32_t format, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] hlOnDmabuf for {}", (void*)this);

            sharingData.frameInfoDMA.w   = width;
            sharingData.frameInfoDMA.h   = height;
            sharingData.frameInfoDMA.fmt = format;
            
            Debug::log(LOG, "[sc-diag] Compositor reported DMA info: w={}, h={}, transform={}", sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h, (int)sharingData.transform);
        });
        sharingData.windowFrameCallback->setBufferDone([this](CCHyprlandToplevelExportFrameV1* r) {
            Debug::log(TRACE, "[sc] hlOnBufferDone for {}", (void*)this);

            // v-- ADD THIS BLOCK --v
            // Initialize renderer if it doesn't exist
            if (!g_pPortalManager->m_pRenderer) {
                if (g_pPortalManager->m_sWaylandConnection.gbmDevice) {
                    g_pPortalManager->m_pRenderer = std::make_unique<CRenderer>();
                    if (!g_pPortalManager->m_pRenderer->m_bGood) {
                        Debug::log(WARN, "[core] Failed to initialize renderer. Transform will not work.");
                        g_pPortalManager->m_pRenderer.reset();
                    }
                } else {
                    Debug::log(WARN, "[core] No GBM device, cannot initialize renderer. Transform will not work.");
                }
            }
            // ^-- ADD THIS BLOCK --^

            if (selection.needsTransform && !sharingData.compositor_gbm_bo) {
                // Use the compositor's reported native dimensions directly
                int sourceWidth = sharingData.frameInfoDMA.w;
                int sourceHeight = sharingData.frameInfoDMA.h;

                uint32_t sourceFormat = sharingData.frameInfoDMA.fmt;
                Debug::log(LOG, "[screencopy] Attempting to create NATIVE-sized GBM BO with: w={}, h={}, format={}", sourceWidth, sourceHeight, sourceFormat);

                sharingData.compositor_gbm_bo = gbm_bo_create(g_pPortalManager->m_sWaylandConnection.gbmDevice, sourceWidth, sourceHeight, sourceFormat, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
                if (!sharingData.compositor_gbm_bo) { Debug::log(ERR, "[screencopy] Failed to create dedicated compositor GBM buffer."); return; }
                
                if (!g_pPortalManager->m_sWaylandConnection.linuxDmabuf) {
                    Debug::log(ERR, "[screencopy] zwp_linux_dmabuf_v1 protocol not available, cannot import buffer.");
                    gbm_bo_destroy(sharingData.compositor_gbm_bo);
                    sharingData.compositor_gbm_bo = nullptr;
                    return;
                }

                auto params = makeShared<CCZwpLinuxBufferParamsV1>(g_pPortalManager->m_sWaylandConnection.linuxDmabuf->sendCreateParams());
                if (!params) { Debug::log(ERR, "[screencopy] zwp_linux_dmabuf_v1_create_params failed for compositor buffer."); gbm_bo_destroy(sharingData.compositor_gbm_bo); sharingData.compositor_gbm_bo = nullptr; return; }
                
                uint64_t modifier = gbm_bo_get_modifier(sharingData.compositor_gbm_bo);
                params->sendAdd(gbm_bo_get_fd(sharingData.compositor_gbm_bo), 0, 0, gbm_bo_get_stride(sharingData.compositor_gbm_bo), modifier >> 32, modifier & 0xffffffff);
                sharingData.compositor_wl_buffer = makeShared<CCWlBuffer>(params->sendCreateImmed(sourceWidth, sourceHeight, sourceFormat, (zwpLinuxBufferParamsV1Flags)0));
                if (!sharingData.compositor_wl_buffer) { Debug::log(ERR, "[screencopy] Failed to create dedicated compositor wl_buffer via dmabuf."); gbm_bo_destroy(sharingData.compositor_gbm_bo); sharingData.compositor_gbm_bo = nullptr; return; }
                
                Debug::log(LOG, "[screencopy] Dedicated compositor buffer created successfully.");
            }

            const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);

            if (!PSTREAM) {
                // Stream doesn't exist yet, create it now that we have frame info.
                Debug::log(LOG, "[screencopy] First frame info received, creating PipeWire stream now.");
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->createStream(this);
                
                // Now that the stream is being created, we must wait for it to be ready.
                // We will re-queue the frame copy from the PipeWire state change handler.
                // So, we simply return here. The next steps will happen asynchronously.
                return;
            }

            Debug::log(TRACE, "[sc] pw format {} size {}x{}", (int)PSTREAM->pwVideoInfo.format, PSTREAM->pwVideoInfo.size.width, PSTREAM->pwVideoInfo.size.height);
            Debug::log(TRACE, "[sc] hl format {} size {}x{}", (int)sharingData.frameInfoSHM.fmt, sharingData.frameInfoSHM.w, sharingData.frameInfoSHM.h);
            Debug::log(TRACE, "[sc] hl format dma {} size {}x{}", (int)sharingData.frameInfoDMA.fmt, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h);

            if (!selection.needsTransform) {
                const auto FMT = PSTREAM->isDMA ? sharingData.frameInfoDMA.fmt : sharingData.frameInfoSHM.fmt;
                if ((PSTREAM->pwVideoInfo.format != pwFromDrmFourcc(FMT) && PSTREAM->pwVideoInfo.format != pwStripAlpha(pwFromDrmFourcc(FMT))) ||
                    (PSTREAM->pwVideoInfo.size.width != sharingData.frameInfoDMA.w || PSTREAM->pwVideoInfo.size.height != sharingData.frameInfoDMA.h)) {
                    Debug::log(LOG, "[sc] Incompatible formats, renegotiate stream");
                    sharingData.status = FRAME_RENEG;
                    sharingData.windowFrameCallback.reset();
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                    sharingData.status = FRAME_NONE;
                    return;
                }
            }

            

            if (!PSTREAM->currentPWBuffer) {
                Debug::log(LOG, "[sc] Dequeuing buffer for render pass.");
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->dequeue(this);
            }

            if (!PSTREAM->currentPWBuffer) {
                sharingData.windowFrameCallback.reset();
                Debug::log(LOG, "[screencopy/pipewire] Out of buffers");
                sharingData.status = FRAME_NONE;
                if (sharingData.copyRetries++ < MAX_RETRIES) {
                    Debug::log(LOG, "[sc] Retrying screencopy ({}/{})", sharingData.copyRetries, MAX_RETRIES);
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                }
                return;
            }

            if (selection.needsTransform) {
                // Compositor copies to our dedicated buffer.
                sharingData.windowFrameCallback->sendCopy(sharingData.compositor_wl_buffer->resource(), false);
            } else {
                // No transform, compositor copies directly to the PipeWire buffer.
                sharingData.windowFrameCallback->sendCopy(PSTREAM->currentPWBuffer->wlBuffer->resource(), false);
            }

            sharingData.copyRetries = 0;

            Debug::log(TRACE, "[sc] hl frame copied");
        });
    }
}

SSession::~SSession() {
    if (sharingData.compositor_gbm_bo) {
        gbm_bo_destroy(sharingData.compositor_gbm_bo);
        sharingData.compositor_gbm_bo = nullptr;
        Debug::log(LOG, "[screencopy] SSession destructor: Cleaned up compositor_gbm_bo.");
    }
}

void CScreencopyPortal::queueNextShareFrame(SSession* pSession) {
    const auto PSTREAM = m_pPipewire->streamFromSession(pSession);

    if (PSTREAM && !PSTREAM->streamState)
        return;

    // calculate frame delta and queue next frame
    const auto FRAMETOOKMS           = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - pSession->sharingData.begunFrame).count() / 1000.0;
    const auto MSTILNEXTREFRESH      = 1000.0 / (pSession->sharingData.framerate) - FRAMETOOKMS;
    pSession->sharingData.begunFrame = std::chrono::system_clock::now();

    Debug::log(TRACE, "[screencopy] set fps {}, frame took {:.2f}ms, ms till next refresh {:.2f}, estimated actual fps: {:.2f}", pSession->sharingData.framerate, FRAMETOOKMS,
               MSTILNEXTREFRESH, std::clamp(1000.0 / FRAMETOOKMS, 1.0, (double)pSession->sharingData.framerate));

    g_pPortalManager->addTimer(
        {std::clamp(MSTILNEXTREFRESH - 1.0 /* safezone */, 6.0, 1000.0), [pSession]() { g_pPortalManager->m_sPortals.screencopy->startFrameCopy(pSession); }});
}
bool CScreencopyPortal::hasToplevelCapabilities() {
    return m_sState.toplevel;
}

SSession* CScreencopyPortal::getSession(sdbus::ObjectPath& path) {
    for (auto& s : m_vSessions) {
        if (s->sessionHandle == path)
            return s.get();
    }

    return nullptr;
}

CScreencopyPortal::CScreencopyPortal(SP<CCZwlrScreencopyManagerV1> mgr) {
    m_pObject = sdbus::createObject(*g_pPortalManager->getConnection(), OBJECT_PATH);

    m_pObject
        ->addVTable(sdbus::registerMethod("CreateSession")
                        .implementedAs([this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s1, std::unordered_map<std::string, sdbus::Variant> m1) {
                            return onCreateSession(o1, o2, s1, m1);
                        }),
                    sdbus::registerMethod("SelectSources")
                        .implementedAs([this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s1, std::unordered_map<std::string, sdbus::Variant> m1) {
                            return onSelectSources(o1, o2, s1, m1);
                        }),
                    sdbus::registerMethod("Start").implementedAs([this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s1, std::string s2,
                                                                        std::unordered_map<std::string, sdbus::Variant> m1) { return onStart(o1, o2, s1, s2, m1); }),
                    sdbus::registerProperty("AvailableSourceTypes").withGetter([]() { return uint32_t{VIRTUAL | MONITOR | WINDOW}; }),
                    sdbus::registerProperty("AvailableCursorModes").withGetter([]() { return uint32_t{HIDDEN | EMBEDDED}; }),
                    sdbus::registerProperty("version").withGetter([]() { return uint32_t{3}; }))
        .forInterface(INTERFACE_NAME);

    m_sState.screencopy = mgr;
    m_pPipewire         = std::make_unique<CPipewireConnection>();

    Debug::log(LOG, "[screencopy] init successful");
}

void SSession::getLogicalDimensions(int& width, int& height) {
    // Start with the native dimensions from the compositor
    width = sharingData.frameInfoDMA.w;
    height = sharingData.frameInfoDMA.h;

    Debug::log(LOG, "[sc-diag] getLogicalDimensions: initial dims {}x{}, transform is {}", width, height, (int)sharingData.transform);

    // If the screen is rotated 90/270, the surface dimensions are swapped
    if (sharingData.transform == WL_OUTPUT_TRANSFORM_90 || sharingData.transform == WL_OUTPUT_TRANSFORM_270 ||
        sharingData.transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 || sharingData.transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        std::swap(width, height);
        Debug::log(LOG, "[sc-diag] getLogicalDimensions: swapped dims to {}x{}", width, height);
    }
}

void SSession::getTargetDimensions(int& width, int& height) {
    if (selection.type == TYPE_GEOMETRY && selection.needsTransform) {
        // For a transformed region, the target dimensions are simply the
        // logical width and height of the selection itself.
        width = selection.w;
        height = selection.h;
    } else if (selection.needsTransform) {
        // For a transformed full screen, get the full logical dimensions.
        getLogicalDimensions(width, height);
    } else {
        // For non-transformed selections, use the native compositor dimensions.
        width = sharingData.frameInfoDMA.w;
        height = sharingData.frameInfoDMA.h;
    }
    Debug::log(LOG, "[sc-diag] getTargetDimensions finished. Target outputs: width={}, height={}", width, height);
}

void CScreencopyPortal::appendToplevelExport(SP<CCHyprlandToplevelExportManagerV1> proto) {
    m_sState.toplevel = proto;

    Debug::log(LOG, "[screencopy] Registered for toplevel export");
}

void SSession::getPhysicalCastingBox(SRenderBox* pBox) {
    if (selection.type != TYPE_GEOMETRY || !selection.needsTransform) {
        pBox = nullptr;
        return;
    }

    int physicalW = sharingData.frameInfoDMA.w;
    int physicalH = sharingData.frameInfoDMA.h;

    // These formulas are derived from the inverse of the transforms defined by wlroots
    // See: https://github.com/swaywm/wlroots/blob/master/include/wlr/util/box.h#L131
    switch (sharingData.transform) {
        case WL_OUTPUT_TRANSFORM_NORMAL:
            pBox->x = selection.x;
            pBox->y = selection.y;
            pBox->w = selection.w;
            pBox->h = selection.h;
            break;
        case WL_OUTPUT_TRANSFORM_90:
            pBox->x = selection.y;
            pBox->y = physicalH - selection.x - selection.w; // Use physicalH
            pBox->w = selection.h;
            pBox->h = selection.w;
            break;
        case WL_OUTPUT_TRANSFORM_180:
            pBox->x = physicalW - selection.x - selection.w;
            pBox->y = physicalH - selection.y - selection.h;
            pBox->w = selection.w;
            pBox->h = selection.h;
            break;
        case WL_OUTPUT_TRANSFORM_270:
            pBox->x = physicalW - selection.y - selection.h; // Use physicalW
            pBox->y = selection.x;
            pBox->w = selection.h;
            pBox->h = selection.w;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED:
            pBox->x = physicalW - selection.x - selection.w;
            pBox->y = selection.y;
            pBox->w = selection.w;
            pBox->h = selection.h;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            pBox->x = selection.y;
            pBox->y = selection.x;
            pBox->w = selection.h;
            pBox->h = selection.w;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
            pBox->x = selection.x;
            pBox->y = physicalH - selection.y - selection.h;
            pBox->w = selection.w;
            pBox->h = selection.h;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            pBox->x = physicalH - selection.y - selection.h;
            pBox->y = physicalW - selection.x - selection.w;
            pBox->w = selection.h;
            pBox->h = selection.w;
            break;
        default:
            pBox = nullptr; // Should not happen
            break;
    }
    Debug::log(LOG, "[sc-diag] Calculated physical crop box: x={}, y={}, w={}, h={}", pBox->x, pBox->y, pBox->w, pBox->h);
}
