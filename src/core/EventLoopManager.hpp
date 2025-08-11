#pragma once

#include "PortalManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/Timer.hpp"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <poll.h>

class CPortalManager;

class CEventLoopManager {
  public:
    CEventLoopManager(CPortalManager* pPortalManager);
    ~CEventLoopManager(); // Declared destructor
    void startEventLoop();
    void addTimer(const CTimer& timer);

  private:
    CPortalManager* m_pPortalManager;

    std::thread m_pollThread;
    std::unique_ptr<std::thread> m_timersThread;

    // These were originally in CPortalManager, now managed by EventLoopManager
    struct {
        std::mutex              loopMutex;
        std::condition_variable loopSignal;
        bool                    shouldProcess = false;
        std::mutex              loopRequestMutex;
    } m_sEventLoopInternals;

    struct {
        std::vector<std::unique_ptr<CTimer>> timers;
        std::mutex                           loopMutex;
        std::condition_variable              loopSignal;
        bool                                 shouldProcess = false;
        std::unique_ptr<std::thread>         thread;
    } m_sTimersThread;

    std::mutex m_mEventLock;
};