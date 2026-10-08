// license:BSD-3-Clause
#pragma once
#include "engine.h"
#include <optional>
#include <thread>

namespace ui {
struct midi_change_result { bool selected = false; std::string error; };

// Device drivers may block while opening/closing. Pause the engine, do that
// work off the UI thread, and publish the result only after the worker joins.
class midi_session {
public:
	~midi_session() { join(); }
	bool busy() const { return m_thread.joinable(); }
	void begin(engine &eng, midi_routing routes, bool missing_ok)
	{
		m_engine = &eng;
		m_previous_state = eng.state.exchange(0);
		m_done.store(false);
		m_thread = std::thread([this, &eng, routes = std::move(routes), missing_ok] {
			while (eng.in_fill.load()) smu2000::sleep_ms(1);
			m_result = {};
			m_result.selected = eng.midi.apply(routes, missing_ok, m_result.error);
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
		m_engine->state.store(m_previous_state);
		return m_result;
	}
private:
	engine *m_engine = nullptr;
	int m_previous_state = 0;
	std::thread m_thread;
	std::atomic<bool> m_done{false};
	midi_change_result m_result;
};
} // namespace ui
