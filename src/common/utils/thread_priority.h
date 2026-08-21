// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace vivora::utils {

// Raise the calling thread to a real-time-ish priority band so capture,
// encode, and send run without being preempted by background work.
// CLAUDE.md requires this for every hot-path thread.
//
// Windows: THREAD_PRIORITY_TIME_CRITICAL (within a NORMAL_PRIORITY_CLASS
// process this is below the REALTIME band, so it won't starve system
// components but still wins against normal UI work).
// macOS:   QOS_CLASS_USER_INTERACTIVE via pthread_set_qos_class_self_np.
// Linux:   setpriority(PRIO_PROCESS, 0, -10) — best-effort, silently
//          falls back to no-op when the process lacks CAP_SYS_NICE.
void boost_current_thread_priority();

} // namespace vivora::utils
