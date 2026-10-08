// license:BSD-3-Clause
//
// FM ボードの音色の組（src/vboard_fm.h の fm_bank）の置き場と、いま開いている組。
// 組は設定のフォルダーの下の fmsets に、名前ごとに 1 ファイル（<名前>.smufm）で置く。変えるたびに書く。
// 何も開いていない間は、ボードは初期の 32 個で鳴る。音色エディタ（fm_voice_editor.h）が最初に何かを変えたとき、
// 初期の組の写しを「FM SET」として作る。音源（mu2000）へは bridge::post で渡す

#ifndef S_MU2000_UI_FM_BANKS_H
#define S_MU2000_UI_FM_BANKS_H

#pragma once

#include "bridge.h"
#include "../compat/paths.h"
#include "../vboard_fm.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ui::fm_banks {

using bank = smu2000::vboard::fm_bank;
using voice = smu2000::vboard::fm_voice;

inline constexpr const char *EXT = ".smufm";

struct state {
	std::mutex lock;
	std::shared_ptr<const bank> now;
	std::string path;
	bool unsaved = false;
};
inline state &st()
{
	static state s;
	return s;
}

// 置き場（無ければ作る）。作れなければ空
inline std::string dir()
{
	const std::string base = smu2000::ensure_config_dir();
	if (base.empty())
		return {};
	const std::string d = smu2000::join(base, "fmsets");
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
		stem = "fmset";
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

// いま開いている組（無ければ nullptr = 初期の音色）
inline std::shared_ptr<const bank> current()
{
	std::lock_guard<std::mutex> g(st().lock);
	return st().now;
}
inline std::string current_path()
{
	std::lock_guard<std::mutex> g(st().lock);
	return st().path;
}
inline std::string current_stem()
{
	const std::string p = current_path();
	const size_t slash = p.find_last_of("/\\"), n = std::char_traits<char>::length(EXT);
	const std::string name = p.substr(slash == std::string::npos ? 0 : slash + 1);
	return name.size() > n ? name.substr(0, name.size() - n) : std::string();
}

// 起動のとき: 設定に覚えてあった組を読んでおく（音源へは呼んだ側が渡す）
inline std::shared_ptr<const bank> adopt(const std::string &path, std::string &err)
{
	std::shared_ptr<const bank> b = smu2000::vboard::load_fm_bank(path, err);
	if (b) {
		std::lock_guard<std::mutex> g(st().lock);
		st().now = b;
		st().path = path;
	}
	return b;
}

inline void to_engine(bridge &br, std::shared_ptr<const bank> b, const std::string &path)
{
	br.post([b, path](mu2000 &mu) {
		mu.set_fm_bank(b, path);
		return std::string();
	});
}

// 変えた組を音源へ渡す。ファイルへはまだ書かない（つまみを引いている間は書かず、save で 1 度に書く）
inline void commit(bridge &br, std::shared_ptr<const bank> b)
{
	std::string path;
	{
		std::lock_guard<std::mutex> g(st().lock);
		st().now = b;
		st().unsaved = true;
		if (st().path.empty())
			st().path = file_for(b->name);
		path = st().path;
	}
	to_engine(br, std::move(b), path);
}

// まだ書いていない変更をファイルへ。名前を変えていたら、ファイルの名前も変える（古いほうは消す）
inline bool save(bridge &br)
{
	std::shared_ptr<const bank> b;
	std::string old;
	{
		std::lock_guard<std::mutex> g(st().lock);
		if (!st().unsaved || !st().now)
			return true;
		st().unsaved = false;
		b = st().now;
		old = st().path;
	}
	const std::string path = file_for(b->name);
	if (path.empty() || !smu2000::vboard::save_fm_bank(path, *b))
		return false;
	if (path != old) {
		if (!old.empty())
			std::remove(old.c_str());
		{
			std::lock_guard<std::mutex> g(st().lock);
			st().path = path;
		}
		to_engine(br, b, path);
	}
	return true;
}
inline bool unsaved()
{
	std::lock_guard<std::mutex> g(st().lock);
	return st().unsaved;
}

// 置き場の組を開く
inline bool open(bridge &br, const std::string &stem, std::string &err)
{
	save(br);
	const std::string d = dir();
	if (d.empty()) {
		err = "no settings folder";
		return false;
	}
	const std::string path = smu2000::join(d, stem + EXT);
	std::shared_ptr<const bank> b = adopt(path, err);
	if (!b)
		return false;
	to_engine(br, std::move(b), path);
	return true;
}

// 初期の音色の写しで、まだ使われていない名前の組を作る（置き場には書かない）。いま開いている組は閉じた扱いにする
inline std::shared_ptr<bank> fresh()
{
	auto b = std::make_shared<bank>();
	for (int n = 1; n < 100; n++) {
		std::snprintf(b->name, sizeof(b->name), n == 1 ? "FM SET" : "FM SET %d", n);
		const std::string p = file_for(b->name);
		if (p.empty() || !smu2000::is_file(p))
			break;
	}
	std::lock_guard<std::mutex> g(st().lock);
	st().path.clear();
	return b;
}

// 新しい組（初期の音色の写し）。同じ名前のファイルがあれば、番号を足す
inline void create(bridge &br)
{
	save(br);
	commit(br, fresh());
	save(br);
}

// 初期の音色に戻す（組を閉じる。ファイルは残る）
inline void close(bridge &br)
{
	save(br);
	{
		std::lock_guard<std::mutex> g(st().lock);
		st().now.reset();
		st().path.clear();
		st().unsaved = false;
	}
	to_engine(br, nullptr, std::string());
}

} // namespace ui::fm_banks

#endif // S_MU2000_UI_FM_BANKS_H
