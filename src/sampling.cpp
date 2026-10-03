// license:BSD-3-Clause
//
// サンプリングの表をパネルを通さずに読み書きする（表の形は src/sampling.h、doc/sampling-ram.md）
#include "mu2000.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sp = smu2000::sampling;

namespace {

u32 rd32(const std::vector<u8> &m, u32 off)
{
	return u32(m[off]) << 24 | u32(m[off + 1]) << 16 | u32(m[off + 2]) << 8 | m[off + 3];
}

void wr32(std::vector<u8> &m, u32 off, u32 v)
{
	m[off] = u8(v >> 24);
	m[off + 1] = u8(v >> 16);
	m[off + 2] = u8(v >> 8);
	m[off + 3] = u8(v);
}

constexpr u32 DRAM = 0x1000000;
constexpr u32 WORD_FLAG = 0x01000000;   // 語の位置に付いている上の印

u32 sample_rec(int n) { return sp::TAB_SAMPLE + 36 * u32(n - 1) - DRAM; }
u32 play_rec(int n)   { return sp::TAB_PLAY + 16 * u32(n) - DRAM; }
u32 voice_rec(int slot) { return sp::TAB_VOICE + sp::VOICE_SIZE * u32(slot) - DRAM; }

// 表の名前（空白か 0 で終わる）を std::string に
std::string text(const std::vector<u8> &m, u32 off, int len)
{
	std::string s(reinterpret_cast<const char *>(&m[off]), size_t(len));
	const size_t z = s.find('\0');
	if (z != std::string::npos)
		s.resize(z);
	while (!s.empty() && s.back() == ' ')
		s.pop_back();
	return s;
}

// LCD に出せる文字だけにして len に詰める
void put_text(std::vector<u8> &m, u32 off, int len, const std::string &s, u8 fill)
{
	for (int i = 0; i < len; i++) {
		u8 c = i < int(s.size()) ? u8(s[size_t(i)]) : fill;
		if (i < int(s.size()) && (c < 0x20 || c > 0x7e))
			c = '_';
		m[off + u32(i)] = c;
	}
}

} // namespace

std::vector<sp::sample> mu2000::sampling_list() const
{
	std::vector<sp::sample> out;
	for (int n = 1; n <= sp::MAX_SAMPLES; n++) {
		const u32 o = sample_rec(n);
		if (!(m_dram[o + 2] & 0x40))
			continue;
		sp::sample s;
		s.number = n;
		s.rate = rd32(m_dram, o + 12);
		s.start = rd32(m_dram, o + 16) & 0xffffff;
		s.end = rd32(m_dram, o + 20) & 0xffffff;
		s.name = text(m_dram, o + 28, 8);
		out.push_back(s);
	}
	return out;
}

int mu2000::sampling_peak(const sp::sample &s) const
{
	int peak = 0;
	for (u32 i = s.start * 2; i < s.end * 2 && (i + 1) * 2 <= m_sampram.size(); i++) {
		const s16 v = s16(m_sampram[i * 2] | m_sampram[i * 2 + 1] << 8);
		peak = std::max(peak, v < 0 ? -int(v) : int(v));
	}
	return peak;
}

int mu2000::sampling_gain(int number, double gain)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		int peak = 0;
		for (u32 i = s.start * 2; i < s.end * 2 && (i + 1) * 2 <= m_sampram.size(); i++) {
			const s16 v = s16(m_sampram[i * 2] | m_sampram[i * 2 + 1] << 8);
			const long w = std::clamp(std::lround(double(v) * gain), -32768L, 32767L);
			m_sampram[i * 2] = u8(w);
			m_sampram[i * 2 + 1] = u8(u16(w) >> 8);
			peak = std::max(peak, int(w < 0 ? -w : w));
		}
		return peak;
	}
	return -1;
}

bool mu2000::sampling_overview(int number, int buckets, std::vector<s16> &lo, std::vector<s16> &hi, u32 &frames) const
{
	lo.clear();
	hi.clear();
	frames = 0;
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		frames = s.frames();
		if (!frames || buckets <= 0)
			return false;
		lo.assign(size_t(buckets), 0);
		hi.assign(size_t(buckets), 0);
		for (u32 i = 0; i < frames; i++) {
			const u32 at = (s.start * 2 + i) * 2;
			if (at + 1 >= m_sampram.size())
				break;
			const s16 v = s16(m_sampram[at] | m_sampram[at + 1] << 8);
			const size_t b = size_t(u64(i) * u64(buckets) / frames);
			lo[b] = std::min(lo[b], v);
			hi[b] = std::max(hi[b], v);
		}
		return true;
	}
	return false;
}

u32 mu2000::sampling_free_frames() const
{
	const u32 next = rd32(m_dram, sp::NEXT_FREE - DRAM) & 0xffffff;
	return next >= sp::RAM_WORDS ? 0 : (sp::RAM_WORDS - next) * 2;
}

int mu2000::sampling_add(const s16 *pcm, size_t frames, const std::string &name, std::string &err)
{
	if (frames < 8) {
		err = "the sample is too short";
		return 0;
	}
	if (frames > sampling_free_frames()) {
		err = "not enough sampling memory left";
		return 0;
	}
	// firmware は番号の若い空きから使う（REC の Sp= に出る番号）
	int n = 0;
	for (int i = 1; i <= sp::MAX_SAMPLES && !n; i++)
		if (!(m_dram[sample_rec(i) + 2] & 0x40))
			n = i;
	if (!n) {
		err = "all 512 sample slots are in use";
		return 0;
	}
	const u32 start = rd32(m_dram, sp::NEXT_FREE - DRAM) & 0xffffff;
	const u32 words = u32((frames + 1) / 2);
	const u32 end = start + words;
	// 波形。1 語に 2 つ、下の 16bit が先（swp30.cpp の sample_step と同じ）
	for (size_t i = 0; i < size_t(words) * 2; i++) {
		const u16 v = u16(i < frames ? pcm[i] : 0);
		const u32 at = (start * 2 + u32(i)) * 2;
		m_sampram[at] = u8(v);
		m_sampram[at + 1] = u8(v >> 8);
	}
	// 鳴らすための表
	const u32 p = play_rec(n);
	wr32(m_dram, p, (end - start) * 2 - 4);
	wr32(m_dram, p + 4, start | WORD_FLAG);
	static const u8 PLAY_TAIL[8] = { 0x00, 0x3c, 0x00, 0xff, 0x40, 0x00, 0x00, 0x00 };
	std::memcpy(&m_dram[p + 8], PLAY_TAIL, 8);
	// サンプルの記録
	const u32 o = sample_rec(n);
	m_dram[o] = u8((n - 1) >> 8);
	m_dram[o + 1] = u8(n - 1);
	m_dram[o + 2] = 0x40;
	m_dram[o + 3] = 0;
	wr32(m_dram, o + 4, 0);
	wr32(m_dram, o + 8, 0xffffffff);
	wr32(m_dram, o + 12, sp::SAMPLE_RATE);
	wr32(m_dram, o + 16, start | WORD_FLAG);
	wr32(m_dram, o + 20, end | WORD_FLAG);
	wr32(m_dram, o + 24, 0x04000000);
	char def[16];
	std::snprintf(def, sizeof(def), "take%03d", n);
	put_text(m_dram, o + 28, 8, name.empty() ? std::string(def) : name, ' ');
	wr32(m_dram, sp::NEXT_FREE - DRAM, end | WORD_FLAG);
	return n;
}

bool mu2000::sampling_voice(int slot, sp::voice &out) const
{
	if (slot < 0 || slot >= sp::MAX_VOICES)
		return false;
	const u32 o = voice_rec(slot);
	out.name = text(m_dram, o + 2, 8);
	out.assigned = m_dram[o + 12] == 0x01;
	const u16 sv = u16(m_dram[o + 14] << 8 | m_dram[o + 15]);
	out.sample = out.assigned ? int(sv & 0x1ff) + 1 : 0;
	out.level = m_dram[o + 0x47];
	out.pan = m_dram[o + 0x51];
	return true;
}

bool mu2000::sampling_set_voice(int slot, const sp::voice &v, std::string &err)
{
	if (slot < 0 || slot >= sp::MAX_VOICES) {
		err = "no such voice";
		return false;
	}
	if (v.assigned && (v.sample < 1 || v.sample > sp::MAX_SAMPLES || !(m_dram[sample_rec(v.sample) + 2] & 0x40))) {
		err = "no such sample";
		return false;
	}
	const u32 o = voice_rec(slot);
	// 名前は 8 文字で、余りは空白（0 で埋めると LCD が CGRAM の 0 番の字を出す）。+10・+11 は別の欄
	if (!v.name.empty())
		put_text(m_dram, o + 2, 8, v.name, ' ');
	if (v.assigned) {
		m_dram[o + 12] = 0x01;
		m_dram[o + 13] = 0x7f;
		const u16 sv = u16(0x4000 | (v.sample - 1));
		m_dram[o + 14] = u8(sv >> 8);
		m_dram[o + 15] = u8(sv);
	} else {
		m_dram[o + 12] = 0x00;
		m_dram[o + 13] = 0x7f;
		m_dram[o + 14] = 0x3f;
		m_dram[o + 15] = 0x7f;
	}
	m_dram[o + 0x47] = u8(std::clamp(v.level, 0, 127));
	m_dram[o + 0x51] = u8(std::clamp(v.pan, 0, 15));
	return true;
}

void mu2000::rec_start(sp::source src, int trigger, u32 max_frames)
{
	m_rec_src = src;
	m_rec_trigger = std::clamp(trigger, 0, 32767);
	m_rec_max = std::min(max_frames, sampling_free_frames());
	m_rec_buf.clear();
	m_rec_buf.reserve(m_rec_max);
	m_rec_state = m_rec_trigger > 0 ? 1 : 2;
}

std::vector<s16> mu2000::rec_take()
{
	m_rec_state = 0;
	std::vector<s16> out;
	out.swap(m_rec_buf);
	return out;
}
