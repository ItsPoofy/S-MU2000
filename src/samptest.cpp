// サンプリングが一回りするかを確かめる。
//
//   samptest <rom ディレクトリ> [-v]
//
// パネルで SAMPLING → REC に入り、A/D INPUT に 440Hz の正弦を流しながら 1 秒ほど録音して止め、
// 残す。SAMPLE の画面で AUDITION を押し、出てきた音が 440Hz かを見る。
// firmware が録音に使う SWP30 の働き（サンプリング RAM と波形アクセス 0x7000）が
// 正しくないと、サンプルが出来ないか、試聴で別の音か無音になる。
// 続けて A/D パートの音量と、SmartMedia への書き出し・読み戻し（書式化 → SAVE → 別の機械で LOAD）、
// REC の InputSrc（AD2 / AD1+2）で録るものが変わるかを見る。
// 食い違えば 1 を返す。
#include "mu2000.h"
#include "ui/bridge.h"
#include "ui/driver.h"
#include "wav_in.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr u32 RATE = 44100;
constexpr double PI = 3.14159265358979323846;

struct rig {
	mu2000 mu;
	bool verbose = false;
	double sine_amp = 0.0;          // A/D INPUT に流す正弦の振幅（0 なら無音）
	double sine2_amp = -1.0;        // AD2 だけ 660Hz にするときの振幅（負なら AD1 と同じもの）
	u64 n = 0;
	std::vector<double> out;        // 集めている間の出力（左右の平均）
	bool collect = false;

	void pump(u32 ms)
	{
		const u64 until = n + u64(ms) * RATE / 1000;
		for (; n < until; n++) {
			const s32 v = s32(std::lround(sine_amp * std::sin(2 * PI * 440.0 * double(n) / RATE)));
			const s32 v2 = sine2_amp < 0 ? v : s32(std::lround(sine2_amp * std::sin(2 * PI * 660.0 * double(n) / RATE)));
			mu.set_audio_input(v, v2);
			s32 l, r;
			mu.run_sample(l, r);
			u8 b;
			while (mu.midi_out_take(b)) {}
			if (collect)
				out.push_back((double(l) + double(r)) * 0.5 / mu2000::DAC_FULL_SCALE);
		}
	}

	std::string lcd()
	{
		const u8 *dd = mu.lcd().ddram();
		std::string s;
		for (int row = 0; row < 2; row++) {
			for (int c = 0; c < 24; c++) {
				const u8 ch = dd[row * 0x40 + c];
				s += (ch >= 32 && ch < 127) ? char(ch) : ' ';
			}
			if (!row)
				s += '|';
		}
		return s;
	}

	void press(mu2000::button b, u32 hold_ms = 80)
	{
		mu.set_button(b, true);
		pump(hold_ms);
		mu.set_button(b, false);
		pump(300);
		if (verbose)
			std::printf("  %-14s [%s]\n", mu2000::button_name(b), lcd().c_str());
	}
};

// 周波数 f の成分の大きさ（Goertzel）
double tone(const std::vector<double> &x, double f)
{
	const double w = 2 * PI * f / RATE, c = 2 * std::cos(w);
	double s1 = 0, s2 = 0;
	for (double v : x) {
		const double s0 = v + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / double(x.size());
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "使い方: samptest <rom ディレクトリ> [-v]\n");
		return 1;
	}
	const std::string dir = argv[1];
	static rig g;
	g.verbose = argc > 2 && !std::strcmp(argv[2], "-v");

	if (!g.mu.load_program(dir + "/mu2000_flash.bin") || !g.mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", g.mu.error().c_str());
		return 1;
	}
	g.mu.load_sintab(dir + "/standin/sin-table.bin");
	g.mu.reset();
	for (u32 i = 0; i < 30 * RATE && !g.mu.midi_ready(); i += RATE / 100)
		g.pump(10);
	g.pump(1500);

	int bad = 0;
	auto expect = [&](const char *what, const char *text) {
		const std::string s = g.lcd();
		const bool ok = s.find(text) != std::string::npos;
		std::printf("%s %-28s [%s]\n", ok ? "合" : "NG", what, s.c_str());
		if (!ok)
			bad++;
	};

	using B = mu2000::button;
	// SAMPLING の品書き: EDIT LOAD SAVE / REC UTIL RAM。REC は 4 つ目
	g.press(B::sampling_mode);
	expect("SAMPLING の品書き", "REC");
	for (int i = 0; i < 3; i++)
		g.press(B::select_right);
	g.press(B::enter);
	expect("REC の画面", "Sp=001");

	// 録音。始める前から正弦を流しておく
	g.sine_amp = 12000;
	g.pump(200);
	g.press(B::enter);
	expect("録音中", "Recording!");
	g.pump(1000);
	g.press(B::enter);                  // 止める
	g.sine_amp = 0;
	g.pump(300);
	g.press(B::exit);
	expect("残すか聞かれる", "Keep Sample 001?");
	g.press(B::enter);
	g.press(B::exit);

	// EDIT → SAMPLE → SMPL001 で試聴
	for (int i = 0; i < 3; i++)
		g.press(B::select_left);
	g.press(B::enter);
	g.press(B::enter);
	expect("出来たサンプル", "SMPL001");
	g.collect = true;
	g.mu.set_button(B::audition, true);
	g.pump(1000);
	g.mu.set_button(B::audition, false);
	g.collect = false;

	double rms = 0;
	for (double v : g.out)
		rms += v * v;
	rms = std::sqrt(rms / std::max<size_t>(1, g.out.size()));
	const double t440 = tone(g.out, 440), t330 = tone(g.out, 330), t587 = tone(g.out, 587);
	const bool loud = rms > 0.01;
	const bool pitch = t440 > 10 * std::max(t330, t587);
	std::printf("%s 試聴の音の大きさ              rms %.4f（全振幅 1）\n", loud ? "合" : "NG", rms);
	std::printf("%s 試聴の音が 440Hz              440Hz %.5f / 330Hz %.5f / 587Hz %.5f\n",
	            pitch ? "合" : "NG", t440, t330, t587);
	if (!loud) bad++;
	if (!pitch) bad++;

	// A/D パート。既定の音量は 0 で、入力は聞こえない。音量を上げると A/D INPUT がそのまま鳴る
	// （スレーブの MELI 6/7 を firmware がミキサに通す）
	g.press(B::exit);
	g.press(B::exit);
	g.press(B::exit);
	auto level_440 = [&](double &rms_out) {
		g.out.clear();
		g.sine_amp = 8000;
		g.pump(200);
		g.collect = true;
		g.pump(500);
		g.collect = false;
		g.sine_amp = 0;
		double sum = 0;
		for (double v : g.out)
			sum += v * v;
		rms_out = std::sqrt(sum / std::max<size_t>(1, g.out.size()));
		return tone(g.out, 440);
	};
	double rms_off = 0, rms_on = 0;
	const double ad_off = level_440(rms_off);
	for (int part = 0; part < 2; part++) {
		const u8 msg[] = { 0xf0, 0x43, 0x10, 0x4c, 0x10, u8(part), 0x0b, 100, 0xf7 };
		for (u8 b : msg)
			g.mu.midi_in(b, 0);
	}
	g.pump(300);
	const double ad_on = level_440(rms_on);
	const bool off_ok = rms_off < 0.001;
	const bool on_ok = rms_on > 0.05 && ad_on > 10 * std::max(tone(g.out, 330), tone(g.out, 587));
	std::printf("%s A/D パートの音量 0 では無音      rms %.5f\n", off_ok ? "合" : "NG", rms_off);
	std::printf("%s A/D パートの音量 100 で入力が鳴る rms %.4f / 440Hz %.5f\n", on_ok ? "合" : "NG", rms_on, ad_on);
	(void)ad_off;
	if (!off_ok) bad++;
	if (!on_ok) bad++;

	// SmartMedia。空のカードを差して UTIL → CARD → Format で書式化し、SAMPLING → SAVE で ALL+SEQ を書く。
	// 書いたカードを新しい機械に差し、SAMPLING → LOAD で読み戻して、サンプリング RAM が同じになるかを見る。
	// SmartMedia の NAND の命令・物理の書式・ECC と、SWP30 の続けて読む働き（波形アクセス 0x9000）を通る
	g.mu.card().create(32);
	g.pump(500);
	g.press(B::util);
	for (int i = 0; i < 4; i++)
		g.press(B::select_right);
	g.press(B::enter);
	for (int i = 0; i < 4; i++)
		g.press(B::select_right);
	expect("UTIL → CARD → Format", "Format");
	g.press(B::enter);
	g.press(B::enter);                  // 書式化してよいか
	for (int i = 0; i < 100 && g.lcd().find("Executing") != std::string::npos; i++)
		g.pump(100);
	expect("書式化を終えた", "Format");
	g.press(B::exit);
	g.press(B::exit);
	g.press(B::exit);

	g.press(B::sampling_mode);
	g.press(B::select_right);
	g.press(B::select_right);
	g.press(B::enter);
	expect("SAVE の画面", "ALL+SEQ");
	g.press(B::enter);                  // 保存先のディレクトリ
	g.pump(1000);
	g.press(B::enter);                  // ファイルの名前
	g.pump(1000);
	expect("ファイルの名前", "ALL_SEQ");
	g.press(B::enter);
	expect("書き出し中", "SAVING");
	for (int i = 0; i < 100 && g.lcd().find("SAVING") != std::string::npos; i++)
		g.pump(100);
	expect("書き終えた", "<SAVE>");

	static rig h;
	if (!h.mu.load_program(dir + "/mu2000_flash.bin") || !h.mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", h.mu.error().c_str());
		return 1;
	}
	h.verbose = g.verbose;
	h.mu.load_sintab(dir + "/standin/sin-table.bin");
	h.mu.reset();
	for (u32 i = 0; i < 30 * RATE && !h.mu.midi_ready(); i += RATE / 100)
		h.pump(10);
	h.mu.card() = g.mu.card();
	h.pump(1500);
	h.press(B::sampling_mode);
	h.press(B::select_right);
	h.press(B::enter);
	h.pump(1000);
	h.press(B::enter);                  // ディレクトリの中
	h.pump(1000);
	{
		const std::string s = h.lcd();
		const bool ok = s.find("ALL_SEQ.M2A") != std::string::npos;
		std::printf("%s %-28s [%s]\n", ok ? "合" : "NG", "カードにファイルがある", s.c_str());
		if (!ok) bad++;
	}
	h.press(B::enter);
	for (int i = 0; i < 100 && h.lcd().find("LOADING") != std::string::npos; i++)
		h.pump(100);
	// 録音の最後の 1 語の後ろ半分（サンプルの長さの外）は書き出されないので、そこだけは違ってよい
	const auto &a = g.mu.sample_ram(), &b = h.mu.sample_ram();
	size_t differ = 0, used = 0;
	for (size_t i = 0; i < a.size(); i++) {
		differ += a[i] != b[i];
		used += a[i] != 0;
	}
	const bool same = used > 50000 && differ <= 2;
	std::printf("%s 読み戻したサンプリング RAM     使っている %zu バイト、違う %zu バイト\n", same ? "合" : "NG", used, differ);
	if (!same) bad++;

	// REC の InputSrc。AD1 に 440Hz、AD2 に 660Hz を入れ、AD2 と AD1+2 で録る。
	// firmware は MELI 6/7 からミキサの出力 8 への音量を切り替えるので、録ったものの周波数で分かる
	auto record_src = [&](int presses, double &f440, double &f660) {
		g.press(B::exit);
		g.press(B::select_right);                       // SAVE の隣が REC
		g.press(B::enter);
		for (int i = 0; i < 3; i++)
			g.press(B::select_right);
		for (int i = 0; i < presses; i++)
			g.press(B::value_plus);
		for (int i = 0; i < 3; i++)
			g.press(B::select_left);
		const std::vector<u8> before = g.mu.sample_ram();
		g.sine_amp = 8000;
		g.sine2_amp = 8000;
		g.pump(200);
		g.press(B::enter);
		g.pump(700);
		g.press(B::enter);
		g.sine_amp = 0;
		g.sine2_amp = -1;
		g.pump(300);
		std::vector<double> x;
		const auto &after = g.mu.sample_ram();
		for (size_t i = 0; i + 1 < after.size(); i += 2)
			if (after[i] != before[i] || after[i + 1] != before[i + 1])
				x.push_back(double(s16(after[i] | (after[i + 1] << 8))) / 32768.0);
		// 途中の 4000 サンプルで見る（変わらなかったバイトを飛ばしているので、全部を繋ぐと位相が飛ぶ）
		const std::vector<double> mid = x.size() > 8000 ? std::vector<double>(x.begin() + 4000, x.begin() + 8000) : std::vector<double>();
		f440 = mid.empty() ? 0 : tone(mid, 440);
		f660 = mid.empty() ? 0 : tone(mid, 660);
		g.press(B::exit);                               // Keep Sample? から抜ける（残すかどうかは見ない）
		g.press(B::exit);
		return x.size();
	};
	{
		double a440 = 0, a660 = 0, b440 = 0, b660 = 0;
		const size_t na = record_src(1, a440, a660);    // AD1 → AD2
		const bool ad2 = a660 > 0.05 && a440 < a660 / 20;
		std::printf("%s InputSrc=AD2 で AD2 だけ録る    %zu サンプル、440Hz %.4f / 660Hz %.4f\n", ad2 ? "合" : "NG", na, a440, a660);
		const size_t nb = record_src(1, b440, b660);    // AD2 → AD1+2
		const bool both = b440 > 0.05 && b660 > 0.05;
		std::printf("%s InputSrc=AD1+2 で両方を録る     %zu サンプル、440Hz %.4f / 660Hz %.4f\n", both ? "合" : "NG", nb, b440, b660);
		if (!ad2) bad++;
		if (!both) bad++;
	}

	// REC の TriggerLvl。レベルを上げて Enter を押すと「Waiting!」で待ち、入力が来ると録音が始まる。
	// firmware は CPU の A/D 変換器の AN0 / AN2（A/D INPUT の大きさ）を回し続けて読む
	g.press(B::exit);
	g.press(B::select_right);
	g.press(B::enter);
	g.press(B::select_right);
	for (int i = 0; i < 6; i++)
		g.press(B::value_plus);
	expect("TriggerLvl を上げた", "TriggerLvl=06");
	g.press(B::select_left);
	g.press(B::enter);
	g.pump(500);
	expect("入力が無いと待つ", "Waiting!");
	g.sine_amp = 12000;
	g.pump(500);
	expect("入力が来ると録音する", "Recording!");
	g.press(B::enter);
	g.sine_amp = 0;
	g.pump(300);

	// パネルを通さない道（src/sampling.cpp）。新しい機械で、A/D INPUT から直に録って firmware の表に足し、
	// 音色に割り当てる。firmware がそれを自分のサンプルとして扱う（一覧・試聴・REC の続き・音色として鳴る）かを見る
	{
		namespace sp = smu2000::sampling;
		static rig k;
		if (!k.mu.load_program(dir + "/mu2000_flash.bin") || !k.mu.load_wave(dir + "/dump")) {
			std::fprintf(stderr, "%s\n", k.mu.error().c_str());
			return 1;
		}
		k.verbose = g.verbose;
		k.mu.load_sintab(dir + "/standin/sin-table.bin");
		k.mu.reset();
		for (u32 i = 0; i < 30 * RATE && !k.mu.midi_ready(); i += RATE / 100)
			k.pump(10);
		k.pump(1500);
		auto check = [&](bool ok, const char *what, const std::string &detail) {
			std::printf("%s %-28s %s\n", ok ? "合" : "NG", what, detail.c_str());
			if (!ok)
				bad++;
		};

		// 引き金つきで録る。300ms は無音なので待ち、正弦が来てから録り始める
		k.mu.rec_start(sp::source::ad1, 4000, 10 * RATE);
		k.pump(300);
		const bool waited = k.mu.rec_state() == 1 && k.mu.rec_frames() == 0;
		k.sine_amp = 12000;
		k.pump(1000);
		k.sine_amp = 0;
		std::vector<s16> pcm = k.mu.rec_take();
		check(waited && pcm.size() > RATE * 9 / 10 && pcm.size() < RATE * 11 / 10 && std::abs(pcm[0]) >= 4000,
		      "直の録音: 引き金を待って録る", std::to_string(pcm.size()) + " サンプル、頭 " + std::to_string(pcm[0]));
		std::string err;
		const int n = k.mu.sampling_add(pcm.data(), pcm.size(), "", err);
		const auto list = k.mu.sampling_list();
		check(n == 1 && list.size() == 1 && list[0].name == "take001" && list[0].frames() >= pcm.size(),
		      "直の録音: firmware の表に足す", err.empty() ? (list.empty() ? std::string() : list[0].name) : err);

		// firmware の SAMPLE の画面に出て、試聴が 440Hz
		k.press(B::sampling_mode);
		k.press(B::enter);
		k.press(B::enter);
		check(k.lcd().find("SMPL001 take001") != std::string::npos, "直の録音: SAMPLE の画面に出る", k.lcd());
		k.out.clear();
		k.collect = true;
		k.mu.set_button(B::audition, true);
		k.pump(800);
		k.mu.set_button(B::audition, false);
		k.collect = false;
		check(tone(k.out, 440) > 0.005 && tone(k.out, 440) > 10 * tone(k.out, 660),
		      "直の録音: 試聴が 440Hz", std::to_string(tone(k.out, 440)));

		// 続けてパネルで録ると、空きの続き（直に足したものの後ろ）に 2 つ目ができる
		k.press(B::exit);
		k.press(B::exit);
		for (int i = 0; i < 3; i++)
			k.press(B::select_right);
		k.press(B::enter);
		check(k.lcd().find("Sp=002") != std::string::npos, "直の録音: REC は Sp=002 から", k.lcd());
		k.sine_amp = 12000;
		k.pump(200);
		k.press(B::enter);
		k.pump(600);
		k.press(B::enter);
		k.sine_amp = 0;
		k.pump(300);
		k.press(B::exit);
		k.press(B::enter);                  // Keep Sample 002?
		k.press(B::exit);
		k.press(B::exit);
		const auto list2 = k.mu.sampling_list();
		check(list2.size() == 2 && list2[1].start == list2[0].end,
		      "直の録音: パネルの録音がその後ろに続く",
		      list2.size() == 2 ? std::to_string(list2[0].end) + " / " + std::to_string(list2[1].start) : std::string());

		// 音色に割り当てて、バンク 16 の PGM001 で鳴らす
		sp::voice v;
		v.assigned = true;
		v.sample = 1;
		v.name = "Direct";
		v.level = 127;
		v.pan = 7;
		const bool set = k.mu.sampling_set_voice(0, v, err);
		sp::voice back;
		k.mu.sampling_voice(0, back);
		check(set && back.assigned && back.sample == 1 && back.name == "Direct", "直の割り当て: 読み戻せる", back.name);
		// 名前の余りは空白（0 だと LCD が CGRAM の字を出す）。名前の欄は 8 文字で、その後ろは触らない
		{
			const auto &d = k.mu.dram();
			const u32 o = sp::TAB_VOICE - 0x1000000;
			const std::string raw(reinterpret_cast<const char *>(&d[o + 2]), 10);
			check(raw == std::string("Direct  ") + std::string(2, '\0'), "直の割り当て: 名前は 8 文字・空白埋め", raw.substr(0, 8));
		}
		k.press(B::play);
		const u8 pc[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x00 };
		for (u8 b : pc)
			k.mu.midi_in(b, 0);
		k.pump(300);
		check(k.lcd().find("Direct") != std::string::npos, "直の割り当て: 音色の名前が出る", k.lcd());
		k.out.clear();
		k.collect = true;
		const u8 on[] = { 0x90, 0x3c, 0x64 };
		for (u8 b : on)
			k.mu.midi_in(b, 0);
		k.pump(600);
		k.collect = false;
		check(tone(k.out, 440) > 0.005 && tone(k.out, 440) > 10 * tone(k.out, 660),
		      "直の割り当て: ノート 60 が 440Hz", std::to_string(tone(k.out, 440)));
		const u8 off[] = { 0x80, 0x3c, 0x40 };
		for (u8 b : off)
			k.mu.midi_in(b, 0);
		k.pump(300);

		// 窓の道（bridge::post → driver::sampling_tick）と WAV の取り込み。48kHz・2ch の WAV
		// （左 660Hz、右 880Hz）を作り、AD2（右）を 44.1kHz に直して足し、PGM002 に割り当てて鳴らす
		std::vector<u8> wav;
		{
			const u32 rate = 48000, frames = rate / 2;
			auto put32 = [&](u32 v) { for (int i = 0; i < 4; i++) wav.push_back(u8(v >> (8 * i))); };
			auto put16 = [&](u32 v) { wav.push_back(u8(v)); wav.push_back(u8(v >> 8)); };
			wav.insert(wav.end(), { 'R', 'I', 'F', 'F' });
			put32(36 + frames * 4);
			wav.insert(wav.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
			put32(16); put16(1); put16(2); put32(rate); put32(rate * 4); put16(4); put16(16);
			wav.insert(wav.end(), { 'd', 'a', 't', 'a' });
			put32(frames * 4);
			for (u32 i = 0; i < frames; i++) {
				put16(u16(s16(std::lround(12000 * std::sin(2 * PI * 660.0 * i / rate)))));
				put16(u16(s16(std::lround(12000 * std::sin(2 * PI * 880.0 * i / rate)))));
			}
		}
		smu2000::wav_data w;
		const bool parsed = smu2000::parse_wav(wav, w, err);
		const std::vector<s16> right = smu2000::wav_for_sampling(w, sp::source::ad2, RATE * 10);
		std::vector<double> rd(right.begin(), right.end());
		check(parsed && right.size() > RATE / 2 - 10 && right.size() <= RATE / 2 &&
		      tone(rd, 880) > 10 * tone(rd, 660),
		      "WAV: 48kHz の右を 44.1kHz に", std::to_string(right.size()) + " サンプル");
		ui::bridge br;
		ui::driver drv;
		auto pcm2 = std::make_shared<std::vector<s16>>(right);
		br.post([pcm2](mu2000 &mu) {
			std::string e;
			const int num = mu.sampling_add(pcm2->data(), pcm2->size(), "wav880", e);
			return num ? "added " + std::to_string(num) : e;
		});
		sp::voice v2;
		v2.assigned = true;
		v2.sample = 3;
		v2.name = "Wav880";
		br.post([v2](mu2000 &mu) {
			std::string e;
			return mu.sampling_set_voice(1, v2, e) ? std::string("voice") : e;
		});
		drv.pump_midi(k.mu, br);
		ui::bridge::sampling_view view;
		br.get_sampling(view);
		check(view.samples.size() == 3 && view.samples[2].name == "wav880" && view.voices[1].name == "Wav880" &&
		      view.message == "voice",
		      "窓の道: 仕事を渡して表を読む", view.message);
		// 写しの入れ物は bridge と driver で入れ替えて使う。何度受け取っても、結果の一言は最後の仕事のまま
		// （入れ物に置いていたときは、新しい一言と古い一言が交互に届いた）
		bool steady = true;
		for (int i = 0; i < 40; i++) {
			drv.pump_midi(k.mu, br);
			br.get_sampling(view);
			steady = steady && view.message == "voice";
		}
		check(steady, "窓の道: 結果の一言が行き来しない", view.message);

		// 音量を変える（サンプル 1 は振幅 12000 の正弦）。2 倍で 24000 前後、もう 2 倍で 16bit の上限で止まる。
		// 見取り図（request_overview）も変えた後の波形になる
		const int before = k.mu.sampling_peak(k.mu.sampling_list()[0]);
		const int doubled = k.mu.sampling_gain(1, 2.0);
		const int clipped = k.mu.sampling_gain(1, 2.0);
		br.request_overview(1);
		drv.pump_midi(k.mu, br);
		br.get_sampling(view);
		s16 wmax = 0;
		for (s16 v : view.wave_hi)
			wmax = std::max(wmax, v);
		// 負の側は -32768 で止まるので、最大の絶対値は 32768 になりうる
		check(std::abs(doubled - 2 * before) <= 2 && clipped >= 32767 && view.wave_number == 1 &&
		      int(view.wave_hi.size()) == ui::bridge::WAVE_BUCKETS && wmax == 32767,
		      "音量を変える・見取り図",
		      std::to_string(before) + " → " + std::to_string(doubled) + " → " + std::to_string(clipped) +
		      "、見取り図の最大 " + std::to_string(wmax));
		const u8 pc2[] = { 0xc0, 0x01 };
		for (u8 b : pc2)
			k.mu.midi_in(b, 0);
		k.pump(300);
		k.out.clear();
		k.collect = true;
		for (u8 b : on)
			k.mu.midi_in(b, 0);
		k.pump(400);
		k.collect = false;
		for (u8 b : off)
			k.mu.midi_in(b, 0);
		check(tone(k.out, 880) > 0.005 && tone(k.out, 880) > 10 * tone(k.out, 660),
		      "窓の道: PGM002 が 880Hz", std::to_string(tone(k.out, 880)));
		k.pump(300);
	}

	std::printf("サンプリング: 食い違い %d\n", bad);
	return bad ? 1 : 0;
}
