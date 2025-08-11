#include "PipewireConnection.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../portals/ScreencopySession.hpp" // Include the new SSession header
#include "../portals/Screencopy.hpp" // Include Screencopy.hpp for g_pPortalManager->m_sPortals.screencopy

#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include "linux-dmabuf-v1.hpp"
#include <unistd.h>

// Helper functions (originally static in Screencopy.cpp)
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

// Pipewire Stream Handlers
static void pwStreamStateChange(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
    const auto PSTREAM = (CPipewireConnection::SPWStream*)data;

    PSTREAM->pSession->sharingData.nodeID = pw_stream_get_node_id(PSTREAM->stream);

    Debug::log(TRACE, "[pw] pwStreamStateChange on {} from {} to {}, node id {}", (void*)PSTREAM, pw_stream_state_as_string(old), pw_stream_state_as_string(state),
               PSTREAM->pSession->sharingData.nodeID);

    switch (state) {
        case PW_STREAM_STATE_STREAMING:
            PSTREAM->streamState = true;
            
            if (PSTREAM->pSession->sharingData.status == FRAME_NONE) {
                Debug::log(LOG, "[screencopy] PipeWire stream is active. Sharing initialized.");
                g_pPortalManager->m_sPortals.screencopy->startFrameCopy(PSTREAM->pSession);
            } else {
                g_pPortalManager->m_sPortals.screencopy->m_pPipewire->removeSessionFrameCallbacks(PSTREAM->pSession);
                g_pPortalManager->m_sPortals.screencopy->startFrameCopy(PSTREAM->pSession);
            }
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
            int targetWidth, targetHeight;
            PSTREAM->pSession->getTargetDimensions(targetWidth, targetHeight);

            params[0] = fixate_format(&dynBuilder[2].b, pwFromDrmFourcc(PSTREAM->pSession->sharingData.frameInfoDMA.fmt), targetWidth,
                                      targetHeight, PSTREAM->pSession->sharingData.framerate, &modifier);

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
            Debug::log(TRACE, "[pw]  | framerate {}", PSTREAM->pSession->sharingData.framerate);

            return;
        }
    }

    Debug::log(TRACE, "[pw] Format renegotiated:");
    Debug::log(TRACE, "[pw]  | buffer_type {}", PSTREAM->isDMA ? "DMA" : "SHM");
    Debug::log(TRACE, "[pw]  | format: {}", (int)PSTREAM->pwVideoInfo.format);
    Debug::log(TRACE, "[pw]  | modifier: {}", PSTREAM->pwVideoInfo.modifier);
    Debug::log(TRACE, "[pw]  | size: {}x{}", PSTREAM->pwVideoInfo.size.width, PSTREAM->pwVideoInfo.size.height);
    Debug::log(TRACE, "[pw]  | framerate {}", PSTREAM->pSession->sharingData.framerate);

    uint32_t blocks = 1;

    params[0] = build_buffer(&dynBuilder[0].b, blocks, PSTREAM->pSession->sharingData.frameInfoSHM.size, PSTREAM->pSession->sharingData.frameInfoSHM.stride, data_type);

    params[1] = (const spa_pod*)spa_pod_builder_add_object(&dynBuilder[1].b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                                           SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

    params[2] = (const spa_pod*)spa_pod_builder_add_object(&dynBuilder[1].b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
                                                           SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_videotransform)));

    params[3] = (const spa_pod*)spa_pod_builder_add_object(
        &dynBuilder[2].b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage), SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * 4, sizeof(struct spa_meta_region) * 1, sizeof(struct spa_meta_region) * 4));

    pw_stream_update_params(PSTREAM->stream, params, 4);
    spa_pod_dynamic_builder_clean(&dynBuilder[0]);
    spa_pod_dynamic_builder_clean(&dynBuilder[1]);
    spa_pod_dynamic_builder_clean(&dynBuilder[2]);
}

static void pwStreamAddBuffer(void* data, pw_buffer* buffer) {
    const auto PSTREAM = (CPipewireConnection::SPWStream*)data;

    Debug::log(TRACE, "[pw] pwStreamAddBuffer with {} on {}", (void*)buffer, (void*)PSTREAM);

    spa_data*     spaData = buffer->buffer->datas;
    spa_data_type type;

    if ((spaData[0].type & (1u << SPA_DATA_MemFd)) > 0) {
        type = SPA_DATA_MemFd;
        Debug::log(WARN, "[pipewire] Asked for a wl_shm buffer which is legacy.");
    } else if ((spaData[0].type & (1u << SPA_DATA_DmaBuf)) > 0) {
        type = SPA_DATA_DmaBuf;
    } else {
        Debug::log(ERR, "[pipewire] wrong format in addbuffer");
        return;
    }

    const auto PBUFFER = PSTREAM->buffers.emplace_back(g_pPortalManager->m_sPortals.screencopy->m_pPipewire->createBuffer(PSTREAM, type == SPA_DATA_DmaBuf)).get();

    PBUFFER->pwBuffer = buffer;
    buffer->user_data = PBUFFER;

    Debug::log(TRACE, "[pw] buffer datas {}", buffer->buffer->n_datas);

    for (uint32_t plane = 0; plane < buffer->buffer->n_datas; plane++) {
        spaData[plane].type          = type;
        spaData[plane].maxsize       = PBUFFER->size[plane];
        spaData[plane].mapoffset     = 0;
        spaData[plane].chunk->size   = PBUFFER->size[plane];
        spaData[plane].chunk->stride = PBUFFER->stride[plane];
        spaData[plane].chunk->offset = PBUFFER->offset[plane];
        spaData[plane].flags         = 0;
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

    if (PBUFFER->isDMABUF)
        gbm_bo_destroy(PBUFFER->bo);

    PBUFFER->wlBuffer.reset();
    for (int plane = 0; plane < PBUFFER->planeCount; plane++) {
        close(PBUFFER->fd[plane]);
    }

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
};

// CPipewireConnection implementations
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

void CPipewireConnection::removeSessionFrameCallbacks(SSession* pSession) {
    Debug::log(TRACE, "[pipewire] removeSessionFrameCallbacks called");

    pSession->sharingData.frameCallback.reset();
    pSession->sharingData.windowFrameCallback.reset();

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

bool CPipewireConnection::good() {
    return m_pContext && m_pCore;
}

void CPipewireConnection::createStream(SSession* pSession) {
    const auto PSTREAM = m_vStreams.emplace_back(std::make_unique<SPWStream>(pSession)).get();

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

    pw_stream_connect(PSTREAM->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, (pw_stream_flags)(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS), params, PARAMCOUNT);

    pSession->sharingData.nodeID = pw_stream_get_node_id(PSTREAM->stream);

    Debug::log(TRACE, "[pw] Stream got nodeid {}", pSession->sharingData.nodeID);
}

void CPipewireConnection::destroyStream(SSession* pSession) {
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

uint32_t CPipewireConnection::buildFormatsFor(spa_pod_builder* b[2], const spa_pod* params[2], CPipewireConnection::SPWStream* stream) {
    uint32_t  paramCount = 0;
    uint32_t  modCount   = 0;
    uint64_t* modifiers  = nullptr;

    int targetWidth, targetHeight;
    stream->pSession->getTargetDimensions(targetWidth, targetHeight);

    if (build_modifierlist(stream, stream->pSession->sharingData.frameInfoDMA.fmt, &modifiers, &modCount) && modCount > 0) {
        Debug::log(LOG, "[pw] Building modifiers for dma");

        paramCount = 2;
        params[0]  = build_format(b[0], pwFromDrmFourcc(stream->pSession->sharingData.frameInfoDMA.fmt), targetWidth,
                                  targetHeight, stream->pSession->sharingData.framerate, modifiers, modCount);
        assert(params[0] != NULL);
        params[1] = build_format(b[1], pwFromDrmFourcc(stream->pSession->sharingData.frameInfoSHM.fmt), targetWidth,
                                 targetHeight, stream->pSession->sharingData.framerate, NULL, 0);
        assert(params[1] != NULL);
    } else {
        Debug::log(LOG, "[pw] Building modifiers for shm");

        paramCount = 1;
        params[0]  = build_format(b[0], pwFromDrmFourcc(stream->pSession->sharingData.frameInfoSHM.fmt), targetWidth,
                                  targetHeight, stream->pSession->sharingData.framerate, NULL, 0);
    }

    if (modifiers)
        free(modifiers);

    return paramCount;
}

bool CPipewireConnection::buildModListFor(CPipewireConnection::SPWStream* stream, uint32_t drmFmt, uint64_t** mods, uint32_t* modCount) {
    return true;
}

CPipewireConnection::SPWStream* CPipewireConnection::streamFromSession(SSession* pSession) {
    for (auto& s : m_vStreams) {
        if (s->pSession == pSession)
            return s.get();
    }
    return nullptr;
}

void CPipewireConnection::enqueue(SSession* pSession) {
    const auto PSTREAM = streamFromSession(pSession);

    if (!PSTREAM) {
        Debug::log(ERR, "[pw] Attempted enqueue on invalid session??");
        return;
    }

    Debug::log(TRACE, "[pw] enqueue on {}", (void*)PSTREAM);

    if (!PSTREAM->currentPWBuffer) {
        Debug::log(ERR, "[pipewire] no buffer in enqueue");
        return;
    }

    spa_buffer* spaBuf  = PSTREAM->currentPWBuffer->pwBuffer->buffer;
    const bool  CORRUPT = pSession->sharingData.status != FRAME_READY;
    if (CORRUPT)
        Debug::log(TRACE, "[pw] buffer corrupt");

    Debug::log(TRACE, "[pw] Enqueue data:");

    spa_meta_header* header = (spa_meta_header*)spa_buffer_find_meta_data(spaBuf, SPA_META_Header, sizeof(*header));
    if (header) {
        header->pts        = pSession->sharingData.tvTimestampNs;
        header->flags      = CORRUPT ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
        header->seq        = PSTREAM->seq++;
        header->dts_offset = 0;
        Debug::log(TRACE, "[pw]  | seq {}", header->seq);
        Debug::log(TRACE, "[pw]  | pts {}", header->pts);
    }

    spa_meta_videotransform* vt = (spa_meta_videotransform*)spa_buffer_find_meta_data(spaBuf, SPA_META_VideoTransform, sizeof(*vt));
    if (vt) {
        if (pSession->selection.needsTransform) {
            vt->transform = WL_OUTPUT_TRANSFORM_NORMAL;
            Debug::log(LOG, "[pw] PipeWire buffer metadata: forcing transform to NORMAL (0)");
        } else {
            vt->transform = pSession->sharingData.transform;
            Debug::log(LOG, "[pw] PipeWire buffer metadata: setting transform to {}", vt->transform);
        }
    }

    spa_meta* damage = spa_buffer_find_meta(spaBuf, SPA_META_VideoDamage);
    if (damage) {
        Debug::log(TRACE, "[pw]  | meta has damage");

        spa_region* damageRegion  = (spa_region*)spa_meta_first(damage);
        uint32_t    damageCounter = 0;
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
            // TODO: merge damage properly
            *damageRegion = SPA_REGION(0, 0, pSession->sharingData.frameInfoDMA.w, pSession->sharingData.frameInfoDMA.h);
            Debug::log(TRACE, "[pw]  | damage overflow, damaged whole");
        }
    }

    spa_data* datas = spaBuf->datas;

    Debug::log(TRACE, "[pw]  | size {}x{}", pSession->sharingData.frameInfoDMA.w, pSession->sharingData.frameInfoDMA.h);

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

    pw_stream_queue_buffer(PSTREAM->stream, PSTREAM->currentPWBuffer->pwBuffer);

    PSTREAM->currentPWBuffer = nullptr;
}

void CPipewireConnection::dequeue(SSession* pSession) {
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

std::unique_ptr<SBuffer> CPipewireConnection::createBuffer(CPipewireConnection::SPWStream* pStream, bool dmabuf) {
    std::unique_ptr<SBuffer> pBuffer = std::make_unique<SBuffer>();

    pBuffer->isDMABUF = dmabuf;

    Debug::log(TRACE, "[pw] createBuffer: type {}", dmabuf ? "dma" : "shm");

    if (dmabuf) {
        pBuffer->w   = pStream->pSession->sharingData.frameInfoDMA.w;
        pBuffer->h   = pStream->pSession->sharingData.frameInfoDMA.h;
        pBuffer->fmt = pStream->pSession->sharingData.frameInfoDMA.fmt;

        uint32_t flags = GBM_BO_USE_RENDERING;

        if (pStream->pwVideoInfo.modifier != DRM_FORMAT_MOD_INVALID) {
            uint64_t* mods = (uint64_t*)&pStream->pwVideoInfo.modifier;
            pBuffer->bo    = gbm_bo_create_with_modifiers2(g_pPortalManager->m_sWaylandConnection.gbmDevice, pBuffer->w, pBuffer->h, pBuffer->fmt, mods, 1, flags);
        } else {
            pBuffer->bo = gbm_bo_create(g_pPortalManager->m_sWaylandConnection.gbmDevice, pBuffer->w, pBuffer->h, pBuffer->fmt, flags);
        }

        if (!pBuffer->bo) {
            Debug::log(ERR, "[pw] Couldn't create a drm buffer");
            return nullptr;
        }

        pBuffer->planeCount = gbm_bo_get_plane_count(pBuffer->bo);

        auto params = makeShared<CCZwpLinuxBufferParamsV1>(g_pPortalManager->m_sWaylandConnection.linuxDmabuf->sendCreateParams());
        if (!params) {
            Debug::log(ERR, "[pw] zwp_linux_dmabuf_v1_create_params failed");
            gbm_bo_destroy(pBuffer->bo);
            return nullptr;
        }

        for (size_t plane = 0; plane < (size_t)pBuffer->planeCount; plane++) {
            pBuffer->size[plane]   = 0;
            pBuffer->stride[plane] = gbm_bo_get_stride_for_plane(pBuffer->bo, plane);
            pBuffer->offset[plane] = gbm_bo_get_offset(pBuffer->bo, plane);
            uint64_t mod           = gbm_bo_get_modifier(pBuffer->bo);
            pBuffer->fd[plane]     = gbm_bo_get_fd_for_plane(pBuffer->bo, plane);

            if (pBuffer->fd[plane] < 0) {
                Debug::log(ERR, "[pw] gbm_bo_get_fd_for_plane failed");
                params.reset();
                gbm_bo_destroy(pBuffer->bo);
                for (size_t plane_tmp = 0; plane_tmp < plane; plane_tmp++) {
                    close(pBuffer->fd[plane_tmp]);
                }
                return NULL;
            }

            params->sendAdd(pBuffer->fd[plane], plane, pBuffer->offset[plane], pBuffer->stride[plane], mod >> 32, mod & 0xffffffff);
        }

        pBuffer->wlBuffer = makeShared<CCWlBuffer>(params->sendCreateImmed(pBuffer->w, pBuffer->h, pBuffer->fmt, /* flags */ (zwpLinuxBufferParamsV1Flags)0));
        params.reset();

        if (!pBuffer->wlBuffer) {
            Debug::log(ERR, "[pw] zwp_linux_buffer_params_v1_create_immed failed");
            gbm_bo_destroy(pBuffer->bo);
            for (size_t plane = 0; plane < (size_t)pBuffer->planeCount; plane++) {
                close(pBuffer->fd[plane]);
            }

            return nullptr;
        }
    } else {

        pBuffer->w   = pStream->pSession->sharingData.frameInfoSHM.w;
        pBuffer->h   = pStream->pSession->sharingData.frameInfoSHM.h;
        pBuffer->fmt = pStream->pSession->sharingData.frameInfoSHM.fmt;

        pBuffer->planeCount = 1;
        pBuffer->size[0]    = pStream->pSession->sharingData.frameInfoSHM.size;
        pBuffer->stride[0]  = pStream->pSession->sharingData.frameInfoSHM.stride;
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

        pBuffer->wlBuffer = import_wl_shm_buffer(pBuffer->fd[0], wlSHMFromDrmFourcc(pStream->pSession->sharingData.frameInfoSHM.fmt), pStream->pSession->sharingData.frameInfoSHM.w,
                                                 pStream->pSession->sharingData.frameInfoSHM.h, pStream->pSession->sharingData.frameInfoSHM.stride);
        if (!pBuffer->wlBuffer) {
            Debug::log(ERR, "[screencopy] import_wl_shm_buffer failed");
            return nullptr;
        }
    }

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
