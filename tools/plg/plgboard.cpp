// PLG1 にボードが挿さっているふりをして、firmware の問い合わせに答えてみる（調べもの）。
// やり取りの形は doc/plg-protocol.md。make では作らない。作り方の例:
//   g++ -std=c++20 -O2 -Isrc -Isrc/compat -Isrc/mame -o plgboard.exe tools/plg/plgboard.cpp <build の下の .o> -static
//
//   plgboard <roms> <秒> [引数 ...]
//
// 引数（数字は 16 進。時刻だけ「秒 × 10」の 10 進）:
//   AAAAAA=データ        機種 4E の番地への返事。011000=000100 010000=<名前 14 文字> など
//   4f7f0000NN=データ    機種 4F の表の NN 番目への返事。最後を ** にすると何番目でも同じ返事
//   qRR=KKデータ         F0 43 40 <RR> ... への返事。KK は返事の種類（40-43）。qRR#3= で 3 回目だけ
//   midi@100=904064      10.0 秒に本体の MIDI IN へ送る
//   key@100=Util         10.0 秒にパネルのボタンを押す（名前は mu2000::button_name のもの）
//
// 環境変数:
//   PLG_RAM=<ファイル>            終わりにワーク RAM を書き出す
//   PLG_TRACE=<ファイル>          SCI4 のレジスタの読み書きを書き出す
//   PLG_PC=1                      9 秒から先で多く居た番地（1 サンプルに 1 回だけ見る粗いもの）
//   PLG_PCSET=<ファイル>,<から>,<まで>  その間（秒 × 10）に入ったブロックを書き出す。2 回を比べて、キーを押したときだけ通る所を探す
//   PLG_LCDHEX=1                  液晶の、文字でない所を <xx> で出す
//   PLG_ITRACE=<ファイルの頭>,<から>  その時刻（秒 × 10）から命令を 1 つずつ書き出す（SMU2000_SH2_JIT=0 と一緒に使う）。
//                                 0.01 秒ごとに <頭>0.txt と <頭>1.txt を交互に書き直し、例外の受け皿に入ったらそこで終わる
// 例外の受け皿（0x0401FE）で止まって終わったときは、どの例外かとスタックを出す
#include "mu2000.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace smu2000 { extern u32 *g_pc_prof; extern std::FILE *g_pc_trace; extern u64 g_pc_trace_left; }
static mu2000 M;
static std::vector<u8> g_msg[0x80];       // 行き先の印（1-7）ごとの、組み立て中のメッセージ
static std::map<std::vector<u8>, std::vector<u8>> g_table;   // 機種から番地の終わりまで → 返すデータ
static std::map<std::vector<u8>, std::vector<u8>> g_wild;    // 番地の最後の 1 バイトは何でもよいもの
static int g_t = 0;
static std::map<int, std::vector<u8>> g_q40;   // F0 43 40 03 ... への返事（種類 40-43 ごと）
static int g_xg_bulk = 0;
static std::map<u32, int> g_pc;
static bool g_pc_on = std::getenv("PLG_PC") != nullptr;
static std::vector<std::pair<int, std::string>> g_keys;       // (秒 × 10, 押すボタンの名前)
static std::vector<std::pair<int, std::vector<u8>>> g_midi;   // (秒 × 10, 本体の MIDI IN へ送るバイト)

static std::vector<u8> hex(const std::string &s)
{
	std::vector<u8> v;
	for (size_t i = 0; i + 1 < s.size(); i += 2)
		v.push_back(u8(std::stoi(s.substr(i, 2), nullptr, 16)));
	return v;
}
static std::string lcd()
{
	const u8 *dd = M.lcd().ddram();
	std::string s;
	for (int row = 0; row < 2; row++) {
		for (int c = 0; c < 24; c++) {
			const u8 ch = dd[row * 0x40 + c];
			if (ch >= 32 && ch < 127) {
				s += char(ch);
			} else if (std::getenv("PLG_LCDHEX")) {
				char t[8];
				std::snprintf(t, sizeof t, "<%02x>", ch);
				s += t;
			} else {
				s += '.';
			}
		}
		s += '|';
	}
	return s;
}
static void show(const char *who, int slots, const std::vector<u8> &m)
{
	std::printf("%5.2fs %s [slots %d] ", g_t / 44100.0, who, slots);
	for (size_t i = 0; i < m.size() && i < 40; i++)
		std::printf("%02x ", m[i]);
	std::printf("%s\n", m.size() > 40 ? "..." : "");
}
static void on_message(int slots, const std::vector<u8> &m)
{
	// XG の設定の一括（機種 4C）は数だけ数える
	if (m.size() > 4 && m[0] == 0xf0 && m[1] == 0x43 && m[3] == 0x4c && (m[2] & 0xf0) == 0x00) {
		g_xg_bulk++;
		return;
	}
	show("MU->board", slots, m);
	if (!(slots & 1))
		return;
	// 問い合わせ: F0 43 40 <要求> a b c d ... F7 → F0 43 40 <種類> a b c d <データ> F7
	// 表は「q<要求>=<種類><データ>」。要求ごとに順番に違う返事をしたいときは q<要求>#<何回目>=
	if (m.size() >= 9 && m[0] == 0xf0 && m[1] == 0x43 && m[2] == 0x40 && m[3] < 0x40) {
		static std::map<int, int> seen;
		const int nth = seen[m[3]]++;
		auto it = g_q40.find(m[3] << 8 | (nth + 1));
		if (it == g_q40.end())
			it = g_q40.find(m[3] << 8);
		if (it == g_q40.end() || it->second.empty())
			return;
		std::vector<u8> r = { 0xf0, 0x43, 0x40, it->second[0], m[4], m[5], m[6], m[7] };
		r.insert(r.end(), it->second.begin() + 1, it->second.end());
		r.push_back(0xf7);
		show("board->MU", 1, r);
		M.plg_reply(0, r);
		return;
	}
	// 問い合わせ: F0 43 3n 4E aa bb cc F7
	if (m.size() >= 8 && m[0] == 0xf0 && m[1] == 0x43 && (m[2] & 0xf0) == 0x30 && (m[3] == 0x4e || m[3] == 0x4f)) {
		const std::vector<u8> addr(m.begin() + 3, m.end() - 1);
		const std::vector<u8> *data = nullptr;
		if (const auto it = g_table.find(addr); it != g_table.end())
			data = &it->second;
		else if (const auto iw = g_wild.find(std::vector<u8>(addr.begin(), addr.end() - 1)); iw != g_wild.end())
			data = &iw->second;
		else if (const auto i4 = g_wild.find(std::vector<u8>(addr.begin(), addr.begin() + 4)); addr.size() > 4 && i4 != g_wild.end())
			data = &i4->second;      // 機種 4F で番地のあとに引数がいくつか付くもの（音色の名前など）。4f7f1000**=
		if (!data) {
			std::printf("        (no answer in the table)\n");
			return;
		}
		std::vector<u8> r = { 0xf0, 0x43, u8(0x10 | (m[2] & 15)) };
		// 機種 4F の表は、聞くときだけ番地の後ろに「何番目」が付く。返事の番地は 3 バイト
		r.insert(r.end(), addr.begin(), m[3] == 0x4f ? addr.begin() + 4 : addr.end());
		r.insert(r.end(), data->begin(), data->end());
		r.push_back(0xf7);
		show("board->MU", 1, r);
		M.plg_reply(0, r);
	}
}

int main(int argc, char **argv)
{
	const std::string dir = argv[1];
	const int secs = std::atoi(argv[2]);
	for (int i = 3; i < argc; i++) {
		const std::string a = argv[i];
		const size_t eq = a.find('=');
		if (a.rfind("key@", 0) == 0) {
			g_keys.push_back({ std::stoi(a.substr(4, eq - 4)), a.substr(eq + 1) });
			continue;
		}
		if (a.rfind("q", 0) == 0 && (eq == 3 || a[3] == '#')) {
			const int nth = eq == 3 ? 0 : std::stoi(a.substr(4, eq - 4));
			g_q40[std::stoi(a.substr(1, 2), nullptr, 16) << 8 | nth] = hex(a.substr(eq + 1));
			continue;
		}
		if (a.rfind("midi@", 0) == 0) {
			g_midi.push_back({ std::stoi(a.substr(5, eq - 5)), hex(a.substr(eq + 1)) });
			continue;
		}
		// 番地は 3 バイト（機種 4E）か、頭に機種を付けたもの。終わりが ** なら最後の 1 バイトは何でもよい
		std::string ks = a.substr(0, eq);
		const bool wild = ks.size() > 2 && ks.substr(ks.size() - 2) == "**";
		if (wild)
			ks.resize(ks.size() - 2);
		std::vector<u8> k = hex(ks);
		if (k.size() == (wild ? 2u : 3u))
			k.insert(k.begin(), 0x4e);
		(wild ? g_wild : g_table)[k] = hex(a.substr(eq + 1));
	}
	if (!M.load_program(dir + "/mu2000_flash.bin") || !M.load_wave(dir + "/dump")) return 1;
	M.load_sintab(dir + "/standin/sin-table.bin");
	M.set_plg_tx([](int slots, u8 byte) {
		std::vector<u8> &m = g_msg[slots & 0x7f];
		if (byte >= 0x80 && byte < 0xf0)
			m.clear();
		if (byte == 0xf0)
			m.clear();
		m.push_back(byte);
		if (byte == 0xf7 || (m[0] != 0xf0 && m.size() >= 3)) {
			on_message(slots, m);
			m.clear();
		}
	});
	std::FILE *tr = nullptr;
	if (const char *t = std::getenv("PLG_TRACE")) {
		tr = std::fopen(t, "w");
		M.set_plg_trace(tr);
	}
	M.reset();
	std::string last;
	for (g_t = 0; g_t < 44100 * secs; g_t++) {
		s32 l, r;
		M.run_sample(l, r);
		// 本体の音が出ているか（0.1 秒ごとの山。鳴り始めと鳴り終わりだけ出す）
		{
			static int peak = 0;
			static bool was = false;
			peak = std::max({ peak, std::abs(int(l)), std::abs(int(r)) });
			if (g_t % 4410 == 4409) {
				const bool on = peak > 200;
				if (on != was)
					std::printf("%5.2fs audio %s (peak %d)%c", g_t / 44100.0, on ? "on" : "off", peak, 10);
				was = on;
				peak = 0;
			}
		}
		u8 b;
		while (M.midi_out_take(b)) {}
		if (g_pc_on && g_t > 44100 * 9)
			g_pc[M.cpu().pc()]++;
		// PLG_PCSET=<ファイル>,<から 0.1 秒>,<まで>: その間に入ったブロック（0x40 バイト刻み）を書き出す。
		// キーを押したとき・押さないときを比べて、押したときだけ通る所を探す
		static const char *pcset = std::getenv("PLG_PCSET");
		static int pc_from = -1, pc_to = -1;
		static std::string pc_file;
		static std::vector<u32> blocks;
		if (pcset && pc_file.empty()) {
			const std::string v = pcset;
			const size_t c1 = v.rfind(','), c0 = v.rfind(',', c1 - 1);
			pc_file = v.substr(0, c0);
			pc_from = std::stoi(v.substr(c0 + 1, c1 - c0 - 1)) * 4410;
			pc_to = std::stoi(v.substr(c1 + 1)) * 4410;
			blocks.assign(0x400000 / 0x40, 0);
		}
		if (g_t == pc_from)
			smu2000::g_pc_prof = blocks.data();
		if (g_t == pc_to) {
			smu2000::g_pc_prof = nullptr;
			std::FILE *f = std::fopen(pc_file.c_str(), "w");
			for (size_t i = 0; i < blocks.size(); i++)
				if (blocks[i])
					std::fprintf(f, "%06x %u%c", unsigned(i * 0x40), blocks[i], 10);
			std::fclose(f);
		}
		// ボタン: 0.1 秒の頭で押して、その 0.05 秒後に離す
		for (const auto &k : g_keys) {
			if (g_t != k.first * 4410 && g_t != k.first * 4410 + 6615)
				continue;
			for (int b = 0; b < int(mu2000::button::count); b++)
				if (k.second == mu2000::button_name(mu2000::button(b))) {
					M.set_button(mu2000::button(b), g_t == k.first * 4410);
					if (g_t == k.first * 4410)
						std::printf("%5.2fs key %s\n", g_t / 44100.0, k.second.c_str());
				}
		}
		{
			static const char *itr = std::getenv("PLG_ITRACE");
			static std::string head;
			static int from = -1, which = 0;
			if (itr && head.empty()) {
				const std::string v = itr;
				const size_t c = v.rfind(',');
				head = v.substr(0, c);
				from = std::stoi(v.substr(c + 1)) * 4410;
			}
			if (itr && g_t >= from && (g_t - from) % 441 == 0) {
				if (smu2000::g_pc_trace)
					std::fclose(smu2000::g_pc_trace);
				which ^= 1;
				smu2000::g_pc_trace = std::fopen((head + char('0' + which) + ".txt").c_str(), "w");
				smu2000::g_pc_trace_left = ~u64(0);
			}
			if (itr && g_t >= from && M.cpu().pc() >= 0x0401f4 && M.cpu().pc() <= 0x040202) {
				std::fclose(smu2000::g_pc_trace);
				smu2000::g_pc_trace = nullptr;
				std::printf("trapped at %.3fs, last trace file %d%c", g_t / 44100.0, which, 10);
				break;
			}
		}
		if (g_t % 4410 == 0) {
			if (tr)
				std::fprintf(tr, "t %d%c", g_t / 4410, 10);
			for (const auto &m : g_midi)
				if (m.first == g_t / 4410) {
					show("host->MU ", 0, m.second);
					for (u8 b : m.second)
						M.midi_in(b, 0);
				}
			const std::string now = lcd();
			if (now != last) {
				std::printf("%5.2fs LCD [%s]\n", g_t / 44100.0, now.c_str());
				last = now;
			}
		}
	}
	std::printf("XG bulk messages to the boards: %d\n", g_xg_bulk);
	// 例外の受け皿（0401fe の輪）で止まっていたら、どの例外でどこから来たかを出す
	if (M.cpu().pc() >= 0x0401f4 && M.cpu().pc() <= 0x040202) {
		const auto *st = M.cpu().m_sh2_state;
		const u32 sp = st->r[15];
		std::printf("TRAPPED: vector %u, sp %08x, stack:", unsigned(st->r[4]), unsigned(sp));
		for (int i = 0; i < 12; i++) {
			const u32 o = sp - 0x400000 + i * 4;
			const auto &w = M.work_ram();
			if (o + 3 < w.size())
				std::printf(" %02x%02x%02x%02x", w[o], w[o + 1], w[o + 2], w[o + 3]);
		}
		std::puts("");
	}
	if (g_pc_on) {
		std::vector<std::pair<int, u32>> top;
		for (const auto &kv : g_pc)
			top.push_back({ kv.second, kv.first });
		std::sort(top.rbegin(), top.rend());
		for (size_t i = 0; i < top.size() && i < 400; i++)
			std::printf("pc %06x x%d%c", top[i].second, top[i].first, 10);
	}
	// ワーク RAM を書き出す（ボードあり・なしを比べる）
	if (const char *out = std::getenv("PLG_RAM")) {
		std::FILE *f = std::fopen(out, "wb");
		std::fwrite(M.work_ram().data(), 1, M.work_ram().size(), f);
		std::fclose(f);
	}
	return 0;
}
