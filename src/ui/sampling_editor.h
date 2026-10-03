// license:BSD-3-Clause
//
// サンプリングの窓（doc/sampling.md の「窓から」）。パネルを通さず、firmware の表を直に読み書きする
// （src/sampling.h、doc/sampling-ram.md）。
//
//   入力     録音デバイス（gui）、録る入力（AD1 / AD2 / AD1+2）、引き金、レベルメーター
//   録音     名前・録音・止める・残りの時間。WAV ファイルから取り込むこともできる
//   サンプル firmware の表にあるサンプルの一覧
//   割り当て サンプル音色（Bank# 0/1 × PGM 1-128）に、サンプル・名前・音量・パンを書く
//
// 窓は音源に触らない。仕事は bridge::post で音を作る糸へ渡す（driver::sampling_tick）

#ifndef S_MU2000_UI_SAMPLING_EDITOR_H
#define S_MU2000_UI_SAMPLING_EDITOR_H

#pragma once

#include "xg_ui.h"

#include <string>
#include <vector>

namespace ui {

class sampling_editor : public imgui_view
{
public:
	const wchar_t *title() const override
	{
		return get_lang() == lang::ja ? L"S-MU2000 サンプリング" : L"S-MU2000 Sampling";
	}
	int default_width() const override  { return 760; }
	int default_height() const override { return 640; }
	void draw(xg::model &m, const xg_snapshot &ram, bridge &br) override;

private:
	void input_pane(bridge &br);
	void record_pane(bridge &br);
	void samples_pane();
	void assign_pane(bridge &br);
	void import_wav(const std::vector<u8> &bytes, bridge &br);

	bridge::sampling_view m_view;
	int m_source = 0;                  // smu2000::sampling::source
	int m_trigger_db = 0;              // 0 = 引き金なし、ほかは -60〜-6 dBFS
	char m_name[9] = {};               // 録るサンプルの名前（空なら firmware と同じ takeNNN）
	char m_path[512] = {};             // WAV の場所（ファイルの窓が無い所で）
	std::string m_note;                // 直前の結果
	u64 m_note_serial = 0;             // m_note を受け取った写しの番号

	// 割り当て
	int m_bank = 0, m_pgm = 1;
	int m_loaded_slot = -1;            // 編集欄に読み込んだ音色
	bool m_dirty = false;              // 編集欄を触った
	int m_sample = 0;                  // 0 = 無し
	char m_voice_name[9] = {};
	int m_level = 127, m_pan = 7;
};

} // namespace ui

#endif // S_MU2000_UI_SAMPLING_EDITOR_H
