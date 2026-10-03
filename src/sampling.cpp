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
u32 play_rec(int n)   { return sp::TAB_PLAY + 16 * u32(n - 1) - DRAM; }
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

// 鳴り始め・ループの頭・鳴り終わりを、長さ frames のサンプルで書ける形にそろえる。
// 鳴り終わりは鳴り始めより 8 以上後、ループの頭は偶数で鳴り始めと鳴り終わりの 8 手前のあいだ
void fit_points(u32 frames, u32 &from, u32 &to, u32 &loop_from)
{
	if (!to || to > frames)
		to = frames;
	to = std::max(to, std::min(frames, 8u));
	from = std::min(from, to >= 8 ? to - 8 : 0);
	loop_from = std::clamp(loop_from & ~1u, (from + 1) & ~1u, to >= 8 ? (to - 8) & ~1u : 0);
	from = std::min(from, loop_from);
}

// 鳴らす表の 1 項目を書く。start・end は語、ほかはサンプル（頭から）。
// ループの頭の語を +12 に、鳴り始めはそこから +4 の下だけ手前、+8 はループの頭から鳴り終わりまで − 4
void put_play(std::vector<u8> &m, u32 p, u32 start, u32 end, bool loop, u32 from, u32 to, u32 loop_from)
{
	fit_points((end - start) * 2, from, to, loop_from);
	static const u8 HEAD[4] = { 0x00, 0x3c, 0x00, 0xff };
	std::memcpy(&m[p], HEAD, 4);
	wr32(m, p + 4, (loop ? 0u : 0x40000000u) | (loop_from - from));
	wr32(m, p + 8, to - loop_from - 4);
	wr32(m, p + 12, (start + loop_from / 2) | WORD_FLAG);
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
		const u32 p = play_rec(n);
		s.loop = !(m_dram[p + 4] & 0x40);
		const u32 head = rd32(m_dram, p + 12) & 0xffffff, back = rd32(m_dram, p + 4) & 0xffffff;
		const u32 len = rd32(m_dram, p + 8);
		s.loop_from = head > s.start && head < s.end ? (head - s.start) * 2 : 0;
		s.play_from = back <= s.loop_from ? s.loop_from - back : 0;
		s.play_to = len <= s.frames() ? s.loop_from + len + 4 : s.frames();
		if (s.play_to > s.frames())
			s.play_to = s.frames();
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

bool mu2000::sampling_trim(int number, u32 from, u32 to, std::string &err)
{
	const std::vector<sp::sample> list = sampling_list();
	const sp::sample *s = nullptr;
	for (const sp::sample &x : list)
		if (x.number == number)
			s = &x;
	if (!s) {
		err = "no such sample";
		return false;
	}
	to = std::min(to, s->frames());
	if (from >= to || to - from < 8) {
		err = "the trimmed sample would be too short";
		return false;
	}
	const u32 start = s->start, old_end = s->end;
	const u32 frames = to - from;
	const u32 new_end = start + (frames + 1) / 2;
	// 残す所を頭へ（前へ写すので重なっても前から順に写せばよい）。奇数なら最後の半語は 0
	u8 *ram = m_sampram.data();
	std::memmove(ram + size_t(start) * 4, ram + (size_t(start) * 2 + from) * 2, size_t(frames) * 2);
	if (frames & 1) {
		ram[(size_t(start) * 2 + frames) * 2] = 0;
		ram[(size_t(start) * 2 + frames) * 2 + 1] = 0;
	}
	// 鳴り始め・ループの頭・鳴り終わりは残した所の中での位置へ（切り落とした所にあれば端へ）
	auto set_range = [&](const sp::sample &x, u32 st, u32 en, u32 shift, u32 limit) {
		const u32 o = sample_rec(x.number);
		auto in = [&](u32 v) { return std::min(v > shift ? v - shift : 0, limit); };
		put_play(m_dram, play_rec(x.number), st, en, x.loop, in(x.play_from), in(x.play_to), in(x.loop_from));
		wr32(m_dram, o + 16, st | WORD_FLAG);
		wr32(m_dram, o + 20, en | WORD_FLAG);
	};
	set_range(*s, start, new_end, from, frames);
	// 後ろにあるものを前へ詰める（番地の若い順に）
	const u32 gap = old_end - new_end;
	if (gap) {
		std::vector<sp::sample> later;
		for (const sp::sample &x : list)
			if (x.number != number && x.start >= old_end)
				later.push_back(x);
		std::sort(later.begin(), later.end(), [](const sp::sample &a, const sp::sample &b) { return a.start < b.start; });
		for (const sp::sample &x : later) {
			std::memmove(ram + size_t(x.start - gap) * 4, ram + size_t(x.start) * 4, size_t(x.end - x.start) * 4);
			set_range(x, x.start - gap, x.end - gap, 0, x.frames());
		}
		const u32 next = rd32(m_dram, sp::NEXT_FREE - DRAM) & 0xffffff;
		if (next >= gap) {
			std::memset(ram + size_t(next - gap) * 4, 0, size_t(gap) * 4);
			wr32(m_dram, sp::NEXT_FREE - DRAM, (next - gap) | WORD_FLAG);
		}
	}
	return true;
}

bool mu2000::sampling_loop(int number, bool on, u32 loop_from)
{
	for (const sp::sample &s : sampling_list())
		if (s.number == number)
			return sampling_points(number, s.play_from, s.play_to, on, loop_from);
	return false;
}

bool mu2000::sampling_points(int number, u32 from, u32 to, bool on, u32 loop_from)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		put_play(m_dram, play_rec(number), s.start, s.end, on, from, to, loop_from);
		// EDIT → SAMPLE の Loop と同じく記録の印にも 0x02（パネルの表示が合う）
		const u32 o = sample_rec(number);
		m_dram[o + 2] = u8(on ? m_dram[o + 2] | 0x02 : m_dram[o + 2] & ~0x02);
		return true;
	}
	return false;
}

namespace {

// サンプリング RAM のサンプル s の頭から i 番目
s16 pcm_at(const std::vector<u8> &ram, const sp::sample &s, u32 i)
{
	const size_t o = (size_t(s.start) * 2 + i) * 2;
	return o + 1 < ram.size() ? s16(ram[o] | ram[o + 1] << 8) : 0;
}

void pcm_put(std::vector<u8> &ram, const sp::sample &s, u32 i, long v)
{
	const size_t o = (size_t(s.start) * 2 + i) * 2;
	if (o + 1 >= ram.size())
		return;
	const s16 w = s16(std::clamp(v, -32768L, 32767L));
	ram[o] = u8(w);
	ram[o + 1] = u8(u16(w) >> 8);
}

} // namespace

bool mu2000::sampling_snap(int number, u32 at, bool even, u32 range, u32 &out) const
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		const u32 n = s.frames();
		// i の直前が負で i が 0 以上（下から上へ横切る）所のうち、at にいちばん近いもの
		auto rising = [&](u32 i) { return i > 0 && i < n && pcm_at(m_sampram, s, i - 1) < 0 && pcm_at(m_sampram, s, i) >= 0; };
		for (u32 d = 0; d <= range; d++) {
			for (int sgn : { -1, 1 }) {
				if (d == 0 && sgn > 0)
					continue;
				const long i = long(at) + sgn * long(d);
				if (i <= 0 || i >= long(n) || (even && (i & 1)))
					continue;
				// 偶数に限るときは、その 1 つ前で横切っていてもよい（ループの頭は語の境にしか置けない）
				if (rising(u32(i)) || (even && rising(u32(i) + 1))) {
					out = u32(i);
					return true;
				}
			}
		}
		return false;
	}
	return false;
}

bool mu2000::sampling_match_end(int number, u32 loop_from, u32 near, u32 range, u32 &out) const
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		const u32 n = s.frames();
		// E のまわり [E - W, E + W) が L のまわり [L - W, L + W) と同じ形なら、E から L へ戻ってもつながる。
		// 差の 2 乗の和を両方の大きさで割ったものがいちばん小さい E（E の後ろが無ければ前の半分だけで比べる）
		const long W = 256;
		const long lo = std::max<long>(long(loop_from) + 64, long(near) - long(range));
		const long hi = std::min<long>(long(n), long(near) + long(range));
		double best = 1e30;
		long best_e = -1;
		for (long e = lo; e <= hi; e++) {
			double diff = 0, energy = 0;
			for (long k = -W; k < W; k++) {
				const long a = long(loop_from) + k, b = e + k;
				if (a < 0 || b < 0 || b >= long(n) || a >= e)
					continue;
				const double x = pcm_at(m_sampram, s, u32(a)), y = pcm_at(m_sampram, s, u32(b));
				diff += (x - y) * (x - y);
				energy += x * x + y * y;
			}
			if (energy <= 0)
				continue;
			const double score = diff / energy;
			if (score < best) {
				best = score;
				best_e = e;
			}
		}
		if (best_e < 0)
			return false;
		out = u32(best_e);
		return true;
	}
	return false;
}

bool mu2000::sampling_pcm(int number, std::vector<s16> &out) const
{
	out.clear();
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		out.resize(s.frames());
		for (u32 i = 0; i < s.frames(); i++)
			out[i] = pcm_at(m_sampram, s, i);
		return true;
	}
	return false;
}

namespace smu2000::sampling {

bool find_loop(const std::vector<s16> &pcm, u32 from, u32 to, u32 min_len, u32 &loop_from, u32 &loop_to)
{
	// 候補は下から上へ 0 を横切る所。L と E のまわり ±W の形を比べ（差の 2 乗の和を両方の大きさで割る）、
	// いちばん似ている組を選ぶ。まず 4 つおきに粗く比べ、よかった組の E を 1 サンプルずつ詰める。
	// 小さすぎる所（区間の中でいちばん大きい所の 1/20 未満の大きさ）は候補にしない（無音どうしは似て見える）
	const long W = 256;
	to = std::min<u32>(to, u32(pcm.size()));
	if (to <= from || to - from < min_len + 2 * W)
		return false;
	auto at = [&](long i) { return i >= 0 && i < long(pcm.size()) ? double(pcm[size_t(i)]) : 0.0; };
	auto energy = [&](long c) {
		double e = 0;
		for (long k = -W; k < W; k += 4)
			e += at(c + k) * at(c + k);
		return e;
	};
	std::vector<u32> cand;
	for (u32 i = std::max<u32>(from, W) + 1; i + W < to; i++)
		if (pcm[i - 1] < 0 && pcm[i] >= 0)
			cand.push_back(i);
	if (cand.size() < 2)
		return false;
	// 多すぎれば等間隔に間引く（組の数が候補の 2 乗になる）
	const size_t MAX = 320;
	if (cand.size() > MAX) {
		std::vector<u32> thin;
		for (size_t j = 0; j < MAX; j++)
			thin.push_back(cand[j * cand.size() / MAX]);
		cand.swap(thin);
	}
	std::vector<double> en(cand.size());
	double emax = 0;
	for (size_t j = 0; j < cand.size(); j++)
		emax = std::max(emax, en[j] = energy(cand[j]));
	auto score = [&](long l, long e, long step) {
		double diff = 0, sum = 0;
		for (long k = -W; k < W; k += step) {
			const double x = at(l + k), y = at(e + k);
			diff += (x - y) * (x - y);
			sum += x * x + y * y;
		}
		return sum > 0 ? diff / sum : 1e30;
	};
	double best = 1e30;
	long bl = -1, be = -1;
	for (size_t a = 0; a < cand.size(); a++) {
		if (en[a] < emax * 0.05)
			continue;
		for (size_t b = a + 1; b < cand.size(); b++) {
			if (cand[b] - cand[a] < min_len || en[b] < emax * 0.05)
				continue;
			const double s = score(cand[a], cand[b], 4);
			if (s < best) {
				best = s;
				bl = cand[a];
				be = cand[b];
			}
		}
	}
	if (bl < 0)
		return false;
	// ループの頭は偶数（語の境）。E をそれに合わせて 1 サンプルずつ詰める
	bl &= ~1L;
	double fine = 1e30;
	long fe = be;
	for (long e = std::max<long>(bl + long(min_len), be - 24); e <= std::min<long>(long(to), be + 24); e++)
		if (const double s = score(bl, e, 1); s < fine) {
			fine = s;
			fe = e;
		}
	loop_from = u32(bl);
	loop_to = u32(fe);
	return true;
}

} // namespace smu2000::sampling

bool mu2000::sampling_crossfade(int number, u32 loop_from, u32 to, u32 len, bool power)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		const u32 n = s.frames();
		to = std::min(to ? to : n, n);
		// E の手前 len を、L の手前 len と少しずつ混ぜる。終わりで L の手前とそろうので、E から L へなめらかにつながる
		len = std::min({ len, loop_from, to > loop_from ? to - loop_from : 0u });
		if (len < 2)
			return false;
		const double PI = 3.14159265358979323846;
		// 似た波形どうしは足して 1 の曲線（音量が揃う）。揺れのある音は 2 乗して足して 1 の曲線（途中で痩せない）
		for (u32 k = 0; k < len; k++) {
			const double t = double(k + 1) / double(len);
			const double wa = power ? std::cos(PI * 0.5 * t) : 0.5 + 0.5 * std::cos(PI * t);
			const double wb = power ? std::sin(PI * 0.5 * t) : 1.0 - wa;
			const double a = pcm_at(m_sampram, s, to - len + k), b = pcm_at(m_sampram, s, loop_from - len + k);
			pcm_put(m_sampram, s, to - len + k, std::lround(a * wa + b * wb));
		}
		// 鳴らすときに E の少し先まで読むことがあるので、E の後ろ（鳴らさない所）を L の後ろと同じに
		for (u32 k = 0; k < 4 && to + k < n; k++)
			pcm_put(m_sampram, s, to + k, pcm_at(m_sampram, s, loop_from + k));
		return true;
	}
	return false;
}

bool mu2000::sampling_bounds(int number, double ratio, u32 &from, u32 &to) const
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		auto at = [&](u32 i) {
			const u32 o = (s.start * 2 + i) * 2;
			const s16 v = s16(m_sampram[o] | m_sampram[o + 1] << 8);
			return v < 0 ? -int(v) : int(v);
		};
		const u32 n = s.frames();
		if ((s.start * 2 + n) * 2 > m_sampram.size())
			return false;
		int peak = 0;
		for (u32 i = 0; i < n; i++)
			peak = std::max(peak, at(i));
		if (!peak)
			return false;
		const int thr = std::max(1, int(std::lround(peak * ratio)));
		u32 a = 0, b = n;
		while (a < n && at(a) < thr)
			a++;
		while (b > a && at(b - 1) < thr)
			b--;
		if (b - a < 8)
			b = std::min(n, a + 8);
		from = a;
		to = b;
		return true;
	}
	return false;
}

bool mu2000::sampling_overview(int number, int buckets, std::vector<s16> &lo, std::vector<s16> &hi, u32 &frames,
                               u32 from, u32 to) const
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
		if (to <= from || to > frames) {
			if (to <= from)
				from = 0;
			to = frames;
		}
		const u32 span = to - from;
		if (u32(buckets) > span)
			buckets = int(span);
		lo.assign(size_t(buckets), 32767);
		hi.assign(size_t(buckets), -32768);
		for (u32 i = from; i < to; i++) {
			const u32 at = (s.start * 2 + i) * 2;
			if (at + 1 >= m_sampram.size())
				break;
			const s16 v = s16(m_sampram[at] | m_sampram[at + 1] << 8);
			const size_t b = size_t(u64(i - from) * u64(buckets) / span);
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
	put_play(m_dram, play_rec(n), start, end, false, 0, 0, 0);
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
	out.coarse = int(m_dram[o + 0x1d]) - 0x40;
	out.fine = int(m_dram[o + 0x1e]) - 0x40;
	out.attack = m_dram[o + 0x55] & 0x3f;
	out.decay1 = m_dram[o + 0x56] & 0x3f;
	out.decay2 = m_dram[o + 0x57] & 0x3f;
	out.release = m_dram[o + 0x58] & 0x3f;
	out.level1 = m_dram[o + 0x59] & 0x7f;
	out.level2 = m_dram[o + 0x5a] & 0x7f;
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
	m_dram[o + 0x1d] = u8(0x40 + std::clamp(v.coarse, -24, 24));
	m_dram[o + 0x1e] = u8(0x40 + std::clamp(v.fine, -64, 63));
	m_dram[o + 0x55] = u8(std::clamp(v.attack, 0, 63));
	m_dram[o + 0x56] = u8(std::clamp(v.decay1, 0, 63));
	m_dram[o + 0x57] = u8(std::clamp(v.decay2, 0, 63));
	m_dram[o + 0x58] = u8(std::clamp(v.release, 0, 63));
	m_dram[o + 0x59] = u8(std::clamp(v.level1, 0, 127));
	m_dram[o + 0x5a] = u8(std::clamp(v.level2, 0, 127));
	return true;
}

bool mu2000::preview_start(int number, u32 from, u32 to, u32 loop_at)
{
	for (const sp::sample &s : sampling_list()) {
		if (s.number != number)
			continue;
		to = std::min(to ? to : s.frames(), s.frames());
		if (from >= to)
			return false;
		m_prev_number = number;
		m_prev_base = s.start * 2;
		m_prev_pos = from;
		m_prev_end = to;
		m_prev_loop = loop_at < to ? loop_at : ~0u;
		m_prev_on = true;
		return true;
	}
	return false;
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
