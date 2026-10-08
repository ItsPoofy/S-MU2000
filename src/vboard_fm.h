// license:BSD-3-Clause
//
// **FM ボード**（架空のプラグインボードの 1 つ。src/vboard.h）。4 オペレーターの FM 音源を 16 チャンネルぶん持つ、
// マルチパートのボード（口 E）。中身は全部ここで書いた計算と、ここで作った音色で、ROM も、実在の機種の音色データも
// 使わない。
//
//   ・1 声 = 正弦波のオペレーター 4 つ。つなぎ方（アルゴリズム）は 8 通り:
//       0  1→2→3→4            4 が鳴る          4  1→2 と 3→4          2 と 4 が鳴る
//       1  (1+2)→3→4          4 が鳴る          5  1→(2・3・4)         2・3・4 が鳴る
//       2  (1+(2→3))→4        4 が鳴る          6  1→2、3、4           2・3・4 が鳴る
//       3  ((1→2)+3)→4        4 が鳴る          7  1、2、3、4          全部が鳴る
//     オペレーター 1 は自分に戻せる（フィードバック）
//   ・オペレーターごとに、周波数の比、大きさ（変調する側なら変調の深さ）、アタック・ディケイ・サステイン・リリース。
//     変調する側は、強く弾くほど深く掛かる（明るくなる）
//   ・音色は 32 個。GM の 16 分類に 2 つずつ割り当ててあり、プログラム番号（1-128）は GM の並びで選べる
//     （分類の前半 4 つが 1 つ目、後半 4 つが 2 つ目）
//   ・チャンネル 10（か、ドラムのパートにしたチャンネル）は FM のドラム。鍵ごとに決まった音（キック・スネア・
//     ハイハット・タム・シンバルなど）。ハイハットは閉じると開いた音が止まる
//   ・音色は書き換えられる。128 個のプログラムそれぞれが自分の音色（fm_voice）を持ち、その組が fm_bank。
//     何もしなければ上の 32 個を並べた初期の組。画面の音色エディタ（src/ui/fm_voice_editor.h）が組を作り直して渡す。
//     組はファイル（.smufm）に保存できる。鳴っている音にも、書き換えた値はすぐ効く
//   ・32 声。ピッチベンド（±2 半音）、モジュレーション（CC1。ビブラート）、サステインペダル（CC64）、
//     CC120 / CC121 / CC123

#ifndef S_MU2000_VBOARD_FM_H
#define S_MU2000_VBOARD_FM_H

#pragma once

#include "compat/mamecompat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace smu2000::vboard {

struct fm_op {
	float ratio;        // 周波数の比（鍵の高さの何倍か）
	float level;        // 0-1。鳴る側は音量、変調する側は深さ（1.0 で 2 周ぶん）
	float attack, decay, sustain, release;      // 秒・秒（60dB 下がる時間。0 = 下がらない）・0-1・秒
};

struct fm_patch {
	const char *name;   // 8 文字
	int alg;            // 0-7
	float feedback;     // オペレーター 1 の戻し（0-1）
	fm_op op[4];
	// ドラム用: 0 でなければ鍵に関係なくこの高さ（MIDI の鍵番号）で鳴る。drop は鳴り始めに上乗せする半音（30ms ほどで
	// 元の高さへ落ちる）。noise は鳴る側の変調に足すノイズ（0-1）。group が同じ音は、後から鳴ったものが前を止める
	float fixed_key = 0.0f, drop = 0.0f, noise = 0.0f;
	int group = 0;
};

namespace fm_detail {

// 音色 32 個（GM の 16 分類 × 2）
inline const fm_patch PATCHES[32] = {
	// ---- Piano
	{ "FM Piano", 4, 0.20f, { { 1.0f, 0.42f, 0.001f, 1.6f, 0.0f, 0.25f }, { 1.0f, 1.0f, 0.001f, 3.5f, 0.0f, 0.30f },
	                          { 3.0f, 0.22f, 0.001f, 0.5f, 0.0f, 0.20f }, { 1.0f, 0.55f, 0.001f, 2.2f, 0.0f, 0.30f } } },
	{ "FM EPno ", 4, 0.00f, { { 14.0f, 0.16f, 0.001f, 0.35f, 0.0f, 0.20f }, { 1.0f, 1.0f, 0.001f, 4.0f, 0.0f, 0.35f },
	                          { 1.0f, 0.28f, 0.001f, 2.5f, 0.0f, 0.30f }, { 1.0f, 0.80f, 0.001f, 4.0f, 0.0f, 0.35f } } },
	// ---- Chromatic percussion
	{ "FM Bell ", 4, 0.00f, { { 3.5f, 0.45f, 0.001f, 2.5f, 0.0f, 1.2f }, { 1.0f, 1.0f, 0.001f, 5.0f, 0.0f, 1.5f },
	                          { 7.07f, 0.30f, 0.001f, 1.2f, 0.0f, 0.8f }, { 2.0f, 0.50f, 0.001f, 3.5f, 0.0f, 1.2f } } },
	{ "FM Marim", 4, 0.00f, { { 4.0f, 0.35f, 0.001f, 0.12f, 0.0f, 0.10f }, { 1.0f, 1.0f, 0.001f, 0.9f, 0.0f, 0.25f },
	                          { 10.0f, 0.15f, 0.001f, 0.05f, 0.0f, 0.05f }, { 1.0f, 0.40f, 0.001f, 0.5f, 0.0f, 0.20f } } },
	// ---- Organ
	{ "FM Organ", 7, 0.15f, { { 0.5f, 0.80f, 0.005f, 0.0f, 1.0f, 0.06f }, { 1.0f, 1.0f, 0.005f, 0.0f, 1.0f, 0.06f },
	                          { 2.0f, 0.60f, 0.005f, 0.0f, 1.0f, 0.06f }, { 3.0f, 0.35f, 0.005f, 0.0f, 1.0f, 0.06f } } },
	{ "FM PipeO", 6, 0.00f, { { 1.0f, 0.18f, 0.03f, 0.0f, 1.0f, 0.15f }, { 1.0f, 1.0f, 0.04f, 0.0f, 1.0f, 0.20f },
	                          { 2.0f, 0.55f, 0.05f, 0.0f, 1.0f, 0.20f }, { 4.0f, 0.30f, 0.06f, 0.0f, 1.0f, 0.20f } } },
	// ---- Guitar
	{ "FM Guitr", 0, 0.10f, { { 3.0f, 0.20f, 0.001f, 0.25f, 0.0f, 0.15f }, { 1.0f, 0.30f, 0.001f, 0.8f, 0.0f, 0.20f },
	                          { 1.0f, 0.38f, 0.001f, 1.5f, 0.0f, 0.20f }, { 1.0f, 1.0f, 0.001f, 2.4f, 0.0f, 0.25f } } },
	{ "FM Clav ", 0, 0.45f, { { 1.0f, 0.45f, 0.001f, 0.6f, 0.0f, 0.08f }, { 3.0f, 0.40f, 0.001f, 0.5f, 0.0f, 0.08f },
	                          { 1.0f, 0.50f, 0.001f, 0.9f, 0.0f, 0.08f }, { 1.0f, 1.0f, 0.001f, 1.4f, 0.0f, 0.08f } } },
	// ---- Bass
	{ "FM Bass ", 0, 0.30f, { { 1.0f, 0.35f, 0.001f, 0.25f, 0.0f, 0.08f }, { 1.0f, 0.45f, 0.001f, 0.35f, 0.2f, 0.08f },
	                          { 0.5f, 0.50f, 0.001f, 0.6f, 0.3f, 0.08f }, { 0.5f, 1.0f, 0.001f, 2.5f, 0.4f, 0.10f } } },
	{ "FM SlapB", 2, 0.50f, { { 1.0f, 0.60f, 0.001f, 0.10f, 0.0f, 0.06f }, { 5.0f, 0.30f, 0.001f, 0.06f, 0.0f, 0.06f },
	                          { 1.0f, 0.45f, 0.001f, 0.30f, 0.0f, 0.06f }, { 0.5f, 1.0f, 0.001f, 1.8f, 0.2f, 0.10f } } },
	// ---- Strings
	{ "FM Strng", 4, 0.35f, { { 1.0f, 0.30f, 0.12f, 0.0f, 1.0f, 0.30f }, { 1.0f, 1.0f, 0.18f, 0.0f, 1.0f, 0.35f },
	                          { 2.0f, 0.22f, 0.15f, 0.0f, 1.0f, 0.30f }, { 1.003f, 0.85f, 0.22f, 0.0f, 1.0f, 0.35f } } },
	{ "FM Pizz ", 4, 0.20f, { { 1.0f, 0.40f, 0.001f, 0.12f, 0.0f, 0.10f }, { 1.0f, 1.0f, 0.001f, 0.35f, 0.0f, 0.15f },
	                          { 2.0f, 0.25f, 0.001f, 0.08f, 0.0f, 0.08f }, { 1.0f, 0.60f, 0.001f, 0.25f, 0.0f, 0.15f } } },
	// ---- Ensemble
	{ "FM Ensmb", 4, 0.25f, { { 1.0f, 0.24f, 0.25f, 0.0f, 1.0f, 0.45f }, { 0.998f, 1.0f, 0.30f, 0.0f, 1.0f, 0.50f },
	                          { 1.0f, 0.24f, 0.28f, 0.0f, 1.0f, 0.45f }, { 1.004f, 1.0f, 0.35f, 0.0f, 1.0f, 0.50f } } },
	{ "FM Choir", 6, 0.00f, { { 1.0f, 0.12f, 0.20f, 0.0f, 1.0f, 0.40f }, { 1.0f, 1.0f, 0.25f, 0.0f, 1.0f, 0.45f },
	                          { 2.0f, 0.22f, 0.30f, 0.0f, 1.0f, 0.45f }, { 3.0f, 0.12f, 0.35f, 0.0f, 1.0f, 0.45f } } },
	// ---- Brass
	{ "FM Brass", 0, 0.40f, { { 1.0f, 0.30f, 0.06f, 0.5f, 0.6f, 0.12f }, { 1.0f, 0.42f, 0.05f, 0.6f, 0.7f, 0.12f },
	                          { 1.0f, 0.50f, 0.04f, 0.8f, 0.8f, 0.12f }, { 1.0f, 1.0f, 0.03f, 0.0f, 1.0f, 0.15f } } },
	{ "FM SynBr", 2, 0.55f, { { 1.0f, 0.50f, 0.02f, 0.4f, 0.6f, 0.15f }, { 2.0f, 0.30f, 0.02f, 0.3f, 0.5f, 0.15f },
	                          { 1.0f, 0.45f, 0.02f, 0.5f, 0.7f, 0.15f }, { 1.0f, 1.0f, 0.015f, 0.0f, 1.0f, 0.18f } } },
	// ---- Reed
	{ "FM Sax  ", 0, 0.50f, { { 1.0f, 0.28f, 0.03f, 0.0f, 1.0f, 0.10f }, { 2.0f, 0.30f, 0.03f, 0.0f, 1.0f, 0.10f },
	                          { 1.0f, 0.55f, 0.03f, 0.0f, 1.0f, 0.10f }, { 1.0f, 1.0f, 0.03f, 0.0f, 1.0f, 0.12f } } },
	{ "FM Oboe ", 0, 0.20f, { { 2.0f, 0.20f, 0.02f, 0.0f, 1.0f, 0.10f }, { 1.0f, 0.30f, 0.02f, 0.0f, 1.0f, 0.10f },
	                          { 3.0f, 0.42f, 0.02f, 0.0f, 1.0f, 0.10f }, { 1.0f, 1.0f, 0.03f, 0.0f, 1.0f, 0.12f } } },
	// ---- Pipe
	{ "FM Flute", 4, 0.10f, { { 1.0f, 0.14f, 0.05f, 0.4f, 0.5f, 0.12f }, { 1.0f, 1.0f, 0.06f, 0.0f, 1.0f, 0.15f },
	                          { 6.0f, 0.10f, 0.01f, 0.08f, 0.0f, 0.05f }, { 2.0f, 0.12f, 0.06f, 0.0f, 1.0f, 0.12f } } },
	{ "FM Whstl", 7, 0.00f, { { 1.0f, 1.0f, 0.06f, 0.0f, 1.0f, 0.12f }, { 2.0f, 0.06f, 0.06f, 0.0f, 1.0f, 0.12f },
	                          { 3.0f, 0.03f, 0.06f, 0.0f, 1.0f, 0.12f }, { 1.0f, 0.0f, 0.06f, 0.0f, 1.0f, 0.12f } } },
	// ---- Synth lead
	{ "FM SqLd ", 4, 0.00f, { { 2.0f, 0.42f, 0.002f, 0.0f, 1.0f, 0.08f }, { 1.0f, 1.0f, 0.002f, 0.0f, 1.0f, 0.10f },
	                          { 2.0f, 0.42f, 0.002f, 0.0f, 1.0f, 0.08f }, { 1.005f, 0.80f, 0.002f, 0.0f, 1.0f, 0.10f } } },
	{ "FM SawLd", 4, 0.75f, { { 1.0f, 0.50f, 0.002f, 0.0f, 1.0f, 0.08f }, { 1.0f, 1.0f, 0.002f, 0.0f, 1.0f, 0.10f },
	                          { 1.0f, 0.50f, 0.002f, 0.0f, 1.0f, 0.08f }, { 1.006f, 0.90f, 0.002f, 0.0f, 1.0f, 0.10f } } },
	// ---- Synth pad
	{ "FM Pad  ", 4, 0.20f, { { 1.0f, 0.20f, 0.6f, 0.0f, 1.0f, 0.9f }, { 1.0f, 1.0f, 0.7f, 0.0f, 1.0f, 1.0f },
	                          { 2.0f, 0.18f, 0.9f, 0.0f, 1.0f, 0.9f }, { 0.997f, 0.90f, 0.8f, 0.0f, 1.0f, 1.0f } } },
	{ "FM Glass", 4, 0.00f, { { 5.0f, 0.28f, 0.01f, 2.0f, 0.2f, 1.0f }, { 1.0f, 1.0f, 0.02f, 4.0f, 0.5f, 1.2f },
	                          { 9.0f, 0.14f, 0.01f, 1.0f, 0.0f, 0.8f }, { 2.0f, 0.45f, 0.02f, 3.0f, 0.3f, 1.2f } } },
	// ---- Synth effects
	{ "FM Rain ", 5, 0.30f, { { 7.0f, 0.30f, 0.001f, 0.3f, 0.0f, 0.3f }, { 1.0f, 1.0f, 0.001f, 1.2f, 0.0f, 0.6f },
	                          { 2.01f, 0.60f, 0.001f, 1.5f, 0.0f, 0.6f }, { 4.02f, 0.40f, 0.001f, 1.0f, 0.0f, 0.6f } } },
	{ "FM Cryst", 4, 0.00f, { { 11.0f, 0.22f, 0.001f, 1.5f, 0.0f, 1.0f }, { 1.0f, 1.0f, 0.001f, 4.0f, 0.0f, 1.5f },
	                          { 13.0f, 0.18f, 0.001f, 0.8f, 0.0f, 0.8f }, { 4.0f, 0.50f, 0.001f, 2.5f, 0.0f, 1.2f } } },
	// ---- Ethnic
	{ "FM Sitar", 0, 0.60f, { { 1.0f, 0.35f, 0.001f, 1.5f, 0.2f, 0.25f }, { 3.0f, 0.40f, 0.001f, 1.2f, 0.2f, 0.25f },
	                          { 1.0f, 0.55f, 0.001f, 2.0f, 0.2f, 0.25f }, { 1.0f, 1.0f, 0.001f, 3.0f, 0.0f, 0.30f } } },
	{ "FM Koto ", 4, 0.10f, { { 5.0f, 0.30f, 0.001f, 0.20f, 0.0f, 0.15f }, { 1.0f, 1.0f, 0.001f, 1.8f, 0.0f, 0.30f },
	                          { 2.0f, 0.35f, 0.001f, 0.6f, 0.0f, 0.20f }, { 1.0f, 0.50f, 0.001f, 1.2f, 0.0f, 0.30f } } },
	// ---- Percussive
	{ "FM Tom  ", 4, 0.00f, { { 1.0f, 0.30f, 0.001f, 0.10f, 0.0f, 0.10f }, { 1.0f, 1.0f, 0.001f, 0.45f, 0.0f, 0.20f },
	                          { 1.6f, 0.20f, 0.001f, 0.06f, 0.0f, 0.06f }, { 1.0f, 0.50f, 0.001f, 0.30f, 0.0f, 0.20f } }, 0.0f, 7.0f },
	{ "FM Block", 4, 0.00f, { { 3.3f, 0.40f, 0.001f, 0.04f, 0.0f, 0.04f }, { 1.0f, 1.0f, 0.001f, 0.12f, 0.0f, 0.08f },
	                          { 5.1f, 0.30f, 0.001f, 0.03f, 0.0f, 0.03f }, { 2.0f, 0.50f, 0.001f, 0.08f, 0.0f, 0.08f } } },
	// ---- Sound effects
	{ "FM Noise", 4, 1.00f, { { 11.3f, 1.0f, 0.01f, 0.0f, 1.0f, 0.3f }, { 7.7f, 1.0f, 0.02f, 0.0f, 1.0f, 0.4f },
	                          { 13.1f, 1.0f, 0.01f, 0.0f, 1.0f, 0.3f }, { 5.3f, 0.80f, 0.02f, 0.0f, 1.0f, 0.4f } }, 0.0f, 0.0f, 0.8f },
	{ "FM Zap  ", 0, 0.70f, { { 0.5f, 0.60f, 0.001f, 0.25f, 0.0f, 0.10f }, { 1.5f, 0.50f, 0.001f, 0.20f, 0.0f, 0.10f },
	                          { 1.0f, 0.60f, 0.001f, 0.30f, 0.0f, 0.10f }, { 1.0f, 1.0f, 0.001f, 0.50f, 0.0f, 0.12f } }, 0.0f, 36.0f },
};

// ドラム（鍵ごとに決まった音）。{ 名前, つなぎ方, 戻し, オペレーター 4 つ, 高さ（鍵番号）, 落ち幅, ノイズ, 組 }
inline const fm_patch DRUM_KICK   = { "Kick    ", 4, 0.0f, { { 1.0f, 0.25f, 0.001f, 0.05f, 0.0f, 0.05f }, { 1.0f, 1.0f, 0.001f, 0.35f, 0.0f, 0.20f },
                                                             { 2.3f, 0.30f, 0.001f, 0.02f, 0.0f, 0.02f }, { 1.0f, 0.60f, 0.001f, 0.20f, 0.0f, 0.15f } }, 31.0f, 22.0f, 0.0f, 0 };
inline const fm_patch DRUM_SNARE  = { "Snare   ", 4, 0.0f, { { 1.6f, 0.30f, 0.001f, 0.08f, 0.0f, 0.08f }, { 1.0f, 0.90f, 0.001f, 0.16f, 0.0f, 0.12f },
                                                             { 7.3f, 1.0f, 0.001f, 0.20f, 0.0f, 0.15f }, { 3.1f, 1.0f, 0.001f, 0.22f, 0.0f, 0.15f } }, 55.0f, 5.0f, 1.0f, 0 };
inline const fm_patch DRUM_RIM    = { "Rim     ", 4, 0.0f, { { 3.3f, 0.50f, 0.001f, 0.02f, 0.0f, 0.02f }, { 1.0f, 1.0f, 0.001f, 0.05f, 0.0f, 0.04f },
                                                             { 5.7f, 0.40f, 0.001f, 0.02f, 0.0f, 0.02f }, { 2.4f, 0.70f, 0.001f, 0.04f, 0.0f, 0.04f } }, 76.0f, 0.0f, 0.1f, 0 };
inline const fm_patch DRUM_CLAP   = { "Clap    ", 7, 0.0f, { { 5.3f, 1.0f, 0.001f, 0.18f, 0.0f, 0.12f }, { 7.9f, 1.0f, 0.001f, 0.16f, 0.0f, 0.12f },
                                                             { 11.3f, 0.8f, 0.001f, 0.14f, 0.0f, 0.12f }, { 3.7f, 0.8f, 0.001f, 0.18f, 0.0f, 0.12f } }, 64.0f, 0.0f, 1.0f, 0 };
inline const fm_patch DRUM_HAT_C  = { "ClosedHH", 7, 0.0f, { { 1.0f, 0.8f, 0.001f, 0.05f, 0.0f, 0.04f }, { 1.41f, 0.8f, 0.001f, 0.05f, 0.0f, 0.04f },
                                                             { 1.73f, 0.8f, 0.001f, 0.04f, 0.0f, 0.04f }, { 2.37f, 0.8f, 0.001f, 0.04f, 0.0f, 0.04f } }, 103.0f, 0.0f, 1.0f, 1 };
inline const fm_patch DRUM_HAT_O  = { "Open HH ", 7, 0.0f, { { 1.0f, 0.8f, 0.001f, 0.45f, 0.0f, 0.10f }, { 1.41f, 0.8f, 0.001f, 0.40f, 0.0f, 0.10f },
                                                             { 1.73f, 0.8f, 0.001f, 0.35f, 0.0f, 0.10f }, { 2.37f, 0.8f, 0.001f, 0.30f, 0.0f, 0.10f } }, 103.0f, 0.0f, 1.0f, 1 };
inline const fm_patch DRUM_TOM    = { "Tom     ", 4, 0.0f, { { 1.0f, 0.30f, 0.001f, 0.10f, 0.0f, 0.10f }, { 1.0f, 1.0f, 0.001f, 0.40f, 0.0f, 0.25f },
                                                             { 1.6f, 0.20f, 0.001f, 0.06f, 0.0f, 0.06f }, { 1.0f, 0.50f, 0.001f, 0.28f, 0.0f, 0.20f } }, 45.0f, 7.0f, 0.0f, 0 };
inline const fm_patch DRUM_CRASH  = { "Crash   ", 7, 0.0f, { { 1.0f, 0.8f, 0.001f, 1.8f, 0.0f, 1.2f }, { 1.47f, 0.8f, 0.001f, 1.6f, 0.0f, 1.2f },
                                                             { 1.83f, 0.8f, 0.001f, 1.4f, 0.0f, 1.2f }, { 2.71f, 0.8f, 0.001f, 1.2f, 0.0f, 1.2f } }, 96.0f, 0.0f, 1.0f, 0 };
inline const fm_patch DRUM_RIDE   = { "Ride    ", 7, 0.0f, { { 1.0f, 0.8f, 0.001f, 1.0f, 0.0f, 0.8f }, { 1.52f, 0.8f, 0.001f, 0.9f, 0.0f, 0.8f },
                                                             { 2.19f, 0.6f, 0.001f, 0.8f, 0.0f, 0.8f }, { 3.11f, 0.5f, 0.001f, 0.7f, 0.0f, 0.8f } }, 98.0f, 0.0f, 0.35f, 0 };
inline const fm_patch DRUM_BELL   = { "Cowbell ", 6, 0.0f, { { 1.0f, 0.0f, 0.001f, 0.2f, 0.0f, 0.1f }, { 1.0f, 1.0f, 0.001f, 0.25f, 0.0f, 0.15f },
                                                             { 1.5f, 0.9f, 0.001f, 0.22f, 0.0f, 0.15f }, { 2.52f, 0.2f, 0.001f, 0.10f, 0.0f, 0.10f } }, 79.0f, 0.0f, 0.0f, 0 };
inline const fm_patch DRUM_PERC   = { "Perc    ", 4, 0.0f, { { 2.7f, 0.35f, 0.001f, 0.05f, 0.0f, 0.05f }, { 1.0f, 1.0f, 0.001f, 0.18f, 0.0f, 0.12f },
                                                             { 4.3f, 0.25f, 0.001f, 0.04f, 0.0f, 0.04f }, { 1.5f, 0.50f, 0.001f, 0.12f, 0.0f, 0.10f } }, 0.0f, 3.0f, 0.0f, 0 };

// 鍵 → ドラムの音。tune はその音を何半音ずらすか（タムの高さの違い）
inline const fm_patch &drum_for_key(int key, float &tune)
{
	tune = 0.0f;
	switch (key) {
	case 35: case 36: return DRUM_KICK;
	case 37: return DRUM_RIM;
	case 38: case 40: tune = key == 40 ? 2.0f : 0.0f; return DRUM_SNARE;
	case 39: return DRUM_CLAP;
	case 42: case 44: return DRUM_HAT_C;
	case 46: return DRUM_HAT_O;
	case 41: case 43: case 45: case 47: case 48: case 50: {
		static const float TUNE[10] = { 0, 0, 3, 0, 6, 0, 9, 12, 0, 15 };
		tune = TUNE[key - 41];
		return DRUM_TOM;
	}
	case 49: case 57: tune = key == 57 ? 2.0f : 0.0f; return DRUM_CRASH;
	case 52: case 55: tune = key == 55 ? 5.0f : -3.0f; return DRUM_CRASH;
	case 51: case 59: tune = key == 59 ? 2.0f : 0.0f; return DRUM_RIDE;
	case 53: tune = 6.0f; return DRUM_RIDE;
	case 56: return DRUM_BELL;
	default: return DRUM_PERC;
	}
}

inline const float *sine_table()
{
	static const std::array<float, 4097> table = [] {
		std::array<float, 4097> t{};
		for (int i = 0; i <= 4096; i++)
			t[size_t(i)] = float(std::sin(double(i) * 6.283185307179586 / 4096.0));
		return t;
	}();
	return table.data();
}

} // namespace fm_detail

// プログラム番号（0-127、GM の並び）→ 音色
inline const fm_patch &fm_patch_of(int program)
{
	program &= 127;
	return fm_detail::PATCHES[(program / 8) * 2 + ((program % 8) >= 4 ? 1 : 0)];
}

// ---- 書き換えられる音色と、その 128 個の組

struct fm_voice {
	char name[9] = "        ";      // 8 文字
	int alg = 7;
	float feedback = 0.0f;
	fm_op op[4] = { { 1, 1, 0.002f, 0, 1, 0.1f }, { 1, 0, 0.002f, 0, 1, 0.1f }, { 1, 0, 0.002f, 0, 1, 0.1f }, { 1, 0, 0.002f, 0, 1, 0.1f } };
	float drop = 0.0f;              // 鳴り始めに上乗せする半音（30ms ほどで元の高さへ落ちる）
	float noise = 0.0f;             // 鳴る側の変調に足すノイズ（0-1）
};

// そのつなぎ方で、オペレーター i（0-3）は鳴る側か
inline bool fm_is_carrier(int alg, int i)
{
	static const u8 MASK[8] = { 0x8, 0x8, 0x8, 0x8, 0xa, 0xe, 0xe, 0xf };
	return (MASK[alg & 7] >> i) & 1;
}

// 初期の音色（プログラム番号ごと。内蔵の 32 個を GM の並びに置いたもの）
inline fm_voice fm_default_voice(int program)
{
	const fm_patch &p = fm_patch_of(program);
	fm_voice v;
	std::snprintf(v.name, sizeof(v.name), "%-8.8s", p.name);
	v.alg = p.alg;
	v.feedback = p.feedback;
	for (int i = 0; i < 4; i++)
		v.op[i] = p.op[i];
	v.drop = p.drop;
	v.noise = p.noise;
	return v;
}

struct fm_bank {
	char name[15] = "FM SET";       // 組の名前（ファイルの名前にもなる）
	std::array<fm_voice, 128> prog;
	fm_bank()
	{
		for (int i = 0; i < 128; i++)
			prog[size_t(i)] = fm_default_voice(i);
	}
};

// 1 周期の形（n 点）。包絡線は全部いちばん上（鳴り始めの形）、ノイズと落ち幅は入れない。画面の絵用。
// フィードバックは同じ周期を何度か回して落ち着かせる
inline void fm_cycle(const fm_voice &v, float *out, int n)
{
	const auto wave = [](float cycles) { return float(std::sin(double(cycles) * 6.283185307179586)); };
	// 比が整数でないオペレーターは 1 周期で閉じないが、絵では基音 1 周期ぶんをそのまま描く
	std::vector<float> o1(size_t(n), 0.0f);
	for (int pass = 0; pass < 6; pass++) {
		float fb0 = o1[size_t(n - 1)], fb1 = o1[size_t(n - 2 >= 0 ? n - 2 : 0)];
		for (int k = 0; k < n; k++) {
			const float t = float(k) / float(n);
			const float cur = wave(t * v.op[0].ratio + 2.0f * v.feedback * 0.5f * (fb0 + fb1)) * v.op[0].level;
			fb1 = fb0;
			fb0 = cur;
			o1[size_t(k)] = cur;
		}
	}
	for (int k = 0; k < n; k++) {
		const float t = float(k) / float(n);
		const auto osc = [&](int i, float mod) { return wave(t * v.op[i].ratio + 2.0f * mod) * v.op[i].level; };
		const float a = o1[size_t(k)];
		float s = 0.0f;
		switch (v.alg & 7) {
		case 0: s = osc(3, osc(2, osc(1, a))); break;
		case 1: s = osc(3, osc(2, a + osc(1, 0.0f))); break;
		case 2: s = osc(3, a + osc(2, osc(1, 0.0f))); break;
		case 3: s = osc(3, osc(1, a) + osc(2, 0.0f)); break;
		case 4: s = (osc(1, a) + osc(3, osc(2, 0.0f))) * 0.5f; break;
		case 5: s = (osc(1, a) + osc(2, a) + osc(3, a)) * (1.0f / 3.0f); break;
		case 6: s = (osc(1, a) + osc(2, 0.0f) + osc(3, 0.0f)) * (1.0f / 3.0f); break;
		default: s = (a + osc(1, 0.0f) + osc(2, 0.0f) + osc(3, 0.0f)) * 0.25f; break;
		}
		out[k] = s;
	}
}

// ---- 組のファイル。全部リトルエンディアン
//   "SMUFMBNK"  u32 版（1）  名前 16 バイト
//   音色 128 個: 名前 8 バイト  u8 つなぎ方  u8 × 3 空き  f32 フィードバック・落ち幅・ノイズ
//                オペレーター 4 つ × f32 6 個（比・大きさ・アタック・ディケイ・サステイン・リリース）

namespace fm_detail {
inline void put_f32(std::vector<u8> &o, float f)
{
	u32 v;
	std::memcpy(&v, &f, 4);
	for (int i = 0; i < 4; i++)
		o.push_back(u8(v >> (8 * i)));
}
inline float get_f32(const u8 *p, float lo, float hi, float fallback)
{
	const u32 v = u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
	float f;
	std::memcpy(&f, &v, 4);
	return f >= lo && f <= hi ? f : fallback;       // NaN もここで落ちる
}
inline void clean_text(char *dst, const u8 *src, size_t n, bool pad)
{
	size_t i = 0;
	for (; i < n && src[i]; i++)
		dst[i] = src[i] >= 0x20 && src[i] < 0x7f ? char(src[i]) : '?';
	const size_t end = i;
	for (; i < n; i++)
		dst[i] = ' ';
	dst[pad ? n : end] = 0;
}
constexpr size_t FM_VOICE_BYTES = 8 + 4 + 12 + 4 * 24;
} // namespace fm_detail

inline std::vector<u8> write_fm_bank(const fm_bank &b)
{
	std::vector<u8> o = { 'S', 'M', 'U', 'F', 'M', 'B', 'N', 'K', 1, 0, 0, 0 };
	char name[16] = {};
	std::snprintf(name, sizeof(name), "%s", b.name);
	o.insert(o.end(), name, name + 16);
	for (const fm_voice &v : b.prog) {
		o.insert(o.end(), v.name, v.name + 8);
		o.insert(o.end(), { u8(v.alg & 7), 0, 0, 0 });
		fm_detail::put_f32(o, v.feedback);
		fm_detail::put_f32(o, v.drop);
		fm_detail::put_f32(o, v.noise);
		for (const fm_op &p : v.op)
			for (float f : { p.ratio, p.level, p.attack, p.decay, p.sustain, p.release })
				fm_detail::put_f32(o, f);
	}
	return o;
}

inline std::shared_ptr<fm_bank> read_fm_bank(const u8 *d, size_t n, std::string &err)
{
	if (n < 28 || std::memcmp(d, "SMUFMBNK", 8) != 0) {
		err = "not an FM voice set";
		return nullptr;
	}
	if (d[8] != 1 || d[9] || d[10] || d[11]) {
		err = "unknown version";
		return nullptr;
	}
	if (n < 28 + 128 * fm_detail::FM_VOICE_BYTES) {
		err = "file is cut short";
		return nullptr;
	}
	auto b = std::make_shared<fm_bank>();
	fm_detail::clean_text(b->name, d + 12, 14, false);
	const u8 *p = d + 28;
	for (fm_voice &v : b->prog) {
		fm_detail::clean_text(v.name, p, 8, true);
		v.alg = p[8] & 7;
		v.feedback = fm_detail::get_f32(p + 12, 0.0f, 1.0f, 0.0f);
		v.drop = fm_detail::get_f32(p + 16, -48.0f, 48.0f, 0.0f);
		v.noise = fm_detail::get_f32(p + 20, 0.0f, 1.0f, 0.0f);
		const u8 *q = p + 24;
		for (fm_op &op : v.op) {
			op.ratio = fm_detail::get_f32(q, 0.05f, 32.0f, 1.0f);
			op.level = fm_detail::get_f32(q + 4, 0.0f, 1.0f, 0.0f);
			op.attack = fm_detail::get_f32(q + 8, 0.0f, 10.0f, 0.002f);
			op.decay = fm_detail::get_f32(q + 12, 0.0f, 30.0f, 0.0f);
			op.sustain = fm_detail::get_f32(q + 16, 0.0f, 1.0f, 1.0f);
			op.release = fm_detail::get_f32(q + 20, 0.0f, 30.0f, 0.1f);
			q += 24;
		}
		p += fm_detail::FM_VOICE_BYTES;
	}
	return b;
}

inline bool save_fm_bank(const std::string &path, const fm_bank &b)
{
	const std::vector<u8> bytes = write_fm_bank(b);
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	return std::fclose(f) == 0 && ok;
}

inline std::shared_ptr<fm_bank> load_fm_bank(const std::string &path, std::string &err)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		err = "cannot open the file";
		return nullptr;
	}
	std::vector<u8> bytes;
	u8 buf[16384];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < (1u << 20))
		bytes.insert(bytes.end(), buf, buf + n);
	std::fclose(f);
	return read_fm_bank(bytes.data(), bytes.size(), err);
}

class fm_synth
{
public:
	static constexpr int VOICES = 32;
	static constexpr double RATE = 44100.0;

	// 音色の組を替える（nullptr なら初期の組）。鳴っている音にも、新しい値をすぐ効かせる（包絡線の進み具合はそのまま）
	void set_bank(std::shared_ptr<const fm_bank> bank)
	{
		m_bank = std::move(bank);
		for (voice &v : m_v)
			if (v.on && v.prog >= 0) {
				const fm_voice fv = voice_of(v.prog);
				load(v, fv);
				setup_ops(v, v.vel);
				v.tuned = false;
			}
	}
	const fm_bank *bank() const { return m_bank.get(); }
	// そのプログラムの音色（組が無ければ初期の音色）
	fm_voice voice_of(int program) const { return m_bank ? m_bank->prog[size_t(program & 127)] : fm_default_voice(program); }

	void reset()
	{
		for (voice &v : m_v)
			v.on = false;
		for (int c = 0; c < 16; c++) {
			m_c[size_t(c)] = chan();
			m_c[size_t(c)].drum = c == 9;
		}
	}

	u8 program(int channel) const { return m_c[size_t(channel & 15)].program; }
	bool drum(int channel) const { return m_c[size_t(channel & 15)].drum; }
	void set_drum(int channel, bool on) { m_c[size_t(channel & 15)].drum = on; }

	void midi(u8 status, u8 d0, u8 d1)
	{
		const int ch = status & 15;
		chan &c = m_c[size_t(ch)];
		switch (status & 0xf0) {
		case 0x90:
			if (d1) {
				note_on(ch, d0, d1);
				break;
			}
			[[fallthrough]];
		case 0x80:
			for (voice &v : m_v)
				if (v.on && v.ch == ch && v.key == d0 && !v.released) {
					if (c.pedal)
						v.held = true;
					else
						v.released = true;
				}
			break;
		case 0xb0:
			switch (d0) {
			case 1:   c.mod = d1 / 127.0f; break;
			case 64:
				c.pedal = d1 >= 64;
				if (!c.pedal)
					for (voice &v : m_v)
						if (v.on && v.ch == ch && v.held) {
							v.held = false;
							v.released = true;
						}
				break;
			case 120:
				for (voice &v : m_v)
					if (v.ch == ch)
						v.on = false;
				break;
			case 121:
				c.mod = 0.0f;
				c.bend = 0.0f;
				c.pedal = false;
				[[fallthrough]];
			case 123:
				for (voice &v : m_v)
					if (v.on && v.ch == ch && (d0 == 123 || v.held)) {
						v.held = false;
						v.released = true;
					}
				break;
			default: break;
			}
			break;
		case 0xc0:
			c.program = d0 & 127;
			break;
		case 0xe0:
			c.bend = float((((d1 << 7) | d0) - 8192) / 8192.0 * 2.0);      // 半音
			break;
		default:
			break;
		}
	}

	bool sounding() const
	{
		for (const voice &v : m_v)
			if (v.on)
				return true;
		return false;
	}

	// 1 サンプル（44.1kHz）。チャンネルごとの音を足し込む（モノラル）。±1.0 が全振幅
	void render(float out[16])
	{
		const float *sine = fm_detail::sine_table();
		m_lfo += 5.5 / RATE;
		if (m_lfo >= 1.0)
			m_lfo -= 1.0;
		const float vib = float(std::sin(m_lfo * 6.283185307179586)) * 0.4f;      // ±0.4 半音まで
		const bool retune = !(m_tick++ & 15);
		for (voice &v : m_v) {
			if (!v.on)
				continue;
			const fm_patch &p = v.cur;
			const chan &c = m_c[size_t(v.ch)];
			if (retune || !v.tuned) {
				// 高さ（16 サンプルごと）。ドラムの落ち幅は 30ms ほどで消える
				v.tuned = true;
				const float semis = v.base_key + c.bend + c.mod * vib + v.drop;
				v.drop *= 0.985f;
				const double hz = 440.0 * std::pow(2.0, (double(semis) - 69.0) / 12.0);
				for (int i = 0; i < 4; i++)
					v.inc[i] = u32(std::min(hz * double(p.op[i].ratio), RATE * 0.45) / RATE * 4294967296.0);
			}
			// 包絡線。速い立ち上がり（10ms まで）は、途中で離されても上がりきってから下げる（ゲートの短い音が消えないように）
			bool alive = false;
			for (int i = 0; i < 4; i++) {
				op_state &o = v.op[i];
				if (v.released && !(o.rising && o.d_attack >= QUICK)) {
					o.env *= o.k_release;
					o.rising = false;
				} else if (o.rising) {
					o.env += o.d_attack;
					if (o.env >= 1.0f) {
						o.env = 1.0f;
						o.rising = false;
					}
				} else if (o.k_decay < 1.0f) {
					o.env = o.sustain + (o.env - o.sustain) * o.k_decay;
				}
				if (o.carrier && (o.rising || o.env > 1e-4f))
					alive = true;
			}
			if (!alive) {
				v.on = false;
				continue;
			}
			// オペレーター。変調は周の数で足す（1.0 の深さで 2 周ぶん）
			const auto osc = [&](int i, float mod) {
				op_state &o = v.op[i];
				o.phase += v.inc[i];
				// 2 周 × 2^32 は 32 ビットに入らないので、16 分の 1 にして掛け直す（変調は ±3.9 までに抑える）
				const u32 ph = o.phase + u32(s32(std::clamp(mod, -3.9f, 3.9f) * 536870912.0f)) * 16u;
				const u32 idx = ph >> 20;
				const float frac = float(ph & 0xfffff) * (1.0f / 1048576.0f);
				return (sine[idx] + (sine[idx + 1] - sine[idx]) * frac) * o.env * o.level;
			};
			float nz = 0.0f;
			if (p.noise > 0.0f) {
				v.lfsr = v.lfsr * 1664525u + 1013904223u;
				nz = (float(v.lfsr >> 8) * (1.0f / 8388608.0f) - 1.0f) * p.noise;
			}
			const float o1 = osc(0, p.feedback * 0.5f * (v.fb0 + v.fb1));
			v.fb1 = v.fb0;
			v.fb0 = o1;
			float s = 0.0f;
			switch (p.alg) {
			case 0: s = osc(3, osc(2, osc(1, o1)) + nz); break;
			case 1: s = osc(3, osc(2, o1 + osc(1, 0.0f)) + nz); break;
			case 2: s = osc(3, o1 + osc(2, osc(1, 0.0f)) + nz); break;
			case 3: s = osc(3, osc(1, o1) + osc(2, 0.0f) + nz); break;
			case 4: s = (osc(1, o1 + nz) + osc(3, osc(2, 0.0f) + nz)) * 0.5f; break;
			case 5: s = (osc(1, o1 + nz) + osc(2, o1 + nz) + osc(3, o1 + nz)) * (1.0f / 3.0f); break;
			case 6: s = (osc(1, o1 + nz) + osc(2, nz) + osc(3, nz)) * (1.0f / 3.0f); break;
			default: s = (o1 + osc(1, nz) + osc(2, nz) + osc(3, nz)) * 0.25f; break;
			}
			out[v.ch] += s * v.gain;
		}
	}

private:
	static constexpr float QUICK = 1.0f / 441.0f;     // 10 ミリ秒で上がりきる、1 サンプルごとの上がり幅

	struct chan {
		u8 program = 0;
		bool drum = false, pedal = false;
		float bend = 0.0f, mod = 0.0f;
	};
	struct op_state {
		u32 phase = 0;
		float env = 0.0f, level = 0.0f, sustain = 1.0f;
		float d_attack = 1.0f, k_decay = 1.0f, k_release = 0.0f;
		bool rising = true, carrier = false;
	};
	struct voice {
		bool on = false, released = false, held = false, tuned = false;
		int ch = 0, group = 0;
		u8 key = 60;
		fm_patch cur{};               // 鳴らしている音色の写し（書き換えが来たら入れ直す）
		int prog = -1;                // メロディの音色ならプログラム番号、ドラムは -1
		float vel = 1.0f;
		float base_key = 60.0f, drop = 0.0f, gain = 0.0f;
		float fb0 = 0.0f, fb1 = 0.0f;
		u32 inc[4] = { 0, 0, 0, 0 };
		u32 lfsr = 1;
		op_state op[4];
		u32 age = 0;
	};

	// seconds で 60dB 下がる、1 サンプルごとの倍率
	static float fall(float seconds) { return seconds <= 0.0f ? 1.0f : float(std::pow(0.001, 1.0 / (double(seconds) * RATE))); }
	// 書き換えられる音色を、声の写しに入れる
	static void load(voice &v, const fm_voice &fv)
	{
		v.cur.alg = fv.alg & 7;
		v.cur.feedback = fv.feedback;
		for (int i = 0; i < 4; i++)
			v.cur.op[i] = fv.op[i];
		v.cur.noise = fv.noise;
		v.cur.drop = fv.drop;
		v.cur.fixed_key = 0.0f;
		v.cur.group = 0;
	}
	// オペレーターごとの大きさと包絡線の速さを、声の写しから決める（包絡線の今の位置は触らない）
	static void setup_ops(voice &v, float vv)
	{
		for (int i = 0; i < 4; i++) {
			const fm_op &src = v.cur.op[i];
			op_state &o = v.op[i];
			o.carrier = fm_is_carrier(v.cur.alg, i);
			// 変調する側は、強く弾くほど深く（半分は強さによらない）
			o.level = o.carrier ? src.level : src.level * (0.5f + 0.5f * vv);
			o.sustain = src.sustain;
			o.d_attack = src.attack <= 0.0f ? 1.0f : float(1.0 / (double(src.attack) * RATE));
			o.k_decay = src.decay <= 0.0f || src.sustain >= 1.0f ? 1.0f : fall(src.decay);
			o.k_release = fall(std::max(src.release, 0.003f));
		}
	}

	void note_on(int ch, u8 key, u8 vel)
	{
		const chan &c = m_c[size_t(ch)];
		float tune = 0.0f;
		const fm_patch *drum = c.drum ? &fm_detail::drum_for_key(key, tune) : nullptr;
		const int group = drum ? drum->group : 0;
		// 同じ組（ハイハットの開閉）で鳴っている音は止める
		if (group)
			for (voice &o : m_v)
				if (o.on && o.ch == ch && o.group == group)
					o.on = false;
		voice *use = nullptr;
		for (voice &v : m_v)
			if (!v.on) {
				use = &v;
				break;
			}
		// 空きが無ければ、いちばん小さく鳴っている声（鳴る側の包絡線の和が小さいもの）を譲ってもらう
		if (!use) {
			float least = 1e9f;
			for (voice &v : m_v) {
				float e = 0.0f;
				for (const op_state &o : v.op)
					if (o.carrier)
						e += o.rising ? 1.0f : o.env;
				if (e * v.gain < least) {
					least = e * v.gain;
					use = &v;
				}
			}
		}
		voice &v = *use;
		v = voice();
		v.on = true;
		v.ch = ch;
		v.key = key;
		if (drum) {
			v.cur = *drum;
			v.prog = -1;
		} else {
			load(v, voice_of(c.program));
			v.prog = c.program;
		}
		v.group = group;
		v.base_key = (v.cur.fixed_key > 0.0f ? v.cur.fixed_key : float(key)) + tune;
		v.drop = v.cur.drop;
		v.lfsr = (++m_age) * 2654435761u | 1u;
		v.age = m_age;
		const float vv = vel / 127.0f;
		v.vel = vv;
		v.gain = 0.35f * vv * vv;
		setup_ops(v, vv);
	}

	std::shared_ptr<const fm_bank> m_bank;
	std::array<voice, VOICES> m_v{};
	std::array<chan, 16> m_c = [] { std::array<chan, 16> a{}; a[9].drum = true; return a; }();
	double m_lfo = 0.0;
	u32 m_tick = 0, m_age = 0;
};

} // namespace smu2000::vboard

#endif // S_MU2000_VBOARD_FM_H
