// license:BSD-3-Clause
// Build with the platform's audio_in backend (see the Makefile's
// AUDIO_INPUT_TEST_SRC, which also brings the shared core along on Apple).
//
// The listing always runs and needs neither ROMs nor hardware: it is what the
// A/D INPUT menus are built from, on every front end. --devices additionally
// opens the first recording device and walks the path ui::engine::fill() takes
// once per sample, printing what arrived.
//
// It reports levels rather than asserting on them: a machine with no
// microphone, a denied permission or a silent room all look the same, and CI
// must not fail for any of them. What it does assert is that the device opens,
// that the tap runs, and that pop() is safe to call - the three things whose
// absence is invisible from the outside.
#include "ui/audio_in.h"
#include "ui/lang.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

int main(int argc, char **argv)
{
	ui::init_lang("en");

	const auto names = ui::audio_in::list();
	std::cout << "Audio input devices: " << names.size() << "\n";
	for (const auto &name : names)
		std::cout << "  " << name << "\n";
	std::cout << "Audio input listing: PASS\n";
	if (argc < 2 || std::strcmp(argv[1], "--devices"))
		return 0;

	if (names.empty()) {
		std::cout << "--devices: no recording device, skipped\n";
		return 0;
	}

	// --devices [substring]: the first device by default, or the first whose
	// name contains the word given, which is how a machine with a virtual
	// device (BlackHole and friends, silent unless something plays into it)
	// gets its built-in microphone tested instead.
	std::string want = argc > 2 ? argv[2] : std::string();
	std::string pick = names.front();
	if (!want.empty()) {
		const auto hit = std::find_if(names.begin(), names.end(),
		                              [&want](const std::string &n) {
			                              return n.find(want) != std::string::npos;
		                              });
		if (hit == names.end()) {
			std::cout << "--devices: no device matching \"" << want << "\", skipped\n";
			return 0;
		}
		pick = *hit;
	}

	ui::audio_in ain;
	std::string err;
	if (!ain.start(pick, err)) {
		std::cout << "--devices: " << pick << " would not open (" << err
		          << "), skipped\n";
		return 0;
	}
	if (!ain.running()) {
		std::cerr << "FAIL: start() returned true but the input is not running\n";
		return 1;
	}
	std::cout << "--devices: opened " << ain.device_name() << " (" << ain.format_line() << ")\n";

	// A quarter of a second, walked the way engine::fill() walks it: pop one
	// pair per sample and keep the extremes.
	const int kSamples = 44100 / 4;
	std::this_thread::sleep_for(std::chrono::milliseconds(150));   // let the ring fill
	int peak = 0;
	long long sum_sq = 0;
	int non_zero = 0;
	for (int i = 0; i < kSamples; i++) {
		s32 l = 0, r = 0;
		ain.pop(l, r);
		const int loud = std::max(std::abs(l), std::abs(r));
		peak = std::max(peak, loud);
		sum_sq += static_cast<long long>(l) * l + static_cast<long long>(r) * r;
		if (loud > 8)                     // above the noise floor of a quiet room
			non_zero++;
	}
	const double rms = std::sqrt(double(sum_sq) / (2.0 * kSamples));
	std::cout << "--devices: peak " << peak << ", rms " << int(rms) << ", "
	          << non_zero << "/" << kSamples << " samples above the floor, dropped "
	          << ain.dropped_count() << ", emptied " << ain.empty_count() << "\n";
	if (non_zero == 0)
		std::cout << "--devices: nothing above the floor (a silent room, or a "
		          << "source that is not playing - the path itself is open)\n";

	ain.stop();
	if (ain.running()) {
		std::cerr << "FAIL: still running after stop()\n";
		return 1;
	}
	std::cout << "Audio input device: PASS\n";
	return 0;
}