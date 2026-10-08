// license:BSD-3-Clause
//
// 1 パートの架空のボード（FC ボード・オリジナルのボード。src/vboard.h、src/vboard_user.h）が借りているパートを、
// 音色の窓で見ているときの面。左の音色選びと、右の編集の面を、本体のもの（内蔵の音色の分類・VIB・FILTER・EG）から
// ボードのものに差し替える。
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
	// 1 コマに 1 度。差込口 3 つの様子を音声の糸に置いてもらう（答えは次のコマから読める）
	void poll(bridge &br)
	{
		br.post([info = m_info](mu2000 &mu) {
			slot now[mu2000::PLG_SLOTS];
			for (int i = 0; i < mu2000::PLG_SLOTS; i++)
				now[i] = { mu.virtual_board_kind(i), mu.virtual_board_part(i), mu.virtual_board_assigned(i) };
			std::lock_guard<std::mutex> g(info->lock);
			std::copy(std::begin(now), std::end(now), std::begin(info->s));
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
	static std::string program_name(int kind, int prog)
	{
		std::string name;
		if (kind == mu2000::VBOARD_USER) {
			const std::shared_ptr<const user_boards::board> b = user_boards::current();
			if (b && prog >= 0 && prog < user_boards::board::PROGRAMS && b->program[size_t(prog)])
				name = b->program[size_t(prog)]->name;
		} else {
			name = smu2000::vboard::fc_program_name(prog);
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
			for (int i = 0; i < programs(kind); i++) {
				const std::string name = program_name(kind, i);
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

	// 右の面: ボードの音色の中身
	void edit(int slot, int kind, int part, int prog, bridge &br)
	{
		(void)part;
		const float fs = ImGui::GetFontSize();
		char title[80];
		std::snprintf(title, sizeof(title), "PLG-%d  %s    %03d  %s", slot + 1, board_name(kind).c_str(), prog + 1, program_name(kind, prog).c_str());
		ImGui::SeparatorText(title);
		if (kind == mu2000::VBOARD_USER)
			edit_user(prog, br, fs);
		else
			edit_fc(prog, fs);
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", UI_TEXT(sb_note, "The board makes this part's sound. The row above still works on it: volume, expression, pan and the reverb and chorus sends. The MU's own filter, EG, vibrato and part EQ belong to the built-in voices and do not reach the board, so they are not shown here."));
		ImGui::PopStyleColor();
	}

private:
	enum { PARTS = 64 };
	struct slot { int kind = 0, part = 0; bool on = false; };
	struct info { std::mutex lock; slot s[mu2000::PLG_SLOTS]; };
	std::shared_ptr<info> m_info = std::make_shared<info>();
	int m_back[PARTS] = {};            // ボードへ切り替える前の内蔵の音色（MSB << 14 | LSB << 7 | プログラム）
	char m_name[9] = "";               // オリジナルのボードの名前の欄
	int m_name_for = -1;

	static int programs(int kind) { return kind == mu2000::VBOARD_USER ? user_boards::board::PROGRAMS : 16; }
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

	// ---- FC ボード: 音は決まっている。波の形と音量の動きを絵にする
	void edit_fc(int prog, float fs)
	{
		const int timbre = prog & 7;
		const bool decays = (prog & 8) != 0;
		const char *const about[8] = {
			UI_TEXT(sb_fc_t0, "Square wave, 1/2 duty: the hollow, round lead."),
			UI_TEXT(sb_fc_t1, "Square wave, 1/4 duty: brighter and thinner."),
			UI_TEXT(sb_fc_t2, "Square wave, 1/8 duty: the thinnest, nasal one."),
			UI_TEXT(sb_fc_t3, "Triangle wave in 16 steps. As on the real chip it has no volume: it sounds or it does not."),
			UI_TEXT(sb_fc_t4, "Noise with a long period: hiss, snares and explosions. The key sets how fast it runs."),
			UI_TEXT(sb_fc_t5, "Noise with a short period: metallic, buzzing."),
			UI_TEXT(sb_fc_t6, "Square wave whose duty changes every 1/30 second (1/8, 1/4, 1/2, 1/4)."),
			UI_TEXT(sb_fc_t7, "Square wave alternating with the octave above every 1/60 second: the fast arpeggio."),
		};
		ImGui::TextDisabled("%s", UI_TEXT(sb_wave, "Wave"));
		picture("fcwave", fs * 7.0f, [&](ImDrawList *dl, ImVec2 a, ImVec2 b) {
			const float w = b.x - a.x, h = b.y - a.y, mid = a.y + h * 0.5f, amp = h * 0.36f;
			dl->AddLine(ImVec2(a.x, mid), ImVec2(b.x, mid), axis_color());
			const int n = std::max(16, int(w));
			ImVec2 prev;
			u32 lfsr = 1;
			for (int i = 0; i <= n; i++) {
				const double t = double(i) / n;        // 絵の左端から右端
				double y;
				if (timbre == 4 || timbre == 5) {
					// 音源と同じ 15 ビットの帰還シフトレジスタ。96 歩ぶん
					static int at = -1;
					const int step = int(t * 96.0);
					if (i == 0)
						at = -1;
					for (; at < step; at++) {
						const u32 tap = timbre == 5 ? 6 : 1;
						const u32 fb = (lfsr ^ (lfsr >> tap)) & 1;
						lfsr = (lfsr >> 1) | (fb << 14);
					}
					y = (lfsr & 1) ? 1.0 : -1.0;
				} else {
					// 4 周期。6 と 7 は周期ごとに形が替わるところを見せる
					const double cyc = t * 4.0;
					const int k = std::min(3, int(cyc));
					double ph = cyc - std::floor(cyc);
					if (timbre == 3) {
						const int s = int(ph * 32.0) & 31;
						y = (s < 16 ? s : 31 - s) / 7.5 - 1.0;
					} else {
						static constexpr double DUTY[4] = { 0.5, 0.25, 0.125, 0.25 }, SWEEP[4] = { 0.125, 0.25, 0.5, 0.25 };
						double duty = timbre == 6 ? SWEEP[k] : timbre == 7 ? 0.5 : DUTY[timbre & 3];
						if (timbre == 7 && (k & 1))
							ph = std::fmod(ph * 2.0, 1.0);        // 1 オクターブ上
						y = ph < duty ? 1.0 : -1.0;
					}
				}
				const ImVec2 p(a.x + float(t) * w, mid - float(y) * amp);
				if (i) {
					dl->AddLine(prev, ImVec2(p.x, prev.y), line_color(), 1.5f);
					dl->AddLine(ImVec2(p.x, prev.y), p, line_color(), 1.5f);
				}
				prev = p;
			}
		});
		ImGui::TextWrapped("%s", about[timbre]);
		ImGui::Spacing();
		ImGui::TextDisabled("%s", UI_TEXT(sb_volume, "Volume (16 steps, one key held for 1 second, then released)"));
		picture("fcvol", fs * 5.0f, [&](ImDrawList *dl, ImVec2 a, ImVec2 b) {
			// 60 分の 1 秒ごとの段。1 秒押して離す。減衰する鳴り方は 4 コマごとに 1 段、離すと 1 コマに 2 段
			const float w = b.x - a.x, h = b.y - a.y, pad = h * 0.12f;
			const int frames = 75;
			int vol = 15;
			ImVec2 prev(a.x, b.y - pad - (h - pad * 2) * (timbre == 3 ? 1.0f : vol / 15.0f));
			for (int f = 1; f <= frames && vol >= 0; f++) {
				if (f > 60)
					vol = std::max(0, vol - 2);
				else if (decays && !(f & 3))
					vol = std::max(0, vol - 1);
				const float level = timbre == 3 ? (vol > 0 ? 1.0f : 0.0f) : vol / 15.0f;
				const ImVec2 p(a.x + w * f / frames, b.y - pad - (h - pad * 2) * level);
				dl->AddLine(prev, ImVec2(p.x, prev.y), line_color(), 1.5f);
				dl->AddLine(ImVec2(p.x, prev.y), p, line_color(), 1.5f);
				prev = p;
			}
			const float off_x = a.x + w * 60.0f / frames;
			dl->AddLine(ImVec2(off_x, a.y), ImVec2(off_x, b.y), axis_color());
		});
		ImGui::TextWrapped("%s", decays ? UI_TEXT(sb_fc_decay, "Programs 9-16 fade while the key is held, one step every 1/15 second.")
		                               : UI_TEXT(sb_fc_hold, "Programs 1-8 hold their volume while the key is held. Programs 9-16 are the same sounds, fading."));
		ImGui::Spacing();
		ImGui::TextWrapped("%s", UI_TEXT(sb_fc_fixed, "The FC board's sounds are fixed, there is nothing to edit. Velocity sets the volume, pitch bend moves it up to 2 semitones, and the modulation wheel adds vibrato. 8 notes at once."));
	}

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
