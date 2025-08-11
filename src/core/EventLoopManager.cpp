#include "EventLoopManager.hpp"
#include "../helpers/Log.hpp"
#include <string.h>
#include <errno.h>

// Portal includes
#include "../portals/Screencopy.hpp"
#include "../portals/GlobalShortcuts.hpp"
#include "../portals/Screenshot.hpp"
#include "../portals/ScreencopyPicker.hpp"

// Shared includes
#include "../shared/ToplevelManager.hpp"
#include "../shared/ToplevelMappingManager.hpp"

CEventLoopManager::CEventLoopManager(CPortalManager* pPortalManager) : m_pPortalManager(pPortalManager) {
    // Constructor logic if any
}

CEventLoopManager::~CEventLoopManager() {
    if (m_timersThread && m_timersThread->joinable())
        m_timersThread->join();
    if (m_pollThread.joinable())
        m_pollThread.join();
}

void CEventLoopManager::startEventLoop() {
    pollfd pollfds[] = {
        {
            .fd     = m_pPortalManager->getConnection()->getEventLoopPollData().fd,
            .events = POLLIN,
        },
        {
            .fd     = wl_display_get_fd(m_pPortalManager->m_sWaylandConnection.display),
            .events = POLLIN,
        },
        {
            .fd     = pw_loop_get_fd(m_pPortalManager->m_sPipewire.loop),
            .events = POLLIN,
        },
    };

    m_pollThread = std::thread([this, &pollfds]() {
        while (1) {
            int ret = poll(pollfds, 3, 5000 /* 5 seconds, reasonable. It's because we might need to terminate */);
            if (ret < 0) {
                Debug::log(CRIT, "[core] Polling fds failed with {}", strerror(errno));
                m_pPortalManager->terminate();
            }

            for (size_t i = 0; i < 3; ++i) {
                if (pollfds[i].revents & POLLHUP) {
                    Debug::log(CRIT, "[core] Disconnected from pollfd id {}", i);
                    m_pPortalManager->terminate();
                }
            }

            if (m_pPortalManager->m_bTerminate)
                break;

            if (ret != 0) {
                Debug::log(TRACE, "[core] got poll event");
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                m_sEventLoopInternals.loopSignal.notify_all();
            }
        }
    });

    m_timersThread = std::make_unique<std::thread>([this] {
        while (1) {
            std::unique_lock lk(m_sTimersThread.loopMutex);

            // find nearest timer ms
            m_mEventLock.lock();
            float nearest = 60000; /* reasonable timeout */
            for (auto& t : m_sTimersThread.timers) {
                float until = t->duration() - t->passedMs();
                if (until < nearest)
                    nearest = until;
            }
            m_mEventLock.unlock();

            m_sTimersThread.loopSignal.wait_for(lk, std::chrono::milliseconds((int)nearest), [this] { return m_sTimersThread.shouldProcess; });
            m_sTimersThread.shouldProcess = false;

            if (m_pPortalManager->m_bTerminate)
                break;

            // awakened. Check if any timers passed
            m_mEventLock.lock();
            bool notify = false;
            for (auto& t : m_sTimersThread.timers) {
                if (t->passed()) {
                    Debug::log(TRACE, "[core] got timer event");
                    notify = true;
                    break;
                }
            }
            m_mEventLock.unlock();

            if (notify) {
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                m_sEventLoopInternals.loopSignal.notify_all();
            }
        }
    });

    while (1) { // dbus events
        // wait for being awakened
        std::unique_lock lk(m_sEventLoopInternals.loopMutex);
        if (m_sEventLoopInternals.shouldProcess == false) // avoid a lock if a thread managed to request something already since we .unlock()ed
            m_sEventLoopInternals.loopSignal.wait_for(lk, std::chrono::seconds(5), [this] { return m_sEventLoopInternals.shouldProcess == true; }); // wait for events

        std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);

        if (m_pPortalManager->m_bTerminate)
            break;

        m_sEventLoopInternals.shouldProcess = false;

        m_mEventLock.lock();

        if (pollfds[0].revents & POLLIN /* dbus */) {
            while (m_pPortalManager->getConnection()->processPendingEvent()) {
                ;
            }
        }

        if (pollfds[1].revents & POLLIN /* wl */) {
            wl_display_flush(m_pPortalManager->m_sWaylandConnection.display);
            if (wl_display_prepare_read(m_pPortalManager->m_sWaylandConnection.display) == 0) {
                wl_display_read_events(m_pPortalManager->m_sWaylandConnection.display);
                wl_display_dispatch_pending(m_pPortalManager->m_sWaylandConnection.display);
            } else {
                wl_display_dispatch(m_pPortalManager->m_sWaylandConnection.display);
            }
        }

        if (pollfds[2].revents & POLLIN /* pw */) {
            while (pw_loop_iterate(m_pPortalManager->m_sPipewire.loop, 0) != 0) {
                ;
            }
        }

        std::vector<CTimer*> toRemove;
        for (auto& t : m_sTimersThread.timers) {
            if (t->passed()) {
                t->m_fnCallback();
                toRemove.emplace_back(t.get());
                Debug::log(TRACE, "[core] calling timer {}", (void*)t.get());
            }
        }

        int ret = 0;
        do {
            ret = wl_display_dispatch_pending(m_pPortalManager->m_sWaylandConnection.display);
            wl_display_flush(m_pPortalManager->m_sWaylandConnection.display);
        } while (ret > 0);

        if (!toRemove.empty())
            std::erase_if(m_sTimersThread.timers,
                          [&](const auto& t) { return std::find_if(toRemove.begin(), toRemove.end(), [&](const auto& other) { return other == t.get(); }) != toRemove.end(); });

        m_mEventLock.unlock();
    }

    Debug::log(ERR, "[core] Terminated");

    // Removed reset calls from here

    m_pPortalManager->m_pConnection.reset();
    pw_loop_destroy(m_pPortalManager->m_sPipewire.loop);
    wl_display_disconnect(m_pPortalManager->m_sWaylandConnection.display);

    // Threads are joined in destructor
}

void CEventLoopManager::addTimer(const CTimer& timer) {
    Debug::log(TRACE, "[core] adding timer for {}ms", timer.duration());
    m_sTimersThread.timers.emplace_back(std::make_unique<CTimer>(timer));
    m_sTimersThread.shouldProcess = true;
    m_sTimersThread.loopSignal.notify_all();
}
