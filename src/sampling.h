// license:BSD-3-Clause
//
// サンプリングの管理情報（firmware が CPU の DRAM に持つ表）の形と、録音の道具。
// パネルを通さず、firmware と同じ形で表を読み書きする（doc/sampling-ram.md）。
//
// 調べ方: パネルで録る・残す・音色に割り当てる前後で DRAM とワーク RAM を突き合わせ（src/smpre.cpp）、
// 波形と表だけを起動したての機械に写して、firmware が一覧に出し、試聴でき、音色として鳴ることを確かめた。
// ワーク RAM にあるサンプルの数や残りの容量の写しは、firmware が SAMPLING に入るたびに表から数え直す。
#ifndef S_MU2000_SAMPLING_H
#define S_MU2000_SAMPLING_H
#pragma once

#include "compat/mamecompat.h"

#include <string>

namespace smu2000::sampling {

// ---- DRAM の番地（CPU から見た番地）
// 鳴らすための表。16 バイト × 512。サンプル n（1 から）は TAB_PLAY + 16 * n。
//   +0 u32 長さ（サンプル数 − 4。終わりの語と開始の語の差の 2 倍から 4 引いたもの）
//   +4 u32 開始の語（サンプリング RAM の 32bit 語の位置）| 0x01000000
//   +8 00 3c 00 ff 40 00 00 00（基準の鍵 60 ほか。録った直後はいつもこの形）
constexpr u32 TAB_PLAY = 0x103fa20;
// サンプルの記録。36 バイト × 512。サンプル n（1 から）は TAB_SAMPLE + 36 * (n - 1)。
//   +0 u16 番号（n − 1、ビッグエンディアン）  +2 印（0x40 = 使っている）  +3 0
//   +4 00 00 00 00  +8 ff ff ff ff  +12 u32 サンプリング周波数（44100）
//   +16 u32 開始の語 | 0x01000000  +20 u32 終わりの語 | 0x01000000
//   +24 04 00 00 00  +28 名前 8 文字（空白で埋める）
constexpr u32 TAB_SAMPLE = 0x106be04;
// 次に録る語 | 0x01000000（表のすぐ後ろ）
constexpr u32 NEXT_FREE = 0x1070604;
constexpr int MAX_SAMPLES = 512;
// サンプル音色。350 バイト × 256（Bank# 0 の PGM001-128、Bank# 1 の PGM129-256）。
//   +0 01 7f  +2 名前 8 文字（空白で埋める）  +10 00 00
//   +12 1 つ目の要素: 鳴らすなら 01、+13 7f、+14 u16 0x4000 | (サンプル n − 1)。割り当て無しは 00 7f 3f 7f
//   +0x1d 半音（0x40 = 0、±12 で 1 オクターブ）  +0x1e 微調（0x40 = 0、1 でおよそ 1 セント）
//   +0x47 Level（0-127）  +0x51 Pan（0 = L7、7 = C、14 = R7、15 = Scaling）
constexpr u32 TAB_VOICE = 0x1054e00;
constexpr u32 VOICE_SIZE = 350;
constexpr int MAX_VOICES = 256;
constexpr u32 SAMPLE_RATE = 44100;
// サンプリング RAM は 4MB = 0x100000 語（1 語に 16bit のサンプル 2 つ、下の 16bit が先）
constexpr u32 RAM_WORDS = 0x100000;

struct sample
{
	int number = 0;          // 1 から
	std::string name;
	u32 start = 0, end = 0;  // サンプリング RAM の語の位置
	u32 rate = SAMPLE_RATE;
	int peak = -1;           // 波形の最大の絶対値（16bit）。-1 はまだ測っていない（sampling_peak）
	u32 frames() const { return (end - start) * 2; }
};

struct voice
{
	bool assigned = false;   // 1 つ目の要素がサンプルを鳴らすか
	std::string name;
	int sample = 0;          // 1 から（assigned のとき）
	int level = 127;         // 0-127
	int pan = 7;             // 0 = L7、7 = C、14 = R7、15 = Scaling
	int coarse = 0;          // 半音（-24〜+24）
	int fine = 0;            // セント（-64〜+63）
};

// 録音で入力のどれを録るか（firmware の InputSrc と同じ並び）
enum class source { ad1, ad2, both };

} // namespace smu2000::sampling

#endif
