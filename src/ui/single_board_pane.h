// license:BSD-3-Clause
//
// 1 パートの架空のボード（FC ボード・オリジナルのボード。src/vboard.h、src/vboard_user.h）が借りているパートを、
// 音色の窓で見ているときの面。左の音色選びと、右の編集の面を、本体のもの（内蔵の音色の分類・VIB・FILTER・EG）から
// ボードのものに差し替える。FC ボードの右の面は音色エディタ（fc_voice_editor.h）、オリジナルのボードは名前と包絡線。
//
// 実機のプラグインボードと同じで、そのパートのバンクがボードのバンク（MSB 90・91・92、LSB 0）のときだけ
// ボードが鳴り、ほかのバンクなら内蔵の音色が鳴る。だから差し替えの合図は「いまのバンク」:
//   ・内蔵の音色の面の上に「ボードで鳴らす」のボタン（ボードのバンクを選ぶ）
//   ・ボードの面の上に「内蔵の音色へ戻す」のボタン（切り替える前の音色に戻す）
// マルチパートのボード（口 E）は別の話で、board_view.h と overview の board_* が受け持つ

#ifndef S_MU2000_UI_SINGLE_BOARD_PANE_H
#define S_MU2000_UI_SINGLE_BOARD_PANE_H

#pragma once

#include "bridge.h"
#include "fc_voice_editor.h"
#include "texts.h"
#include "user_boards.h"
#include "xg_ui.h"
#include "../vboard.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace ui {

class single_board_pane
{
public:
	// 1 コマに 1 度。差込口（PLG-1〜6）の様子を音声の糸に置いてもらう（答えは次のコマから読める）。
	// FC ボードは、いま鳴らす音色（コントロールチェンジで触ってある値を重ねたもの）も。channel16 は、16 パートの
	// FC ボードのチャンネルを出しているときのその番号（0-15。出していなければ負）
	void poll(bridge &br, int channel16 = -1)
	{
		br.post([info = m_info, channel16](mu2000 &mu) {
			slot now[mu2000::PLG_SLOTS];
			mu2000::fc_live live[mu2000::PLG_SLOTS], live16;
			for (int i = 0; i < mu2000::PLG_SLOTS; i++) {
				now[i] = { mu.virtual_board_kind(i), mu.virtual_board_part(i), mu.virtual_board_assigned(i) };
				if (now[i].kind == mu2000::VBOARD_FC)
					live[i] = mu.fc_board_live(i);
			}
			if (channel16 >= 0)
				live16 = mu.fc_board_live(-1, channel16);
			std::lock_guard<std::mutex> g(info->lock);
			std::copy(std::begin(now), std::end(now), std::begin(info->s));
			std::copy(std::begin(live), std::end(live), std::begin(info->live));
			info->live16 = live16;
			info->live16_channel = channel16;
			return std::string();
		});
	}

	// このパートを借りている 1 パートのボードの差込口（0-2。無ければ -1）と、その種類
	int slot_of(int part, int &kind) const
	{
		std::lock_guard<std::mutex> g(m_info->lock);
		for (int i = 0; i < mu2000::PLG_SLOTS; i++) {
			const slot &s = m_info->s[i];
			if ((s.kind == mu2000::VBOARD_FC || s.kind == mu2000::VBOARD_USER) && s.on && s.part == part) {
				kind = s.kind;
				return i;
			}
		}
		kind = 0;
		return -1;
	}

	// いまのバンクがボードのものか（そのときボードが鳴り、内蔵の音は消えている）
	static bool playing(int slot, int msb, int lsb)
	{
		return slot >= 0 && msb == mu2000::board_bank_msb(slot) && lsb == mu2000::VBOARD_BANK_LSB;
	}

	static std::string board_name(int kind)
	{
		if (kind == mu2000::VBOARD_USER) {
			const std::shared_ptr<const user_boards::board> b = user_boards::current();
			return b ? b->name : "MY BOARD";
		}
		return "FC BOARD";
	}

	// プログラムの名前（後ろの空白は落とす）。オリジナルのボードで波形の入っていない番号は空
	static std::string program_name(int kind, int prog, int slot)
	{
		std::string name;
		if (kind == mu2000::VBOARD_USER) {
			const std::shared_ptr<const user_boards::board> b = user_boards::current();
			if (b && prog >= 0 && prog < user_boards::board::PROGRAMS && b->program[size_t(prog)])
				name = b->program[size_t(prog)]->name;
		} else {
			const std::shared_ptr<const fc_banks::bank> b = fc_banks::current(slot);
			name = b ? b->prog[size_t(prog & 127)].name : smu2000::vboard::fc_program_name(prog);
		}
		while (!name.empty() && name.back() == ' ')
			name.pop_back();
		return name;
	}

	// 内蔵の音色の面の上に置く 1 行: ボードのバンクへ切り替えるボタン
	void switch_row(int slot, int kind, int part, int msb, int lsb, int prog, xg::model &m, bridge &br)
	{
		char label[64];
		std::snprintf(label, sizeof(label), UI_TEXT(sb_use_board_fmt, "Play PLG-%d  %s"), slot + 1, board_name(kind).c_str());
		if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) {
			// 戻るときのために、いまの内蔵の音色を覚える
			if (part >= 0 && part < PARTS)
				m_back[part] = (msb << 14) | (lsb << 7) | prog;
			xgui::pick_voice(part, mu2000::board_bank_msb(slot), mu2000::VBOARD_BANK_LSB, first_program(kind), m, br);
		}
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(sb_use_board_tip, "Play this part with the plug-in board\nSelects the board's bank on this part. The built-in voice goes silent and the board sounds instead; the voice list and the editing area change to the board's."));
	}

	// 左の面: ボードの音色選び
	void voices(int slot, int kind, int part, int prog, xg::model &m, bridge &br)
	{
		xgui::audition_step(br);
		if (ImGui::Button(UI_TEXT(sb_back, "Back to built-in voices"), ImVec2(-FLT_MIN, 0))) {
			const int back = part >= 0 && part < PARTS ? m_back[part] : 0;
			xgui::pick_voice(part, (back >> 14) & 127, (back >> 7) & 127, back & 127, m, br);
		}
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(sb_back_tip, "Back to the MU's built-in voices\nSelects the voice this part had before the board (bank 0, program 1 if there was none). The board stays plugged in and goes silent."));
		char title[48];
		std::snprintf(title, sizeof(title), "PLG-%d  %s", slot + 1, board_name(kind).c_str());
		ImGui::SeparatorText(title);
		if (ImGui::BeginChild("sbvoices", ImVec2(0, 0))) {
			int shown = 0;
			for (int i = 0; i < programs(kind, slot); i++) {
				const std::string name = program_name(kind, i, slot);
				if (name.empty() && kind == mu2000::VBOARD_USER && !has_program(i))
					continue;
				shown++;
				char row[32];
				std::snprintf(row, sizeof(row), "%03d  %s", i + 1, name.c_str());
				ImGui::PushID(i);
				if (ImGui::Selectable(row, i == prog))
					xgui::pick_voice(part, mu2000::board_bank_msb(slot), mu2000::VBOARD_BANK_LSB, i, m, br);
				if (i == prog && ImGui::IsWindowAppearing())
					ImGui::SetScrollHereY();
				ImGui::PopID();
			}
			if (!shown)
				ImGui::TextWrapped("%s", UI_TEXT(sb_user_no_programs, "This board has no programs yet. Put waves on it in the Sampling window (Make a wave > Your own board)."));
		}
		ImGui::EndChild();
	}

	// FC ボードの音色エディタだけ（16 パートの FC ボードのチャンネルを出しているとき。音色の組は 16 パートのボードのもの）
	void edit_fc_voice(int prog, bridge &br, int channel)
	{
		mu2000::fc_live live;
		{
			std::lock_guard<std::mutex> g(m_info->lock);
			if (m_info->live16_channel == channel)
				live = m_info->live16;
		}
		m_fc.draw(prog, br, fc_banks::MULTI, &live, channel);
	}

	// 右の面: ボードの音色の中身
	void edit(int slot, int kind, int part, int prog, bridge &br)
	{
		(void)part;
		const float fs = ImGui::GetFontSize();
		if (kind == mu2000::VBOARD_USER) {
			char title[80];
			std::snprintf(title, sizeof(title), "PLG-%d  %s    %03d  %s", slot + 1, board_name(kind).c_str(), prog + 1, program_name(kind, prog, slot).c_str());
			ImGui::SeparatorText(title);
			edit_user(prog, br, fs);
		} else {
			// 番号と名前はエディタの 1 行目に出る。音色の組はこの差込口のボードのもの
			mu2000::fc_live live;
			{
				std::lock_guard<std::mutex> g(m_info->lock);
				live = m_info->live[std::clamp(slot, 0, mu2000::PLG_SLOTS - 1)];
			}
			m_fc.draw(prog, br, slot, &live);
		}
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", UI_TEXT(sb_note, "The board makes this part's sound. The row above still works on it: volume, expression, pan and the reverb and chorus sends. The MU's own filter, EG, vibrato and part EQ belong to the built-in voices and do not reach the board, so they are not shown here."));
		ImGui::PopStyleColor();
	}

private:
	enum { PARTS = 64 };
	struct slot { int kind = 0, part = 0; bool on = false; };
	struct info {
		std::mutex lock;
		slot s[mu2000::PLG_SLOTS];
		mu2000::fc_live live[mu2000::PLG_SLOTS], live16;       // FC ボードがいま鳴らす音色（差込口ごと、16 パートのボードの 1 チャンネル）
		int live16_channel = -1;
	};
	std::shared_ptr<info> m_info = std::make_shared<info>();
	int m_back[PARTS] = {};            // ボードへ切り替える前の内蔵の音色（MSB << 14 | LSB << 7 | プログラム）
	char m_name[9] = "";               // オリジナルのボードの名前の欄
	fc_voice_editor m_fc;              // FC ボードの音色エディタ
	int m_name_for = -1;

	// FC ボードは、初期の音色なら 16 個（17 以降はそのくり返し）、音色の組を開いていれば 128 個
	static int programs(int kind, int slot) { return kind == mu2000::VBOARD_USER ? user_boards::board::PROGRAMS : fc_banks::current(slot) ? 128 : 16; }
	static bool has_program(int prog)
	{
		const std::shared_ptr<const user_boards::board> b = user_boards::current();
		return b && prog >= 0 && prog < user_boards::board::PROGRAMS && b->program[size_t(prog)] != nullptr;
	}
	// 切り替えたときに選ぶ番号（オリジナルのボードは、波形の入っている最初の番号）
	static int first_program(int kind)
	{
		if (kind == mu2000::VBOARD_USER)
			for (int i = 0; i < user_boards::board::PROGRAMS; i++)
				if (has_program(i))
					return i;
		return 0;
	}

	// 絵の枠。中を描く関数に左上と右下を渡す
	template <typename F> static void picture(const char *id, float h, F &&draw)
	{
		const ImVec2 a = ImGui::GetCursorScreenPos();
		const ImVec2 size(std::max(8.0f, ImGui::GetContentRegionAvail().x), h);
		ImGui::InvisibleButton(id, size);
		const ImVec2 b(a.x + size.x, a.y + size.y);
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
		dl->PushClipRect(a, b, true);
		draw(dl, a, b);
		dl->PopClipRect();
	}
	static ImU32 line_color() { return IM_COL32(90, 170, 255, 255); }
	static ImU32 axis_color() { return IM_COL32(255, 255, 255, 40); }

	// ---- オリジナルのボード: 名前と包絡線はここで変えられる（波形はサンプリングの窓の「波形を作る」で入れる）
	void edit_user(int prog, bridge &br, float fs)
	{
		namespace ub = user_boards;
		const std::shared_ptr<const ub::board> cur = ub::current();
		const std::shared_ptr<const ub::program> p = cur && prog >= 0 && prog < ub::board::PROGRAMS ? cur->program[size_t(prog)] : nullptr;
		if (!p) {
			ImGui::TextWrapped("%s", UI_TEXT(sb_user_empty, "There is no wave on this program number, so it is silent. Put one on in the Sampling window (Make a wave > Your own board), or pick another program on the left."));
			return;
		}
		const auto set = [&](const auto &f) {
			auto np = std::make_shared<ub::program>(*p);
			f(*np);
			auto nb = std::make_shared<ub::board>(*cur);
			nb->program[size_t(prog)] = np;
			ub::commit(br, nb);
		};
		// 名前（8 文字。液晶に出る）
		if (m_name_for != prog && !ImGui::IsAnyItemActive()) {
			m_name_for = prog;
			std::snprintf(m_name, sizeof(m_name), "%s", p->name);
			for (size_t n = std::strlen(m_name); n && m_name[n - 1] == ' '; n--)
				m_name[n - 1] = 0;
		}
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(UI_TEXT(smp_name, "Name"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 7);
		if (ImGui::InputText("##sbname", m_name, sizeof(m_name)))
			set([&](ub::program &q) { smu2000::vboard::detail::clean_name(q.name, m_name, 8, true); });
		ImGui::SameLine(0, fs);
		const size_t frames = p->pcm ? p->pcm->size() : 0;
		ImGui::TextDisabled(p->loop ? UI_TEXT(smp_ub_loop_fmt, "loop %.2f s") : UI_TEXT(smp_ub_once_fmt, "once %.2f s"), double(frames) / 44100.0);

		ImGui::TextDisabled("%s", UI_TEXT(sb_wave, "Wave"));
		picture("ubwave", fs * 5.0f, [&](ImDrawList *dl, ImVec2 a, ImVec2 b) {
			const float w = b.x - a.x, h = b.y - a.y, mid = a.y + h * 0.5f, amp = h * 0.45f;
			dl->AddLine(ImVec2(a.x, mid), ImVec2(b.x, mid), axis_color());
			if (!frames)
				return;
			const std::vector<s16> &pcm = *p->pcm;
			const int cols = std::max(1, int(w));
			for (int x = 0; x < cols; x++) {
				const size_t i0 = size_t(u64(x) * frames / cols), i1 = std::max(i0 + 1, size_t(u64(x + 1) * frames / cols));
				int lo = 32767, hi = -32768;
				for (size_t i = i0; i < i1 && i < frames; i++) {
					lo = std::min(lo, int(pcm[i]));
					hi = std::max(hi, int(pcm[i]));
				}
				dl->AddLine(ImVec2(a.x + x, mid - hi / 32768.0f * amp), ImVec2(a.x + x, mid - lo / 32768.0f * amp + 1.0f), line_color());
			}
		});

		float at = p->attack, de = p->decay, su = p->sustain, re = p->release;
		ImGui::TextDisabled("%s", UI_TEXT(sb_env, "Envelope (one key held for 1 second, then released)"));
		picture("ubenv", fs * 5.0f, [&](ImDrawList *dl, ImVec2 a, ImVec2 b) {
			const float w = b.x - a.x, h = b.y - a.y, pad = h * 0.1f;
			const float hold = 1.0f, total = hold + std::max(0.3f, std::min(re, 4.0f)) * 1.1f;
			const auto level = [&](float t) {
				// 押している間: アタックで上がり、ディケイでサステインへ（decay 0 は下がらない）
				const auto held = [&](float u) {
					if (at > 0.0f && u < at)
						return u / at;
					if (de <= 0.0f)
						return 1.0f;
					const float k = std::min(1.0f, (u - at) / de);
					return 1.0f + (su - 1.0f) * k;
				};
				if (t <= hold)
					return held(t);
				const float from = held(hold);
				return re <= 0.0f ? 0.0f : from * std::max(0.0f, 1.0f - (t - hold) / re);
			};
			const int n = std::max(16, int(w / 2));
			ImVec2 prev;
			for (int i = 0; i <= n; i++) {
				const float t = total * i / n;
				const ImVec2 q(a.x + w * i / n, b.y - pad - (h - pad * 2) * std::clamp(level(t), 0.0f, 1.0f));
				if (i)
					dl->AddLine(prev, q, line_color(), 1.5f);
				prev = q;
			}
			const float off_x = a.x + w * hold / total;
			dl->AddLine(ImVec2(off_x, a.y), ImVec2(off_x, b.y), axis_color());
		});
		// 4 つのつまみを横に並べる
		if (ImGui::BeginTable("sbadsr", 4, ImGuiTableFlags_SizingStretchSame)) {
			const auto drag = [&](const char *label, const char *id, float &v, float speed, float lo, float hi, const char *fmt) {
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", label);
				ImGui::SetNextItemWidth(-FLT_MIN);
				return ImGui::DragFloat(id, &v, speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
			};
			if (drag(UI_TEXT(smp_ub_col_attack, "Attack"), "##sba", at, 0.005f, 0.0f, 3.0f, "%.3f s"))
				set([&](ub::program &q) { q.attack = at; });
			if (drag(UI_TEXT(smp_ub_col_decay, "Decay"), "##sbd", de, 0.02f, 0.0f, 20.0f, de <= 0.0f ? "-" : "%.2f s"))
				set([&](ub::program &q) { q.decay = de; });
			if (drag(UI_TEXT(smp_ub_col_sustain, "Sustain"), "##sbs", su, 0.005f, 0.0f, 1.0f, "%.2f"))
				set([&](ub::program &q) { q.sustain = su; });
			if (drag(UI_TEXT(smp_ub_col_release, "Release"), "##sbr", re, 0.01f, 0.0f, 10.0f, "%.2f s"))
				set([&](ub::program &q) { q.release = re; });
			ImGui::EndTable();
		}
		// つまみを離したら、ファイルへ書く
		if (ub::unsaved() && !ImGui::IsAnyItemActive())
			ub::save(br);
		ImGui::Spacing();
		ImGui::TextWrapped("%s", UI_TEXT(sb_user_note, "Attack is the rise, Decay the fall to the Sustain level while the key is held (\"-\" keeps the level), Release the fade after the key is let go. Changes are heard at once and saved to the board file. Waves are put on the board in the Sampling window (Make a wave > Your own board)."));
	}
};

} // namespace ui

#endif // S_MU2000_UI_SINGLE_BOARD_PANE_H
