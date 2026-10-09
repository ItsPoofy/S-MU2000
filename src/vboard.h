// license:BSD-3-Clause
//
// **架空のプラグインボード**。実在しないボードを MU2000 に挿したことにして、その音を MU のミキサーとエフェクトに通す。
//
// 実機のプラグインボード（PLG100 / PLG150）は、割り当てたパートの MIDI を受けて自分で音を作り、その音を本体の
// DSP に渡す。本体はそのパートの音量・パン・エフェクトの送りを、内蔵の音色と同じように効かせる。ここも同じ形にする:
//   ・ボードは、選んだパートが受ける MIDI（そのパートの受信チャンネル）で鳴る
//   ・本体の内蔵の音は、そのパートだけ消す（mu2000 の中のパートのミュート）
//   ・ボードの音は、そのパートの音量・エクスプレッション・パン・リバーブ／コーラスの送りを掛けて、
//     MU のエフェクトの入口へ入れる（mu2000::set_external_audio）
// firmware はボードを知らない（液晶に PLG の印は出ない）。ボードと本体のやり取りを再現するのは別の話。
//
// 1 枚目は **FC ボード**: 80 年代の家庭用ゲーム機の音源（矩形波 2・三角波・ノイズ）ふうの音を、8 音まで重ねて鳴らす。
// 中身は全部ここで書いた計算で、ROM もサンプルも使わない。
//
//   初期のプログラム（下 3 ビットが音、bit3 が鳴り方）
//     0 矩形波 50%   1 矩形波 25%   2 矩形波 12.5%   3 三角波（16 段）
//     4 ノイズ（長い周期）   5 ノイズ（短い周期。金属的）   6 デューティを 60 分の 1 秒ごとに切り替える矩形波
//     7 1 オクターブ上と交互に鳴る矩形波（高速アルペジオ）
//     +8 すると、押している間も音量が段々に下がる（減衰する鳴り方）
//   音量は 16 段（強さで決まる）、60 分の 1 秒ごとに段を進める。三角波は実物どおり音量を持たない（鳴るか鳴らないか）
//   ピッチベンド（±2 半音）、モジュレーション（CC1。ビブラート）、CC123 / CC120（全部止める）
//
//   音色は書き換えられる。128 個のプログラムそれぞれが自分の音色（fc_voice）を持ち、その組が fc_bank。
//   触れるのは、当時のゲームの音作りで使われた手だけ（全部 60 分の 1 秒の刻み）:
//     ・波（矩形波・三角波・ノイズ 2 種）
//     ・デューティの並び（4 つまで。1/8・1/4・1/2・3/4 を何コマごとに切り替えるか）
//     ・アルペジオ（4 つまでの半音のずれを、何コマごとに切り替えるか）
//     ・音量の下がり方（何コマで 1 段下がるか、どの段で止まるか、離してから 1 コマに何段下がるか）
//     ・鳴り始めの音程のずれ（何半音ずれた所から、何コマで元の高さへ寄るか）
//     ・自動のビブラート（深さと、掛かり始めるまでのコマ数）
//   組を渡さなければ（set_bank(nullptr)）初期の音色で、プログラム 17 以降は 1〜16 のくり返し（今までと同じ鳴り方）。
//   ファイル（.smufc）は下の write_fc_bank / read_fc_bank の形
//
//   音色の値は、コントロールチェンジでも動かせる（シーケンサーから曲の途中で音を作り変える）。初期の番号は下の FC_PARAM_CC:
//     CC20-31 が波・デューティ・音量・ビブラート、CC102-109 がアルペジオと鳴り始めのずれ。値はエディタの数字そのまま
//     （半音のずれだけ 64 が 0）。範囲の外は端に寄せる。番号は付け替えられる（fc_cc_map。ボードが別の用に聞く番号は使えない）
//   動くのは**そのボード（16 パートのボードならそのチャンネル）がいま選んでいるプログラムの音だけ**で、音色の組も
//   ファイルも書き換えない。触っていない値は組の音色のまま。プログラムチェンジを受けると、触った値は全部捨てて組の音色に戻る

#ifndef S_MU2000_VBOARD_H
#define S_MU2000_VBOARD_H

#pragma once

#include "compat/mamecompat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace smu2000::vboard {

// FC ボードの初期のプログラム（0-15）の名前（8 文字。液晶と画面に出す）
inline const char *fc_program_name(int program)
{
	static const char *const names[16] = {
		"Square50", "Square25", "Square12", "Triangle", "Noise   ", "MetalNz ", "DutySwp ", "OctArp  ",
		"Sq50 Dcy", "Sq25 Dcy", "Sq12 Dcy", "Tri Dcy ", "NoiseDcy", "MetalDcy", "SweepDcy", "ArpDcy  ",
	};
	return names[program & 15];
}

// ---- 書き換えられる音色と、その 128 個の組。値は全部「コマ」（60 分の 1 秒）と「段」（音量の 16 段）で持つ

struct fc_voice {
	enum { SQUARE = 0, TRIANGLE = 1, NOISE = 2, METAL = 3, WAVES };
	char name[9] = "        ";      // 8 文字
	u8 wave = SQUARE;
	u8 duty_len = 1;                // デューティの並びの長さ（1-4）。1 なら切り替えない
	u8 duty_frames = 2;             // 何コマごとに次へ進むか（1-30）
	u8 duty[4] = { 2, 2, 2, 2 };    // 0 = 1/8、1 = 1/4、2 = 1/2、3 = 3/4
	u8 arp_len = 1;                 // アルペジオの並びの長さ（1-4）。1 なら切り替えない
	u8 arp_frames = 1;              // 何コマごとに次へ進むか（1-30）
	s8 arp[4] = { 0, 0, 0, 0 };     // 鍵からの半音のずれ（-24〜24）
	u8 decay = 0;                   // 押している間、何コマで 1 段下がるか（0 = 下がらない。1-60）
	u8 floor = 0;                   // 下がって止まる段（0 = 消えるまで。0-15）
	u8 release = 2;                 // 離してから、1 コマに何段下がるか（1-15）
	s8 sweep = 0;                   // 鳴り始めの音程のずれ（半音。-48〜48）
	u8 sweep_frames = 0;            // 何コマで元の高さに寄るか（0 = ずらさない。1-60）
	u8 vib_depth = 0;               // 自動のビブラートの深さ（セント。0-100）
	u8 vib_delay = 0;               // 掛かり始めるまでのコマ数（0-120）
};

inline const double FC_DUTY[4] = { 0.125, 0.25, 0.5, 0.75 };

// 初期の音色（プログラム番号ごと。16 個のくり返し）
inline fc_voice fc_default_voice(int program)
{
	fc_voice v;
	std::snprintf(v.name, sizeof(v.name), "%-8.8s", fc_program_name(program));
	switch (program & 7) {
	case 0: break;
	case 1: v.duty[0] = 1; break;
	case 2: v.duty[0] = 0; break;
	case 3: v.wave = fc_voice::TRIANGLE; break;
	case 4: v.wave = fc_voice::NOISE; break;
	case 5: v.wave = fc_voice::METAL; break;
	case 6: v.duty_len = 4; v.duty_frames = 2; v.duty[0] = 0; v.duty[1] = 1; v.duty[2] = 2; v.duty[3] = 1; break;
	case 7: v.arp_len = 2; v.arp_frames = 1; v.arp[1] = 12; break;
	}
	if (program & 8)
		v.decay = 4;
	return v;
}

struct fc_bank {
	char name[15] = "FC SET";       // 組の名前（ファイルの名前にもなる）
	std::array<fc_voice, 128> prog;
	fc_bank()
	{
		for (int i = 0; i < 128; i++)
			prog[size_t(i)] = fc_default_voice(i);
	}
};

// 範囲の外の値を直す（ファイルから読んだ値、画面から来た値）
inline void fc_clamp(fc_voice &v)
{
	v.wave = u8(std::min<int>(v.wave, fc_voice::WAVES - 1));
	v.duty_len = u8(std::clamp<int>(v.duty_len, 1, 4));
	v.duty_frames = u8(std::clamp<int>(v.duty_frames, 1, 30));
	v.arp_len = u8(std::clamp<int>(v.arp_len, 1, 4));
	v.arp_frames = u8(std::clamp<int>(v.arp_frames, 1, 30));
	for (int i = 0; i < 4; i++) {
		v.duty[i] &= 3;
		v.arp[i] = s8(std::clamp<int>(v.arp[i], -24, 24));
	}
	v.decay = u8(std::min<int>(v.decay, 60));
	v.floor = u8(std::min<int>(v.floor, 15));
	v.release = u8(std::clamp<int>(v.release, 1, 15));
	v.sweep = s8(std::clamp<int>(v.sweep, -48, 48));
	v.sweep_frames = u8(std::min<int>(v.sweep_frames, 60));
	v.vib_depth = u8(std::min<int>(v.vib_depth, 100));
	v.vib_delay = u8(std::min<int>(v.vib_delay, 120));
}

// ---- コントロールチェンジで触る値。並びは fc_voice の欄で、番号（FC_PARAM_CC）は XG が使っていない所
//   値は欄の数字そのまま。符号のある欄（アルペジオのずれ・鳴り始めのずれ）は 64 を 0 として送る

enum fc_param {
	FC_P_WAVE, FC_P_DUTY_LEN, FC_P_DUTY_FRAMES, FC_P_DUTY1, FC_P_DUTY2, FC_P_DUTY3, FC_P_DUTY4,
	FC_P_DECAY, FC_P_FLOOR, FC_P_RELEASE, FC_P_VIB_DEPTH, FC_P_VIB_DELAY,
	FC_P_ARP_LEN, FC_P_ARP_FRAMES, FC_P_ARP1, FC_P_ARP2, FC_P_ARP3, FC_P_ARP4, FC_P_SWEEP, FC_P_SWEEP_FRAMES,
	FC_PARAMS
};

inline const u8 FC_PARAM_CC[FC_PARAMS] = {
	20, 21, 22, 23, 24, 25, 26,
	27, 28, 29, 30, 31,
	102, 103, 104, 105, 106, 107, 108, 109,
};

// その番号のコントロールチェンジが触る値（無ければ -1）
inline int fc_param_of_cc(u8 cc)
{
	return cc >= 20 && cc <= 31 ? cc - 20 : cc >= 102 && cc <= 109 ? FC_P_ARP_LEN + (cc - 102) : -1;
}

// コントロールチェンジの値（0-127）を音色の欄に入れる。範囲の外は fc_clamp が端に寄せる
inline void fc_set_param(fc_voice &v, int param, int value)
{
	const u8 x = u8(value & 127);
	switch (param) {
	case FC_P_WAVE:         v.wave = x; break;
	case FC_P_DUTY_LEN:     v.duty_len = x; break;
	case FC_P_DUTY_FRAMES:  v.duty_frames = x; break;
	case FC_P_DUTY1: case FC_P_DUTY2: case FC_P_DUTY3: case FC_P_DUTY4:
		v.duty[param - FC_P_DUTY1] = u8(std::min<int>(x, 3));
		break;
	case FC_P_DECAY:        v.decay = x; break;
	case FC_P_FLOOR:        v.floor = x; break;
	case FC_P_RELEASE:      v.release = x; break;
	case FC_P_VIB_DEPTH:    v.vib_depth = x; break;
	case FC_P_VIB_DELAY:    v.vib_delay = x; break;
	case FC_P_ARP_LEN:      v.arp_len = x; break;
	case FC_P_ARP_FRAMES:   v.arp_frames = x; break;
	case FC_P_ARP1: case FC_P_ARP2: case FC_P_ARP3: case FC_P_ARP4:
		v.arp[param - FC_P_ARP1] = s8(int(x) - 64);
		break;
	case FC_P_SWEEP:        v.sweep = s8(int(x) - 64); break;
	case FC_P_SWEEP_FRAMES: v.sweep_frames = x; break;
	default: break;
	}
	fc_clamp(v);
}

// ---- 番号の付け替え。欄ごとのコントロールチェンジの番号（0 = 割り当てない）
//   使えない番号: ボードや本体のパートが別の用に聞くもの（バンク・モジュレーション・データエントリー・音量・パン・
//   エクスプレッション・ホールド・エフェクトの送り・RPN / NRPN）と、120 以上（チャンネルモードメッセージ）

inline bool fc_cc_allowed(int cc)
{
	switch (cc) {
	case 0: case 1: case 6: case 7: case 10: case 11: case 32: case 38: case 64:
	case 91: case 93: case 94: case 96: case 97: case 98: case 99: case 100: case 101:
		return false;
	default:
		return cc > 0 && cc < 120;
	}
}

struct fc_cc_map {
	u8 cc[FC_PARAMS];
	fc_cc_map() { std::copy(FC_PARAM_CC, FC_PARAM_CC + FC_PARAMS, cc); }
	int param_of(u8 number) const
	{
		if (number)
			for (int i = 0; i < FC_PARAMS; i++)
				if (cc[i] == number)
					return i;
		return -1;
	}
	// 欄に番号を付ける（0 = 外す）。使えない番号は断る。ほかの欄がその番号を使っていたら、そちらを外す
	bool assign(int param, int number)
	{
		if (param < 0 || param >= FC_PARAMS || (number && !fc_cc_allowed(number)))
			return false;
		if (number)
			for (u8 &c : cc)
				if (c == number)
					c = 0;
		cc[param] = u8(number);
		return true;
	}
	bool operator==(const fc_cc_map &o) const { return std::equal(cc, cc + FC_PARAMS, o.cc); }
};

// 設定ファイルに書く形: 欄の順に、番号をコンマで並べる（"20,21,22,…"。0 = 割り当てない）
inline std::string fc_cc_text(const fc_cc_map &m)
{
	std::string s;
	for (int i = 0; i < FC_PARAMS; i++)
		s += (i ? "," : "") + std::to_string(m.cc[i]);
	return s;
}

// 読む。足りない欄は初期の番号、使えない番号や重なった番号は 0（割り当てない）
inline fc_cc_map fc_cc_parse(const std::string &text)
{
	fc_cc_map m;
	if (text.empty())
		return m;
	int given[FC_PARAMS];
	int n = 0;
	for (size_t at = 0; n < FC_PARAMS && at <= text.size();) {
		const size_t end = std::min(text.find(',', at), text.size());
		given[n++] = std::atoi(text.substr(at, end - at).c_str());
		at = end + 1;
	}
	// 書いてある欄を先に空けてから入れる（初期の番号が残って、書いてある番号を押しのけないように）
	for (int i = 0; i < n; i++)
		m.cc[i] = 0;
	for (int i = 0; i < n; i++)
		if (given[i] > 0 && fc_cc_allowed(given[i]) && m.param_of(u8(given[i])) < 0)
			m.cc[i] = u8(given[i]);
	return m;
}

// 音色の欄を、コントロールチェンジで送る値にする（fc_set_param の逆）
inline int fc_get_param(const fc_voice &v, int param)
{
	switch (param) {
	case FC_P_WAVE:         return v.wave;
	case FC_P_DUTY_LEN:     return v.duty_len;
	case FC_P_DUTY_FRAMES:  return v.duty_frames;
	case FC_P_DUTY1: case FC_P_DUTY2: case FC_P_DUTY3: case FC_P_DUTY4:
		return v.duty[param - FC_P_DUTY1];
	case FC_P_DECAY:        return v.decay;
	case FC_P_FLOOR:        return v.floor;
	case FC_P_RELEASE:      return v.release;
	case FC_P_VIB_DEPTH:    return v.vib_depth;
	case FC_P_VIB_DELAY:    return v.vib_delay;
	case FC_P_ARP_LEN:      return v.arp_len;
	case FC_P_ARP_FRAMES:   return v.arp_frames;
	case FC_P_ARP1: case FC_P_ARP2: case FC_P_ARP3: case FC_P_ARP4:
		return v.arp[param - FC_P_ARP1] + 64;
	case FC_P_SWEEP:        return v.sweep + 64;
	case FC_P_SWEEP_FRAMES: return v.sweep_frames;
	default:                return 0;
	}
}

// ---- ファイル
//   "SMUFCBNK"  u32 版（1）  組の名前 16 バイト  音色 128 個
//   音色 1 つ = 名前 8 バイト + 24 バイト:
//     波、デューティの長さ・コマ・並び 4 つ、アルペジオの長さ・コマ・並び 4 つ（符号つき）、
//     下がるコマ・止まる段・離したときの段、ずれ（符号つき）・ずれのコマ、ビブラートの深さ・遅れ、残りは 0

namespace fc_detail {
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
constexpr size_t FC_VOICE_BYTES = 8 + 24;
} // namespace fc_detail

inline std::vector<u8> write_fc_bank(const fc_bank &b)
{
	std::vector<u8> o = { 'S', 'M', 'U', 'F', 'C', 'B', 'N', 'K', 1, 0, 0, 0 };
	char name[16] = {};
	std::snprintf(name, sizeof(name), "%s", b.name);
	o.insert(o.end(), name, name + 16);
	for (const fc_voice &v : b.prog) {
		o.insert(o.end(), v.name, v.name + 8);
		const u8 raw[24] = {
			v.wave, v.duty_len, v.duty_frames, v.duty[0], v.duty[1], v.duty[2], v.duty[3],
			v.arp_len, v.arp_frames, u8(v.arp[0]), u8(v.arp[1]), u8(v.arp[2]), u8(v.arp[3]),
			v.decay, v.floor, v.release, u8(v.sweep), v.sweep_frames, v.vib_depth, v.vib_delay, 0, 0, 0, 0,
		};
		o.insert(o.end(), raw, raw + 24);
	}
	return o;
}

inline std::shared_ptr<fc_bank> read_fc_bank(const u8 *d, size_t n, std::string &err)
{
	if (n < 28 || std::memcmp(d, "SMUFCBNK", 8) != 0) {
		err = "not an FC voice set";
		return nullptr;
	}
	if (d[8] != 1 || d[9] || d[10] || d[11]) {
		err = "unknown version";
		return nullptr;
	}
	if (n < 28 + 128 * fc_detail::FC_VOICE_BYTES) {
		err = "file is cut short";
		return nullptr;
	}
	auto b = std::make_shared<fc_bank>();
	fc_detail::clean_text(b->name, d + 12, 14, false);
	const u8 *p = d + 28;
	for (fc_voice &v : b->prog) {
		fc_detail::clean_text(v.name, p, 8, true);
		const u8 *q = p + 8;
		v.wave = q[0];
		v.duty_len = q[1];
		v.duty_frames = q[2];
		v.arp_len = q[7];
		v.arp_frames = q[8];
		for (int i = 0; i < 4; i++) {
			v.duty[i] = q[3 + i];
			v.arp[i] = s8(q[9 + i]);
		}
		v.decay = q[13];
		v.floor = q[14];
		v.release = q[15];
		v.sweep = s8(q[16]);
		v.sweep_frames = q[17];
		v.vib_depth = q[18];
		v.vib_delay = q[19];
		fc_clamp(v);
		p += fc_detail::FC_VOICE_BYTES;
	}
	return b;
}

inline bool save_fc_bank(const std::string &path, const fc_bank &b)
{
	const std::vector<u8> bytes = write_fc_bank(b);
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	return std::fclose(f) == 0 && ok;
}

inline std::shared_ptr<fc_bank> load_fc_bank(const std::string &path, std::string &err)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		err = "cannot open the file";
		return nullptr;
	}
	std::vector<u8> bytes;
	u8 buf[8192];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < (1u << 20))
		bytes.insert(bytes.end(), buf, buf + n);
	std::fclose(f);
	return read_fc_bank(bytes.data(), bytes.size(), err);
}

class fc_board
{
public:
	static constexpr int VOICES = 8;
	static constexpr double RATE = 44100.0;
	static constexpr int OVER = 4;               // 1 サンプルを 4 つに割って作り、平均する（折り返しを減らす）

	// 音色の組を替える（nullptr なら初期の組）。声は鳴るたびに組を見るので、鳴っている音にもすぐ効く
	void set_bank(std::shared_ptr<const fc_bank> bank) { m_bank = std::move(bank); }
	const fc_bank *bank() const { return m_bank.get(); }
	// そのプログラムの音色（組が無ければ初期の音色）
	const fc_voice &voice_of(int program) const { return (m_bank ? *m_bank : initial()).prog[size_t(program & 127)]; }
	const char *name(int program) const { return voice_of(program).name; }
	// 音色の値を動かすコントロールチェンジの番号を付け替える。もう届いている値は、欄ごとに持っているのでそのまま
	void set_cc_map(const fc_cc_map &map) { m_map = map; }
	// いま選んでいるプログラムの、いま鳴る音色（組の音色に、コントロールチェンジで触った値を重ねたもの）
	fc_voice current() const
	{
		fc_voice v = voice_of(m_program);
		for (int i = 0; i < FC_PARAMS; i++)
			if (m_cc_mask & (1u << i))
				fc_set_param(v, i, m_cc[i]);
		return v;
	}
	// コントロールチェンジで触ってある値（bit = fc_param）。0 なら組の音色のまま
	u32 edited() const { return m_cc_mask; }
	// 触ってある値を捨てる（mask の bit の欄だけ。画面のつまみで同じ欄を動かしたとき）
	void clear_edits(u32 mask = ~u32(0)) { m_cc_mask &= ~mask; }
	// MIDI を通らずにプログラムが変わった（パネルで変えたのを本体が知らせてきた）。同じ番号なら何もしない
	// （MIDI のプログラムチェンジを本体が後から知らせ直してくるので、その間に届いた値を捨てないように）
	void set_program(u8 program)
	{
		if ((program & 127) != m_program)
			midi(0xc0, program, 0);
	}

	void reset()
	{
		for (voice &v : m_v)
			v = voice();
		m_program = 0;
		m_cc_mask = 0;
		m_bend = 0.0;
		m_mod = 0.0;
		m_frame = 0.0;
		m_lfo = 0.0;
		m_age = 0;
	}

	// チャンネルメッセージ 1 つ（status は上 4 ビットだけ見る）
	void midi(u8 status, u8 d0, u8 d1)
	{
		switch (status & 0xf0) {
		case 0x90:
			if (d1) {
				note_on(d0, d1);
				break;
			}
			[[fallthrough]];
		case 0x80:
			for (voice &v : m_v)
				if (v.on && v.key == d0 && !v.released) {
					v.released = true;
					v.frames = 0;
				}
			break;
		case 0xb0:
			if (d0 == 1)
				m_mod = d1 / 127.0;
			else if (d0 == 120)
				for (voice &v : m_v)
					v.on = false;
			else if (d0 == 123) {
				for (voice &v : m_v)
					if (v.on && !v.released) {
						v.released = true;
						v.frames = 0;
					}
			} else if (const int param = m_map.param_of(d0); param >= 0) {
				m_cc[param] = d1 & 127;
				m_cc_mask |= 1u << param;
			}
			break;
		case 0xc0:
			// 触った値は捨てる（曲の頭のプログラムチェンジで、いつも組の音色から始まる）
			m_program = d0 & 127;
			m_cc_mask = 0;
			break;
		case 0xe0:
			m_bend = (((d1 << 7) | d0) - 8192) / 8192.0 * 2.0;      // 半音
			break;
		default:
			break;
		}
	}

	u8 program() const { return m_program; }

	// 鳴っている声があるか（無ければ本体は入口を空にできる）
	bool sounding() const
	{
		for (const voice &v : m_v)
			if (v.on)
				return true;
		return false;
	}

	// 1 サンプル（44.1kHz）。±1.0 が全振幅で、1 声の最大はおよそ 0.14
	float render()
	{
		// 60 分の 1 秒ごとに、音量の段・デューティ・アルペジオを進める
		m_frame += 60.0 / RATE;
		const bool tick = m_frame >= 1.0;
		if (tick)
			m_frame -= 1.0;
		m_lfo += 6.0 / RATE;
		if (m_lfo >= 1.0)
			m_lfo -= 1.0;
		const double wobble = std::sin(m_lfo * 6.283185307179586);
		// コントロールチェンジで触ってあれば、いまのプログラムで鳴っている声はその音色で（ほかのプログラムの声は組のまま）
		fc_voice touched;
		if (m_cc_mask)
			touched = current();
		double sum = 0.0;
		for (voice &v : m_v) {
			if (!v.on)
				continue;
			const fc_voice &fv = m_cc_mask && v.program == m_program ? touched : voice_of(v.program);
			if (tick)
				step_frame(v, fv);
			if (!v.on)
				continue;
			// ホイールのビブラート（±0.5 半音まで）に、音色が持つ自動のビブラートを足す
			const double vib = (m_mod * 0.5 + (fv.vib_depth && v.life >= fv.vib_delay ? fv.vib_depth / 100.0 : 0.0)) * wobble;
			double semis = double(v.key) + m_bend + vib;
			if (fv.arp_len > 1)
				semis += fv.arp[(v.frames / fv.arp_frames) % fv.arp_len];
			if (fv.sweep_frames && v.life < fv.sweep_frames)
				semis += fv.sweep * double(fv.sweep_frames - v.life) / fv.sweep_frames;
			const double freq = 440.0 * std::pow(2.0, (semis - 69.0) / 12.0);
			double out = 0.0;
			if (fv.wave == fc_voice::NOISE || fv.wave == fc_voice::METAL) {
				// ノイズ: 15 ビットの帰還シフトレジスタを、鍵の高さに合わせた速さで回す
				const double step = std::min(freq * 16.0 / (RATE * OVER), 1.0);
				for (int k = 0; k < OVER; k++) {
					v.phase += step;
					if (v.phase >= 1.0) {
						v.phase -= 1.0;
						const u32 tap = fv.wave == fc_voice::METAL ? 6 : 1;
						const u32 fb = (v.lfsr ^ (v.lfsr >> tap)) & 1;
						v.lfsr = (v.lfsr >> 1) | (fb << 14);
					}
					out += (v.lfsr & 1) ? 1.0 : -1.0;
				}
			} else {
				const double step = freq / (RATE * OVER);
				const double duty = FC_DUTY[fv.duty[fv.duty_len > 1 ? (v.frames / fv.duty_frames) % fv.duty_len : 0] & 3];
				for (int k = 0; k < OVER; k++) {
					v.phase += step;
					if (v.phase >= 1.0)
						v.phase -= 1.0;
					if (fv.wave == fc_voice::TRIANGLE) {
						// 三角波: 16 段の階段（0-15-0）
						const int s = int(v.phase * 32.0) & 31;
						const int lvl = s < 16 ? s : 31 - s;
						out += lvl / 7.5 - 1.0;
					} else {
						out += v.phase < duty ? 1.0 : -1.0;
					}
				}
			}
			out /= OVER;
			// 三角波は音量を持たない（実物と同じ）。ほかは 16 段
			const double level = fv.wave == fc_voice::TRIANGLE ? (v.volume > 0 ? 1.0 : 0.0) : v.volume / 15.0;
			sum += out * level * 0.14;
		}
		return float(sum);
	}

private:
	struct voice {
		bool on = false, released = false;
		u8 key = 60, program = 0;
		int volume = 0;         // 0-15
		int frames = 0;         // 60 分の 1 秒の数（押してから、または離してから）
		int life = 0;           // 60 分の 1 秒の数（押してから。離しても戻さない）
		double phase = 0.0;
		u32 lfsr = 1;
		u32 age = 0;
	};

	void note_on(u8 key, u8 vel)
	{
		// 空いている声、無ければ離している声、それも無ければいちばん古い声
		voice *use = nullptr;
		for (voice &v : m_v)
			if (!v.on) {
				use = &v;
				break;
			}
		if (!use)
			for (voice &v : m_v)
				if (v.released && (!use || v.age < use->age))
					use = &v;
		if (!use)
			for (voice &v : m_v)
				if (!use || v.age < use->age)
					use = &v;
		*use = voice();
		use->on = true;
		use->key = key;
		use->program = m_program;
		use->volume = 1 + (vel * 14 + 63) / 127;       // 1-15
		use->age = ++m_age;
	}

	// 60 分の 1 秒ぶん。離した声は 1 コマに release 段ずつ下がって消える（初期の音色は 2 段。0.13 秒以内）。
	// decay があれば、押している間も decay コマごとに 1 段（floor の段で止まる。0 なら消える）
	void step_frame(voice &v, const fc_voice &fv)
	{
		v.frames++;
		v.life++;
		if (v.released) {
			v.volume -= std::max<int>(fv.release, 1);
			if (v.volume <= 0)
				v.on = false;
		} else if (fv.decay && v.frames % fv.decay == 0) {
			if (v.volume > fv.floor && --v.volume == 0)
				v.on = false;
		}
	}

	static const fc_bank &initial()
	{
		static const fc_bank b;
		return b;
	}

	std::shared_ptr<const fc_bank> m_bank;
	std::array<voice, VOICES> m_v{};
	u8 m_program = 0;
	fc_cc_map m_map;                // 欄ごとのコントロールチェンジの番号
	u8 m_cc[FC_PARAMS] = {};        // コントロールチェンジで届いた値（m_cc_mask の bit が立っている欄だけ）
	u32 m_cc_mask = 0;
	double m_bend = 0.0, m_mod = 0.0, m_frame = 0.0, m_lfo = 0.0;
	u32 m_age = 0;
};

// パートの設定（MIDI の 0-127）を、ボードの音に掛ける倍率にする。本体の内蔵の音色と同じ「2 乗」の曲線（40log）
inline float level_of(int value)
{
	const float v = float(value < 0 ? 0 : value > 127 ? 127 : value) / 127.0f;
	return v * v;
}

// パン（1-127、64 が真ん中。0 はランダムなので真ん中として扱う）を左右の倍率に。真ん中で両方 1.0
inline void pan_of(int pan, float &left, float &right)
{
	const float p = pan <= 0 ? 0.5f : float(pan - 1) / 126.0f;       // 0 = 左、1 = 右
	left = std::min(1.0f, 2.0f * (1.0f - p));
	right = std::min(1.0f, 2.0f * p);
}

} // namespace smu2000::vboard

#endif // S_MU2000_VBOARD_H
