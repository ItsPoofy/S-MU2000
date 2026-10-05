// license:BSD-3-Clause
//
// The platform's real-time audio workgroup, behind one small class.
//
// Only macOS and iOS hand over a group (Apple's parallel real-time threads
// pattern: a persistent helper renders in sync with the I/O thread, so it
// joins the I/O unit's group instead of floating). Both are asked the same
// question - audio_out::realtime_workgroup() - and both answer it with the
// same property read (kAudioOutputUnitProperty_OSWorkgroup) off the unit they
// render through; the difference is only how they get a handle to that unit:
// macOS owns an AudioUnit, iOS takes the one AVAudioIONode.audioUnit hands out.
// The group belongs to the device underneath either way, so it is the same
// group. Everywhere else this is an empty class with the same API, so the
// engine loop carries no #ifdefs and the join/leave logic is reviewed once,
// here.
//
// The render block itself needs no join on either platform: it already runs on
// the audio stack's own real-time thread, which Apple puts in the deadline
// group itself. It is the slave that has to join, and it re-checks every
// sample, so a handle that arrives late (iOS opens the engine after boot) is
// picked up without any ordering dance.
//
// The handle stays void* throughout: only the Apple front ends set it, and
// only this file knows it is an os_workgroup_t.

#ifndef S_MU2000_COMPAT_REALTIME_H
#define S_MU2000_COMPAT_REALTIME_H

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <os/workgroup.h>
#include <pthread/qos.h>
#endif

namespace smu2000 {

// SMU2000_<NAME>=0 opts out, anything else (or unset) opts in. One binary
// stays measurable both ways.
inline bool realtime_env_on(const char *name, bool dflt)
{
	if (const char *e = std::getenv(name))
		return std::atoi(e) != 0;
	return dflt;
}

// Ask for performance cores on this thread. pthread priority numbers do not
// place threads on Apple silicon (the QoS class does); USER_INTERACTIVE
// matches what the audio stacks use. Nothing elsewhere.
inline void realtime_raise_self()
{
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// Joins whatever workgroup handle the front end set (null keeps today's
// behavior) and leaves it on the way out. The wanted handle can change
// under us (a host re-graph), so the caller re-checks every sample -- one
// relaxed load, off the hot path; active() lets the caller skip even that
// when the join is off.
class realtime_join {
public:
	realtime_join()
	{
		realtime_raise_self();
#if defined(__APPLE__)
		// On by default: a plug-in host cannot set environment variables,
		// so an opt-in flag would leave the AUv3 path dead in practice.
		m_on = realtime_env_on("SMU2000_AUDIO_WORKGROUP", true);
#endif
	}

	~realtime_join()
	{
#if defined(__APPLE__)
		// Leave directly: reset(nullptr) cannot be used here, since a
		// null want also matches the initial refused=nullptr guard.
		if (m_wg) {
			os_workgroup_leave(m_wg, &m_token);
			m_wg = nullptr;
		}
#endif
	}

	bool active() const { return m_on; }
	void reset(void *want)
	{
#if defined(__APPLE__)
		os_workgroup_t w = static_cast<os_workgroup_t>(want);
		if (w == m_wg)
			return;
		// refused only guards real handles; a null want must always
		// fall through so the destructor path can leave.
		if (w && w == m_refused)
			return;
		if (m_wg) {
			os_workgroup_leave(m_wg, &m_token);
			m_wg = nullptr;
		}
		if (w && os_workgroup_join(w, &m_token) == 0) {
			m_wg = w;
			if (!m_logged) {
				m_logged = true;
				std::fprintf(stderr, "[wg] slave joined\n");
			}
		} else {
			m_refused = w;
			if (!m_logged) {
				m_logged = true;
				std::fprintf(stderr, "[wg] slave join refused, staying out\n");
			}
		}
#else
		(void)want;
#endif
	}

	realtime_join(const realtime_join &) = delete;
	realtime_join &operator=(const realtime_join &) = delete;

private:
#if defined(__APPLE__)
	bool m_on = false;
	os_workgroup_t m_wg = nullptr;
	os_workgroup_join_token_s m_token{};
	os_workgroup_t m_refused = nullptr;
	bool m_logged = false;
#else
	// Nothing to join anywhere else; active() is false so the caller
	// skips even the handle load and reset() never runs.
	static constexpr bool m_on = false;
#endif
};

} // namespace smu2000

#endif // S_MU2000_COMPAT_REALTIME_H
