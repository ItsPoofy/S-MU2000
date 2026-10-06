// PLG1 にボードが挿さっているふりをして、firmware の問い合わせに答えてみる（調べもの）。
//   plgboard <roms> <秒> [返事の表 ...]
// 返事の表は「番地=データ」を 16 進で: 例 011000=010203 010000=41424344...
// 本体が `F0 43 3n 4E <番地> F7` と聞いてきたら、表にあれば `F0 43 1n 4E <番地> <データ> F7` を返す。
// boot=... を付けると、起動して最初の問い合わせを待たずに、その並びを最初に送る
#include "mu2000.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

static mu2000 M;
static std::vector<u8> g_msg[8];       // 行き先の印（1-7）ごとの、組み立て中のメッセージ
static std::map<u32, std::vector<u8>> g_table;
static int g_t = 0;
static int g_xg_bulk = 0;
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
			s += (ch >= 32 && ch < 127) ? char(ch) : '.';
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
	// 問い合わせ: F0 43 3n 4E aa bb cc F7
	if (m.size() == 8 && m[0] == 0xf0 && m[1] == 0x43 && (m[2] & 0xf0) == 0x30 && (m[3] == 0x4e || m[3] == 0x4f)) {
		const u32 addr = u32(m[3]) << 24 | u32(m[4]) << 16 | u32(m[5]) << 8 | m[6];
		const auto it = g_table.find(addr);
		if (it == g_table.end()) {
			std::printf("        (no answer in the table for %02x %02x %02x %02x)\n", m[3], m[4], m[5], m[6]);
			return;
		}
		std::vector<u8> r = { 0xf0, 0x43, u8(0x10 | (m[2] & 15)), m[3], m[4], m[5], m[6] };
		r.insert(r.end(), it->second.begin(), it->second.end());
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
		if (a.rfind("midi@", 0) == 0) {
			g_midi.push_back({ std::stoi(a.substr(5, eq - 5)), hex(a.substr(eq + 1)) });
			continue;
		}
		const std::vector<u8> k = hex(a.substr(0, eq));
		// 番地は 3 バイト（機種 4E）か、頭に機種を付けた 4 バイト
		const u32 addr = k.size() == 4 ? u32(k[0]) << 24 | u32(k[1]) << 16 | u32(k[2]) << 8 | k[3]
		                               : u32(0x4e) << 24 | u32(k[0]) << 16 | u32(k[1]) << 8 | k[2];
		g_table[addr] = hex(a.substr(eq + 1));
	}
	if (!M.load_program(dir + "/mu2000_flash.bin") || !M.load_wave(dir + "/dump")) return 1;
	M.load_sintab(dir + "/standin/sin-table.bin");
	M.set_plg_tx([](int slots, u8 byte) {
		std::vector<u8> &m = g_msg[slots & 7];
		if (byte == 0xf0)
			m.clear();
		m.push_back(byte);
		if (byte == 0xf7 || (m[0] != 0xf0 && m.size() >= 3)) {
			on_message(slots, m);
			m.clear();
		}
	});
	M.reset();
	std::string last;
	for (g_t = 0; g_t < 44100 * secs; g_t++) {
		s32 l, r;
		M.run_sample(l, r);
		u8 b;
		while (M.midi_out_take(b)) {}
		if (g_t % 4410 == 0) {
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
	// ワーク RAM を書き出す（ボードあり・なしを比べる）
	if (const char *out = std::getenv("PLG_RAM")) {
		std::FILE *f = std::fopen(out, "wb");
		std::fwrite(M.work_ram().data(), 1, M.work_ram().size(), f);
		std::fclose(f);
	}
	return 0;
}
