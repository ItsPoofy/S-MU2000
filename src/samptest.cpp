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
#include "card_fs.h"
#include "m2a.h"
#include "wavegen.h"
#include "ui/panel_macro.h"

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
	double sum_l = 0, sum_r = 0;    // 集めている間の左・右の二乗の和（パンを見る）
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
			if (collect) {
				out.push_back((double(l) + double(r)) * 0.5 / mu2000::DAC_FULL_SCALE);
				sum_l += (double(l) / mu2000::DAC_FULL_SCALE) * (double(l) / mu2000::DAC_FULL_SCALE);
				sum_r += (double(r) / mu2000::DAC_FULL_SCALE) * (double(r) / mu2000::DAC_FULL_SCALE);
			}
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
	// 新しいカードを作るとき（smartmedia::format）に書く論理の書式は、firmware の書式化と同じ
	{
		for (int i = 0; i < 50; i++)
			g.pump(100);   // 書式化の後片付けが残っていれば待つ
		smu2000::smartmedia fresh;
		const bool made = fresh.create(32) && fresh.format();
		const bool same = made && fresh.raw() == g.mu.card().raw();
		std::printf("%s 新しいカードの書式が firmware の書式化と同じ\n", same ? "合" : "NG");
		if (!same)
			bad++;
	}
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

	// カードの中身を本体を通さずに読む（サンプリングの窓の「カード」。card_fs.h・m2a.h）。
	// 書いた M2A の波形が、firmware の表のサンプル 1 と同じ長さ・同じ音で、そのまま試聴できる
	std::vector<u8> saved_m2a;
	{
		std::vector<smu2000::cardfs::entry> files;
		std::vector<u8> m2a;
		std::vector<smu2000::m2a::wave> waves;
		std::string err;
		bool listed = smu2000::cardfs::list(g.mu.card().raw(), files, err);
		bool found = false;
		for (const auto &e : files)
			found |= e.path == "ALL_SEQ.M2A";
		const bool read = found && smu2000::cardfs::read(g.mu.card().raw(), "ALL_SEQ.M2A", m2a, err);
		const bool parsed = read && smu2000::m2a::parse(m2a, waves, err);
		const auto list = g.mu.sampling_list();
		const bool same_len = parsed && waves.size() == 1 && !list.empty() &&
		                      waves[0].frames + 2 >= list[0].frames() && waves[0].frames <= list[0].frames();
		std::vector<s16> pcm = same_len ? smu2000::m2a::pcm(m2a, waves[0]) : std::vector<s16>();
		std::vector<double> x;
		for (size_t i = RATE / 10; i < pcm.size() && i < RATE / 2; i++)
			x.push_back(pcm[i] / 32768.0);
		const bool tone_ok = !x.empty() && tone(x, 440) > 10 * std::max(tone(x, 330), tone(x, 587));
		std::printf("%s カードを外から読む             %zu ファイル、波形 %zu 個、%u / %u サンプル %s\n",
		            listed && tone_ok ? "合" : "NG", files.size(), waves.size(), waves.empty() ? 0u : waves[0].frames,
		            list.empty() ? 0u : list[0].frames(), err.c_str());
		if (!(listed && tone_ok))
			bad++;
		saved_m2a = m2a;

		// 外の PCM の試聴（preview_pcm）。音源を通さずに鳴り、終われば止まる
		g.out.clear();
		const size_t n = pcm.size();
		g.mu.preview_pcm(std::move(pcm));
		g.collect = true;
		g.pump(200);
		g.collect = false;
		const bool playing = g.mu.preview_number() == -1;
		const double p440 = tone(g.out, 440), p330 = tone(g.out, 330);
		g.pump(u32(n * 1000 / RATE) + 100);
		const bool stopped = g.mu.preview_number() == 0;
		const bool prev_ok = n && playing && stopped && p440 > 0.01 && p440 > 10 * p330;
		std::printf("%s 外の PCM の試聴                440Hz %.4f / 330Hz %.5f\n", prev_ok ? "合" : "NG", p440, p330);
		if (!prev_ok)
			bad++;
	}

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
	// 読み戻すカードは、取り出した M2A を PC の側で新しいカードに書き直したもの（smartmedia::format に
	// ファイルを渡す。サンプリングの窓で M2A のファイルを直に読み込むときと同じ）。firmware がそれを読めるか
	smu2000::smartmedia made;
	{
		const u32 mb = smu2000::smartmedia::megabytes_for(saved_m2a.size());
		std::vector<smu2000::smartmedia::root_file> put(1);
		put[0].name = "FROMPC.M2A";
		put[0].bytes = saved_m2a;
		const bool ok_made = mb && made.create(mb) && made.format(put);
		std::vector<smu2000::cardfs::entry> files;
		std::vector<u8> back;
		std::string err;
		const bool same = ok_made && smu2000::cardfs::list(made.raw(), files, err) && files.size() == 1 &&
		                  smu2000::cardfs::read(made.raw(), "FROMPC.M2A", back, err) && back == saved_m2a;
		std::printf("%s M2A を新しいカードに書く         %u MB、%zu バイト %s\n", same ? "合" : "NG", mb, saved_m2a.size(), err.c_str());
		if (!same)
			bad++;

		// 16MB を超えるファイル（論理ブロックがゾーン 1 へ入る）と、2 つ目のファイルも書いて読み戻せる
		std::vector<smu2000::smartmedia::root_file> big(2);
		big[0].name = "BIG.M2A";
		big[0].bytes.resize(20u << 20);
		for (size_t i = 0; i < big[0].bytes.size(); i++)
			big[0].bytes[i] = u8((i * 2654435761u) >> 13);
		big[1].name = "SMALL.TXT";
		big[1].bytes.assign(100, u8('x'));
		smu2000::smartmedia large;
		const u32 mb2 = smu2000::smartmedia::megabytes_for(big[0].bytes.size() + big[1].bytes.size());
		std::vector<u8> b0, b1;
		const bool big_ok = mb2 == 32 && large.create(mb2) && large.format(big) &&
		                    smu2000::cardfs::read(large.raw(), "BIG.M2A", b0, err) && b0 == big[0].bytes &&
		                    smu2000::cardfs::read(large.raw(), "SMALL.TXT", b1, err) && b1 == big[1].bytes;
		std::printf("%s 20MB のファイルを 32MB のカードに  %s\n", big_ok ? "合" : "NG", err.c_str());
		if (!big_ok)
			bad++;
	}
	// 読み戻しは、サンプリングの窓の「この M2A を読み込む」と同じボタンの押し方（ui::panel_macro）で。
	// まず firmware が書いたカードから読み、次にカードを PC で書いたものへ差し替えて（card_swapped）読む。
	// firmware は前のカードの FAT を覚えているので、差し替えに気づかないと新しいカードのファイルが見つからない
	auto load_by_macro = [&](const char *name, const char *what) {
		ui::panel_macro macro;
		macro.start(ui::panel_macro::load_m2a(name), "done");
		std::string msg;
		bool finished = false;
		size_t last = ~size_t(0);
		for (int i = 0; i < 200 * 100 && !finished; i++) {
			h.pump(10);
			finished = macro.tick(h.mu, msg);
			if (g.verbose && macro.at() != last) {
				last = macro.at();
				std::printf("    段 %zu [%s]\n", last, h.lcd().c_str());
			}
		}
		const bool ok = finished && msg == "done";
		std::printf("%s %-28s [%s] %s\n", ok ? "合" : "NG", what, h.lcd().c_str(), msg.c_str());
		if (!ok) bad++;
	};
	h.mu.card() = g.mu.card();
	h.pump(1500);
	load_by_macro("ALL_SEQ.M2A", "ボタンのマクロで LOAD");
	h.mu.card() = made;
	h.mu.card_swapped();
	h.pump(1000);
	load_by_macro("FROMPC.M2A", "差し替えたカードから LOAD");
	// 窓の道（bridge::request_macro → driver::sampling_tick）は早送りする。10ms のブロックを回して、
	// 何ブロックで終わるか（= 実時間で鳴らしていたら何秒か）を見る。サンプルがあるので Overwrite ALL? も通る
	{
		ui::bridge br;
		ui::driver drv;
		br.request_macro(ui::panel_macro::load_m2a("FROMPC.M2A"), "done");
		int blocks = 0;
		ui::bridge::sampling_view view;
		for (; blocks < 3000; blocks++) {
			drv.pump_midi(h.mu, br);
			h.pump(10);
			br.get_sampling(view);
			if (!view.message.empty())
				break;
		}
		const bool ok = view.message == "done" && blocks < 300;
		std::printf("%s 窓の道の LOAD（早送り）        %d ブロック（実時間で %.2f 秒） %s\n", ok ? "合" : "NG", blocks,
		            blocks / 100.0, view.message.c_str());
		if (!ok) bad++;
	}
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
		v.el[0].assigned = true;
		v.el[0].sample = 1;
		v.name = "Direct";
		v.el[0].level = 127;
		v.el[0].pan = 7;
		const bool set = k.mu.sampling_set_voice(0, v, err);
		sp::voice back;
		k.mu.sampling_voice(0, back);
		check(set && back.el[0].assigned && back.el[0].sample == 1 && back.name == "Direct", "直の割り当て: 読み戻せる", back.name);
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

		// 内蔵ウェーブ。要素の波形の欄に内蔵の波形の組（0x4000 を立てない）を書くと、サンプルでなく
		// ROM の波形が鳴る（s45e_mid さんの見つけたこと。doc/sampling-ram.md）。PGM010 に組 16（Syn Drum など）、
		// 比べに組無しを書いて、鍵 60 の大きさを見る
		{
			auto rms_of = [&](int wave) {
				sp::voice w;
				w.name = "RomWave";
				w.el[0].rom_wave = wave;
				std::string e;
				const bool ok = k.mu.sampling_set_voice(9, w, e);
				sp::voice back;
				k.mu.sampling_voice(9, back);
				const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x09 };
				for (u8 b : sel)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.out.clear();
				k.collect = true;
				for (u8 b : on)
					k.mu.midi_in(b, 0);
				k.pump(500);
				k.collect = false;
				for (u8 b : off)
					k.mu.midi_in(b, 0);
				k.pump(500);
				double sum = 0;
				for (double v : k.out)
					sum += v * v;
				return std::make_pair(ok && !back.el[0].assigned && back.el[0].rom_wave == wave, std::sqrt(sum / std::max<size_t>(1, k.out.size())));
			};
			const auto with = rms_of(16), without = rms_of(-1);
			check(with.first && without.first && with.second > 0.003 && without.second < 0.0001,
			      "内蔵ウェーブを割り当てると鳴る",
			      "組 16 で rms " + std::to_string(with.second) + "、組無しで " + std::to_string(without.second));

			// サンプル音色を書く SysEx（機種 0x68）。PGM010 に組 16 と音程・エンベロープを書き、その記録を
			// SysEx にして PGM021 へ送ると、firmware が同じ記録を作る（要素の [0] は firmware が付ける印なので除く）
			sp::voice w;
			w.name = "SxCopy";
			w.el[0].rom_wave = 16;
			w.el[0].coarse = 7;
			w.el[0].attack = 40;
			w.el[0].release = 20;
			w.el[0].pan = 3;
			std::string e;
			k.mu.sampling_set_voice(9, w, e);
			const u32 v9 = sp::TAB_VOICE + sp::VOICE_SIZE * 9 - 0x1000000, v20 = sp::TAB_VOICE + sp::VOICE_SIZE * 20 - 0x1000000;
			const std::vector<u8> rec9(k.mu.dram().begin() + v9, k.mu.dram().begin() + v9 + sp::VOICE_SIZE);
			const auto msgs = sp::voice_sysex(20, rec9.data());
			size_t bytes = 0;
			for (const auto &m : msgs) {
				for (u8 b : m)
					k.mu.midi_in(b, 0);
				bytes += m.size();
				k.pump(5);
			}
			k.pump(300);
			int differ = 0;
			for (u32 i = 0; i < sp::VOICE_SIZE; i++) {
				const bool elem0 = i >= 12 && i < 12 + 4 * 84 && (i - 12) % 84 == 0;
				if (!elem0 && k.mu.dram()[v20 + i] != rec9[i]) {
					if (!differ)
						std::printf("    最初の違い +%x: %02x → %02x\n", i, rec9[i], k.mu.dram()[v20 + i]);
					differ++;
				}
			}
			sp::voice back;
			k.mu.sampling_voice(20, back);
			check(differ == 0 && back.name == "SxCopy" && back.el[0].rom_wave == 16 && back.el[0].coarse == 7,
			      "サンプル音色の SysEx で写す",
			      std::to_string(msgs.size()) + " 通 " + std::to_string(bytes) + " バイト、違う " + std::to_string(differ) + " バイト");

			// 要素を重ねる。要素 1 = 組 16 を左いっぱい、要素 2 = 組 39 を右いっぱいにして、右の大きさを見る
			// （右にもリバーブで左の音が少し回るので、差は 20dB ほど）
			// （実機でも 2 要素が鳴ることを確かめた。doc/sampling-ram.md）
			auto lr_of = [&](const sp::voice &mv, int key) {
				std::string err;
				k.mu.sampling_set_voice(11, mv, err);
				const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x0b };
				for (u8 b : sel)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.sum_l = k.sum_r = 0;
				k.collect = true;
				const u8 non[] = { 0x90, u8(key), 0x64 }, noff[] = { 0x80, u8(key), 0x40 };
				for (u8 b : non)
					k.mu.midi_in(b, 0);
				k.pump(400);
				k.collect = false;
				for (u8 b : noff)
					k.mu.midi_in(b, 0);
				k.pump(600);
				return std::make_pair(k.sum_l, k.sum_r);
			};
			sp::voice mv;
			mv.name = "Layer";
			mv.el[0].rom_wave = 16;
			mv.el[0].pan = 0;
			mv.el[1] = mv.el[0];
			mv.el[1].rom_wave = 39;
			mv.el[1].pan = 14;
			const auto both = lr_of(mv, 60);
			sp::voice mb;
			k.mu.sampling_voice(11, mb);
			mv.el[1].on = false;
			const auto one = lr_of(mv, 60);
			mv.el[1].on = true;
			mv.el[1].key_lo = 72;
			const auto split60 = lr_of(mv, 60), split72 = lr_of(mv, 72);
			auto db = [](double a, double b) { return 10 * std::log10((a + 1e-30) / (b + 1e-30)); };
			check(mb.el[0].on && mb.el[1].on && !mb.el[2].on && mb.el[1].rom_wave == 39 &&
			      both.second > 0 && db(both.second, one.second) > 15 && db(split72.second, split60.second) > 15,
			      "要素を重ねる・鍵で分ける",
			      "右: 2 要素 " + std::to_string(db(both.second, one.second)) + " dB 上、鍵で分けて 72 は 60 より " +
			      std::to_string(db(split72.second, split60.second)) + " dB 上");
		}

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
		v2.el[0].assigned = true;
		v2.el[0].sample = 3;
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

		// 拡大した部分（request_detail）。狭い範囲なら 1 サンプルに 1 つで、波形そのもの
		{
			br.request_detail(2, 1, 3000, 3100);
			drv.pump_midi(k.mu, br);
			br.get_sampling(view);
			const auto &d = view.details[2];
			const sp::sample s1 = k.mu.sampling_list()[0];
			bool same = d.number == 1 && d.from == 3000 && d.to == 3100 && d.lo.size() == 100;
			for (size_t i = 0; same && i < d.lo.size(); i++) {
				const size_t o = (size_t(s1.start) * 2 + 3000 + i) * 2;
				same = d.lo[i] == d.hi[i] && d.lo[i] == s16(k.mu.sample_ram()[o] | k.mu.sample_ram()[o + 1] << 8);
			}
			check(same, "窓の道: 拡大した部分の波形", std::to_string(d.lo.size()) + " 点");
			br.request_detail(2, 0, 0, 0);
		}

		// トリム。サンプル 1 の [1000, 1000 + 0.25 秒) だけを残す。後ろのサンプル 2・3 は前へ詰まり、空きが増える。
		// サンプル 3 を使う PGM002 は、この後の確認で 880Hz のまま鳴る
		{
			const auto l0 = k.mu.sampling_list();
			const u32 free0 = k.mu.sampling_free_frames();
			const u32 keep = RATE / 4;
			const bool ok = k.mu.sampling_trim(1, 1000, 1000 + keep, err);
			const auto l1 = k.mu.sampling_list();
			const u32 freed = l0[0].end - l1[0].end;
			check(ok && l1.size() == 3 && l1[0].frames() == (keep + 1) / 2 * 2 && l1[1].start == l1[0].end &&
			      l1[2].start == l1[1].end && l1[2].frames() == l0[2].frames() &&
			      k.mu.sampling_free_frames() == free0 + freed * 2,
			      "トリム: 残して後ろを詰める",
			      std::to_string(l0[0].frames()) + " → " + std::to_string(l1[0].frames()) + " フレーム、空き +" +
			      std::to_string(k.mu.sampling_free_frames() - free0));
		}
		// 前後の無音を除いた範囲と、範囲を決めた見取り図（拡大）。無音 2000・正弦 4410・無音 3000 のサンプルを足す
		{
			std::vector<s16> pad(2000 + 4410 + 3000, 0);
			for (int i = 0; i < 4410; i++)
				pad[size_t(2000 + i)] = s16(std::lround(10000 * std::sin(2 * PI * 440.0 * i / RATE + 0.3)));
			const int n4 = k.mu.sampling_add(pad.data(), pad.size(), "padded", err);
			u32 a = 0, b = 0;
			const bool found = n4 > 0 && k.mu.sampling_bounds(n4, 0.01, a, b);
			std::vector<s16> lo, hi;
			u32 fr = 0;
			k.mu.sampling_overview(n4, ui::bridge::WAVE_BUCKETS, lo, hi, fr, 2000, 2100);
			bool exact = lo.size() == 100;
			for (size_t i = 0; exact && i < lo.size(); i++)
				exact = lo[i] == hi[i] && lo[i] == pad[2000 + i];
			check(found && a >= 2000 && a < 2010 && b > 6400 && b <= 6410 && exact,
			      "無音を除いた範囲・拡大した見取り図",
			      std::to_string(a) + " - " + std::to_string(b) + "、見取り図 " + std::to_string(lo.size()) + " 点");
		}
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

		// 音程。PGM002（880Hz のサンプル）を半音 -12 にすると 440Hz、微調 +31 で 448Hz ほど
		{
			sp::voice v3;
			k.mu.sampling_voice(1, v3);
			v3.el[0].coarse = -12;
			v3.el[0].fine = 31;
			k.mu.sampling_set_voice(1, v3, err);
			sp::voice back3;
			k.mu.sampling_voice(1, back3);
			for (u8 b : pc2)
				k.mu.midi_in(b, 0);
			k.pump(300);
			k.out.clear();
			k.collect = true;
			for (u8 b : on)
				k.mu.midi_in(b, 0);
			k.pump(600);
			k.collect = false;
			for (u8 b : off)
				k.mu.midi_in(b, 0);
			k.pump(300);
			double best_f = 0, best = 0;
			for (double f = 400; f <= 500; f += 0.1)
				if (const double t = tone(k.out, f); t > best) {
					best = t;
					best_f = f;
				}
			check(back3.el[0].coarse == -12 && back3.el[0].fine == 31 && best_f > 446.0 && best_f < 450.0,
			      "音程: 半音 -12・微調 +31", std::to_string(best_f) + " Hz");
		}

		// 試聴。サンプル 3（880Hz）の頭 0.1 秒を、音源を通さずにそのまま鳴らす。終わったら止まる
		{
			k.out.clear();
			k.collect = true;
			const bool started = k.mu.preview_start(3, 0, RATE / 10);
			k.pump(80);
			const bool during = k.mu.preview_number() == 3;
			k.pump(100);
			k.collect = false;
			check(started && during && k.mu.preview_number() == 0 && tone(k.out, 880) > 0.05,
			      "試聴: 選んだ範囲を鳴らして止まる", std::to_string(tone(k.out, 880)));
		}

		// ループ。0.2 秒の 440Hz と 0.2 秒の 660Hz をつないだサンプルを PGM003 に。ループなしなら 0.4 秒で消え、
		// ループの頭を 660Hz の頭にすると、押しているあいだ 660Hz だけが続く。トリムしてもループの頭は同じ音の所
		{
			std::vector<s16> two(RATE * 2 / 5);
			for (size_t i = 0; i < two.size(); i++)
				two[i] = s16(std::lround(12000 * std::sin(2 * PI * (i < RATE / 5 ? 440.0 : 660.0) * double(i) / RATE)));
			const int n5 = k.mu.sampling_add(two.data(), two.size(), "looped", err);
			sp::voice v5;
			v5.el[0].assigned = true;
			v5.el[0].sample = n5;
			v5.name = "Looped";
			k.mu.sampling_set_voice(2, v5, err);
			const u8 pc3[] = { 0xc0, 0x02 };
			// 押して 1.2 秒。0.8 秒から後の 0.4 秒を見る
			auto hold = [&]() {
				for (u8 b : pc3)
					k.mu.midi_in(b, 0);
				k.pump(300);
				for (u8 b : on)
					k.mu.midi_in(b, 0);
				k.pump(800);
				k.out.clear();
				k.collect = true;
				k.pump(400);
				k.collect = false;
				for (u8 b : off)
					k.mu.midi_in(b, 0);
				k.pump(300);
			};
			hold();
			const double once = tone(k.out, 660);
			const u32 at = RATE / 5;
			k.mu.sampling_loop(n5, true, at);
			hold();
			const double l440 = tone(k.out, 440), l660 = tone(k.out, 660);
			sp::sample s5;
			for (const sp::sample &x : k.mu.sampling_list())
				if (x.number == n5)
					s5 = x;
			check(once < 0.002 && l660 > 0.01 && l660 > 10 * l440 && s5.loop && s5.loop_from == at,
			      "ループ: 頭から終わりをくり返す",
			      "なし 660 " + std::to_string(once) + "、あり 440 " + std::to_string(l440) + " 660 " + std::to_string(l660));
			// 頭の 1000 を切る。ループの頭は 1000 前へ
			const bool trimmed = k.mu.sampling_trim(n5, 1000, u32(two.size()), err);
			for (const sp::sample &x : k.mu.sampling_list())
				if (x.number == n5)
					s5 = x;
			hold();
			check(trimmed && s5.loop && s5.loop_from == at - 1000 && tone(k.out, 660) > 0.01 &&
			      tone(k.out, 660) > 10 * tone(k.out, 440),
			      "ループ: トリムの後もループの頭は同じ所", std::to_string(s5.loop_from));
			// 試聴もループの頭へ戻って続く
			k.mu.preview_start(n5, 0, s5.frames(), s5.loop_from);
			k.pump(600);
			const bool still = k.mu.preview_number() == n5;
			k.mu.preview_stop();
			check(still, "ループ: 試聴も止めるまで続く", std::to_string(k.mu.preview_pos()));

			// 鳴り始め・鳴り終わり（波形は切らない）。440Hz の所は [0, at - 1000)、660Hz はその後
			const u32 mid = at - 1000;
			k.mu.sampling_points(n5, 0, mid, true, 0);   // 440Hz だけをくり返す
			hold();
			const double p440 = tone(k.out, 440), p660 = tone(k.out, 660);
			k.mu.sampling_points(n5, mid, 0, false, 0);  // 660Hz から 1 度だけ
			for (u8 b : pc3)
				k.mu.midi_in(b, 0);
			k.pump(300);
			k.out.clear();
			k.collect = true;
			for (u8 b : on)
				k.mu.midi_in(b, 0);
			k.pump(150);
			k.collect = false;
			for (u8 b : off)
				k.mu.midi_in(b, 0);
			k.pump(300);
			const double q440 = tone(k.out, 440), q660 = tone(k.out, 660);
			sp::sample s6;
			for (const sp::sample &x : k.mu.sampling_list())
				if (x.number == n5)
					s6 = x;
			check(p440 > 0.01 && p440 > 10 * p660 && q660 > 0.01 && q660 > 10 * q440 && s6.play_from == mid &&
			      s6.play_to == s6.frames() && !s6.loop && s6.loop_from == mid,
			      "鳴り始め・鳴り終わり: 終点までループ、始点から鳴る",
			      "E まで 440 " + std::to_string(p440) + " 660 " + std::to_string(p660) + "、S から 440 " +
			      std::to_string(q440) + " 660 " + std::to_string(q660));
			k.mu.sampling_points(n5, 0, 0, true, at - 1000);

			// つなぎ目の道具。だんだん小さくなる 440Hz（1 周期 100.227 サンプル）で
			{
				std::vector<s16> dec(RATE / 2);
				for (size_t i = 0; i < dec.size(); i++)
					dec[i] = s16(std::lround(16000.0 * (1.0 - 0.8 * double(i) / double(dec.size())) *
					                         std::sin(2 * PI * 440.0 * double(i) / RATE)));
				const int n7 = k.mu.sampling_add(dec.data(), dec.size(), "decay", err);
				sp::sample s7;
				for (const sp::sample &x : k.mu.sampling_list())
					if (x.number == n7)
						s7 = x;
				auto raw = [&](u32 i) {
					const size_t o = (size_t(s7.start) * 2 + i) * 2;
					return int(s16(k.mu.sample_ram()[o] | k.mu.sample_ram()[o + 1] << 8));
				};
				// ゼロクロス: 1000 の近くで下から上へ横切る所（周期の 10 倍 1002.3 のあたり）。偶数に限ると偶数
				u32 z = 0, ze = 0;
				const bool zok = k.mu.sampling_snap(n7, 1000, false, 441, z) && k.mu.sampling_snap(n7, 1000, true, 441, ze);
				check(zok && raw(z - 1) < 0 && raw(z) >= 0 && z > 990 && z < 1010 && !(ze & 1) && ze + 2 >= z && ze <= z,
				      "つなぎ目: ゼロクロスに吸い付ける", std::to_string(z) + "・偶数 " + std::to_string(ze));
				// 終点をループに合わせる: L = 2000 から、E - L が周期の整数倍に近い所
				u32 e = 0;
				const bool mok = k.mu.sampling_match_end(n7, 2000, 9000, RATE / 20, e);
				const double periods = double(e - 2000) * 440.0 / RATE;
				check(mok && std::fabs(periods - std::round(periods)) < 0.02,
				      "つなぎ目: 終点をループに合わせる", std::to_string(e) + "（" + std::to_string(periods) + " 周期）");
				// クロスフェード: 終点の手前が、ループの頭の手前と同じ形になる
				std::vector<int> before_l;
				for (u32 i = 0; i < 16; i++)
					before_l.push_back(raw(6000 - 16 + i));
				const int mid_before = raw(18000 - 1000);
				const bool xok = k.mu.sampling_crossfade(n7, 6000, 18000, 2000);
				int worst = 0;
				for (u32 i = 0; i < 16; i++)
					worst = std::max(worst, std::abs(raw(18000 - 16 + i) - before_l[i]));
				check(xok && worst <= 300 && raw(18000 - 1000) != mid_before,
				      "つなぎ目: クロスフェード", "終点の手前とループの頭の手前の差 " + std::to_string(worst));
				// 等パワーの曲線も、終わりではループの頭の手前と同じ形
				std::vector<int> before_l2;
				for (u32 i = 0; i < 16; i++)
					before_l2.push_back(raw(4000 - 16 + i));
				const bool pok = k.mu.sampling_crossfade(n7, 4000, 12000, 3000, true);
				int worst2 = 0;
				for (u32 i = 0; i < 16; i++)
					worst2 = std::max(worst2, std::abs(raw(12000 - 16 + i) - before_l2[i]));
				check(pok && worst2 <= 300, "つなぎ目: 等パワーのクロスフェード", "差 " + std::to_string(worst2));
			}

			// ループ区間を探す。音程がゆっくり揺れる（±1% のビブラート 5Hz）220Hz の中で、0.3 秒以上の組。
			// 見つかった組は、つなぎ目のまわりの形の差が、適当に選んだ組（周期の整数倍の長さ）より小さい
			{
				std::vector<s16> vib(RATE * 2);
				double ph = 0;
				for (size_t i = 0; i < vib.size(); i++) {
					const double f = 220.0 * (1.0 + 0.01 * std::sin(2 * PI * 5.0 * double(i) / RATE));
					ph += 2 * PI * f / RATE;
					vib[i] = s16(std::lround(12000 * std::sin(ph)));
				}
				u32 l = 0, e = 0;
				const bool fok = sp::find_loop(vib, 0, u32(vib.size()), RATE * 3 / 10, l, e);
				auto mismatch = [&](u32 a, u32 b) {
					double d = 0, s = 0;
					for (int k2 = -256; k2 < 256; k2++) {
						const double x = vib[size_t(long(a) + k2)], y = vib[size_t(long(b) + k2)];
						d += (x - y) * (x - y);
						s += x * x + y * y;
					}
					return d / s;
				};
				// 比べる組: 頭 10000、長さは 220Hz の 80 周期（ビブラートで周期がずれる）
				const double found = fok ? mismatch(l, e) : 1.0, naive = mismatch(10000, 10000 + u32(80 * RATE / 220));
				check(fok && !(l & 1) && e - l >= RATE * 3 / 10 && found < naive * 0.2 && found < 0.01,
				      "ループ区間を探す",
				      std::to_string(l) + " - " + std::to_string(e) + "、差 " + std::to_string(found) + "（適当な組 " +
				      std::to_string(naive) + "）");
			}

			// エンベロープ（ループの入った PGM003 で）。押して 0.8 秒あとと離して 0.15 秒あとの大きさを、既定と比べる
			auto env = [&](double &held, double &after, double &first) {
				for (u8 b : pc3)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.out.clear();
				k.collect = true;
				for (u8 b : on)
					k.mu.midi_in(b, 0);
				k.pump(1000);
				for (u8 b : off)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.collect = false;
				k.pump(1500);
				auto rms = [&](u32 ms) {
					const size_t c = size_t(ms) * RATE / 1000;
					double s = 0;
					for (size_t i = c - RATE / 200; i < c + RATE / 200; i++)
						s += k.out[i] * k.out[i];
					return std::sqrt(s / double(RATE / 100));
				};
				first = rms(20);
				held = rms(800);
				after = rms(1150);
			};
			double h0, a0, f0, h1, a1, f1;
			env(h0, a0, f0);
			sp::voice e;
			k.mu.sampling_voice(2, e);
			e.el[0].attack = 24;    // ゆっくり立ち上がる（0.4 秒ほど）
			e.el[0].decay1 = 63;
			e.el[0].level1 = 96;    // すぐ -24dB ほどへ
			e.el[0].release = 16;   // 離しても長く残る
			k.mu.sampling_set_voice(2, e, err);
			sp::voice eb;
			k.mu.sampling_voice(2, eb);
			env(h1, a1, f1);
			const double db = 20 * std::log10(h1 / h0);
			check(eb.el[0].attack == 24 && eb.el[0].decay1 == 63 && eb.el[0].level1 == 96 && eb.el[0].release == 16 && f1 < 0.2 * f0 &&
			      db < -18 && db > -30 && a1 > 0.5 * h1 && a0 < 0.1 * h0,
			      "エンベロープ: アタック・レベル・リリース",
			      "頭 " + std::to_string(f1 / f0) + "、押している間 " + std::to_string(db) + " dB、離した後 " +
			      std::to_string(a1 / h1) + "（既定 " + std::to_string(a0 / h0) + "）");
		}

		// 波形を作る（wavegen.h）。ノコギリを作ってサンプルに足し、ループを入れて PGM013 に割り当てる。
		// 4214 サンプルに 25 周期なので、音程を直さなくても鍵 60 が C3（261.63Hz）、鍵 72 がその倍。
		// サインには 2 倍音が無く、ノコギリにはある
		{
			namespace wg = smu2000::wavegen;
			auto play = [&](const std::vector<s16> &pcm, const char *name, int key) {
				std::string err;
				const int n = k.mu.sampling_add(pcm.data(), pcm.size(), name, err);
				k.mu.sampling_loop(n, true, 0);
				sp::voice v;
				v.name = name;
				v.el[0].assigned = true;
				v.el[0].sample = n;
				k.mu.sampling_set_voice(12, v, err);
				const u8 sel[] = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x00, 0xc0, 0x0c };
				for (u8 b : sel)
					k.mu.midi_in(b, 0);
				k.pump(300);
				k.out.clear();
				k.collect = true;
				const u8 non[] = { 0x90, u8(key), 0x64 }, noff[] = { 0x80, u8(key), 0x40 };
				for (u8 b : non)
					k.mu.midi_in(b, 0);
				k.pump(700);       // ループの長さ（0.1 秒）より長く鳴らす
				k.collect = false;
				for (u8 b : noff)
					k.mu.midi_in(b, 0);
				k.pump(400);
				return n;
			};
			const std::vector<s16> saw = wg::render(wg::basic(wg::shape::saw)), sine = wg::render(wg::basic(wg::shape::sine));
			int peak = 0;
			for (s16 s : saw)
				peak = std::max(peak, std::abs(int(s)));
			const int n1 = play(saw, "saw", 60);
			const double c3 = tone(k.out, 261.63), b2 = tone(k.out, 246.94), cs3 = tone(k.out, 277.18), saw2 = tone(k.out, 523.25);
			play(sine, "sine", 60);
			const double sine1 = tone(k.out, 261.63), sine2 = tone(k.out, 523.25);
			play(saw, "saw2", 72);
			const double c4 = tone(k.out, 523.25), c3at72 = tone(k.out, 261.63);
			check(n1 > 0 && saw.size() == wg::LOOP_FRAMES && peak > 29000 && peak <= 32767 * 9 / 10 + 1 &&
			      c3 > 0.003 && c3 > 20 * b2 && c3 > 20 * cs3 && saw2 > 0.2 * c3 && sine1 > 0.003 && sine2 < 0.02 * sine1 &&
			      c4 > 20 * c3at72,
			      "作った波形: 鍵 60 が C3、ノコギリに倍音",
			      "ノコギリ 261.6Hz " + std::to_string(c3) + "（隣の鍵 " + std::to_string(std::max(b2, cs3)) + "）、2 倍音 " +
			      std::to_string(saw2 / c3) + " 倍、サインの 2 倍音 " + std::to_string(sine2 / sine1) + " 倍");
		}
	}

	std::printf("サンプリング: 食い違い %d\n", bad);
	return bad ? 1 : 0;
}
