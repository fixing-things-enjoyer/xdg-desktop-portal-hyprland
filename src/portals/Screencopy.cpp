#include "Screencopy.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../vulkan/VulkanRotator.hpp"

#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include "linux-dmabuf-v1.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>

constexpr static int MAX_RETRIES = 10;
constexpr static auto PICKER_DEBOUNCE_WINDOW = std::chrono::seconds(20);

static uint32_t drmBytesPerPixel(uint32_t drmFormat);
static uint32_t drmFormatWithoutAlpha(uint32_t drmFormat);
static double getTargetShareFPS(CScreencopyPortal::SSession* pSession);
static bool usePipewireProcessScheduling(CScreencopyPortal::SSession* pSession);
static uint32_t pickPreferredSHMFormat(const std::vector<uint32_t>& formats, uint32_t preferred);
static uint32_t alignPipewireStride(uint32_t stride);

static bool isRotatorCompatibleFormat(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_RGBX8888: return true;
        default: return false;
    }
}

static int formatPreference(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_XRGB8888: return 0;
        case DRM_FORMAT_ARGB8888: return 1;
        case DRM_FORMAT_BGRX8888: return 2;
        case DRM_FORMAT_BGRA8888: return 3;
        case DRM_FORMAT_XBGR8888: return 4;
        case DRM_FORMAT_ABGR8888: return 5;
        case DRM_FORMAT_RGBX8888: return 6;
        case DRM_FORMAT_RGBA8888: return 7;
        default: return 100;
    }
}

static int modifierPreference(uint64_t modifier) {
    if (modifier == DRM_FORMAT_MOD_LINEAR)
        return 0;
    if (modifier == DRM_FORMAT_MOD_INVALID)
        return 1;
    return 2;
}

static uint32_t alignPipewireStride(uint32_t stride) {
    if (stride == 0)
        return 0;

    return (stride + XDPH_PWR_ALIGN - 1) & ~(XDPH_PWR_ALIGN - 1);
}

static xdph::vulkan::Region getLogicalCaptureRegion(CScreencopyPortal::SSession* pSession, uint32_t physicalW, uint32_t physicalH) {
    if (pSession->selection.type == TYPE_GEOMETRY)
        return {(int32_t)pSession->selection.x, (int32_t)pSession->selection.y, (int32_t)pSession->selection.w, (int32_t)pSession->selection.h};

    const auto logical = xdph::vulkan::getLogicalDimensions({physicalW, physicalH}, pSession->sharingData.transform);
    return {0, 0, (int32_t)logical.width, (int32_t)logical.height};
}

static bool usePipewireProcessScheduling(CScreencopyPortal::SSession* pSession) {
    if (!pSession)
        return false;

    return g_pPortalManager->m_sPortals.screencopy->shouldUseExtImageCopy(pSession);
}

static uint32_t pickPreferredSHMFormat(const std::vector<uint32_t>& formats, uint32_t preferred) {
    if (preferred != DRM_FORMAT_INVALID && std::find(formats.begin(), formats.end(), preferred) != formats.end())
        return preferred;

    uint32_t best = DRM_FORMAT_INVALID;
    for (const auto fmt : formats) {
        if (!isRotatorCompatibleFormat(fmt))
            continue;

        if (best == DRM_FORMAT_INVALID || formatPreference(fmt) < formatPreference(best))
            best = fmt;
    }

    return best;
}

SBuffer::~SBuffer() {
    wlBuffer.reset();

    if (mapped && mappedSize > 0)
        munmap(mapped, mappedSize);

    if (bo)
        gbm_bo_destroy(bo);

    for (int& planeFd : fd) {
        if (planeFd >= 0) {
            close(planeFd);
            planeFd = -1;
        }
    }
}

//
static sdbus::Struct<std::string, uint32_t, sdbus::Variant> getFullRestoreStruct(const SSelectionData& data, uint32_t cursor) {
    std::unordered_map<std::string, sdbus::Variant> mapData;

    mapData["type"] = sdbus::Variant{uint32_t(data.type)};

    switch (data.type) {
        case TYPE_GEOMETRY:
            mapData["output"] = sdbus::Variant{data.output};
            mapData["x"]      = sdbus::Variant{uint32_t(data.x)};
            mapData["y"]      = sdbus::Variant{uint32_t(data.y)};
            mapData["w"]      = sdbus::Variant{uint32_t(data.w)};
            mapData["h"]      = sdbus::Variant{uint32_t(data.h)};
            break;
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
    mapData["rotationFix"] = sdbus::Variant{uint32_t(data.rotationFix ? 1 : 0)};

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

    const Hyprutils::Memory::CWeakPointer<SSession> PSESSION = m_vSessions.emplace_back(Hyprutils::Memory::makeUnique<SSession>(appID, requestHandle, sessionHandle));
    PSESSION->self                                           = PSESSION;

    // create objects
    PSESSION->session            = createDBusSession(sessionHandle);
    PSESSION->session->onDestroy = [PSESSION, this]() {
        if (PSESSION->sharingData.active) {
            m_pPipewire->destroyStream(PSESSION.get());
            Debug::log(LOG, "[screencopy] Stream destroyed");
        }
        destroyExtImageCopySession(PSESSION.get());
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
        bool           exists       = false;
        eSelectionType type         = TYPE_INVALID;
        std::string    token, output;
        uint64_t       windowHandle = 0;
        bool           withCursor   = false;
        uint64_t       timeIssued   = 0;
        std::string    windowClass;
        uint32_t       x            = 0;
        uint32_t       y            = 0;
        uint32_t       w            = 0;
        uint32_t       h            = 0;
        bool           rotationFix  = false;
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
                restoreData.type         = restoreData.windowHandle != 0 ? TYPE_WINDOW : TYPE_OUTPUT;

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
                        restoreData.withCursor = tkval.get<uint32_t>() == EMBEDDED;
                    else if (tkkey == "timeIssued")
                        restoreData.timeIssued = tkval.get<uint64_t>();
                    else if (tkkey == "token")
                        restoreData.token = tkval.get<std::string>();
                    else if (tkkey == "type")
                        restoreData.type = (eSelectionType)tkval.get<uint32_t>();
                    else if (tkkey == "x")
                        restoreData.x = tkval.get<uint32_t>();
                    else if (tkkey == "y")
                        restoreData.y = tkval.get<uint32_t>();
                    else if (tkkey == "w")
                        restoreData.w = tkval.get<uint32_t>();
                    else if (tkkey == "h")
                        restoreData.h = tkval.get<uint32_t>();
                    else if (tkkey == "rotationFix")
                        restoreData.rotationFix = tkval.get<uint32_t>() != 0;
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

    const bool restoreOutputValid = !restoreData.output.empty() && g_pPortalManager->getOutputFromName(restoreData.output);
    const bool restoreWindowValid =
        !restoreData.windowClass.empty() && g_pPortalManager->m_sHelpers.toplevel->handleFromClass(restoreData.windowClass);
    const bool restoreGeometryValid = restoreData.type == TYPE_GEOMETRY && restoreOutputValid && restoreData.w > 0 && restoreData.h > 0;
    const bool restoreOutputShareValid =
        (restoreData.type == TYPE_OUTPUT || restoreData.type == TYPE_INVALID) && restoreOutputValid && !restoreGeometryValid;
    const bool restoreWindowShareValid = (restoreData.type == TYPE_WINDOW || restoreData.type == TYPE_INVALID) && restoreWindowValid;
    const bool RESTOREDATAVALID        = restoreData.exists && (restoreGeometryValid || restoreOutputShareValid || restoreWindowShareValid);

    const bool requestPersistentToken = PSESSION->persistMode != 0;

    SSelectionData SHAREDATA;
    if (RESTOREDATAVALID) {
        Debug::log(LOG, "[screencopy] restore data valid, not prompting");

        const bool GEOMETRY    = restoreGeometryValid;
        const bool WINDOW      = !GEOMETRY && restoreWindowShareValid;
        const auto HANDLEMATCH = WINDOW && restoreData.windowHandle != 0 ? g_pPortalManager->m_sHelpers.toplevel->handleFromHandleFull(restoreData.windowHandle) : nullptr;

        SHAREDATA.output       = restoreData.output;
        SHAREDATA.type         = GEOMETRY ? TYPE_GEOMETRY : (WINDOW ? TYPE_WINDOW : TYPE_OUTPUT);
        SHAREDATA.windowHandle = WINDOW ? (HANDLEMATCH ? HANDLEMATCH->handle : g_pPortalManager->m_sHelpers.toplevel->handleFromClass(restoreData.windowClass)->handle) : nullptr;
        SHAREDATA.windowClass  = restoreData.windowClass;
        SHAREDATA.x            = restoreData.x;
        SHAREDATA.y            = restoreData.y;
        SHAREDATA.w            = restoreData.w;
        SHAREDATA.h            = restoreData.h;
        SHAREDATA.rotationFix  = restoreData.rotationFix;
        SHAREDATA.allowToken   = true; // user allowed token before
        PSESSION->cursorMode   = restoreData.withCursor ? EMBEDDED : HIDDEN;
    } else if (m_sRecentSelection.valid && std::chrono::system_clock::now() - m_sRecentSelection.issuedAt <= PICKER_DEBOUNCE_WINDOW) {
        Debug::log(LOG, "[screencopy] reusing recent picker selection for {} (previous appid: {})", appID, m_sRecentSelection.appid);
        SHAREDATA            = m_sRecentSelection.selection;
        PSESSION->cursorMode = m_sRecentSelection.cursorMode;
        if (requestPersistentToken && !SHAREDATA.allowToken) {
            Debug::log(LOG, "[screencopy] enabling restore token on reused selection due to persist_mode request");
            SHAREDATA.allowToken = true;
        }
    } else {
        Debug::log(LOG, "[screencopy] restore data invalid / missing, prompting");

        SHAREDATA = promptForScreencopySelection(requestPersistentToken);

        if (SHAREDATA.type != TYPE_INVALID)
            PSESSION->cursorMode = SHAREDATA.withCursor ? EMBEDDED : HIDDEN;

        if (SHAREDATA.type != TYPE_INVALID) {
            m_sRecentSelection.valid      = true;
            m_sRecentSelection.appid      = appID;
            m_sRecentSelection.cursorMode = PSESSION->cursorMode;
            m_sRecentSelection.selection  = SHAREDATA;
            m_sRecentSelection.issuedAt   = std::chrono::system_clock::now();
        }
    }

    Debug::log(LOG, "[screencopy] SHAREDATA returned selection {}", (int)SHAREDATA.type);

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
                PSESSION->sharingData.framerate = std::clamp(POUTPUT->refreshRate, 1.F, (float)**PFPS);
        }
    }

    PSESSION->selection = SHAREDATA;
    PSESSION->sharingData.rotationRequested = SHAREDATA.rotationFix;

    return {SHAREDATA.type == TYPE_INVALID ? 1 : 0, {}};
}

dbUasv CScreencopyPortal::onStart(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow,
                                  std::unordered_map<std::string, sdbus::Variant> opts) {
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

    std::unordered_map<std::string, sdbus::Variant> options;

    if (PSESSION->selection.allowToken) {
        // give them a token :)
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

    auto [streamW, streamH] = getStreamDimensions(PSESSION, PSESSION->sharingData.frameInfoSHM.w, PSESSION->sharingData.frameInfoSHM.h);

    std::unordered_map<std::string, sdbus::Variant>                                       streamData;
    if (PSESSION->selection.type == TYPE_GEOMETRY)
        streamData["position"] = sdbus::Variant{sdbus::Struct<int32_t, int32_t>{(int32_t)PSESSION->selection.x, (int32_t)PSESSION->selection.y}};
    else
        streamData["position"] = sdbus::Variant{sdbus::Struct<int32_t, int32_t>{0, 0}};
    streamData["size"]        = sdbus::Variant{sdbus::Struct<int32_t, int32_t>{(int32_t)streamW, (int32_t)streamH}};
    streamData["source_type"] = sdbus::Variant{uint32_t{type}};
    streams.emplace_back(sdbus::Struct<uint32_t, std::unordered_map<std::string, sdbus::Variant>>{PSESSION->sharingData.nodeID, streamData});

    options["streams"] = sdbus::Variant{streams};

    return {0, options};
}

void CScreencopyPortal::startSharing(CScreencopyPortal::SSession* pSession) {
    pSession->sharingData.active = true;

    if (pSession->selection.type == TYPE_OUTPUT || pSession->selection.type == TYPE_GEOMETRY) {
        if (const auto POUTPUT = g_pPortalManager->getOutputFromName(pSession->selection.output); POUTPUT)
            pSession->sharingData.transform = POUTPUT->transform;
    } else {
        pSession->sharingData.transform = WL_OUTPUT_TRANSFORM_NORMAL;
    }

    const bool useExtImageCopy = shouldUseExtImageCopy(pSession) && ensureExtImageCopySession(pSession);
    if (!useExtImageCopy) {
        if (shouldUseExtImageCopy(pSession))
            Debug::log(WARN, "[extcopy] Falling back to legacy screencopy path for this session");

        startFrameCopy(pSession);

        wl_display_dispatch(g_pPortalManager->m_sWaylandConnection.display);
        wl_display_roundtrip(g_pPortalManager->m_sWaylandConnection.display);
    }

    if (pSession->sharingData.frameInfoDMA.fmt == DRM_FORMAT_INVALID) {
        Debug::log(ERR, "[screencopy] Couldn't obtain a format from dma"); // todo: blocks shm
        return;
    }

    // We already know the output transform before creating the PipeWire stream.
    // Advertising the rotated dimensions from the first negotiation avoids a mid-stream
    // size flip, which Chromium/Vesktop reacts to by falling back to SHM.
    pSession->sharingData.rotationReady = shouldApplyGpuRotation(pSession) && getRotator() != nullptr;
    pSession->sharingData.lastTransform = pSession->sharingData.transform;
    Debug::log(LOG, "[screencopy] Initial rotation state: requested={} ready={} transform={}", pSession->sharingData.rotationRequested,
               pSession->sharingData.rotationReady, (int)pSession->sharingData.transform);

    m_pPipewire->createStream(pSession);

    while (pSession->sharingData.nodeID == SPA_ID_INVALID) {
        int ret = pw_loop_iterate(g_pPortalManager->m_sPipewire.loop, 0);
        if (ret < 0) {
            Debug::log(ERR, "[pipewire] pw_loop_iterate failed with {}", spa_strerror(ret));
            return;
        }
    }

    Debug::log(LOG, "[screencopy] Sharing initialized");

    if (!usePipewireProcessScheduling(pSession)) {
        g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(pSession);
        Debug::log(TRACE, "[sc] queued frame in {}ms", 1000.0 / pSession->sharingData.framerate);
    } else {
        Debug::log(TRACE, "[sc] using PipeWire-driven frame scheduling");
    }
}

void CScreencopyPortal::startFrameCopy(CScreencopyPortal::SSession* pSession) {
    pSession->startCopy();

    Debug::log(TRACE, "[screencopy] frame callbacks initialized");
}

void CScreencopyPortal::SSession::startCopy() {
    const auto     POUTPUT       = g_pPortalManager->getOutputFromName(selection.output);
    const uint32_t OVERLAYCURSOR = cursorMode == EMBEDDED ? 1 : 0;
    auto*          portal        = g_pPortalManager->m_sPortals.screencopy.get();

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

    if (sharingData.extImageCopyFrame) {
        Debug::log(ERR, "[extcopy] tried scheduling with an active ext-image-copy frame");
        return;
    }

    sharingData.damageCount = 0;

    if (portal->shouldUseExtImageCopy(this)) {
        if (!portal->ensureExtImageCopySession(this)) {
            Debug::log(WARN, "[extcopy] constraints unavailable, aborting ext frame start");
            return;
        }

        const auto PSTREAM = portal->m_pPipewire->streamFromSession(this);
        if (!PSTREAM) {
            Debug::log(ERR, "[extcopy] no PipeWire stream for session");
            return;
        }

        if (!PSTREAM->currentPWBuffer)
            portal->m_pPipewire->dequeue(this);

        if (!PSTREAM->currentPWBuffer) {
            Debug::log(LOG, "[extcopy] Out of PipeWire buffers before capture");
            portal->queueNextShareFrame(this);
            return;
        }

        auto* captureBuffer = portal->m_pPipewire->ensureSessionCaptureBuffer(PSTREAM);
        if (!captureBuffer) {
            Debug::log(LOG, "[extcopy] No reusable capture buffer available");
            portal->queueNextShareFrame(this);
            return;
        }

        captureBuffer->awaitingRelease = true;
        sharingData.extImageCopyFrame  = makeShared<CCExtImageCopyCaptureFrameV1>(sharingData.extImageCopySession->sendCreateFrame());
        sharingData.status             = FRAME_QUEUED;
        initCallbacks();
        return;
    }

    if (selection.type == TYPE_GEOMETRY) {
        sharingData.frameCallback = makeShared<CCZwlrScreencopyFrameV1>(g_pPortalManager->m_sPortals.screencopy->m_sState.screencopy->sendCaptureOutputRegion(
            OVERLAYCURSOR, POUTPUT->output->resource(), selection.x, selection.y, selection.w, selection.h));
        sharingData.transform     = POUTPUT->transform;
    } else if (selection.type == TYPE_OUTPUT) {
        sharingData.frameCallback =
            makeShared<CCZwlrScreencopyFrameV1>(g_pPortalManager->m_sPortals.screencopy->m_sState.screencopy->sendCaptureOutput(OVERLAYCURSOR, POUTPUT->output->resource()));
        sharingData.transform = POUTPUT->transform;
    } else if (selection.type == TYPE_WINDOW) {
        if (!selection.windowHandle) {
            Debug::log(ERR, "[screencopy] selected invalid window?");
            return;
        }
        sharingData.windowFrameCallback = makeShared<CCHyprlandToplevelExportFrameV1>(
            g_pPortalManager->m_sPortals.screencopy->m_sState.toplevel->sendCaptureToplevelWithWlrToplevelHandle(OVERLAYCURSOR, selection.windowHandle->resource()));
        sharingData.transform = WL_OUTPUT_TRANSFORM_NORMAL;
    } else {
        Debug::log(ERR, "[screencopy] Unsupported selection {}", (int)selection.type);
        return;
    }

    sharingData.status = FRAME_QUEUED;

    initCallbacks();
}

void CScreencopyPortal::SSession::initCallbacks() {
    if (sharingData.extImageCopyFrame) {
        sharingData.extImageCopyFrame->setTransform([this, self = self](CCExtImageCopyCaptureFrameV1*, uint32_t transform) {
            if (!self)
                return;

            sharingData.transform = (wl_output_transform)transform;
        });
        sharingData.extImageCopyFrame->setDamage([this, self = self](CCExtImageCopyCaptureFrameV1*, int32_t x, int32_t y, int32_t width, int32_t height) {
            if (!self)
                return;

            if (sharingData.damageCount > 3) {
                sharingData.damage[0] = {0, 0, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h};
                return;
            }

            sharingData.damage[sharingData.damageCount++] = {(uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height};
        });
        sharingData.extImageCopyFrame->setPresentationTime([this, self = self](CCExtImageCopyCaptureFrameV1*, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
            if (!self)
                return;

            sharingData.tvSec         = ((((uint64_t)tv_sec_hi) << 32) + (uint64_t)tv_sec_lo);
            sharingData.tvNsec        = tv_nsec;
            sharingData.tvTimestampNs = sharingData.tvSec * SPA_NSEC_PER_SEC + sharingData.tvNsec;
        });
        sharingData.extImageCopyFrame->setReady([this, self = self](CCExtImageCopyCaptureFrameV1*) {
            if (!self)
                return;

            const size_t captureIndex = sharingData.rotationCaptureBufferActiveIndex;
            if (captureIndex < sharingData.rotationCaptureBuffers.size()) {
                if (const auto& captureBuffer = sharingData.rotationCaptureBuffers[captureIndex]; captureBuffer)
                    captureBuffer->awaitingRelease = false;
            }

            sharingData.status = FRAME_READY;

            g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);

            if (!usePipewireProcessScheduling(this) && g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this))
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);

            sharingData.extImageCopyFrame.reset();
        });
        sharingData.extImageCopyFrame->setFailed([this, self = self](CCExtImageCopyCaptureFrameV1*, extImageCopyCaptureFrameV1FailureReason reason) {
            if (!self)
                return;

            const size_t captureIndex = sharingData.rotationCaptureBufferActiveIndex;
            if (captureIndex < sharingData.rotationCaptureBuffers.size()) {
                if (const auto& captureBuffer = sharingData.rotationCaptureBuffers[captureIndex]; captureBuffer)
                    captureBuffer->awaitingRelease = false;
            }

            Debug::log(WARN, "[extcopy] frame failed with reason {}", (int)reason);

            if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
                g_pPortalManager->m_sPortals.screencopy->destroyExtImageCopySession(this);
                sharingData.status = FRAME_RENEG;
            } else {
                sharingData.status = FRAME_FAILED;
            }

            const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);
            if (usePipewireProcessScheduling(this) && PSTREAM)
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);
            else if (!usePipewireProcessScheduling(this) && PSTREAM)
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);

            sharingData.extImageCopyFrame.reset();
        });

        const size_t captureIndex = sharingData.rotationCaptureBufferActiveIndex;
        if (captureIndex >= sharingData.rotationCaptureBuffers.size() || !sharingData.rotationCaptureBuffers[captureIndex]) {
            Debug::log(ERR, "[extcopy] Missing active capture buffer");
            sharingData.extImageCopyFrame.reset();
            return;
        }

        auto* captureBuffer = sharingData.rotationCaptureBuffers[captureIndex].get();
        sharingData.extImageCopyFrame->sendAttachBuffer(captureBuffer->wlBuffer->resource());
        sharingData.extImageCopyFrame->sendDamageBuffer(0, 0, captureBuffer->w, captureBuffer->h);
        sharingData.extImageCopyFrame->sendCapture();
    } else if (sharingData.frameCallback) {
        sharingData.frameCallback->setBuffer([this, self = self](CCZwlrScreencopyFrameV1* r, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
            Debug::log(TRACE, "[sc] wlrOnBuffer for {}", (void*)self.get());
            if (!self)
                return;

            sharingData.frameInfoSHM.w      = width;
            sharingData.frameInfoSHM.h      = height;
            sharingData.frameInfoSHM.fmt    = drmFourccFromSHM((wl_shm_format)format);
            sharingData.frameInfoSHM.size   = stride * height;
            sharingData.frameInfoSHM.stride = stride;

            // todo: done if ver < 3
        });
        sharingData.frameCallback->setReady([this, self = self](CCZwlrScreencopyFrameV1* r, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
            Debug::log(TRACE, "[sc] wlrOnReady for {}", (void*)self.get());
            if (!self)
                return;

            sharingData.status = FRAME_READY;

            sharingData.tvSec         = ((((uint64_t)tv_sec_hi) << 32) + (uint64_t)tv_sec_lo);
            sharingData.tvNsec        = tv_nsec;
            sharingData.tvTimestampNs = sharingData.tvSec * SPA_NSEC_PER_SEC + sharingData.tvNsec;

            Debug::log(TRACE, "[sc] frame timestamp sec: {} nsec: {} combined: {}ns", sharingData.tvSec, sharingData.tvNsec, sharingData.tvTimestampNs);

            g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);

            if (!usePipewireProcessScheduling(this) && g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this))
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);

            sharingData.frameCallback.reset();
        });
        sharingData.frameCallback->setFailed([this, self = self](CCZwlrScreencopyFrameV1* r) {
            Debug::log(TRACE, "[sc] wlrOnFailed for {}", (void*)self.get());
            if (!self)
                return;
            sharingData.status = FRAME_FAILED;

            const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);
            if (usePipewireProcessScheduling(this) && PSTREAM)
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);
        });
        sharingData.frameCallback->setDamage([this, self = self](CCZwlrScreencopyFrameV1* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] wlrOnDamage for {}", (void*)self.get());
            if (!self)
                return;

            if (sharingData.damageCount > 3) {
                sharingData.damage[0] = {0, 0, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h};
                return;
            }

            sharingData.damage[sharingData.damageCount++] = {x, y, width, height};

            Debug::log(TRACE, "[sc] wlr damage: {} {} {} {}", x, y, width, height);
        });
        sharingData.frameCallback->setLinuxDmabuf([this, self = self](CCZwlrScreencopyFrameV1* r, uint32_t format, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] wlrOnDmabuf for {}", (void*)self.get());
            if (!self)
                return;

            sharingData.frameInfoDMA.w   = width;
            sharingData.frameInfoDMA.h   = height;
            sharingData.frameInfoDMA.fmt = format;
        });
        sharingData.frameCallback->setBufferDone([this, self = self](CCZwlrScreencopyFrameV1* r) {
            Debug::log(TRACE, "[sc] wlrOnBufferDone for {}", (void*)self.get());
            if (!self)
                return;

            const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);

            if (!PSTREAM) {
                Debug::log(TRACE, "[sc] wlrOnBufferDone: no stream");
                sharingData.status = FRAME_NONE;
                sharingData.frameCallback.reset();
                return;
            }

            Debug::log(TRACE, "[sc] pw format {} size {}x{}", (int)PSTREAM->pwVideoInfo.format, PSTREAM->pwVideoInfo.size.width, PSTREAM->pwVideoInfo.size.height);
            Debug::log(TRACE, "[sc] wlr format {} size {}x{}", (int)sharingData.frameInfoSHM.fmt, sharingData.frameInfoSHM.w, sharingData.frameInfoSHM.h);
            Debug::log(TRACE, "[sc] wlr format dma {} size {}x{}", (int)sharingData.frameInfoDMA.fmt, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h);

            const auto FMT = PSTREAM->isDMA ? sharingData.frameInfoDMA.fmt : sharingData.frameInfoSHM.fmt;

            auto [streamW, streamH] = g_pPortalManager->m_sPortals.screencopy->getStreamDimensions(this, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h);

            const bool transformSwapsNow        = xdph::vulkan::transformSwapsDimensions(sharingData.transform);
            const bool transformSwappedBefore   = xdph::vulkan::transformSwapsDimensions(sharingData.lastTransform);
            const bool transformDimensionChange = g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(this) && (transformSwapsNow != transformSwappedBefore);

            if (transformDimensionChange) {
                Debug::log(LOG, "[sc] Transform changed from {} to {}, renegotiate stream", (int)sharingData.lastTransform, (int)sharingData.transform);
            }

            if ((PSTREAM->pwVideoInfo.format != pwFromDrmFourcc(FMT) && PSTREAM->pwVideoInfo.format != pwStripAlpha(pwFromDrmFourcc(FMT))) ||
                (PSTREAM->pwVideoInfo.size.width != streamW || PSTREAM->pwVideoInfo.size.height != streamH) || transformDimensionChange) {
                Debug::log(LOG, "[sc] Incompatible formats, renegotiate stream");
                sharingData.lastTransform = sharingData.transform;
                sharingData.status        = FRAME_RENEG;
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                sharingData.status = FRAME_NONE;
                sharingData.frameCallback.reset();
                return;
            }

            if (!PSTREAM->currentPWBuffer) {
                Debug::log(TRACE, "[sc] wlrOnBufferDone: dequeue, no current buffer");
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->dequeue(this);
            }

            if (!PSTREAM->currentPWBuffer) {
                Debug::log(LOG, "[screencopy/pipewire] Out of buffers");
                sharingData.status = FRAME_NONE;
                if (sharingData.copyRetries++ < MAX_RETRIES) {
                    Debug::log(LOG, "[sc] Retrying screencopy ({}/{})", sharingData.copyRetries, MAX_RETRIES);
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                }
                sharingData.frameCallback.reset();
                return;
            }

            SBuffer* copyTarget = PSTREAM->currentPWBuffer;
            const bool useRotation = g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(this);
            if (useRotation) {
                if (auto* captureBuffer = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->ensureSessionCaptureBuffer(PSTREAM); captureBuffer)
                    copyTarget = captureBuffer;
                else {
                    Debug::log(LOG, "[sc] No reusable rotation capture buffer available, skipping frame");
                    sharingData.status = FRAME_NONE;
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                    sharingData.frameCallback.reset();
                    return;
                }
            }

            copyTarget->awaitingRelease = true;
            if (useRotation)
                sharingData.frameCallback->sendCopy(copyTarget->wlBuffer->resource());
            else
                sharingData.frameCallback->sendCopyWithDamage(copyTarget->wlBuffer->resource());
            sharingData.copyRetries = 0;

            Debug::log(TRACE, "[sc] wlr frame copied");
        });
    } else if (sharingData.windowFrameCallback) {
        sharingData.windowFrameCallback->setBuffer([this, self = self](CCHyprlandToplevelExportFrameV1* r, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
            Debug::log(TRACE, "[sc] hlOnBuffer for {}", (void*)self.get());
            if (!self)
                return;

            sharingData.frameInfoSHM.w      = width;
            sharingData.frameInfoSHM.h      = height;
            sharingData.frameInfoSHM.fmt    = drmFourccFromSHM((wl_shm_format)format);
            sharingData.frameInfoSHM.size   = stride * height;
            sharingData.frameInfoSHM.stride = stride;

            // todo: done if ver < 3
        });
        sharingData.windowFrameCallback->setReady([this, self = self](CCHyprlandToplevelExportFrameV1* r, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
            Debug::log(TRACE, "[sc] hlOnReady for {}", (void*)self.get());
            if (!self)
                return;

            sharingData.status = FRAME_READY;

            sharingData.tvSec         = ((((uint64_t)tv_sec_hi) << 32) + (uint64_t)tv_sec_lo);
            sharingData.tvNsec        = tv_nsec;
            sharingData.tvTimestampNs = sharingData.tvSec * SPA_NSEC_PER_SEC + sharingData.tvNsec;

            Debug::log(TRACE, "[sc] frame timestamp sec: {} nsec: {} combined: {}ns", sharingData.tvSec, sharingData.tvNsec, sharingData.tvTimestampNs);

            g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);

            if (!usePipewireProcessScheduling(this) && g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this))
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);

            sharingData.windowFrameCallback.reset();
        });
        sharingData.windowFrameCallback->setFailed([this, self = self](CCHyprlandToplevelExportFrameV1* r) {
            Debug::log(TRACE, "[sc] hlOnFailed for {}", (void*)self.get());
            if (!self)
                return;
            sharingData.status = FRAME_FAILED;

            const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);
            if (usePipewireProcessScheduling(this) && PSTREAM)
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(this);
        });
        sharingData.windowFrameCallback->setDamage([this, self = self](CCHyprlandToplevelExportFrameV1* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] hlOnDamage for {}", (void*)self.get());
            if (!self)
                return;

            if (sharingData.damageCount > 3) {
                sharingData.damage[0] = {0, 0, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h};
                return;
            }

            sharingData.damage[sharingData.damageCount++] = {x, y, width, height};

            Debug::log(TRACE, "[sc] hl damage: {} {} {} {}", x, y, width, height);
        });
        sharingData.windowFrameCallback->setLinuxDmabuf([this, self = self](CCHyprlandToplevelExportFrameV1* r, uint32_t format, uint32_t width, uint32_t height) {
            Debug::log(TRACE, "[sc] hlOnDmabuf for {}", (void*)self.get());
            if (!self)
                return;

            sharingData.frameInfoDMA.w   = width;
            sharingData.frameInfoDMA.h   = height;
            sharingData.frameInfoDMA.fmt = format;
        });
        sharingData.windowFrameCallback->setBufferDone([this, self = self](CCHyprlandToplevelExportFrameV1* r) {
            Debug::log(TRACE, "[sc] hlOnBufferDone for {}", (void*)self.get());
            if (!self)
                return;

            const auto PSTREAM = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->streamFromSession(this);

            if (!PSTREAM) {
                Debug::log(TRACE, "[sc] hlOnBufferDone: no stream");
                sharingData.status = FRAME_NONE;
                sharingData.windowFrameCallback.reset();
                return;
            }

            Debug::log(TRACE, "[sc] pw format {} size {}x{}", (int)PSTREAM->pwVideoInfo.format, PSTREAM->pwVideoInfo.size.width, PSTREAM->pwVideoInfo.size.height);
            Debug::log(TRACE, "[sc] hl format {} size {}x{}", (int)sharingData.frameInfoSHM.fmt, sharingData.frameInfoSHM.w, sharingData.frameInfoSHM.h);
            Debug::log(TRACE, "[sc] hl format dma {} size {}x{}", (int)sharingData.frameInfoDMA.fmt, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h);

            const auto FMT = PSTREAM->isDMA ? sharingData.frameInfoDMA.fmt : sharingData.frameInfoSHM.fmt;

            auto [streamW, streamH] = g_pPortalManager->m_sPortals.screencopy->getStreamDimensions(this, sharingData.frameInfoDMA.w, sharingData.frameInfoDMA.h);

            const bool transformSwapsNow        = xdph::vulkan::transformSwapsDimensions(sharingData.transform);
            const bool transformSwappedBefore   = xdph::vulkan::transformSwapsDimensions(sharingData.lastTransform);
            const bool transformDimensionChange = g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(this) && (transformSwapsNow != transformSwappedBefore);

            if (transformDimensionChange) {
                Debug::log(LOG, "[sc] Transform changed from {} to {}, renegotiate stream", (int)sharingData.lastTransform, (int)sharingData.transform);
            }

            if ((PSTREAM->pwVideoInfo.format != pwFromDrmFourcc(FMT) && PSTREAM->pwVideoInfo.format != pwStripAlpha(pwFromDrmFourcc(FMT))) ||
                (PSTREAM->pwVideoInfo.size.width != streamW || PSTREAM->pwVideoInfo.size.height != streamH) || transformDimensionChange) {
                Debug::log(LOG, "[sc] Incompatible formats, renegotiate stream");
                sharingData.lastTransform = sharingData.transform;
                sharingData.status        = FRAME_RENEG;
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                sharingData.status = FRAME_NONE;
                sharingData.windowFrameCallback.reset();
                return;
            }

            sharingData.lastTransform = sharingData.transform;

            if (!PSTREAM->currentPWBuffer) {
                Debug::log(TRACE, "[sc] hlOnBufferDone: dequeue, no current buffer");
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->dequeue(this);
            }

            if (!PSTREAM->currentPWBuffer) {
                Debug::log(LOG, "[screencopy/pipewire] Out of buffers");
                sharingData.status = FRAME_NONE;
                if (sharingData.copyRetries++ < MAX_RETRIES) {
                    Debug::log(LOG, "[sc] Retrying screencopy ({}/{})", sharingData.copyRetries, MAX_RETRIES);
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                }
                sharingData.windowFrameCallback.reset();
                return;
            }

            SBuffer* copyTarget = PSTREAM->currentPWBuffer;
            if (g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(this)) {
                if (auto* captureBuffer = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->ensureSessionCaptureBuffer(PSTREAM); captureBuffer)
                    copyTarget = captureBuffer;
                else {
                    Debug::log(LOG, "[sc] No reusable rotation capture buffer available, skipping window frame");
                    sharingData.status = FRAME_NONE;
                    g_pPortalManager->m_sPortals.screencopy->queueNextShareFrame(this);
                    sharingData.windowFrameCallback.reset();
                    return;
                }
            }

            copyTarget->awaitingRelease = true;
            sharingData.windowFrameCallback->sendCopy(copyTarget->wlBuffer->resource(), false);
            sharingData.copyRetries = 0;

            Debug::log(TRACE, "[sc] hl frame copied");
        });
    }
}

void CScreencopyPortal::queueNextShareFrame(CScreencopyPortal::SSession* pSession) {
    if (usePipewireProcessScheduling(pSession)) {
        Debug::log(TRACE, "[screencopy] PipeWire will request the next rotated frame");
        return;
    }

    const auto PSTREAM = m_pPipewire->streamFromSession(pSession);

    if (PSTREAM && !PSTREAM->streamState)
        return;

    if (pSession->selection.type == TYPE_WINDOW && pSession->sharingData.status == FRAME_READY) {
        if (pSession->sharingData.damageCount == 0)
            pSession->sharingData.idleFrameStreak = std::min<uint32_t>(pSession->sharingData.idleFrameStreak + 1, 10'000);
        else
            pSession->sharingData.idleFrameStreak = 0;
    } else if (pSession->selection.type == TYPE_WINDOW) {
        pSession->sharingData.idleFrameStreak = 0;
    } else if (pSession->selection.type != TYPE_WINDOW) {
        pSession->sharingData.idleFrameStreak = 0;
    }

    // calculate frame delta and queue next frame
    const double targetFPS           = getTargetShareFPS(pSession);
    const auto   FRAMETOOKMS         = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - pSession->sharingData.begunFrame).count() / 1000.0;
    const auto   MSTILNEXTREFRESH    = 1000.0 / targetFPS - FRAMETOOKMS;
    pSession->sharingData.begunFrame = std::chrono::system_clock::now();

    Debug::log(TRACE, "[screencopy] target fps {:.2f}, frame took {:.2f}ms, ms till next refresh {:.2f}, estimated actual fps: {:.2f}", targetFPS, FRAMETOOKMS,
               MSTILNEXTREFRESH, std::clamp(1000.0 / std::max(FRAMETOOKMS, 0.001), 1.0, targetFPS));

    g_pPortalManager->addTimer(
        {std::clamp(MSTILNEXTREFRESH - 1.0 /* safezone */, 6.0, 1000.0), [pSession]() { g_pPortalManager->m_sPortals.screencopy->startFrameCopy(pSession); }});
}
bool CScreencopyPortal::hasToplevelCapabilities() {
    return m_sState.toplevel;
}

CScreencopyPortal::SSession* CScreencopyPortal::getSession(sdbus::ObjectPath& path) {
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

CScreencopyPortal::~CScreencopyPortal() = default;

bool CScreencopyPortal::shouldUseExtImageCopy(SSession* pSession) {
    if (!pSession || !shouldApplyGpuRotation(pSession))
        return false;

    if (pSession->selection.type != TYPE_OUTPUT && pSession->selection.type != TYPE_GEOMETRY)
        return false;

    return g_pPortalManager->m_sWaylandConnection.extImageCopyCaptureMgr && g_pPortalManager->m_sWaylandConnection.extOutputImageSourceMgr;
}

bool CScreencopyPortal::ensureExtImageCopySession(SSession* pSession) {
    if (!shouldUseExtImageCopy(pSession))
        return false;

    auto& sharingData = pSession->sharingData;

    if (!sharingData.extImageCopySession) {
        const auto POUTPUT = g_pPortalManager->getOutputFromName(pSession->selection.output);
        if (!POUTPUT) {
            Debug::log(ERR, "[extcopy] Output {} not found", pSession->selection.output);
            return false;
        }

        sharingData.extImageCopyConstraintsReady = false;
        sharingData.extImageCopyBufferW          = 0;
        sharingData.extImageCopyBufferH          = 0;
        sharingData.extImageCopyDMAFormat        = DRM_FORMAT_INVALID;
        sharingData.extImageCopyDMAModifier      = DRM_FORMAT_MOD_INVALID;
        sharingData.extImageCopySHMFormats.clear();
        sharingData.extImageCopyDMAFormats.clear();

        const auto options =
            pSession->cursorMode == EMBEDDED ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : static_cast<extImageCopyCaptureManagerV1Options>(0);

        sharingData.extImageSource = makeShared<CCExtImageCaptureSourceV1>(
            g_pPortalManager->m_sWaylandConnection.extOutputImageSourceMgr->sendCreateSource(POUTPUT->output->resource()));
        sharingData.extImageCopySession = makeShared<CCExtImageCopyCaptureSessionV1>(
            g_pPortalManager->m_sWaylandConnection.extImageCopyCaptureMgr->sendCreateSession(sharingData.extImageSource->resource(), options));

        sharingData.extImageCopySession->setBufferSize([pSession](CCExtImageCopyCaptureSessionV1*, uint32_t width, uint32_t height) {
            auto& data                  = pSession->sharingData;
            data.extImageCopyBufferW    = width;
            data.extImageCopyBufferH    = height;
        });

        sharingData.extImageCopySession->setShmFormat([pSession](CCExtImageCopyCaptureSessionV1*, uint32_t format) {
            const auto drmFormat = drmFourccFromSHM((wl_shm_format)format);
            if (!isRotatorCompatibleFormat(drmFormat))
                return;

            auto& formats = pSession->sharingData.extImageCopySHMFormats;
            if (std::find(formats.begin(), formats.end(), drmFormat) == formats.end())
                formats.push_back(drmFormat);
        });

        sharingData.extImageCopySession->setDmabufDevice([](CCExtImageCopyCaptureSessionV1*, wl_array*) {});

        sharingData.extImageCopySession->setDmabufFormat([pSession](CCExtImageCopyCaptureSessionV1*, uint32_t format, wl_array* modifiers) {
            if (!isRotatorCompatibleFormat(format))
                return;

            auto& pairs = pSession->sharingData.extImageCopyDMAFormats;
            auto* mod   = (uint64_t*)modifiers->data;
            const auto count = modifiers->size / sizeof(uint64_t);
            for (size_t i = 0; i < count; ++i) {
                const auto entry = std::pair<uint32_t, uint64_t>{format, mod[i]};
                if (std::find(pairs.begin(), pairs.end(), entry) == pairs.end())
                    pairs.push_back(entry);
            }
        });

        sharingData.extImageCopySession->setDone([pSession](CCExtImageCopyCaptureSessionV1*) {
            auto& sharingData = pSession->sharingData;

            std::pair<uint32_t, uint64_t> best = {DRM_FORMAT_INVALID, DRM_FORMAT_MOD_INVALID};
            for (const auto& pair : sharingData.extImageCopyDMAFormats) {
                if (best.first == DRM_FORMAT_INVALID ||
                    formatPreference(pair.first) < formatPreference(best.first) ||
                    (pair.first == best.first && modifierPreference(pair.second) < modifierPreference(best.second))) {
                    best = pair;
                }
            }

            if (best.first == DRM_FORMAT_INVALID || sharingData.extImageCopyBufferW == 0 || sharingData.extImageCopyBufferH == 0) {
                Debug::log(WARN, "[extcopy] No compatible DMA constraints received");
                sharingData.extImageCopyConstraintsReady = false;
                return;
            }

            sharingData.extImageCopyDMAFormat        = best.first;
            sharingData.extImageCopyDMAModifier      = best.second;
            sharingData.frameInfoDMA.w               = sharingData.extImageCopyBufferW;
            sharingData.frameInfoDMA.h               = sharingData.extImageCopyBufferH;
            sharingData.frameInfoDMA.fmt             = best.first;
            sharingData.frameInfoSHM.w               = sharingData.extImageCopyBufferW;
            sharingData.frameInfoSHM.h               = sharingData.extImageCopyBufferH;
            const uint32_t preferredSHM = drmFormatWithoutAlpha(best.first);
            const uint32_t selectedSHM  = pickPreferredSHMFormat(sharingData.extImageCopySHMFormats, preferredSHM);
            sharingData.frameInfoSHM.fmt = selectedSHM != DRM_FORMAT_INVALID ? selectedSHM : preferredSHM;
            sharingData.frameInfoSHM.stride          = sharingData.frameInfoSHM.w * drmBytesPerPixel(sharingData.frameInfoSHM.fmt);
            sharingData.frameInfoSHM.size            = sharingData.frameInfoSHM.stride * sharingData.frameInfoSHM.h;
            sharingData.extImageCopyConstraintsReady = true;

            Debug::log(LOG, "[extcopy] Constraints ready: {}x{} dma fmt {} modifier {:#x}, shm fmt {}", sharingData.extImageCopyBufferW, sharingData.extImageCopyBufferH,
                       sharingData.extImageCopyDMAFormat, sharingData.extImageCopyDMAModifier, sharingData.frameInfoSHM.fmt);
        });

        sharingData.extImageCopySession->setStopped([this, pSession](CCExtImageCopyCaptureSessionV1*) {
            Debug::log(WARN, "[extcopy] Capture session stopped");
            destroyExtImageCopySession(pSession);
        });
    }

    if (!sharingData.extImageCopyConstraintsReady)
        wl_display_roundtrip(g_pPortalManager->m_sWaylandConnection.display);

    return sharingData.extImageCopyConstraintsReady && sharingData.frameInfoDMA.fmt != DRM_FORMAT_INVALID;
}

void CScreencopyPortal::destroyExtImageCopySession(SSession* pSession) {
    if (!pSession)
        return;

    auto& sharingData = pSession->sharingData;
    sharingData.extImageCopyFrame.reset();
    sharingData.extImageCopySession.reset();
    sharingData.extImageSource.reset();
    sharingData.extImageCopyConstraintsReady = false;
    sharingData.extImageCopyBufferW          = 0;
    sharingData.extImageCopyBufferH          = 0;
    sharingData.extImageCopyDMAFormat        = DRM_FORMAT_INVALID;
    sharingData.extImageCopyDMAModifier      = DRM_FORMAT_MOD_INVALID;
    sharingData.extImageCopySHMFormats.clear();
    sharingData.extImageCopyDMAFormats.clear();
}

xdph::vulkan::VulkanRotator* CScreencopyPortal::getRotator() {
    static auto* const* PENABLEGPUROTATION = (Hyprlang::INT* const*)g_pPortalManager->m_sConfig.config->getConfigValuePtr("screencopy:enable_gpu_rotation")->getDataStaticPtr();

    if (!**PENABLEGPUROTATION)
        return nullptr;

    if (!m_pRotator) {
        m_pRotator = xdph::vulkan::VulkanRotator::create();
        if (!m_pRotator)
            Debug::log(WARN, "[screencopy] Failed to create VulkanRotator, rotation will be unavailable");
    }

    return m_pRotator.get();
}

void CScreencopyPortal::appendToplevelExport(SP<CCHyprlandToplevelExportManagerV1> proto) {
    m_sState.toplevel = proto;

    Debug::log(LOG, "[screencopy] Registered for toplevel export");
}

bool CScreencopyPortal::shouldApplyGpuRotation(SSession* pSession) {
    if (!pSession->sharingData.rotationRequested)
        return false;

    const bool needs = pSession->sharingData.transform != WL_OUTPUT_TRANSFORM_NORMAL;
    if (!needs)
        Debug::log(TRACE, "[sc] GPU rotation not needed (transform NORMAL)");
    else
        Debug::log(TRACE, "[sc] GPU rotation needed, transform {}", (int)pSession->sharingData.transform);
    return needs;
}

std::pair<uint32_t, uint32_t> CScreencopyPortal::getStreamDimensions(SSession* pSession, uint32_t physicalW, uint32_t physicalH) {
    if (shouldUseExtImageCopy(pSession) && pSession->selection.type == TYPE_GEOMETRY)
        return {pSession->selection.w, pSession->selection.h};

    if (!shouldApplyGpuRotation(pSession) || !pSession->sharingData.rotationReady)
        return {physicalW, physicalH};

    if (xdph::vulkan::transformSwapsDimensions(pSession->sharingData.transform))
        return {physicalH, physicalW};

    return {physicalW, physicalH};
}

static double getTargetShareFPS(CScreencopyPortal::SSession* pSession) {
    const double configuredFPS = std::max<uint32_t>(pSession->sharingData.framerate, 1);

    if (pSession->selection.type != TYPE_WINDOW)
        return configuredFPS;

    const double idleSeconds = pSession->sharingData.idleFrameStreak / configuredFPS;

    if (idleSeconds >= 2.0)
        return std::max(5.0, configuredFPS / 4.0);

    if (idleSeconds >= 0.5)
        return std::max(15.0, configuredFPS / 2.0);

    return configuredFPS;
}

bool CPipewireConnection::good() {
    return m_pContext && m_pCore;
}

CPipewireConnection::CPipewireConnection() {
    m_pContext = pw_context_new(g_pPortalManager->m_sPipewire.loop, nullptr, 0);

    if (!m_pContext) {
        Debug::log(ERR, "[pipewire] pw didn't allow for a context");
        return;
    }

    m_pCore = pw_context_connect(m_pContext, nullptr, 0);

    if (!m_pCore) {
        Debug::log(ERR, "[pipewire] pw didn't allow for a context connection");
        return;
    }

    Debug::log(LOG, "[pipewire] connected");
}

void CPipewireConnection::removeSessionFrameCallbacks(CScreencopyPortal::SSession* pSession) {
    Debug::log(TRACE, "[pipewire] removeSessionFrameCallbacks called");

    const size_t captureIndex = pSession->sharingData.rotationCaptureBufferActiveIndex;
    if (captureIndex < pSession->sharingData.rotationCaptureBuffers.size()) {
        if (const auto& captureBuffer = pSession->sharingData.rotationCaptureBuffers[captureIndex]; captureBuffer)
            captureBuffer->awaitingRelease = false;
    }

    pSession->sharingData.extImageCopyFrame.reset();
    pSession->sharingData.frameCallback.reset();
    pSession->sharingData.windowFrameCallback.reset();

    pSession->sharingData.extImageCopyFrame    = nullptr;
    pSession->sharingData.windowFrameCallback = nullptr;
    pSession->sharingData.frameCallback       = nullptr;

    pSession->sharingData.status = FRAME_NONE;
}

CPipewireConnection::~CPipewireConnection() {
    if (m_pCore)
        pw_core_disconnect(m_pCore);
    if (m_pContext)
        pw_context_destroy(m_pContext);
}

// --------------- Pipewire Stream Handlers --------------- //

static void pwStreamStateChange(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
    const auto PSTREAM = (CPipewireConnection::SPWStream*)data;
    const bool useProcessScheduling = usePipewireProcessScheduling(PSTREAM->pSession);

    PSTREAM->pSession->sharingData.nodeID = pw_stream_get_node_id(PSTREAM->stream);

    Debug::log(TRACE, "[pw] pwStreamStateChange on {} from {} to {}, node id {}", (void*)PSTREAM, pw_stream_state_as_string(old), pw_stream_state_as_string(state),
               PSTREAM->pSession->sharingData.nodeID);

    switch (state) {
        case PW_STREAM_STATE_STREAMING:
            PSTREAM->streamState = true;
            if (!useProcessScheduling) {
                if (PSTREAM->pSession->sharingData.status == FRAME_NONE)
                    g_pPortalManager->m_sPortals.screencopy->startFrameCopy(PSTREAM->pSession);
                else {
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->removeSessionFrameCallbacks(PSTREAM->pSession);
                    g_pPortalManager->m_sPortals.screencopy->startFrameCopy(PSTREAM->pSession);
                }
            }
            break;
        case PW_STREAM_STATE_PAUSED:
            if (old == PW_STREAM_STATE_STREAMING && useProcessScheduling) {
                if (PSTREAM->currentPWBuffer) {
                    Debug::log(LOG, "[pw] PAUSED: queue outstanding process-scheduled buffer");
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->enqueue(PSTREAM->pSession);
                }
            }
            PSTREAM->streamState = false;
            if (!useProcessScheduling)
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->removeSessionFrameCallbacks(PSTREAM->pSession);
            break;
        default: {
            PSTREAM->streamState = false;
            g_pPortalManager->m_sPortals.screencopy->m_pPipewire->removeSessionFrameCallbacks(PSTREAM->pSession);
            break;
        }
    }

    if (state == PW_STREAM_STATE_UNCONNECTED) {
        g_pPortalManager->m_sPortals.screencopy->m_pPipewire->removeSessionFrameCallbacks(PSTREAM->pSession);
        g_pPortalManager->m_sPortals.screencopy->m_pPipewire->destroyStream(PSTREAM->pSession);
    }
}

static void pwStreamProcess(void* data) {
    const auto PSTREAM = (CPipewireConnection::SPWStream*)data;

    if (!PSTREAM->streamState) {
        Debug::log(TRACE, "[pw] process ignored, stream not active");
        return;
    }

    if (!usePipewireProcessScheduling(PSTREAM->pSession)) {
        Debug::log(TRACE, "[pw] process ignored, session uses timer scheduling");
        return;
    }

    if (PSTREAM->currentPWBuffer) {
        Debug::log(TRACE, "[pw] process ignored, buffer already dequeued");
        return;
    }

    if (PSTREAM->pSession->sharingData.status != FRAME_NONE) {
        Debug::log(TRACE, "[pw] process ignored, frame already in progress with status {}", (int)PSTREAM->pSession->sharingData.status);
        return;
    }

    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->dequeue(PSTREAM->pSession);

    if (!PSTREAM->currentPWBuffer) {
        Debug::log(TRACE, "[pw] process: no buffer available");
        return;
    }

    Debug::log(TRACE, "[pw] process: starting new capture");
    g_pPortalManager->m_sPortals.screencopy->startFrameCopy(PSTREAM->pSession);
}

static void pwStreamParamChanged(void* data, uint32_t id, const spa_pod* param) {
    const auto PSTREAM = (CPipewireConnection::SPWStream*)data;

    Debug::log(TRACE, "[pw] pwStreamParamChanged on {}", (void*)PSTREAM);

    if (id != SPA_PARAM_Format || !param) {
        Debug::log(TRACE, "[pw] invalid call in pwStreamParamChanged");
        return;
    }

    spa_pod_dynamic_builder dynBuilder[3];
    const spa_pod*          params[4];
    uint8_t                 params_buffer[3][1024];

    spa_pod_dynamic_builder_init(&dynBuilder[0], params_buffer[0], sizeof(params_buffer[0]), 2048);
    spa_pod_dynamic_builder_init(&dynBuilder[1], params_buffer[1], sizeof(params_buffer[1]), 2048);
    spa_pod_dynamic_builder_init(&dynBuilder[2], params_buffer[2], sizeof(params_buffer[2]), 2048);

    spa_format_video_raw_parse(param, &PSTREAM->pwVideoInfo);
    Debug::log(TRACE, "[pw] Framerate: {}/{}", PSTREAM->pwVideoInfo.max_framerate.num, PSTREAM->pwVideoInfo.max_framerate.denom);
    PSTREAM->pSession->sharingData.framerate = PSTREAM->pwVideoInfo.max_framerate.num / PSTREAM->pwVideoInfo.max_framerate.denom;
    const uint32_t requestedFramerate = std::max<uint32_t>(PSTREAM->pSession->sharingData.framerate, 1);
    const uint32_t targetFramerate    = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(getTargetShareFPS(PSTREAM->pSession))));
    if (targetFramerate != requestedFramerate)
        Debug::log(LOG, "[pw] Capping rotated framerate from {} to {} for {}x{}", requestedFramerate, targetFramerate, PSTREAM->pwVideoInfo.size.width,
                   PSTREAM->pwVideoInfo.size.height);
    PSTREAM->pSession->sharingData.framerate = targetFramerate;

    PSTREAM->isDMA             = false;
    uint32_t                   data_type = 1 << SPA_DATA_MemFd;

    const struct spa_pod_prop* prop_modifier;
    if ((prop_modifier = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier))) {
        Debug::log(TRACE, "[pipewire] pw requested dmabuf");
        PSTREAM->isDMA = true;
        data_type      = 1 << SPA_DATA_DmaBuf;

        RASSERT(PSTREAM->pwVideoInfo.format == pwFromDrmFourcc(PSTREAM->pSession->sharingData.frameInfoDMA.fmt), "invalid format in dma pw param change");

        if ((prop_modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) > 0) {
            Debug::log(TRACE, "[pw] don't fixate");
            const spa_pod* pod_modifier = &prop_modifier->value;

            uint32_t       n_modifiers = SPA_POD_CHOICE_N_VALUES(pod_modifier) - 1;
            uint64_t*      modifiers   = (uint64_t*)SPA_POD_CHOICE_VALUES(pod_modifier);
            modifiers++;
            uint32_t         flags = GBM_BO_USE_RENDERING;
            uint64_t         modifier;
            uint32_t         n_params;
            spa_pod_builder* builder[2] = {&dynBuilder[0].b, &dynBuilder[1].b};

            gbm_bo*          bo =
                gbm_bo_create_with_modifiers2(g_pPortalManager->m_sWaylandConnection.gbmDevice, PSTREAM->pSession->sharingData.frameInfoDMA.w,
                                              PSTREAM->pSession->sharingData.frameInfoDMA.h, PSTREAM->pSession->sharingData.frameInfoDMA.fmt, modifiers, n_modifiers, flags);
            if (bo) {
                modifier = gbm_bo_get_modifier(bo);
                gbm_bo_destroy(bo);
                goto fixate_format;
            }

            Debug::log(TRACE, "[pw] unable to allocate a dmabuf with modifiers. Falling back to the old api");
            for (uint32_t i = 0; i < n_modifiers; i++) {
                switch (modifiers[i]) {
                    case DRM_FORMAT_MOD_INVALID:
                        flags =
                            GBM_BO_USE_RENDERING; // ;cast->ctx->state->config->screencast_conf.force_mod_linear ? GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR : GBM_BO_USE_RENDERING;
                        break;
                    case DRM_FORMAT_MOD_LINEAR: flags = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR; break;
                    default: continue;
                }
                bo = gbm_bo_create(g_pPortalManager->m_sWaylandConnection.gbmDevice, PSTREAM->pSession->sharingData.frameInfoDMA.w, PSTREAM->pSession->sharingData.frameInfoDMA.h,
                                   PSTREAM->pSession->sharingData.frameInfoDMA.fmt, flags);
                if (bo) {
                    modifier = gbm_bo_get_modifier(bo);
                    gbm_bo_destroy(bo);
                    goto fixate_format;
                }
            }

            Debug::log(ERR, "[pw] failed to alloc dma");
            return;

        fixate_format:
            auto [streamW, streamH] = g_pPortalManager->m_sPortals.screencopy->getStreamDimensions(PSTREAM->pSession, PSTREAM->pSession->sharingData.frameInfoDMA.w,
                                                                                                     PSTREAM->pSession->sharingData.frameInfoDMA.h);

            params[0] = fixate_format(&dynBuilder[2].b, pwFromDrmFourcc(PSTREAM->pSession->sharingData.frameInfoDMA.fmt), streamW, streamH,
                                      targetFramerate, &modifier);

            n_params = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->buildFormatsFor(builder, &params[1], PSTREAM);
            n_params++;

            pw_stream_update_params(PSTREAM->stream, params, n_params);
            spa_pod_dynamic_builder_clean(&dynBuilder[0]);
            spa_pod_dynamic_builder_clean(&dynBuilder[1]);
            spa_pod_dynamic_builder_clean(&dynBuilder[2]);

            Debug::log(TRACE, "[pw] Format fixated:");
            Debug::log(TRACE, "[pw]  | buffer_type {}", "DMA (No fixate)");
            Debug::log(TRACE, "[pw]  | format: {}", (int)PSTREAM->pwVideoInfo.format);
            Debug::log(TRACE, "[pw]  | modifier: {}", PSTREAM->pwVideoInfo.modifier);
            Debug::log(TRACE, "[pw]  | size: {}x{}", PSTREAM->pwVideoInfo.size.width, PSTREAM->pwVideoInfo.size.height);
            Debug::log(TRACE, "[pw]  | framerate {}", targetFramerate);

            return;
        }
    }

    Debug::log(LOG, "[pw] Format renegotiated: buffer_type={} format={} modifier={} size={}x{} framerate={}", PSTREAM->isDMA ? "DMA" : "SHM",
               (int)PSTREAM->pwVideoInfo.format, PSTREAM->pwVideoInfo.modifier, PSTREAM->pwVideoInfo.size.width, PSTREAM->pwVideoInfo.size.height,
               PSTREAM->pSession->sharingData.framerate);

    uint32_t blocks       = 1;
    uint32_t bufferStride = 0;
    uint32_t bufferSize   = 0;
    uint32_t streamW      = PSTREAM->pwVideoInfo.size.width;
    uint32_t streamH      = PSTREAM->pwVideoInfo.size.height;

    if (PSTREAM->isDMA) {
        const uint32_t bpp = drmBytesPerPixel(PSTREAM->pSession->sharingData.frameInfoDMA.fmt);
        if (bpp > 0 && streamW > 0 && streamH > 0) {
            bufferStride = streamW * bpp;
            bufferSize   = bufferStride * streamH;
        }
    } else if (g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(PSTREAM->pSession)) {
        const uint32_t shmFormat =
            PSTREAM->pwVideoInfo.format != SPA_VIDEO_FORMAT_UNKNOWN ? drmFourccFromPW(PSTREAM->pwVideoInfo.format) : PSTREAM->pSession->sharingData.frameInfoSHM.fmt;
        const uint32_t bpp = drmBytesPerPixel(shmFormat);
        if (bpp > 0 && streamW > 0 && streamH > 0) {
            bufferStride = alignPipewireStride(streamW * bpp);
            bufferSize   = bufferStride * streamH;
        }
    }

    Debug::log(LOG, "[pw] Advertising buffer layout: data_type={} size={} stride={}", PSTREAM->isDMA ? "DMA" : "SHM", bufferSize, bufferStride);

    params[0] = build_buffer(&dynBuilder[0].b, blocks, bufferSize, bufferStride, data_type);

    params[1] = (const spa_pod*)spa_pod_builder_add_object(&dynBuilder[1].b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                                           SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

    params[2] = (const spa_pod*)spa_pod_builder_add_object(&dynBuilder[1].b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
                                                           SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_videotransform)));

    params[3] = (const spa_pod*)spa_pod_builder_add_object(
        &dynBuilder[2].b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage), SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * 4, sizeof(struct spa_meta_region) * 1, sizeof(struct spa_meta_region) * 4));
    uint32_t paramCount = 4;

    pw_stream_update_params(PSTREAM->stream, params, paramCount);
    spa_pod_dynamic_builder_clean(&dynBuilder[0]);
    spa_pod_dynamic_builder_clean(&dynBuilder[1]);
    spa_pod_dynamic_builder_clean(&dynBuilder[2]);
}

static void pwStreamAddBuffer(void* data, pw_buffer* buffer) {
    const auto PSTREAM = (CPipewireConnection::SPWStream*)data;

    Debug::log(TRACE, "[pw] pwStreamAddBuffer with {} on {}", (void*)buffer, (void*)PSTREAM);

    spa_data*     spaData = buffer->buffer->datas;
    spa_data_type type;
    uint32_t      flags = SPA_DATA_FLAG_READABLE;

    if ((spaData[0].type & (1u << SPA_DATA_MemFd)) > 0) {
        type = SPA_DATA_MemFd;
        Debug::log(WARN, "[pipewire] Asked for a wl_shm buffer which is legacy.");
#ifdef SPA_DATA_FLAG_MAPPABLE
        flags |= SPA_DATA_FLAG_MAPPABLE;
#endif
    } else if ((spaData[0].type & (1u << SPA_DATA_DmaBuf)) > 0) {
        type = SPA_DATA_DmaBuf;
    } else {
        Debug::log(ERR, "[pipewire] wrong format in addbuffer");
        return;
    }

    PSTREAM->isDMA = type == SPA_DATA_DmaBuf;

    auto newBuf = g_pPortalManager->m_sPortals.screencopy->m_pPipewire->createBuffer(PSTREAM, type == SPA_DATA_DmaBuf);

    if (!newBuf) {
        Debug::log(ERR, "[pw] createBuffer failed, skipping addBuffer for {}", (void*)buffer);
        return;
    }

    const auto PBUFFER = newBuf.get();
    PSTREAM->buffers.emplace_back(std::move(newBuf));

    PBUFFER->pwBuffer = buffer;
    buffer->user_data = PBUFFER;

    Debug::log(TRACE, "[pw] buffer datas {}", buffer->buffer->n_datas);

    for (uint32_t plane = 0; plane < buffer->buffer->n_datas; plane++) {
        spaData[plane].type          = type;
        spaData[plane].maxsize       = PBUFFER->size[plane];
        spaData[plane].mapoffset     = 0;
        if (!spaData[plane].chunk) {
            Debug::log(ERR, "[pw] missing chunk metadata on plane {}", plane);
            continue;
        }

        spaData[plane].chunk->size   = PBUFFER->size[plane];
        spaData[plane].chunk->stride = PBUFFER->stride[plane];
        spaData[plane].chunk->offset = PBUFFER->offset[plane];
        spaData[plane].flags         = flags;
        spaData[plane].fd            = PBUFFER->fd[plane];
        spaData[plane].data          = NULL;
        // clients have implemented to check chunk->size if the buffer is valid instead
        // of using the flags. Until they are patched we should use some arbitrary value.
        if (PBUFFER->isDMABUF && spaData[plane].chunk->size == 0) {
            spaData[plane].chunk->size = 9; // This was choosen by a fair d20.
        }
    }
}

static void pwStreamRemoveBuffer(void* data, pw_buffer* buffer) {
    const auto PSTREAM = (CPipewireConnection::SPWStream*)data;
    const auto PBUFFER = (SBuffer*)buffer->user_data;

    Debug::log(TRACE, "[pw] pwStreamRemoveBuffer with {} on {}", (void*)buffer, (void*)PSTREAM);

    if (!PBUFFER)
        return;

    if (PSTREAM->currentPWBuffer == PBUFFER)
        PSTREAM->currentPWBuffer = nullptr;

    for (uint32_t plane = 0; plane < buffer->buffer->n_datas; plane++) {
        buffer->buffer->datas[plane].fd = -1;
    }

    std::erase_if(PSTREAM->buffers, [&](const auto& other) { return other.get() == PBUFFER; });

    buffer->user_data = nullptr;
}

static const pw_stream_events pwStreamEvents = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .state_changed = pwStreamStateChange,
    .param_changed = pwStreamParamChanged,
    .add_buffer    = pwStreamAddBuffer,
    .remove_buffer = pwStreamRemoveBuffer,
    .process       = pwStreamProcess,
};

// ------------------------------------------------------- //

void CPipewireConnection::createStream(CScreencopyPortal::SSession* pSession) {
    const auto PSTREAM = m_vStreams.emplace_back(std::make_unique<SPWStream>(pSession)).get();
    const bool useProcessScheduling = usePipewireProcessScheduling(pSession);

    pw_loop_enter(g_pPortalManager->m_sPipewire.loop);

    uint8_t                 buffer[2][1024];
    spa_pod_dynamic_builder dynBuilder[2];
    spa_pod_dynamic_builder_init(&dynBuilder[0], buffer[0], sizeof(buffer[0]), 2048);
    spa_pod_dynamic_builder_init(&dynBuilder[1], buffer[1], sizeof(buffer[1]), 2048);

    const std::string NAME = getRandName("xdph-streaming-");

    PSTREAM->stream = pw_stream_new(m_pCore, NAME.c_str(), pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", nullptr));

    Debug::log(TRACE, "[pw] New stream name {}", NAME);

    if (!PSTREAM->stream) {
        Debug::log(ERR, "[pipewire] refused to create stream");
        g_pPortalManager->terminate();
        return;
    }

    spa_pod_builder* builder[2] = {&dynBuilder[0].b, &dynBuilder[1].b};
    const spa_pod*   params[2];
    const auto       PARAMCOUNT = buildFormatsFor(builder, params, PSTREAM);

    spa_pod_dynamic_builder_clean(&dynBuilder[0]);
    spa_pod_dynamic_builder_clean(&dynBuilder[1]);

    pw_stream_add_listener(PSTREAM->stream, &PSTREAM->streamListener, &pwStreamEvents, PSTREAM);

    const auto streamFlags =
        useProcessScheduling ? (pw_stream_flags)(PW_STREAM_FLAG_ALLOC_BUFFERS) : (pw_stream_flags)(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS);

    Debug::log(LOG, "[pw] Connecting stream with {} scheduling", useProcessScheduling ? "PipeWire process" : "driver/timer");

    pw_stream_connect(PSTREAM->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, streamFlags, params, PARAMCOUNT);

    pSession->sharingData.nodeID = pw_stream_get_node_id(PSTREAM->stream);

    Debug::log(TRACE, "[pw] Stream got nodeid {}", pSession->sharingData.nodeID);
}

void CPipewireConnection::destroyStream(CScreencopyPortal::SSession* pSession) {
    // Disconnecting the stream can cause reentrance to this function.
    if (pSession->sharingData.active == false)
        return;
    pSession->sharingData.active = false;

    const auto PSTREAM = streamFromSession(pSession);

    if (!PSTREAM || !PSTREAM->stream)
        return;

    if (!PSTREAM->buffers.empty()) {
        std::vector<SBuffer*> bufs;

        for (auto& b : PSTREAM->buffers) {
            bufs.push_back(b.get());
        }

        for (auto& b : bufs) {
            pwStreamRemoveBuffer(PSTREAM, b->pwBuffer);
        }
    }

    pw_stream_flush(PSTREAM->stream, false);
    pw_stream_disconnect(PSTREAM->stream);
    pw_stream_destroy(PSTREAM->stream);

    std::erase_if(m_vStreams, [&](const auto& other) { return other.get() == PSTREAM; });
}

static bool wlr_query_dmabuf_modifiers(uint32_t drm_format, uint32_t num_modifiers, uint64_t* modifiers, uint32_t* max_modifiers) {
    if (g_pPortalManager->m_vDMABUFMods.empty())
        return false;

    if (num_modifiers == 0) {
        *max_modifiers = 0;
        for (auto& mod : g_pPortalManager->m_vDMABUFMods) {
            if (mod.fourcc == drm_format &&
                (mod.mod == DRM_FORMAT_MOD_INVALID || gbm_device_get_format_modifier_plane_count(g_pPortalManager->m_sWaylandConnection.gbmDevice, mod.fourcc, mod.mod) > 0))
                (*max_modifiers)++;
        }
        return true;
    }

    size_t i = 0;
    for (const auto& mod : g_pPortalManager->m_vDMABUFMods) {
        if (i >= num_modifiers)
            break;

        if (mod.fourcc == drm_format &&
            (mod.mod == DRM_FORMAT_MOD_INVALID || gbm_device_get_format_modifier_plane_count(g_pPortalManager->m_sWaylandConnection.gbmDevice, mod.fourcc, mod.mod) > 0)) {
            modifiers[i] = mod.mod;
            ++i;
        }
    }

    *max_modifiers = num_modifiers;
    return true;
}

static bool build_modifierlist(CPipewireConnection::SPWStream* stream, uint32_t drm_format, uint64_t** modifiers, uint32_t* modifier_count) {
    // When GPU rotation is requested we force linear (INVALID modifier) to keep import/export simple
    // and avoid compositors selecting tiled/AFBC modifiers our Vulkan path can't currently handle.
    if (stream && stream->pSession && stream->pSession->sharingData.rotationRequested) {
        *modifier_count = 1;
        *modifiers      = (uint64_t*)calloc(1, sizeof(uint64_t));
        if (!*modifiers)
            return false;
        (*modifiers)[0] = DRM_FORMAT_MOD_INVALID; // linear
        Debug::log(TRACE, "[pw] build_modifierlist: forcing linear modifier for gpu rotation");
        return true;
    }

    if (!wlr_query_dmabuf_modifiers(drm_format, 0, nullptr, modifier_count)) {
        *modifiers      = NULL;
        *modifier_count = 0;
        return false;
    }
    if (*modifier_count == 0) {
        Debug::log(ERR, "[pw] build_modifierlist: no mods");
        *modifiers = NULL;
        return true;
    }
    *modifiers = (uint64_t*)calloc(*modifier_count, sizeof(uint64_t));
    bool ret   = wlr_query_dmabuf_modifiers(drm_format, *modifier_count, *modifiers, modifier_count);
    Debug::log(TRACE, "[pw] build_modifierlist: count {}", *modifier_count);
    return ret;
}

static uint32_t drmBytesPerPixel(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_RGBX1010102:
        case DRM_FORMAT_BGRX1010102:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_RGBA1010102:
        case DRM_FORMAT_BGRA1010102: return 4;
        case DRM_FORMAT_BGR888: return 3;
        default: return 0;
    }
}

static uint32_t drmFormatWithoutAlpha(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ARGB8888: return DRM_FORMAT_XRGB8888;
        case DRM_FORMAT_ABGR8888: return DRM_FORMAT_XBGR8888;
        case DRM_FORMAT_RGBA8888: return DRM_FORMAT_RGBX8888;
        case DRM_FORMAT_BGRA8888: return DRM_FORMAT_BGRX8888;
        case DRM_FORMAT_ARGB2101010: return DRM_FORMAT_XRGB2101010;
        case DRM_FORMAT_ABGR2101010: return DRM_FORMAT_XBGR2101010;
        case DRM_FORMAT_RGBA1010102: return DRM_FORMAT_RGBX1010102;
        case DRM_FORMAT_BGRA1010102: return DRM_FORMAT_BGRX1010102;
        default: return drmFormat;
    }
}

uint32_t CPipewireConnection::buildFormatsFor(spa_pod_builder* b[2], const spa_pod* params[2], CPipewireConnection::SPWStream* stream) {
    uint32_t  paramCount = 0;
    uint32_t  modCount   = 0;
    uint64_t* modifiers  = nullptr;
    const uint32_t targetFramerate = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(getTargetShareFPS(stream->pSession))));

    auto [dmaW, dmaH] =
        g_pPortalManager->m_sPortals.screencopy->getStreamDimensions(stream->pSession, stream->pSession->sharingData.frameInfoDMA.w, stream->pSession->sharingData.frameInfoDMA.h);
    auto [shmW, shmH] =
        g_pPortalManager->m_sPortals.screencopy->getStreamDimensions(stream->pSession, stream->pSession->sharingData.frameInfoSHM.w, stream->pSession->sharingData.frameInfoSHM.h);

    if (g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(stream->pSession)) {
        const uint32_t rotatedStreamFmt = drmFormatWithoutAlpha(stream->pSession->sharingData.frameInfoSHM.fmt);
        Debug::log(LOG, "[pw] Building SHM-only formats for rotated session (stream size {}x{}, drm fmt {} -> {}, fps {})", shmW, shmH,
                   stream->pSession->sharingData.frameInfoSHM.fmt, rotatedStreamFmt, targetFramerate);
        params[0] = build_format(b[0], pwFromDrmFourcc(rotatedStreamFmt), shmW, shmH, targetFramerate, NULL, 0);
        return 1;
    }

    if (build_modifierlist(stream, stream->pSession->sharingData.frameInfoDMA.fmt, &modifiers, &modCount) && modCount > 0) {
        Debug::log(LOG, "[pw] Building modifiers for dma (stream size {}x{}, physical {}x{})", dmaW, dmaH, stream->pSession->sharingData.frameInfoDMA.w,
                   stream->pSession->sharingData.frameInfoDMA.h);

        paramCount = 2;
        params[0]  = build_format(b[0], pwFromDrmFourcc(stream->pSession->sharingData.frameInfoDMA.fmt), dmaW, dmaH, targetFramerate, modifiers, modCount);
        assert(params[0] != NULL);
        params[1] = build_format(b[1], pwFromDrmFourcc(stream->pSession->sharingData.frameInfoSHM.fmt), shmW, shmH, targetFramerate, NULL, 0);
        assert(params[1] != NULL);
    } else {
        Debug::log(LOG, "[pw] Building modifiers for shm (stream size {}x{})", shmW, shmH);

        paramCount = 1;
        params[0]  = build_format(b[0], pwFromDrmFourcc(stream->pSession->sharingData.frameInfoSHM.fmt), shmW, shmH, targetFramerate, NULL, 0);
    }

    if (modifiers)
        free(modifiers);

    return paramCount;
}

bool CPipewireConnection::buildModListFor(CPipewireConnection::SPWStream* stream, uint32_t drmFmt, uint64_t** mods, uint32_t* modCount) {
    return true;
}

CPipewireConnection::SPWStream* CPipewireConnection::streamFromSession(CScreencopyPortal::SSession* pSession) {
    for (auto& s : m_vStreams) {
        if (s->pSession == pSession)
            return s.get();
    }
    return nullptr;
}

SBuffer* CPipewireConnection::ensureSessionCaptureBuffer(CPipewireConnection::SPWStream* pStream) {
    const auto& frame   = pStream->pSession->sharingData.frameInfoDMA;
    auto& sharingData   = pStream->pSession->sharingData;

    auto needsRecreate = [&](const std::unique_ptr<SBuffer>& buf) {
        return !buf || !buf->isDMABUF || buf->w != frame.w || buf->h != frame.h || buf->fmt != frame.fmt;
    };

    for (size_t attempt = 0; attempt < sharingData.rotationCaptureBuffers.size(); ++attempt) {
        const size_t slotIndex = (sharingData.rotationCaptureBufferIndex + attempt) % sharingData.rotationCaptureBuffers.size();
        auto&        captureBuffer = sharingData.rotationCaptureBuffers[slotIndex];

        if (captureBuffer && captureBuffer->awaitingRelease)
            continue;

        if (needsRecreate(captureBuffer)) {
            captureBuffer = createBuffer(pStream, true, false);
            if (!captureBuffer) {
                Debug::log(ERR, "[pw] Failed to create rotation capture buffer for slot {}", slotIndex);
                return nullptr;
            }

            Debug::log(LOG, "[pw] Prepared rotation capture buffer slot {} {}x{} fmt {} type=dma", slotIndex, frame.w, frame.h, frame.fmt);
        }

        sharingData.rotationCaptureBufferIndex       = (slotIndex + 1) % sharingData.rotationCaptureBuffers.size();
        sharingData.rotationCaptureBufferActiveIndex = slotIndex;
        return captureBuffer.get();
    }

    Debug::log(WARN, "[pw] No released rotation capture buffer available across {} slots", sharingData.rotationCaptureBuffers.size());
    return nullptr;
}

void CPipewireConnection::enqueue(CScreencopyPortal::SSession* pSession) {
    const auto PSTREAM = streamFromSession(pSession);

    if (!PSTREAM) {
        Debug::log(ERR, "[pw] Attempted enqueue on invalid session??");
        return;
    }

    Debug::log(TRACE, "[pw] enqueue on {}", (void*)PSTREAM);

    if (!PSTREAM->currentPWBuffer) {
        Debug::log(LOG, "[pipewire] enqueue without a dequeued buffer, dropping stale frame state");
        pSession->sharingData.status = FRAME_NONE;
        return;
    }

    spa_buffer* spaBuf  = PSTREAM->currentPWBuffer->pwBuffer->buffer;
    SBuffer* rotationSource = PSTREAM->currentPWBuffer;
    const size_t captureIndex = pSession->sharingData.rotationCaptureBufferActiveIndex;
    if (captureIndex < pSession->sharingData.rotationCaptureBuffers.size()) {
        if (const auto& captureBuffer = pSession->sharingData.rotationCaptureBuffers[captureIndex]; captureBuffer)
            rotationSource = captureBuffer.get();
    }

    const bool CORRUPT = PSTREAM->pSession->sharingData.status != FRAME_READY;
    if (CORRUPT)
        Debug::log(TRACE, "[pw] buffer corrupt");

    int rotatedFd = -1;
    bool rotationApplied = false;
    const bool outputIsDMABuffer = PSTREAM->currentPWBuffer->isDMABUF;
    const bool sourceIsUsable = rotationSource && ((rotationSource->isDMABUF && rotationSource->fd[0] >= 0) || (!rotationSource->isDMABUF && rotationSource->mapped));
    const bool needsRotation = g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(pSession) && sourceIsUsable && !CORRUPT;

    if (!needsRotation)
        Debug::log(LOG, "[pw] GPU rotation bypass: outputDMABUF={} sourceDMABUF={} sourceMapped={} corrupt={} requested={} transform={}", outputIsDMABuffer,
                   rotationSource ? rotationSource->isDMABUF : false, rotationSource ? rotationSource->mapped != nullptr : false, CORRUPT,
                   pSession->sharingData.rotationRequested, (int)pSession->sharingData.transform);

    if (needsRotation) {
        auto* rotator = g_pPortalManager->m_sPortals.screencopy->getRotator();
        if (rotator && rotationSource->planeCount == 1) {
            xdph::vulkan::FrameInput input{};
            input.dmaBufFd  = rotationSource->fd[0];
            input.width     = rotationSource->w;
            input.height    = rotationSource->h;
            input.stride    = rotationSource->stride[0];
            input.format    = rotationSource->fmt;
            input.modifier  = rotationSource->modifier;
            input.transform = pSession->sharingData.transform;
            Debug::log(LOG, "[pw] GPU rotation enqueue transform={} w={} h={} stride={} modifier={:#x}", (int)input.transform, input.width, input.height,
                       input.stride, input.modifier);
            input.region    = getLogicalCaptureRegion(pSession, input.width, input.height);

            if (!rotationSource->isDMABUF) {
                input.hostSrcData   = rotationSource->mapped;
                input.hostSrcStride = rotationSource->stride[0];
                input.hostSrcSize   = rotationSource->size[0];
            }

            if (!outputIsDMABuffer) {
                input.hostDstData   = PSTREAM->currentPWBuffer->mapped;
                input.hostDstStride = PSTREAM->currentPWBuffer->stride[0];
                input.hostDstSize   = PSTREAM->currentPWBuffer->size[0];
            }

            auto result = rotator->process(input);
            if (result.success) {
                rotationApplied = true;
                if (outputIsDMABuffer) {
                    rotatedFd                      = result.dmaBufFd;
                    spaBuf->datas[0].fd            = rotatedFd;
                    spaBuf->datas[0].maxsize       = result.size;
                    spaBuf->datas[0].chunk->stride = result.stride;
                    spaBuf->datas[0].chunk->size   = result.size;
                } else {
                    spaBuf->datas[0].chunk->stride = PSTREAM->currentPWBuffer->stride[0];
                    spaBuf->datas[0].chunk->size   = PSTREAM->currentPWBuffer->size[0];
                }

                Debug::log(TRACE, "[pw] Applied GPU rotation transform {}", (int)pSession->sharingData.transform);
                const bool firstReady = !pSession->sharingData.rotationReady;
                pSession->sharingData.rotationReady = true;

                if (firstReady)
                    g_pPortalManager->m_sPortals.screencopy->m_pPipewire->updateStreamParam(PSTREAM);
            } else {
                Debug::log(WARN, "[pw] GPU rotation failed: {}", result.error);
                pSession->sharingData.rotationReady = false;
            }
        }
    }

    Debug::log(TRACE, "[pw] Enqueue data:");

    spa_meta_header* header = (spa_meta_header*)spa_buffer_find_meta_data(spaBuf, SPA_META_Header, sizeof(*header));
    if (header) {
        header->pts        = PSTREAM->pSession->sharingData.tvTimestampNs;
        header->flags      = CORRUPT ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
        header->seq        = PSTREAM->seq++;
        header->dts_offset = 0;
        Debug::log(TRACE, "[pw]  | seq {}", header->seq);
        Debug::log(TRACE, "[pw]  | pts {}", header->pts);
    }

    spa_meta_videotransform* vt = (spa_meta_videotransform*)spa_buffer_find_meta_data(spaBuf, SPA_META_VideoTransform, sizeof(*vt));
    if (vt) {
        if (rotationApplied) {
            vt->transform = WL_OUTPUT_TRANSFORM_NORMAL;
        } else {
            vt->transform = pSession->sharingData.transform;
        }
        Debug::log(TRACE, "[pw]  | meta transform {}", vt->transform);
    }

    spa_meta* damage = spa_buffer_find_meta(spaBuf, SPA_META_VideoDamage);
    if (damage) {
        Debug::log(TRACE, "[pw]  | meta has damage");

        spa_region* damageRegion  = (spa_region*)spa_meta_first(damage);
        uint32_t    damageCounter = 0;

        if (rotationApplied) {
            *damageRegion = SPA_REGION(0, 0, PSTREAM->currentPWBuffer->w, PSTREAM->currentPWBuffer->h);
            Debug::log(TRACE, "[pw]  | rotated frame, forcing full-frame damage {}x{}", PSTREAM->currentPWBuffer->w, PSTREAM->currentPWBuffer->h);
            if (spa_meta_check(damageRegion + 1, damage))
                *(damageRegion + 1) = SPA_REGION(0, 0, 0, 0);
            goto damage_done;
        }

        do {
            if (damageCounter >= pSession->sharingData.damageCount) {
                *damageRegion = SPA_REGION(0, 0, 0, 0);
                Debug::log(TRACE, "[pw]  | end damage @ {}: {} {} {} {}", damageCounter, damageRegion->position.x, damageRegion->position.y, damageRegion->size.width,
                           damageRegion->size.height);
                break;
            }

            *damageRegion = SPA_REGION(pSession->sharingData.damage[damageCounter].x, pSession->sharingData.damage[damageCounter].y, pSession->sharingData.damage[damageCounter].w,
                                       pSession->sharingData.damage[damageCounter].h);
            Debug::log(TRACE, "[pw]  | damage @ {}: {} {} {} {}", damageCounter, damageRegion->position.x, damageRegion->position.y, damageRegion->size.width,
                       damageRegion->size.height);
            damageCounter++;
        } while (spa_meta_check(damageRegion + 1, damage) && damageRegion++);

        if (damageCounter < pSession->sharingData.damageCount) {
            *damageRegion = SPA_REGION(0, 0, pSession->sharingData.frameInfoDMA.w, pSession->sharingData.frameInfoDMA.h);
            Debug::log(TRACE, "[pw]  | damage overflow, damaged whole");
        }

    damage_done:;
    }

    spa_data* datas = spaBuf->datas;

    Debug::log(TRACE, "[pw]  | size {}x{}", PSTREAM->pSession->sharingData.frameInfoDMA.w, PSTREAM->pSession->sharingData.frameInfoDMA.h);

    for (uint32_t plane = 0; plane < spaBuf->n_datas; plane++) {
        datas[plane].chunk->flags = CORRUPT ? SPA_CHUNK_FLAG_CORRUPTED : SPA_CHUNK_FLAG_NONE;

        Debug::log(TRACE, "[pw]  | plane {}", plane);
        Debug::log(TRACE, "[pw]     | fd {}", datas[plane].fd);
        Debug::log(TRACE, "[pw]     | maxsize {}", datas[plane].maxsize);
        Debug::log(TRACE, "[pw]     | size {}", datas[plane].chunk->size);
        Debug::log(TRACE, "[pw]     | stride {}", datas[plane].chunk->stride);
        Debug::log(TRACE, "[pw]     | offset {}", datas[plane].chunk->offset);
        Debug::log(TRACE, "[pw]     | flags {}", datas[plane].chunk->flags);
    }

    Debug::log(TRACE, "[pw] --------------------------------- End enqueue");

    int originalFd = -1;
    uint32_t originalMaxsize = 0;
    if (rotatedFd >= 0) {
        originalFd      = PSTREAM->currentPWBuffer->fd[0];
        originalMaxsize = PSTREAM->currentPWBuffer->size[0];
    }

    pw_stream_queue_buffer(PSTREAM->stream, PSTREAM->currentPWBuffer->pwBuffer);

    pSession->sharingData.status = FRAME_NONE;
    if (rotatedFd >= 0) {
        spaBuf->datas[0].fd      = originalFd;
        spaBuf->datas[0].maxsize = originalMaxsize;
        close(rotatedFd);
    }

    PSTREAM->currentPWBuffer = nullptr;
}

void CPipewireConnection::dequeue(CScreencopyPortal::SSession* pSession) {
    const auto PSTREAM = streamFromSession(pSession);

    if (!PSTREAM) {
        Debug::log(ERR, "[pw] Attempted dequeue on invalid session??");
        return;
    }

    Debug::log(TRACE, "[pw] dequeue on {}", (void*)PSTREAM);

    const auto PWBUF = pw_stream_dequeue_buffer(PSTREAM->stream);

    if (!PWBUF) {
        Debug::log(TRACE, "[pw] dequeue failed");
        PSTREAM->currentPWBuffer = nullptr;
        return;
    }

    const auto PBUF = (SBuffer*)PWBUF->user_data;

    PSTREAM->currentPWBuffer = PBUF;
}

std::unique_ptr<SBuffer> CPipewireConnection::createBuffer(CPipewireConnection::SPWStream* pStream, bool dmabuf, bool useStreamDimensions) {
    std::unique_ptr<SBuffer> pBuffer = std::make_unique<SBuffer>();

    pBuffer->isDMABUF = dmabuf;

    Debug::log(TRACE, "[pw] createBuffer: type {}", dmabuf ? "dma" : "shm");

    if (dmabuf) {
        pBuffer->w   = useStreamDimensions && pStream->pwVideoInfo.size.width > 0 ? pStream->pwVideoInfo.size.width : pStream->pSession->sharingData.frameInfoDMA.w;
        pBuffer->h   = useStreamDimensions && pStream->pwVideoInfo.size.height > 0 ? pStream->pwVideoInfo.size.height : pStream->pSession->sharingData.frameInfoDMA.h;
        pBuffer->fmt = pStream->pSession->sharingData.frameInfoDMA.fmt;

        uint32_t flags = GBM_BO_USE_RENDERING;
        const bool preferExplicitLinear = !useStreamDimensions;
        const uint64_t preferredCaptureModifier =
            preferExplicitLinear ? pStream->pSession->sharingData.extImageCopyDMAModifier : DRM_FORMAT_MOD_INVALID;

        auto     createLinearFallback = [&]() -> gbm_bo* {
            Debug::log(LOG, "[pw] Falling back to linear GBM BO for dma path");
            return gbm_bo_create(g_pPortalManager->m_sWaylandConnection.gbmDevice, pBuffer->w, pBuffer->h, pBuffer->fmt, flags | GBM_BO_USE_LINEAR);
        };

        if (preferExplicitLinear) {
            if (preferredCaptureModifier != DRM_FORMAT_MOD_INVALID) {
                uint64_t requestedModifier = preferredCaptureModifier;
                pBuffer->bo = gbm_bo_create_with_modifiers2(g_pPortalManager->m_sWaylandConnection.gbmDevice, pBuffer->w, pBuffer->h, pBuffer->fmt, &requestedModifier, 1, flags);
            }

            if (!pBuffer->bo) {
                uint64_t linearMod = DRM_FORMAT_MOD_LINEAR;
                pBuffer->bo        = gbm_bo_create_with_modifiers2(g_pPortalManager->m_sWaylandConnection.gbmDevice, pBuffer->w, pBuffer->h, pBuffer->fmt, &linearMod, 1, flags);
            }

            if (!pBuffer->bo)
                pBuffer->bo = createLinearFallback();
        } else if (pStream->pwVideoInfo.modifier != DRM_FORMAT_MOD_INVALID) {
            uint64_t* mods = (uint64_t*)&pStream->pwVideoInfo.modifier;
            pBuffer->bo    = gbm_bo_create_with_modifiers2(g_pPortalManager->m_sWaylandConnection.gbmDevice, pBuffer->w, pBuffer->h, pBuffer->fmt, mods, 1, flags);

            if (!pBuffer->bo)
                pBuffer->bo = createLinearFallback();
        } else {
            pBuffer->bo = gbm_bo_create(g_pPortalManager->m_sWaylandConnection.gbmDevice, pBuffer->w, pBuffer->h, pBuffer->fmt, flags);

            if (!pBuffer->bo)
                pBuffer->bo = createLinearFallback();
        }

        if (!pBuffer->bo) {
            Debug::log(ERR, "[pw] Couldn't create a drm buffer");
            return nullptr;
        }

        pBuffer->planeCount = gbm_bo_get_plane_count(pBuffer->bo);

        auto params = makeShared<CCZwpLinuxBufferParamsV1>(g_pPortalManager->m_sWaylandConnection.linuxDmabuf->sendCreateParams());
        if (!params) {
            Debug::log(ERR, "[pw] zwp_linux_dmabuf_v1_create_params failed");
            return nullptr;
        }

        for (size_t plane = 0; plane < (size_t)pBuffer->planeCount; plane++) {
            pBuffer->size[plane]   = 0;
            pBuffer->stride[plane] = gbm_bo_get_stride_for_plane(pBuffer->bo, plane);
            pBuffer->offset[plane] = gbm_bo_get_offset(pBuffer->bo, plane);
            uint64_t mod           = gbm_bo_get_modifier(pBuffer->bo);
            pBuffer->fd[plane]     = gbm_bo_get_fd_for_plane(pBuffer->bo, plane);

            if (plane == 0) {
                if (preferExplicitLinear && preferredCaptureModifier != DRM_FORMAT_MOD_INVALID)
                    pBuffer->modifier = preferredCaptureModifier;
                else
                    pBuffer->modifier = mod == DRM_FORMAT_MOD_INVALID && preferExplicitLinear ? DRM_FORMAT_MOD_LINEAR : mod;
            }

            if (pBuffer->fd[plane] < 0) {
                Debug::log(ERR, "[pw] gbm_bo_get_fd_for_plane failed");
                params.reset();
                return nullptr;
            }

            params->sendAdd(pBuffer->fd[plane], plane, pBuffer->offset[plane], pBuffer->stride[plane], mod >> 32, mod & 0xffffffff);
        }

        pBuffer->wlBuffer = makeShared<CCWlBuffer>(params->sendCreateImmed(pBuffer->w, pBuffer->h, pBuffer->fmt, /* flags */ (zwpLinuxBufferParamsV1Flags)0));
        params.reset();

        if (!pBuffer->wlBuffer) {
            Debug::log(ERR, "[pw] zwp_linux_buffer_params_v1_create_immed failed");
            return nullptr;
        }
    } else {
        pBuffer->w   = useStreamDimensions && pStream->pwVideoInfo.size.width > 0 ? pStream->pwVideoInfo.size.width : pStream->pSession->sharingData.frameInfoSHM.w;
        pBuffer->h   = useStreamDimensions && pStream->pwVideoInfo.size.height > 0 ? pStream->pwVideoInfo.size.height : pStream->pSession->sharingData.frameInfoSHM.h;
        pBuffer->fmt = pStream->pSession->sharingData.frameInfoSHM.fmt;

        if (useStreamDimensions && pStream->pwVideoInfo.format != SPA_VIDEO_FORMAT_UNKNOWN)
            pBuffer->fmt = drmFourccFromPW(pStream->pwVideoInfo.format);

        const uint32_t bpp           = drmBytesPerPixel(pBuffer->fmt);
        const bool     rotatedStream = useStreamDimensions && g_pPortalManager->m_sPortals.screencopy->shouldApplyGpuRotation(pStream->pSession);

        pBuffer->planeCount = 1;
        pBuffer->stride[0]  = bpp > 0 ? pBuffer->w * bpp : pStream->pSession->sharingData.frameInfoSHM.stride;
        if (rotatedStream)
            pBuffer->stride[0] = alignPipewireStride(pBuffer->stride[0]);
        pBuffer->size[0]    = pBuffer->stride[0] * pBuffer->h;
        pBuffer->offset[0]  = 0;
        pBuffer->fd[0]      = anonymous_shm_open();

        if (pBuffer->fd[0] == -1) {
            Debug::log(ERR, "[screencopy] anonymous_shm_open failed");
            return nullptr;
        }

        if (ftruncate(pBuffer->fd[0], pBuffer->size[0]) < 0) {
            Debug::log(ERR, "[screencopy] ftruncate failed");
            return nullptr;
        }

        pBuffer->mappedSize = pBuffer->size[0];
        pBuffer->mapped     = mmap(nullptr, pBuffer->mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, pBuffer->fd[0], 0);
        if (pBuffer->mapped == MAP_FAILED) {
            pBuffer->mapped     = nullptr;
            pBuffer->mappedSize = 0;
            Debug::log(ERR, "[screencopy] mmap failed");
            return nullptr;
        }

        pBuffer->wlBuffer = import_wl_shm_buffer(pBuffer->fd[0], wlSHMFromDrmFourcc(pBuffer->fmt), pBuffer->w, pBuffer->h, pBuffer->stride[0]);
        if (!pBuffer->wlBuffer) {
            Debug::log(ERR, "[screencopy] import_wl_shm_buffer failed");
            return nullptr;
        }
    }

    pBuffer->wlBuffer->setRelease([buf = pBuffer.get()](CCWlBuffer*) {
        buf->awaitingRelease = false;
    });

    return pBuffer;
}

void CPipewireConnection::updateStreamParam(SPWStream* pStream) {
    Debug::log(TRACE, "[pw] update stream params");

    uint8_t                 paramsBuf[2][1024];
    spa_pod_dynamic_builder dynBuilder[2];
    spa_pod_dynamic_builder_init(&dynBuilder[0], paramsBuf[0], sizeof(paramsBuf[0]), 2048);
    spa_pod_dynamic_builder_init(&dynBuilder[1], paramsBuf[1], sizeof(paramsBuf[1]), 2048);
    const spa_pod*   params[2];

    spa_pod_builder* builder[2] = {&dynBuilder[0].b, &dynBuilder[1].b};
    uint32_t         n_params   = buildFormatsFor(builder, params, pStream);

    pw_stream_update_params(pStream->stream, params, n_params);
    spa_pod_dynamic_builder_clean(&dynBuilder[0]);
    spa_pod_dynamic_builder_clean(&dynBuilder[1]);
}
