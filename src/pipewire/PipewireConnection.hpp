#pragma once

#include <vector>
#include <memory>
#include <pipewire/pipewire.h>
#include <libdrm/drm_fourcc.h>
#include <gbm.h>
#include "../shared/ScreencopyShared.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../portals/ScreencopySession.hpp" // Include the new SSession header

struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_buffer;

struct SBuffer {
    bool           isDMABUF = false;
    uint32_t       w = 0, h = 0, fmt = 0;
    int            planeCount = 0;

    int            fd[4];
    uint32_t       size[4], stride[4], offset[4];

    gbm_bo*        bo = nullptr;

    SP<CCWlBuffer> wlBuffer = nullptr;
    pw_buffer*     pwBuffer = nullptr;
};

class CPipewireConnection {
  public:
    CPipewireConnection();
    ~CPipewireConnection();

    bool good();

    void createStream(SSession* pSession);
    void destroyStream(SSession* pSession);

    void enqueue(SSession* pSession);
    void dequeue(SSession* pSession);

    struct SPWStream {
        SSession*                             pSession    = nullptr;
        pw_stream*                            stream      = nullptr;
        bool                                  streamState = false;
        spa_hook                              streamListener;
        SBuffer*                              currentPWBuffer = nullptr;
        spa_video_info_raw                    pwVideoInfo;
        uint32_t                              seq   = 0;
        bool                                  isDMA = false;

        std::vector<std::unique_ptr<SBuffer>> buffers;
    };

    std::unique_ptr<SBuffer> createBuffer(SPWStream* pStream, bool dmabuf);
    SPWStream*               streamFromSession(SSession* pSession);
    void                     removeSessionFrameCallbacks(SSession* pSession);
    uint32_t                 buildFormatsFor(spa_pod_builder* b[2], const spa_pod* params[2], SPWStream* stream);
    void                     updateStreamParam(SPWStream* pStream);

  private:
    std::vector<std::unique_ptr<SPWStream>> m_vStreams;

    bool                                    buildModListFor(SPWStream* stream, uint32_t drmFmt, uint64_t** mods, uint32_t* modCount);

    pw_context*                             m_pContext = nullptr;
    pw_core*                                m_pCore    = nullptr;
};
