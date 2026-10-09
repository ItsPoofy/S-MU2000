// license:BSD-3-Clause
//
// FC ボードの音色の組（src/vboard.h の fc_bank）の置き場と、いま開いている組。
// **ボード 1 枚ごとに別の組を持つ**（実機のボードが 1 枚ずつ自分の音色を持つのと同じ）。持ち主（target）は 4 つ:
// 差込口 PLG-1・2・3 の 1 パートの FC ボード（0-2）と、16 パートの FC ボード（3 = MULTI）。
// 組は設定のフォルダーの下の fcsets に、名前ごとに 1 ファイル（<名前>.smufc）で置く。変えるたびに書く。
// 何も開いていない間は、そのボードは初期の 16 個で鳴る。音色エディタ（fc_voice_editor.h）が最初に何かを変えたとき、
// 初期の組の写しを「FC SET」として作る（名前が使われていれば番号を足す）。
// 同じファイルを 2 枚のボードで開いてもよい。そのときは、片方で変えるともう片方にもすぐ届く。
// 音源（mu2000）へは bridge::post で渡す

#ifndef S_MU2000_UI_FC_BANKS_H
#define S_MU2000_UI_FC_BANKS_H

#pragma once

#include "bridge.h"
#include "../compat/paths.h"
#include "../vboard.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui::fc_banks {

using bank = smu2000::vboard::fc_bank;
using voice = smu2000::vboard::fc_voice;

inline constexpr const char *EXT = ".smufc";
inline constexpr int TARGETS = mu2000::FC_BANKS;       // PLG-1・2・3 と 16 パートのボード
inline constexpr int MULTI = mu2000::FC_BANK_MULTI;

struct state {
	std::mutex lock;
	struct one {
		std::shared_ptr<const bank> now;
		std::string path;
		bool unsaved = false;
	} t[TARGETS];
};
inline state &st()
{
	static state s;
	return s;
}
inline int clamp_target(int target) { return std::clamp(target, 0, TARGETS - 1); }

// 置き場（無ければ作る）。作れなければ空
inline std::string dir()
{
	const std::string base = smu2000::ensure_config_dir();
	if (base.empty())
		return {};
	const std::string d = smu2000::join(base, "fcsets");
	return smu2000::ensure_dir(d) ? d : std::string();
}

// 組の名前から、ファイルの名前に使える字だけを残す
inline std::string file_for(const char *name)
{
	std::string stem;
	for (const char *p = name; *p; p++)
		stem += (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '-' || *p == '_' ? *p : '_';
	while (!stem.empty() && stem.back() == '_')
		stem.pop_back();
	if (stem.empty())
		stem = "fcset";
	const std::string d = dir();
	return d.empty() ? std::string() : smu2000::join(d, stem + EXT);
}

// 置き場にある組（ファイルの名前から拡張子を取ったもの）
inline std::vector<std::string> list()
{
	std::vector<std::string> out;
	const size_t n = std::char_traits<char>::length(EXT);
	for (const smu2000::dir_entry &e : smu2000::list_dir(dir()))
		if (e.name.size() > n && e.name.compare(e.name.size() - n, n, EXT) == 0)
			out.push_back(e.name.substr(0, e.name.size() - n));
	std::sort(out.begin(), out.end());
	return out;
}

// そのボードがいま開いている組（無ければ nullptr = 初期の音色）
inline std::shared_ptr<const bank> current(int target)
{
	std::lock_guard<std::mutex> g(st().lock);
	return st().t[clamp_target(target)].now;
}
inline std::string current_path(int target)
{
	std::lock_guard<std::mutex> g(st().lock);
	return st().t[clamp_target(target)].path;
}
inline std::string current_stem(int target)
{
	const std::string p = current_path(target);
	const size_t slash = p.find_last_of("/\\"), n = std::char_traits<char>::length(EXT);
	const std::string name = p.substr(slash == std::string::npos ? 0 : slash + 1);
	return name.size() > n ? name.substr(0, name.size() - n) : std::string();
}

// 起動のとき: 設定に覚えてあった組を読んでおく（音源へは呼んだ側が渡す）。
// ほかのボードがもう同じファイルを開いていれば、同じものを使う
inline std::shared_ptr<const bank> adopt(int target, const std::string &path, std::string &err)
{
	target = clamp_target(target);
	std::shared_ptr<const bank> b;
	{
		std::lock_guard<std::mutex> g(st().lock);
		for (const state::one &o : st().t)
			if (o.now && o.path == path)
				b = o.now;
	}
	if (!b)
		b = smu2000::vboard::load_fc_bank(path, err);
	if (b) {
		std::lock_guard<std::mutex> g(st().lock);
		st().t[target].now = b;
		st().t[target].path = path;
		st().t[target].unsaved = false;
	}
	return b;
}

// ---- 音色の値を動かすコントロールチェンジの番号（src/vboard.h の fc_cc_map）。FC ボード全部に共通で、設定に覚える（board_fc_cc=）

inline smu2000::vboard::fc_cc_map &cc_map()
{
	static smu2000::vboard::fc_cc_map m;
	return m;
}
// 画面の控えを決める（起動のとき。音源へは呼ぶ側が渡す）
inline void adopt_cc(const smu2000::vboard::fc_cc_map &m) { cc_map() = m; }
inline void cc_to_engine(bridge &br)
{
	br.post([m = cc_map()](mu2000 &mu) {
		mu.set_fc_cc_map(m);
		return std::string();
	});
}
// 欄に番号を付ける（0 = 外す）。使えない番号なら何もしない
inline bool assign_cc(bridge &br, int param, int number)
{
	if (!cc_map().assign(param, number))
		return false;
	cc_to_engine(br);
	return true;
}
inline void reset_cc(bridge &br)
{
	cc_map() = smu2000::vboard::fc_cc_map();
	cc_to_engine(br);
}

inline void to_engine(bridge &br, int target, std::shared_ptr<const bank> b, const std::string &path)
{
	br.post([target, b, path](mu2000 &mu) {
		mu.set_fc_bank(b, path, target);
		return std::string();
	});
}

// 変えた組を音源へ渡す。ファイルへはまだ書かない（つまみを引いている間は書かず、save で 1 度に書く）。
// 同じファイルを開いているほかのボードにも、同じものを渡す
inline void commit(bridge &br, int target, std::shared_ptr<const bank> b)
{
	target = clamp_target(target);
	std::string path;
	bool also[TARGETS] = {};
	{
		std::lock_guard<std::mutex> g(st().lock);
		state::one &me = st().t[target];
		if (me.path.empty())
			me.path = file_for(b->name);
		path = me.path;
		me.now = b;
		me.unsaved = true;
		for (int i = 0; i < TARGETS; i++)
			if (i != target && !path.empty() && st().t[i].now && st().t[i].path == path) {
				st().t[i].now = b;
				also[i] = true;
			}
	}
	to_engine(br, target, b, path);
	for (int i = 0; i < TARGETS; i++)
		if (also[i])
			to_engine(br, i, b, path);
}

// まだ書いていない変更をファイルへ。名前を変えていたら、ファイルの名前も変える（古いほうは消す。
// 同じファイルを開いていたほかのボードも、新しい名前についてくる）
inline bool save(bridge &br, int target)
{
	target = clamp_target(target);
	std::shared_ptr<const bank> b;
	std::string old;
	{
		std::lock_guard<std::mutex> g(st().lock);
		state::one &me = st().t[target];
		if (!me.unsaved || !me.now)
			return true;
		me.unsaved = false;
		b = me.now;
		old = me.path;
	}
	const std::string path = file_for(b->name);
	if (path.empty() || !smu2000::vboard::save_fc_bank(path, *b))
		return false;
	if (path != old) {
		if (!old.empty())
			std::remove(old.c_str());
		bool moved[TARGETS] = {};
		{
			std::lock_guard<std::mutex> g(st().lock);
			for (int i = 0; i < TARGETS; i++)
				if (i == target || (!old.empty() && st().t[i].path == old)) {
					st().t[i].path = path;
					st().t[i].now = b;
					moved[i] = true;
				}
		}
		for (int i = 0; i < TARGETS; i++)
			if (moved[i])
				to_engine(br, i, b, path);
	}
	return true;
}
inline void save_all(bridge &br)
{
	for (int i = 0; i < TARGETS; i++)
		save(br, i);
}
inline bool unsaved(int target)
{
	std::lock_guard<std::mutex> g(st().lock);
	return st().t[clamp_target(target)].unsaved;
}

// 置き場の組を、そのボードで開く
inline bool open(bridge &br, int target, const std::string &stem, std::string &err)
{
	save(br, target);
	const std::string d = dir();
	if (d.empty()) {
		err = "no settings folder";
		return false;
	}
	const std::string path = smu2000::join(d, stem + EXT);
	std::shared_ptr<const bank> b = adopt(target, path, err);
	if (!b)
		return false;
	to_engine(br, target, std::move(b), path);
	return true;
}

// 初期の音色の写しで、まだ使われていない名前の組を作る（置き場には書かない）。そのボードが開いていた組は閉じた扱いにする。
// ほかのボードが開いている（まだ書いていない）組の名前も避ける
inline std::shared_ptr<bank> fresh(int target)
{
	target = clamp_target(target);
	auto b = std::make_shared<bank>();
	std::lock_guard<std::mutex> g(st().lock);
	for (int n = 1; n < 100; n++) {
		std::snprintf(b->name, sizeof(b->name), n == 1 ? "FC SET" : "FC SET %d", n);
		const std::string p = file_for(b->name);
		bool taken = !p.empty() && smu2000::is_file(p);
		for (int i = 0; i < TARGETS && !taken; i++)
			taken = i != target && !p.empty() && st().t[i].path == p;
		if (!taken)
			break;
	}
	st().t[target].path.clear();
	return b;
}

// 新しい組（初期の音色の写し）。同じ名前のファイルがあれば、番号を足す
inline void create(bridge &br, int target)
{
	save(br, target);
	commit(br, target, fresh(target));
	save(br, target);
}

// 初期の音色に戻す（そのボードで組を閉じる。ファイルは残る）
inline void close(bridge &br, int target)
{
	target = clamp_target(target);
	save(br, target);
	{
		std::lock_guard<std::mutex> g(st().lock);
		st().t[target] = state::one();
	}
	to_engine(br, target, nullptr, std::string());
}

} // namespace ui::fc_banks

#endif // S_MU2000_UI_FC_BANKS_H
