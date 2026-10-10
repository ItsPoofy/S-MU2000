// license:BSD-3-Clause
#pragma once
#include "engine.h"
#include <optional>
#include <thread>

namespace ui {
struct midi_change_result { bool selected = false; std::string error; };

// Driver calls run on a worker while audio continues. Only replacing the
// endpoint lists pauses the engine; changing columns on open ports does not.
class midi_session {
public:
	~midi_session() { join(); }
	bool busy() const { return m_thread.joinable(); }
	void begin(engine &eng, midi_routing routes, bool missing_ok)
	{
		m_done.store(false);
		m_thread = std::thread([this, &eng, routes = std::move(routes), missing_ok] {
			m_result = {};
			m_result.selected = eng.midi.apply(routes, missing_ok, m_result.error, [&](auto &&commit) {
				const int previous = eng.state.exchange(0);
				while (eng.in_fill.load()) smu2000::sleep_ms(1);
				commit();
				eng.state.store(previous);
			});
			m_done.store(true);
		});
	}
	std::optional<midi_change_result> poll()
	{
		return busy() && m_done.load() ? join() : std::nullopt;
	}
	std::optional<midi_change_result> join()
	{
		if (!busy()) return std::nullopt;
		m_thread.join();
		return m_result;
	}
private:
	std::thread m_thread;
	std::atomic<bool> m_done{false};
	midi_change_result m_result;
};
} // namespace ui
