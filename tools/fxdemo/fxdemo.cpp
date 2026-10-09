// license:BSD-3-Clause
//
// The native effects on their own: no MU2000, no firmware, no ROM. This file is
// both a small command-line tool and the worked example for src/dsp/README.md.
// It includes only src/dsp/fx_slot.h (and through it blocks.h, blocks2.h,
// reverb.h, util.h, xg/fx_params.h).
//
//   fxdemo --list                              the effect types and their parameters
//   fxdemo <msb> <lsb> out.wav [in.wav] [Name=raw ...] [--send]
//                                              run one effect over in.wav (16-bit PCM), or over a
//                                              built-in test phrase when no input is given
//   fxdemo --block out.wav                     the same idea one level down: a block used directly
//   fxdemo --selftest                          every type once; fails on NaN, infinity or a blow-up
//
// msb and lsb are the XG effect type in hex (01 00 = HALL 1, 05 00 = DELAY LCR,
// 41 00 = CHORUS 1, 49 00 = DISTORTION ...). Parameters are given by the name the
// MU shows on its LCD and the raw XG value: "ReverbTime=40" "Dry/Wet=90".
// Anything not given starts in the middle of its range.
//
// By default the effect is used the way an insertion effect is: the sound goes
// through it and its Dry/Wet decides the mix. --send uses it the way the system
// reverb and chorus are used: only the effect's own sound comes out.
//
// Build: `make fxdemo`, or by hand:
//   g++ -std=c++20 -O2 -I src -I src/compat -o fxdemo tools/fxdemo/fxdemo.cpp

#include "dsp/fx_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using smu2000::dsp::fx_slot;

constexpr float RATE = 44100.0f;

struct stereo {
	std::vector<float> l, r;
};

// ---- a test phrase, so that the tool makes a sound with nothing to read:
// three plucked notes, then a short burst of noise, then silence for the tail
stereo test_phrase(float seconds)
{
	stereo s;
	const int n = int(seconds * RATE);
	s.l.assign(size_t(n), 0.0f);
	s.r.assign(size_t(n), 0.0f);
	const float notes[3] = { 220.0f, 277.18f, 329.63f };
	for (int k = 0; k < 3; k++) {
		const int at = int((0.1f + 0.35f * float(k)) * RATE);
		for (int i = 0; i < int(0.6f * RATE) && at + i < n; i++) {
			const float t = float(i) / RATE;
			// a saw with a fast decay: enough harmonics for filters and distortion to bite on
			const float ph = std::fmod(notes[k] * t, 1.0f);
			const float v = (2.0f * ph - 1.0f) * std::exp(-t * 5.0f) * 0.35f;
			s.l[size_t(at + i)] += v;
			s.r[size_t(at + i)] += v;
		}
	}
	uint32_t seed = 1;
	const int at = int(1.3f * RATE);
	for (int i = 0; i < int(0.08f * RATE) && at + i < n; i++) {
		seed = seed * 1664525u + 1013904223u;
		const float v = (float(seed >> 8) / 8388608.0f - 1.0f) * 0.3f;
		s.l[size_t(at + i)] += v;
		s.r[size_t(at + i)] += v;
	}
	return s;
}

// ---- 16-bit PCM WAV, the plainest kind
bool read_wav(const char *path, stereo &s)
{
	std::FILE *f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::vector<uint8_t> d;
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		d.insert(d.end(), buf, buf + n);
	std::fclose(f);
	const auto u16 = [&](size_t i) { return unsigned(d[i]) | unsigned(d[i + 1]) << 8; };
	const auto u32 = [&](size_t i) { return uint32_t(u16(i)) | uint32_t(u16(i + 2)) << 16; };
	if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
		return false;
	unsigned channels = 0, bits = 0;
	for (size_t at = 12; at + 8 <= d.size();) {
		const uint32_t size = u32(at + 4);
		if (!std::memcmp(d.data() + at, "fmt ", 4) && at + 24 <= d.size()) {
			if (u16(at + 8) != 1)
				return false;                 // PCM only
			channels = u16(at + 10);
			bits = u16(at + 22);
		} else if (!std::memcmp(d.data() + at, "data", 4)) {
			if (bits != 16 || channels < 1 || channels > 2)
				return false;
			const size_t end = std::min<size_t>(d.size(), at + 8 + size);
			for (size_t i = at + 8; i + 2 * channels <= end; i += 2 * channels) {
				const float a = float(int16_t(u16(i))) / 32768.0f;
				const float b = channels == 2 ? float(int16_t(u16(i + 2))) / 32768.0f : a;
				s.l.push_back(a);
				s.r.push_back(b);
			}
			return true;
		}
		at += 8 + size + (size & 1);
	}
	return false;
}

bool write_wav(const char *path, const stereo &s)
{
	std::FILE *f = std::fopen(path, "wb");
	if (!f)
		return false;
	const uint32_t bytes = uint32_t(s.l.size()) * 4;
	const auto put16 = [&](unsigned v) { std::fputc(int(v & 255), f); std::fputc(int(v >> 8 & 255), f); };
	const auto put32 = [&](uint32_t v) { put16(v & 0xffff); put16(v >> 16); };
	std::fwrite("RIFF", 1, 4, f);
	put32(36 + bytes);
	std::fwrite("WAVEfmt ", 1, 8, f);
	put32(16);
	put16(1);
	put16(2);
	put32(uint32_t(RATE));
	put32(uint32_t(RATE) * 4);
	put16(4);
	put16(16);
	std::fwrite("data", 1, 4, f);
	put32(bytes);
	for (size_t i = 0; i < s.l.size(); i++)
		for (float v : { s.l[i], s.r[i] }) {
			const float c = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
			put16(unsigned(uint16_t(int16_t(std::lrint(c * 32767.0f)))));
		}
	return std::fclose(f) == 0;
}

// The middle of every parameter's range: a neutral place to start from. (The
// real unit loads its own defaults for each type from its ROM; those are not
// part of this table.)
std::vector<int> middle_values(const xg::fx_def *def)
{
	std::vector<int> raw;
	for (int i = 0; def && i < def->count; i++)
		raw.push_back((int(def->params[i].lo) + int(def->params[i].hi) + 1) / 2);
	return raw;
}

const char *kind_name(fx_slot::kind k)
{
	static const char *const names[] = { "none", "thru", "reverb", "early reflections", "delay", "modulation", "rotary/tremolo/pan",
	                                     "distortion", "eq", "wah", "dynamics", "lo-fi", "ring modulator", "slice", "isolator",
	                                     "resonant low-pass", "centre cancel", "enhancer", "pitch change", "talking modulator", "chain" };
	return names[int(k)];
}

int list()
{
	for (const xg::fx_def &d : xg::FX_DEFS) {
		std::printf("%02X %02X  %s\n", d.msb, d.lsb, kind_name(fx_slot::kind_of(d.msb)));
		for (int i = 0; i < d.count; i++) {
			const xg::fx_param &p = d.params[i];
			// what the lowest and highest raw values mean on the LCD
			const auto shown = [&](int v) {
				if (p.fmt == xg::fx_fmt::table && p.texts && p.texts[v - int(p.lo)])
					return std::string(p.texts[v - int(p.lo)]);
				char t[24];
				std::snprintf(t, sizeof(t), p.fmt == xg::fx_fmt::tenths ? "%.1f" : "%.0f", double(smu2000::dsp::fx_value(p, v)));
				return std::string(t);
			};
			std::printf("        %-12s raw %3d..%-3d  (%s .. %s)\n", p.label, int(p.lo), int(p.hi), shown(int(p.lo)).c_str(), shown(int(p.hi)).c_str());
		}
	}
	return 0;
}

// ---- The effect level: a type and raw XG values in, sound out
int run(int msb, int lsb, const char *out_path, const char *in_path, const std::vector<std::string> &sets, bool send)
{
	const int type = msb << 7 | lsb;
	const xg::fx_def *def = xg::fx_find(type);
	if (!def) {
		std::fprintf(stderr, "no parameter table for type %02X %02X (try --list)\n", msb, lsb);
		return 1;
	}
	std::vector<int> raw = middle_values(def);
	for (const std::string &s : sets) {
		const size_t eq = s.find('=');
		bool found = false;
		for (int i = 0; i < def->count; i++)
			if (s.substr(0, eq) == def->params[i].label) {
				raw[size_t(i)] = std::atoi(s.c_str() + eq + 1);
				found = true;
			}
		if (!found) {
			std::fprintf(stderr, "this type has no parameter called \"%s\" (try --list)\n", s.substr(0, eq).c_str());
			return 1;
		}
	}

	stereo in;
	if (in_path) {
		if (!read_wav(in_path, in)) {
			std::fprintf(stderr, "cannot read %s (16-bit PCM WAV, mono or stereo)\n", in_path);
			return 1;
		}
		in.l.resize(in.l.size() + size_t(3 * RATE), 0.0f);      // room for the tail
		in.r.resize(in.l.size(), 0.0f);
	} else {
		in = test_phrase(5.0f);
	}

	// All there is to using an effect:
	fx_slot fx;
	fx.set_rate(RATE);
	fx.set_insertion(!send);                       // insertion: the dry sound is mixed in by Dry/Wet
	fx.set(type, raw.data(), int(raw.size()));     // the type and its raw values; call again whenever one changes
	stereo out;
	out.l.resize(in.l.size());
	out.r.resize(in.l.size());
	fx.process(in.l.data(), in.r.data(), out.l.data(), out.r.data(), int(in.l.size()));

	if (!write_wav(out_path, out)) {
		std::fprintf(stderr, "cannot write %s\n", out_path);
		return 1;
	}
	std::printf("%02X %02X (%s), %s: wrote %s\n", msb, lsb, kind_name(fx.current()), send ? "send" : "insertion", out_path);
	for (int i = 0; i < def->count; i++)
		std::printf("  %-12s = %d\n", def->params[i].label, raw[size_t(i)]);
	return 0;
}

// ---- One level down: a block used directly, with its parameters in plain units.
// Every block in blocks.h / blocks2.h / reverb.h has this shape: a params struct,
// set_rate(), set_params(), reset() and process() one stereo sample at a time.
int block_example(const char *out_path)
{
	const stereo in = test_phrase(6.0f);
	stereo out = in;

	smu2000::dsp::reverb rev;
	smu2000::dsp::reverb::params rp;
	rp.time = 2.8f;            // seconds to fall by 60 dB
	rp.predelay_ms = 30.0f;
	rp.damp_hz = 5000.0f;      // the highs die sooner above this
	rev.set_rate(RATE);
	rev.set_params(rp);

	smu2000::dsp::delay_fx dly;
	smu2000::dsp::delay_fx::params dp;
	dp.l_ms = 250.0f;
	dp.r_ms = 375.0f;
	dp.feedback = 0.4f;
	dly.set_rate(RATE);
	dly.set_params(dp);

	for (size_t i = 0; i < in.l.size(); i++) {
		float dl = 0, dr = 0, rl = 0, rr = 0;
		dly.process(in.l[i], in.r[i], dl, dr);                             // the echoes only
		rev.process(in.l[i] + dl * 0.5f, in.r[i] + dr * 0.5f, rl, rr);     // reverb on the dry sound and the echoes
		out.l[i] = in.l[i] + dl * 0.5f + rl * 0.4f;
		out.r[i] = in.r[i] + dr * 0.5f + rr * 0.4f;
	}
	if (!write_wav(out_path, out)) {
		std::fprintf(stderr, "cannot write %s\n", out_path);
		return 1;
	}
	std::printf("delay into reverb: wrote %s\n", out_path);
	return 0;
}

// ---- Every type once, at three settings of every parameter (lowest, middle,
// highest): nothing may come out as NaN or infinity or grow without bound
int selftest()
{
	const stereo in = test_phrase(2.0f);
	int bad = 0, runs = 0;
	for (const xg::fx_def &d : xg::FX_DEFS) {
		for (int setting = 0; setting < 3; setting++) {
			std::vector<int> raw;
			for (int i = 0; i < d.count; i++)
				raw.push_back(setting == 0 ? int(d.params[i].lo) : setting == 2 ? int(d.params[i].hi)
				                                                                  : (int(d.params[i].lo) + int(d.params[i].hi) + 1) / 2);
			for (int insertion = 0; insertion < 2; insertion++) {
				fx_slot fx;
				fx.set_rate(RATE);
				fx.set_insertion(insertion != 0);
				fx.set(d.msb << 7 | d.lsb, raw.data(), int(raw.size()));
				float peak = 0.0f;
				bool finite = true;
				for (size_t i = 0; i < in.l.size(); i++) {
					float a = 0, b = 0;
					fx.process(in.l[i], in.r[i], a, b);
					finite = finite && std::isfinite(a) && std::isfinite(b);
					peak = std::max({ peak, std::fabs(a), std::fabs(b) });
				}
				runs++;
				if (!finite || peak > 30.0f) {
					bad++;
					std::printf("FAIL %02X %02X (%s) %s, %s values: %s, peak %g\n", d.msb, d.lsb, kind_name(fx.current()),
					            insertion ? "insertion" : "send", setting == 0 ? "lowest" : setting == 1 ? "middle" : "highest",
					            finite ? "finite" : "NOT finite", double(peak));
				}
			}
		}
	}
	std::printf("%s: %d runs over %d types, %d bad\n", bad ? "FAIL" : "OK", runs, int(sizeof(xg::FX_DEFS) / sizeof(xg::FX_DEFS[0])), bad);
	return bad ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2 && !std::strcmp(argv[1], "--list"))
		return list();
	if (argc == 2 && !std::strcmp(argv[1], "--selftest"))
		return selftest();
	if (argc == 3 && !std::strcmp(argv[1], "--block"))
		return block_example(argv[2]);
	if (argc >= 4) {
		const char *in_path = nullptr;
		std::vector<std::string> sets;
		bool send = false;
		for (int i = 4; i < argc; i++) {
			if (!std::strcmp(argv[i], "--send"))
				send = true;
			else if (std::strchr(argv[i], '='))
				sets.push_back(argv[i]);
			else
				in_path = argv[i];
		}
		return run(int(std::strtol(argv[1], nullptr, 16)), int(std::strtol(argv[2], nullptr, 16)), argv[3], in_path, sets, send);
	}
	std::fprintf(stderr,
	             "usage: fxdemo --list\n"
	             "       fxdemo <msb> <lsb> out.wav [in.wav] [Name=raw ...] [--send]\n"
	             "       fxdemo --block out.wav\n"
	             "       fxdemo --selftest\n");
	return 2;
}
