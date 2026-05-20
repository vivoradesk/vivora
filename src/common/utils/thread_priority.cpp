#include "common/utils/thread_priority.h"
#include "common/utils/log.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#elif defined(__APPLE__)
    #include <pthread.h>
    #include <sys/qos.h>
#else
    #include <sys/resource.h>
    #include <sys/time.h>
    #include <unistd.h>
    #include <errno.h>
#endif

namespace vivora::utils {

void boost_current_thread_priority() {
#if defined(_WIN32)
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        log::info("THREAD", "Priority raised to TIME_CRITICAL");
    } else {
        log::warn("THREAD", "SetThreadPriority failed (err=%lu)", GetLastError());
    }
#elif defined(__APPLE__)
    int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc == 0) {
        log::info("THREAD", "QoS set to USER_INTERACTIVE");
    } else {
        log::warn("THREAD", "pthread_set_qos_class_self_np failed (%d)", rc);
    }
#else
    // Requires CAP_SYS_NICE (or a ulimit bump) to take effect; log the
    // result either way so operators know whether the boost landed.
    errno = 0;
    int rc = setpriority(PRIO_PROCESS, 0, -10);
    if (rc == 0 && errno == 0) {
        log::info("THREAD", "nice set to -10");
    } else {
        log::warn("THREAD", "setpriority(-10) failed (errno=%d) — run with CAP_SYS_NICE for latency boost",
                  errno);
    }
#endif
}

} // namespace vivora::utils
