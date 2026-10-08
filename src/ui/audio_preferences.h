// license:BSD-3-Clause
#pragma once
#include "audio_stream.h"

namespace ui {

// ALSA menus append a human-readable description to the PCM identifier.
inline std::string audio_device_key(const std::string &name)
{
#if defined(__linux__)
	return name.substr(0, name.find("  ("));
#else
	return name;
#endif
}

struct audio_preferences {
	int latency_ms = 30;
	bool exclusive = false;
	audio_stream_options stream;
	bool operator==(const audio_preferences &) const = default;
};

struct audio_output_config {
	std::string device; // empty: follow the system output
	audio_preferences preferences;
	bool control_panel = false; // one-time request, never persisted
	bool operator==(const audio_output_config &) const = default;
};

struct audio_channel_route {
	std::string device;
	int left = 0, right = 1;
	audio_driver driver = audio_driver::native;
};
} // namespace ui
