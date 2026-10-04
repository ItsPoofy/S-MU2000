// license:BSD-3-Clause
//
// 波形を 0 から作る（サンプリングの窓の「波形を作る」）。録る代わりに、PC で作った 1 周期の形を
// サンプリング RAM のサンプルにして、サンプル音色で鳴らす。
//
// どの作り方（基本の波形・倍音を足す・手描き）も、いったん倍音ごとの強さ（cos と sin の係数）に直す。
// そこから LOOP_FRAMES サンプルにちょうど CYCLES 周期が入る波形を作る。全体をループにすると
// 44100 × 25 / 4214 = 261.628Hz で、鍵 60 の高さ（261.626Hz）と 0.02 セントしか違わないので、
// 音色の側で音程を直さなくてよい（ループの頭は偶数の位置にしか置けないが、長さは 4214 で偶数）。
// 1 周期は 168.56 サンプルと半端だが、倍音を足して作るので問題にならない。倍音は 20kHz より下だけを足す
// （鍵 60 で 76 倍音まで。ここでは HARMONICS までにする）。

#ifndef S_MU2000_WAVEGEN_H
#define S_MU2000_WAVEGEN_H

#pragma once

#include "compat/mamecompat.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace smu2000::wavegen {

constexpr int HARMONICS = 64;
constexpr u32 LOOP_FRAMES = 4214;
constexpr int CYCLES = 25;
constexpr double PI = 3.14159265358979323846;

// 倍音 h（1 から）の cos と sin の係数。[0] は使わない（直流は入れない）
struct spectrum {
	float a[HARMONICS + 1] = {};
	float b[HARMONICS + 1] = {};
	double mag(int h) const { return std::sqrt(double(a[h]) * a[h] + double(b[h]) * b[h]); }
};

// 1 周期の形（n 点、-1〜1）を倍音に直す
inline spectrum from_cycle(const float *cycle, int n)
{
	spectrum s;
	for (int h = 1; h <= HARMONICS && h * 2 < n; h++) {
		double ca = 0, cb = 0;
		for (int i = 0; i < n; i++) {
			const double t = 2 * PI * h * double(i) / n;
			ca += cycle[i] * std::cos(t);
			cb += cycle[i] * std::sin(t);
		}
		s.a[h] = float(ca * 2 / n);
		s.b[h] = float(cb * 2 / n);
	}
	return s;
}

enum class shape { sine, saw, square, triangle };

// 基本の波形。pulse は矩形の上側の割合（0.05〜0.95）
inline spectrum basic(shape sh, double pulse = 0.5)
{
	constexpr int N = 2048;
	std::vector<float> c(N);
	for (int i = 0; i < N; i++) {
		const double p = (double(i) + 0.5) / N;      // 0〜1
		switch (sh) {
		case shape::sine:     c[size_t(i)] = float(std::sin(2 * PI * p)); break;
		case shape::saw:      c[size_t(i)] = float(1.0 - 2.0 * p); break;
		case shape::square:   c[size_t(i)] = p < pulse ? 1.0f : -1.0f; break;
		case shape::triangle: c[size_t(i)] = float(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4); break;
		}
	}
	return from_cycle(c.data(), N);
}

// 倍音の棒（強さだけ、位相は sin）から
inline spectrum from_bars(const float *amp, int count)
{
	spectrum s;
	for (int h = 1; h <= HARMONICS && h <= count; h++)
		s.b[h] = amp[h - 1];
	return s;
}

// 1 周期の形を n 点で（見せる用）。max_h までの倍音を足す
inline std::vector<float> cycle(const spectrum &s, int n, int max_h = HARMONICS)
{
	std::vector<float> out(size_t(n), 0.0f);
	for (int h = 1; h <= std::min(max_h, HARMONICS); h++) {
		if (s.a[h] == 0.0f && s.b[h] == 0.0f)
			continue;
		for (int i = 0; i < n; i++) {
			const double t = 2 * PI * h * double(i) / n;
			out[size_t(i)] += float(s.a[h] * std::cos(t) + s.b[h] * std::sin(t));
		}
	}
	return out;
}

// サンプルにする波形（LOOP_FRAMES サンプル、CYCLES 周期）。いちばん大きい所が level × 32767 になるようにそろえる。
// 全部の倍音が 0 なら無音
inline std::vector<s16> render(const spectrum &s, int max_h = HARMONICS, double level = 0.9)
{
	std::vector<double> x(LOOP_FRAMES, 0.0);
	const double f0 = 44100.0 * CYCLES / LOOP_FRAMES;
	for (int h = 1; h <= std::min(max_h, HARMONICS); h++) {
		if (h * f0 >= 20000.0 || (s.a[h] == 0.0f && s.b[h] == 0.0f))
			continue;
		for (u32 i = 0; i < LOOP_FRAMES; i++) {
			const double t = 2 * PI * h * CYCLES * double(i) / LOOP_FRAMES;
			x[i] += s.a[h] * std::cos(t) + s.b[h] * std::sin(t);
		}
	}
	double peak = 0;
	for (double v : x)
		peak = std::max(peak, std::fabs(v));
	std::vector<s16> out(LOOP_FRAMES, 0);
	if (peak <= 0)
		return out;
	const double g = std::clamp(level, 0.0, 1.0) * 32767.0 / peak;
	for (u32 i = 0; i < LOOP_FRAMES; i++)
		out[i] = s16(std::lround(x[i] * g));
	return out;
}

// ノイズ（白色）。frames サンプル。高さが無いので全体をループにするだけ
inline std::vector<s16> noise(u32 frames, double level = 0.9, u32 seed = 1)
{
	std::vector<s16> out(frames);
	u32 r = seed ? seed : 1;
	for (u32 i = 0; i < frames; i++) {
		r ^= r << 13;
		r ^= r >> 17;
		r ^= r << 5;
		out[i] = s16(std::lround((double(r & 0xffff) / 32767.5 - 1.0) * level * 32767.0));
	}
	return out;
}

} // namespace smu2000::wavegen

#endif // S_MU2000_WAVEGEN_H
