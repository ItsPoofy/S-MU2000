// license:BSD-3-Clause
//
// 移植が成立しているかの最小確認
#include "mame/sound/swp30.h"
#include "compat/a64asm.h"
#include "ui/bend_thinner.h"
#include "smf.h"
#include "ui/font_check.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// 再生のピッチベンドの間引き（ui/bend_thinner.h）。1ms おきに 1000 個のベンドの途中に音符を挟み、
// 送った数、音符の直前に送ったベンドがその時点の最新の値か、最後の値が届くかを見る
static void check_bend_thinner()
{
	ui::bend_thinner th;
	struct sent { int to; u8 b[3]; };
	std::vector<sent> out;
	auto send = [&out](int to, const u8 *d, size_t n) {
		sent s{ to, { 0, 0, 0 } };
		for (size_t i = 0; i < n && i < 3; i++)
			s.b[i] = d[i];
		out.push_back(s);
	};
	int bends_sent = 0;
	bool note_ok = false;
	u8 latest = 0;
	for (int i = 0; i < 1000; i++) {
		const double t = i * 0.001;
		const u8 msb = u8(i & 0x7f);
		const u8 bend[3] = { 0xe0, 0x00, msb };
		th.event(0, bend, 3, t, send);
		latest = msb;
		th.tick(t, false, send);
		if (i == 500) {
			const u8 on[3] = { 0x90, 60, 100 };
			th.event(0, on, 3, t + 0.0001, send);
			// 音符の直前に出たものが、今の最新のベンド
			note_ok = out.size() >= 2 && out[out.size() - 2].b[0] == 0xe0 && out[out.size() - 2].b[2] == latest &&
			          out.back().b[0] == 0x90;
		}
	}
	th.tick(1.0, true, send);
	for (const sent &s : out)
		if (s.b[0] == 0xe0)
			bends_sent++;
	const bool last_ok = !out.empty() && out.back().b[0] == 0xe0 && out.back().b[2] == latest;
	std::printf("ピッチベンドの間引き: 1000 個 → %d 個、音符の前の高さ %s、最後の値 %s\n",
	            bends_sent, note_ok ? "合" : "NG", last_ok ? "合" : "NG");
}

// MIDI プレイヤーの土台（smf.h、イシュー #124）。小さな SMF を作って、曲名・小節と拍・頭出しの追いかけを見る。
// 3/4 拍子・テンポ 120 で始まり、2 小節目の頭（1440 tick = 1.5 秒）でテンポ 60 になる曲
static void check_smf_player()
{
	auto chunk = [](std::vector<u8> &f, const char *id, const std::vector<u8> &d) {
		f.insert(f.end(), id, id + 4);
		const u32 n = u32(d.size());
		const u8 len[4] = { u8(n >> 24), u8(n >> 16), u8(n >> 8), u8(n) };
		f.insert(f.end(), len, len + 4);
		f.insert(f.end(), d.begin(), d.end());
	};
	auto put = [](std::vector<u8> &t, u32 delta, std::initializer_list<u8> b) {
		u8 v[4];
		int n = 0;
		do { v[n++] = u8(delta & 0x7f); delta >>= 7; } while (delta);
		while (n--) t.push_back(u8(v[n] | (n ? 0x80 : 0)));
		t.insert(t.end(), b);
	};
	std::vector<u8> f, t0, t1;
	chunk(f, "MThd", { 0, 1, 0, 2, 0x01, 0xe0 });                      // format 1、2 トラック、480 tick/拍
	put(t0, 0, { 0xff, 0x03, 9, 'T', 'e', 's', 't', ' ', 'S', 'o', 'n', 'g' });
	put(t0, 0, { 0xff, 0x58, 4, 3, 2, 24, 8 });                         // 3/4
	put(t0, 0, { 0xff, 0x51, 3, 0x07, 0xa1, 0x20 });                    // 120
	put(t0, 1440, { 0xff, 0x51, 3, 0x0f, 0x42, 0x40 });                 // 60
	put(t0, 0, { 0xff, 0x2f, 0 });
	put(t1, 0, { 0xf0, 8, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 });   // XG オン
	put(t1, 0, { 0xb0, 0, 0 });
	put(t1, 0, { 0xb0, 32, 3 });
	put(t1, 0, { 0xc0, 5 });
	put(t1, 0, { 0xb0, 7, 100 });
	put(t1, 0, { 0x90, 60, 100 });
	put(t1, 480, { 0x80, 60, 0 });
	put(t1, 0, { 0xb0, 7, 80 });
	put(t1, 0, { 0xe0, 0, 0x50 });
	put(t1, 960, { 0xc0, 10 });
	put(t1, 1440, { 0x90, 64, 100 });
	put(t1, 480, { 0x80, 64, 0 });
	put(t1, 0, { 0xff, 0x2f, 0 });
	chunk(f, "MTrk", t0);
	chunk(f, "MTrk", t1);

	std::vector<smf::event> ev;
	std::string err;
	smf::song_meta meta;
	if (!smf::load_from_memory(f.data(), f.size(), ev, err, &meta)) {
		std::printf("プレイヤーの土台: SMF が読めない（%s）\n", err.c_str());
		return;
	}
	std::printf("曲名: 「%s」\n", meta.title.c_str());
	const double at[] = { 0.1, 0.6, 1.6, 3.0, 4.6 };
	for (double sec : at) {
		int bar = 0, beat = 0;
		double bpm = 0;
		meta.bar_beat(sec, bar, beat, bpm);
		std::printf("  %.1f 秒: 小節 %d・拍 %d・テンポ %.0f\n", sec, bar, beat, bpm);
	}
	// 2.0 秒へ頭出し。音符は送らない、SysEx と音色は順に全部、ボリュームとベンドは最後の値だけ
	const std::vector<smf::event> c = smf::chase(ev, 2.0);
	int notes = 0, sysex = 0, pcs = 0, vols = 0, bends = 0, last_pc = -1, vol = -1, bank_at = -1, pc_at = -1;
	for (size_t i = 0; i < c.size(); i++) {
		const std::vector<u8> &b = c[i].bytes;
		if (b.empty())
			continue;
		const u8 st = u8(b[0] & 0xf0);
		if (b[0] == 0xf0)
			sysex++;
		else if (st == 0x90 || st == 0x80)
			notes++;
		else if (st == 0xc0) {
			pcs++;
			last_pc = b[1];
			if (pc_at < 0)
				pc_at = int(i);
		} else if (st == 0xb0 && b[1] == 7) {
			vols++;
			vol = b[2];
		} else if (st == 0xb0 && b[1] == 0 && bank_at < 0)
			bank_at = int(i);
		else if (st == 0xe0)
			bends++;
	}
	std::printf("頭出しの追いかけ（2.0 秒へ）: 音符 %d・SysEx %d・音色 %d（最後 %d）・ボリューム %d 個（値 %d）・ベンド %d・バンクが音色より先 %s\n",
	            notes, sysex, pcs, last_pc, vols, vol, bends, bank_at >= 0 && bank_at < pc_at ? "合" : "NG");
	std::printf("頭（0 秒）へ: %zu 個\n", smf::chase(ev, 0.0).size());
}

// 文字ファイルを読めるかの確かめ（ui/font_check.h、issue #135）。表の並びだけの小さな書体を作って試す:
// glyf の書体・CFF の書体は通り、CFF2 だけの書体（可変フォント）・cmap に Unicode の表が無い書体・
// 束（TTC）の無い番号・ごみは通らない
static void check_font_parses()
{
	auto be16 = [](std::vector<u8> &v, u32 x) { v.push_back(u8(x >> 8)); v.push_back(u8(x)); };
	auto be32 = [](std::vector<u8> &v, u32 x) { v.push_back(u8(x >> 24)); v.push_back(u8(x >> 16)); v.push_back(u8(x >> 8)); v.push_back(u8(x)); };
	// 表の名前の並びと、cmap の（platform, encoding）から書体を作る
	auto make = [&](const char *magic, std::initializer_list<const char *> names, u32 platform, u32 encoding) {
		std::vector<u8> f(magic, magic + 4);
		be16(f, u32(names.size()));
		be16(f, 0); be16(f, 0); be16(f, 0);
		const u32 data = 12 + u32(names.size()) * 16;
		u32 at = data;
		for (const char *nm : names) {
			f.insert(f.end(), nm, nm + 4);
			be32(f, 0);
			be32(f, at);
			be32(f, 16);
			at += 16;
		}
		for (const char *nm : names) {
			std::vector<u8> t(16, 0);
			if (!std::strncmp(nm, "cmap", 4)) {
				t = { 0, 0, 0, 1, u8(platform >> 8), u8(platform), u8(encoding >> 8), u8(encoding), 0, 0, 0, 12, 0, 0, 0, 0 };
			}
			f.insert(f.end(), t.begin(), t.end());
		}
		return f;
	};
	const char ttf[4] = { 0, 1, 0, 0 };
	const std::vector<u8> glyf = make(ttf, { "cmap", "glyf", "head", "hhea", "hmtx", "loca" }, 3, 1);
	const std::vector<u8> cff = make("OTTO", { "CFF ", "cmap", "head", "hhea", "hmtx" }, 3, 10);
	const std::vector<u8> cff2 = make("OTTO", { "CFF2", "cmap", "head", "hhea", "hmtx" }, 3, 1);
	const std::vector<u8> symbol = make(ttf, { "cmap", "glyf", "head", "hhea", "hmtx", "loca" }, 3, 0);
	const std::vector<u8> noloca = make(ttf, { "cmap", "glyf", "head", "hhea", "hmtx" }, 0, 3);
	// 束: 2 つ目に glyf の書体を入れる（1 つ目は CFF2）。番地は束の頭から
	std::vector<u8> ttc = { 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 2 };
	be32(ttc, 20);
	be32(ttc, 20 + u32(cff2.size()));
	auto append = [&](const std::vector<u8> &font) {
		const u32 base = u32(ttc.size());
		std::vector<u8> g = font;
		const u32 tables = u32(g[4]) << 8 | g[5];
		for (u32 i = 0; i < tables; i++) {
			const u32 e = 12 + i * 16 + 8;
			const u32 off = (u32(g[e]) << 24 | u32(g[e + 1]) << 16 | u32(g[e + 2]) << 8 | g[e + 3]) + base;
			g[e] = u8(off >> 24); g[e + 1] = u8(off >> 16); g[e + 2] = u8(off >> 8); g[e + 3] = u8(off);
		}
		ttc.insert(ttc.end(), g.begin(), g.end());
	};
	append(cff2);
	append(glyf);
	const std::vector<u8> junk(64, 0x55);
	std::printf("文字ファイルを読めるか: glyf %d・CFF %d・CFF2 だけ %d・Unicode の表なし %d・loca なし %d・束の 1 つ目 %d・2 つ目 %d・無い番号 %d・ごみ %d・途中で切れた %d\n",
	            ui::font_parses(glyf.data(), glyf.size()), ui::font_parses(cff.data(), cff.size()), ui::font_parses(cff2.data(), cff2.size()),
	            ui::font_parses(symbol.data(), symbol.size()), ui::font_parses(noloca.data(), noloca.size()),
	            ui::font_parses(ttc.data(), ttc.size(), 0), ui::font_parses(ttc.data(), ttc.size(), 1), ui::font_parses(ttc.data(), ttc.size(), 2),
	            ui::font_parses(junk.data(), junk.size()), ui::font_parses(glyf.data(), 40));
}

int main()
{
	std::vector<u8>  wave(64 * 1024 * 1024, 0);   // 波形 ROM 相当のダミー
	std::vector<u16> sintab(0x8000, 0);

	swp30_device swp;
	swp.set_wave_rom(wave.data(), wave.size());
	swp.set_sintab(sintab.data(), sintab.size());
	swp.reset();

	// レジスタの読み書きが素通しできるか（ピッチ = slot 0x11）
	swp.write16(0 * 0x40 + 0x11, 0x1234);
	u16 back = swp.read16(0 * 0x40 + 0x11);
	std::printf("ピッチレジスタ 書き 0x1234 -> 読み 0x%04x  %s\n",
	            back, back == 0x1234 ? "一致" : "不一致");

	// 1 サンプル回してみる（無音のはず）
	s32 l = 0, r = 0;
	for (int i = 0; i < 100; i++) swp.run_sample(l, r);
	std::printf("100 サンプル実行  最後の出力 L=%d R=%d\n", l, r);

	// 乱数が MAME と同じ数列か
	// printf の引数は評価順が未規定。1 個ずつ取り出さないと順序が入れ替わる
	std::printf("乱数 1〜3 個目:");
	for (int i = 0; i < 3; i++) std::printf(" %08x", swp.machine().rand());
	std::printf("\n");

	// MEG の JIT が機械語で書いたリバーブ RAM の詰め方・戻し方と係数の広げ方（m1_expand）が、C++ の関数と全部の入力で同じか
	std::printf("MEG の JIT のリバーブ RAM の詰め方と戻し方・係数の広げ方: 食い違い %llu\n",
	            (unsigned long long)swp30_device::meg_jit_selftest());

	// aarch64 emitter selftest: every primitive the arm64 JIT uses, executed
	// on this CPU and compared against plain C++ (a64asm.cpp; 0 on x86-64 builds)
	std::printf("a64 emitter selftest: %llu mismatch(es)\n", (unsigned long long)a64::selftest());

	check_bend_thinner();
	check_smf_player();
	check_font_parses();

	// Roland の液晶のデータ（F0 41 10 45 12）だけを抜き、GS リセット（F0 41 10 42 12）と XG（F0 43）は通す
	{
		ui::bend_thinner th;
		int passed = 0;
		auto send = [&passed](int, const u8 *, size_t) { passed++; };
		const u8 disp[] = { 0xf0, 0x41, 0x10, 0x45, 0x12, 0x10, 0x00, 0x00, 0x48, 0x49, 0x0f, 0xf7 };
		const u8 gs[]   = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
		const u8 xg[]   = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
		th.event(0, disp, sizeof(disp), 0.0, send);
		th.event(0, gs, sizeof(gs), 0.0, send);
		th.event(0, xg, sizeof(xg), 0.0, send);
		std::printf("Roland の液晶のデータを抜く: 抜いた %zu・通した %d（GS リセットと XG）\n", th.dropped(), passed);
	}
	return 0;
}
