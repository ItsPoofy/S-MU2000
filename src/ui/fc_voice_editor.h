// license:BSD-3-Clause
//
// FC ボードの音色エディタ（音色の窓で、FC ボードが鳴らしているパートやチャンネルを出しているときの右の面）。
// いま選んでいるプログラムの音色（src/vboard.h の fc_voice）を触る。音色の組はボード 1 枚ごと（fc_banks.h）:
//   ・波（矩形波・三角波・ノイズ 2 種）と、矩形波のデューティの並び
//   ・音量の下がり方（押している間・離してから）
//   ・音程の動き（アルペジオ、鳴り始めのずれ、自動のビブラート）
//   ・上に絵が 3 つ: 波の形、音量の動き、音程の動き
// 値は全部「コマ」（60 分の 1 秒）と「段」（音量の 16 段）。当時のゲームの音作りと同じ刻み。
// 触った値はすぐ音源へ渡る（鳴っている音にも効く）。組（128 個の音色）は fc_banks.h が設定のフォルダーに保存する。
// 何も開いていない状態で触ると、初期の音色の写しから新しい組を作る。
//
// つまみの見出しには、その値を動かすコントロールチェンジの番号が出る。番号は「CC の割り当て」で付け替えられる
// （fc_banks.h の cc_map。FC ボード全部に共通で、設定に覚える）。
// シーケンサーがコントロールチェンジで動かしている値は、つまみにその値を出す（見出しの番号に色）。
// その値は曲のもので、組には入っていない。ここでその欄を動かすと、動かした欄だけを
// 組に書き、その欄をコントロールチェンジから引き取る（音源の側の値を捨てる）。ほかの欄は組の値のまま

#ifndef S_MU2000_UI_FC_VOICE_EDITOR_H
#define S_MU2000_UI_FC_VOICE_EDITOR_H

#pragma once

#include "fc_banks.h"
#include "xg_ui.h"
#include "ui/texts.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace ui {

class fc_voice_editor
{
public:
	// program はいま選んでいるプログラム（0-127）。target はどのボードの組か（0-2 = PLG-1・2・3、fc_banks::MULTI = 16 パートのボード）。
	// live は、そのボードがいま鳴らす音色（mu2000::fc_board_live。無ければ組の音色だけを出す）。channel は 16 パートのボードのチャンネル
	void draw(int program, bridge &br, int target, const mu2000::fc_live *live = nullptr, int channel = 0)
	{
		namespace fb = fc_banks;
		namespace vb = smu2000::vboard;
		const float fs = ImGui::GetFontSize();
		const ImGuiStyle &st = ImGui::GetStyle();
		program &= 127;

		const double now = ImGui::GetTime();
		if (m_listed < 0 || now - m_listed > 1.0) {
			m_listed = now;
			m_list = fb::list();
		}
		std::shared_ptr<const fb::bank> cur = fb::current(target);
		fb::voice v = cur ? cur->prog[size_t(program)] : vb::fc_default_voice(program);
		bool changed = false;
		// コントロールチェンジで動いている欄（bit = vb::fc_param）には、その値を出す。ここで動かして引き取ったばかりの欄は、
		// 音源からの答えが 1 コマ遅れて来るので、少しの間は見ない
		const fb::voice base = v;
		if (m_dropped_frames > 0 && --m_dropped_frames == 0)
			m_dropped = 0;
		u32 cc = 0;
		if (live && live->program == program) {
			cc = live->edited & ~m_dropped;
			for (int i = 0; i < vb::FC_PARAMS; i++)
				if (cc & (1u << i))
					vb::fc_set_param(v, i, vb::fc_get_param(live->voice, i));
		}
		const fb::voice shown = v;
		const vb::fc_cc_map &ccs = fb::cc_map();
		// 欄の番号の札（「CC27」。割り当てが無ければ出さない）。コントロールチェンジで動いている欄は色を付ける
		const auto cc_tag = [&](int param, bool same_line) {
			if (!ccs.cc[param])
				return;
			if (same_line)
				ImGui::SameLine();
			if (cc & (1u << param))
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cc_color()), "CC%d", ccs.cc[param]);
			else
				ImGui::TextDisabled("CC%d", ccs.cc[param]);
		};
		bool whole = false;                 // 音色を丸ごと入れ替えた（初期に戻す・貼る）
		u32 drop = 0;                       // 音源の側で捨ててもらう欄

		// ---- 組（128 個の音色のまとまり）
		{
			const std::string stem = fb::current_stem(target);
			ImGui::SetNextItemWidth(fs * 9);
			if (ImGui::BeginCombo("##fcset", cur ? cur->name : UI_TEXT(fme_default_set, "(initial voices)"))) {
				if (ImGui::Selectable(UI_TEXT(fme_default_set, "(initial voices)"), !cur) && cur) {
					fb::close(br, target);
					m_note.clear();
				}
				for (const std::string &s : m_list)
					if (ImGui::Selectable(s.c_str(), s == stem) && s != stem) {
						std::string err;
						m_note = fb::open(br, target, s, err) ? std::string() : err;
					}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered())
				xgui::hint("%s", UI_TEXT(fce_set_tip, "Voice set\nThe 128 voices of the FC board, kept as one file in the \"fcsets\" folder of the settings folder. Each FC board has its own: PLG-1, PLG-2, PLG-3 and the 16-part board can each open a different set, or the same one. \"(initial voices)\" is the built-in set of 16; touching anything there makes a copy called FC SET for this board, and its voice list grows to 128."));
			ImGui::SameLine();
			if (ImGui::SmallButton(UI_TEXT(fme_new_set, "New set"))) {
				fb::create(br, target);
				m_listed = -1;
				m_note.clear();
			}
			cur = fb::current(target);
			if (cur) {
				// 組の名前。変えるとファイルの名前も変わる
				const std::string path = fb::current_path(target);
				if ((m_name_for != path || m_name_target != target) && !ImGui::IsAnyItemActive()) {
					m_name_target = target;
					m_name_for = path;
					std::snprintf(m_set_name, sizeof(m_set_name), "%s", cur->name);
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(fs * 8);
				ImGui::InputText("##fcsetname", m_set_name, sizeof(m_set_name));
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					char clean[15];
					vb::fc_detail::clean_text(clean, reinterpret_cast<const u8 *>(m_set_name), 14, false);
					for (size_t n = std::strlen(clean); n && clean[n - 1] == ' '; n--)
						clean[n - 1] = 0;
					const std::string to = fb::file_for(clean);
					if (!clean[0] || (to != path && smu2000::is_file(to))) {
						m_note = UI_TEXT(fme_name_taken, "That name is empty or already used by another set");
					} else {
						m_note.clear();
						auto nb = std::make_shared<fb::bank>(*cur);
						std::snprintf(nb->name, sizeof(nb->name), "%s", clean);
						fb::commit(br, target, nb);
						cur = nb;
					}
					m_name_for.clear();
				}
			}
		}

		// ---- この音色: 番号、名前、初期に戻す、写す・貼る
		ImGui::SameLine(0, fs * 1.5f);
		ImGui::AlignTextToFramePadding();
		ImGui::Text(UI_TEXT(fme_program_fmt, "Program %d"), program + 1);
		ImGui::SameLine();
		{
			char name[9];
			std::snprintf(name, sizeof(name), "%s", v.name);
			for (size_t n = std::strlen(name); n && name[n - 1] == ' '; n--)
				name[n - 1] = 0;
			ImGui::SetNextItemWidth(fs * 6);
			if (ImGui::InputText("##fcname", name, sizeof(name))) {
				vb::fc_detail::clean_text(v.name, reinterpret_cast<const u8 *>(name), 8, true);
				changed = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(fme_reset_voice, "Initial voice"))) {
			v = vb::fc_default_voice(program);
			changed = whole = true;
		}
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(fme_reset_tip, "Initial voice\nPuts the built-in voice of this program number back."));
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(fme_copy, "Copy"))) {
			clip() = v;
			has_clip() = true;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!has_clip());
		if (ImGui::SmallButton(UI_TEXT(fme_paste, "Paste"))) {
			v = clip();
			changed = whole = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(fce_cc_assign, "CC assign...")))
			ImGui::OpenPopup("fccc");
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(fce_cc_assign_tip, "Control change numbers\nEvery value of this editor can be moved from a sequencer with a control change; the number is shown beside each control. Here you choose which number moves which value. The numbers are shared by all FC boards and remembered in the settings."));
		assign_popup(br);

		// コントロールチェンジで動いている欄があれば、その番号を 1 行で
		if (cc) {
			std::string list;
			for (int i = 0; i < vb::FC_PARAMS; i++)
				if (cc & (1u << i))
					list += (list.empty() ? "CC" : " CC") + std::to_string(ccs.cc[i]);
			ImGui::PushStyleColor(ImGuiCol_Text, cc_color());
			ImGui::TextWrapped(UI_TEXT(fce_cc_fmt, "Moved by control change: %s"), list.c_str());
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
				xgui::hint("%s", UI_TEXT(fce_cc_tip, "Values moved by control change\nThe highlighted values are coming from the sequencer as control changes. They belong to the song, not to the voice set: a program change puts the set's values back. Moving one of them here takes it over from the control change and writes it into the set."));
			ImGui::SameLine();
			if (ImGui::SmallButton(UI_TEXT(fce_cc_drop, "Back to the set"))) {
				drop = cc;
				v = base;
			}
			if (ImGui::IsItemHovered())
				xgui::hint("%s", UI_TEXT(fce_cc_drop_tip, "Back to the set\nDrops the values that came by control change, as a program change would. The voice set is not changed."));
		}

		const bool square = v.wave == fb::voice::SQUARE;
		const bool triangle = v.wave == fb::voice::TRIANGLE;

		// ---- 3 列: 波・音量・音程。それぞれ上に絵、下につまみ
		if (ImGui::BeginTable("fcedit", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
			const float pic_h = fs * 4.5f;
			// param は vb::fc_param。見出しの右に、その欄のコントロールチェンジの番号
			const auto int_slider = [&](int param, const char *label, const char *id, int value, int lo, int hi, const char *fmt, const char *tip) {
				ImGui::TextDisabled("%s", label);
				cc_tag(param, true);
				ImGui::SetNextItemWidth(-FLT_MIN);
				int x = value;
				if (ImGui::SliderInt(id, &x, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp))
					changed = true;
				if (ImGui::IsItemHovered())
					xgui::hint("%s", tip);
				return x;
			};
			ImGui::TableNextRow();

			// ======== 波
			ImGui::TableNextColumn();
			ImGui::SeparatorText(UI_TEXT(fce_wave, "Wave"));
			picture("fcwave", pic_h, [&](ImDrawList *dl, ImVec2 a, ImVec2 b) { draw_wave(dl, a, b, v); });
			{
				const char *const names[4] = { UI_TEXT(fce_w_square, "Square"), UI_TEXT(fce_w_triangle, "Triangle"), UI_TEXT(fce_w_noise, "Noise"), UI_TEXT(fce_w_metal, "Metal noise") };
				for (int i = 0; i < 4; i++) {
					if (i % 2)
						ImGui::SameLine();
					if (ImGui::RadioButton(names[i], v.wave == i)) {
						v.wave = u8(i);
						changed = true;
					}
				}
				if (ImGui::IsItemHovered() || ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly))
					xgui::hint("%s", UI_TEXT(fce_wave_tip, "Wave\nSquare: the lead and chord sound, its tone set by the duty below. Triangle: the 16-step bass wave; as on the real chip it has no volume, it sounds or it does not. Noise: hiss, snares, explosions. Metal noise: a short loop of noise, buzzing and metallic. The key sets how fast the noise runs."));
				cc_tag(vb::FC_P_WAVE, false);
			}
			ImGui::BeginDisabled(!square);
			v.duty_len = u8(int_slider(vb::FC_P_DUTY_LEN, UI_TEXT(fce_duty_len, "Duty steps"), "##dlen", v.duty_len, 1, 4, "%d",
			                           UI_TEXT(fce_duty_len_tip, "Duty steps\nHow many duty settings the square wave cycles through. 1 keeps one tone; 2 to 4 switch between them for the shimmering, sweeping sounds.")));
			for (int i = 0; i < v.duty_len; i++) {
				static const char *const DUTY[4] = { "1/8", "1/4", "1/2", "3/4" };
				ImGui::PushID(i);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("%d", i + 1);
				for (int d = 0; d < 4; d++) {
					ImGui::SameLine();
					const bool on = v.duty[i] == d;
					// 選んでいるものは、絵の線と同じ色で塗る
					if (on) {
						ImGui::PushStyleColor(ImGuiCol_Button, line_color());
						ImGui::PushStyleColor(ImGuiCol_ButtonHovered, line_color());
						ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(16, 20, 26, 255));
					}
					if (ImGui::SmallButton(DUTY[d])) {
						v.duty[i] = u8(d);
						changed = true;
					}
					if (on)
						ImGui::PopStyleColor(3);
					if (ImGui::IsItemHovered())
						xgui::hint("%s", UI_TEXT(fce_duty_tip, "Duty\nHow much of each cycle the square wave stays high. 1/2 is hollow and round, 1/4 brighter, 1/8 thin and nasal. 3/4 sounds like 1/4 on its own but differs inside a sequence."));
				}
				cc_tag(vb::FC_P_DUTY1 + i, true);
				ImGui::PopID();
			}
			if (v.duty_len > 1)
				v.duty_frames = u8(int_slider(vb::FC_P_DUTY_FRAMES, UI_TEXT(fce_duty_frames, "Frames per step"), "##dfr", v.duty_frames, 1, 30, "%d",
				                              UI_TEXT(fce_frames_tip, "Frames per step\nHow long each step lasts, in frames of 1/60 second. 1 is the fastest.")));
			ImGui::EndDisabled();

			// ======== 音量
			ImGui::TableNextColumn();
			ImGui::SeparatorText(UI_TEXT(fce_volume, "Volume"));
			picture("fcvol", pic_h, [&](ImDrawList *dl, ImVec2 a, ImVec2 b) { draw_volume(dl, a, b, v); });
			if (triangle)
				ImGui::TextWrapped("%s", UI_TEXT(fce_tri_note, "The triangle has no volume steps: these only decide when it stops."));
			v.decay = u8(int_slider(vb::FC_P_DECAY, UI_TEXT(fce_decay, "Fade while held"), "##dec", v.decay, 0, 60, v.decay ? UI_TEXT(fce_decay_fmt, "1 step / %d frames") : UI_TEXT(fce_decay_off, "holds"),
			                        UI_TEXT(fce_decay_tip, "Fade while held\nWhile the key is down the volume drops one of its 16 steps every this many frames (1/60 second). 0 holds the volume. 4 is the plucked sound of the initial voices 9-16; 1 is a short blip.")));
			ImGui::BeginDisabled(!v.decay);
			v.floor = u8(int_slider(vb::FC_P_FLOOR, UI_TEXT(fce_floor, "Stops at step"), "##flo", v.floor, 0, 15, v.floor ? "%d" : UI_TEXT(fce_floor_off, "0 (silence)"),
			                        UI_TEXT(fce_floor_tip, "Stops at step\nThe fade stops at this volume step and holds there while the key is down. 0 fades to silence. A soft key press that starts below this step does not fade.")));
			ImGui::EndDisabled();
			v.release = u8(int_slider(vb::FC_P_RELEASE, UI_TEXT(fce_release, "Fade after release"), "##rel", v.release, 1, 15, UI_TEXT(fce_release_fmt, "%d steps / frame"),
			                          UI_TEXT(fce_release_tip, "Fade after release\nAfter the key is let go the volume drops this many steps every frame. 15 cuts at once, 2 is the initial voices, 1 leaves a short tail of a quarter second.")));

			// ======== 音程
			ImGui::TableNextColumn();
			ImGui::SeparatorText(UI_TEXT(fce_pitch, "Pitch"));
			picture("fcpitch", pic_h, [&](ImDrawList *dl, ImVec2 a, ImVec2 b) { draw_pitch(dl, a, b, v); });
			v.arp_len = u8(int_slider(vb::FC_P_ARP_LEN, UI_TEXT(fce_arp_len, "Arpeggio steps"), "##alen", v.arp_len, 1, 4, "%d",
			                          UI_TEXT(fce_arp_len_tip, "Arpeggio steps\nOne key plays up to four pitches in turn, fast enough to sound like a chord: the classic way to get harmony out of one channel. 1 turns it off.")));
			if (v.arp_len > 1) {
				const float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * (v.arp_len - 1)) / v.arp_len;
				for (int i = 0; i < v.arp_len; i++) {
					if (i)
						ImGui::SameLine();
					ImGui::PushID(i);
					ImGui::SetNextItemWidth(w);
					int x = v.arp[i];
					if (ImGui::DragInt("##arp", &x, 0.2f, -24, 24, "%+d", ImGuiSliderFlags_AlwaysClamp)) {
						v.arp[i] = s8(x);
						changed = true;
					}
					if (ImGui::IsItemHovered())
						xgui::hint("%s", UI_TEXT(fce_arp_tip, "Arpeggio step\nSemitones above (or below) the key for this step. 0, +4, +7 is a major chord; 0, +3, +7 minor; 0, +12 the octave trill. Drag, or double-click to type."));
					ImGui::PopID();
				}
				// 並びの番号の札（つまみの下に、同じ順で）
				for (int i = 0, shown_tags = 0; i < v.arp_len; i++)
					if (ccs.cc[vb::FC_P_ARP1 + i])
						cc_tag(vb::FC_P_ARP1 + i, shown_tags++ > 0);
				v.arp_frames = u8(int_slider(vb::FC_P_ARP_FRAMES, UI_TEXT(fce_arp_frames, "Frames per step"), "##afr", v.arp_frames, 1, 30, "%d",
				                             UI_TEXT(fce_frames_tip, "Frames per step\nHow long each step lasts, in frames of 1/60 second. 1 is the fastest.")));
			}
			{
				int sw = int_slider(vb::FC_P_SWEEP, UI_TEXT(fce_sweep, "Starts off pitch by"), "##swp", v.sweep, -48, 48, UI_TEXT(fce_sweep_fmt, "%+d semitones"),
				                    UI_TEXT(fce_sweep_tip, "Starts off pitch\nThe note starts this many semitones above (or below) the key and slides to it. A high start falling fast is a kick drum or a tom on the triangle, a laser on the square; a low start rising is a jump."));
				// ずれを決めたのにコマが 0 だと何も起きないので、最初に動かしたときに 6 コマにする
				// （コントロールチェンジで来たずれを出しているだけのときは、コマに触らない）
				if (sw != v.sweep && sw && !v.sweep_frames)
					v.sweep_frames = 6;
				v.sweep = s8(sw);
			}
			ImGui::BeginDisabled(!v.sweep);
			v.sweep_frames = u8(int_slider(vb::FC_P_SWEEP_FRAMES, UI_TEXT(fce_sweep_frames, "Reaches pitch in"), "##swf", v.sweep_frames, v.sweep ? 1 : 0, 60, UI_TEXT(fce_frames_fmt, "%d frames"),
			                               UI_TEXT(fce_sweep_frames_tip, "Reaches pitch in\nHow many frames (1/60 second) the slide takes. It moves in steps, one per frame.")));
			ImGui::EndDisabled();
			v.vib_depth = u8(int_slider(vb::FC_P_VIB_DEPTH, UI_TEXT(fce_vib, "Vibrato"), "##vib", v.vib_depth, 0, 100, v.vib_depth ? UI_TEXT(fce_vib_fmt, "%d cents") : UI_TEXT(fce_off, "off"),
			                            UI_TEXT(fce_vib_tip, "Vibrato\nA vibrato this voice always has, on top of the one the modulation wheel adds. In cents (100 is a semitone each way), about 6 times a second.")));
			ImGui::BeginDisabled(!v.vib_depth);
			v.vib_delay = u8(int_slider(vb::FC_P_VIB_DELAY, UI_TEXT(fce_vib_delay, "Vibrato starts after"), "##vdl", v.vib_delay, 0, 120, UI_TEXT(fce_frames_fmt, "%d frames"),
			                            UI_TEXT(fce_vib_delay_tip, "Vibrato starts after\nHow long a note is held straight before the vibrato comes in, in frames (60 is one second). Short notes stay clean; long ones sing.")));
			ImGui::EndDisabled();
			ImGui::EndTable();
		}

		if (changed) {
			vb::fc_clamp(v);
			if (whole) {
				drop |= cc;
			} else if (cc) {
				// コントロールチェンジの値を出しているとき: 動かした欄だけを組に書き、その欄をコントロールチェンジから引き取る
				const fb::voice moved = v;
				v = base;
				std::memcpy(v.name, moved.name, sizeof(v.name));
				for (int i = 0; i < vb::FC_PARAMS; i++)
					if (vb::fc_get_param(moved, i) != vb::fc_get_param(shown, i)) {
						vb::fc_set_param(v, i, vb::fc_get_param(moved, i));
						drop |= cc & (1u << i);
					}
			}
			std::shared_ptr<fb::bank> nb;
			if (cur) {
				nb = std::make_shared<fb::bank>(*cur);
			} else {
				nb = fb::fresh(target);             // 初期の音色の写しから、新しい組を作る
				m_listed = -1;
			}
			nb->prog[size_t(program)] = v;
			fb::commit(br, target, nb);
		}
		if (drop) {
			m_dropped |= drop;
			m_dropped_frames = 4;
			br.post([slot = target == fb::MULTI ? -1 : target, channel, drop](mu2000 &mu) {
				mu.fc_board_clear_edits(slot, channel, drop);
				return std::string();
			});
		}
		// つまみを離したら、ファイルへ書く
		if (fb::unsaved(target) && !ImGui::IsAnyItemActive()) {
			if (!fb::save(br, target))
				m_note = UI_TEXT(fme_save_fail, "Could not write the voice set file");
			m_listed = -1;
		}
		if (!m_note.empty())
			ImGui::TextDisabled("%s", m_note.c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", UI_TEXT(fce_note, "Everything moves in frames of 1/60 second and in 16 volume steps, as on the old game consoles. Velocity sets the starting volume, pitch bend moves up to 2 semitones, the modulation wheel adds vibrato. 8 notes at once."));
		ImGui::PopStyleColor();
	}

private:
	// 写す・貼るの入れ物は、窓の中の 2 つのエディタ（1 パートのボード用と 16 パートのボード用）で共有する
	static fc_banks::voice &clip() { static fc_banks::voice v; return v; }
	static bool &has_clip() { static bool b = false; return b; }

	static ImU32 line_color() { return IM_COL32(110, 200, 255, 255); }
	static ImU32 axis_color() { return IM_COL32(80, 90, 110, 255); }
	static ImU32 cc_color() { return IM_COL32(255, 190, 90, 255); }       // コントロールチェンジで動いている欄

	// 「CC の割り当て」の小窓。欄ごとに番号を決める（0 = 割り当てない）。使えない番号は飛ばし、ほかの欄が使っている
	// 番号を選ぶと、そちらの割り当てが外れる
	static void assign_popup(bridge &br)
	{
		namespace fb = fc_banks;
		namespace vb = smu2000::vboard;
		if (!ImGui::BeginPopup("fccc"))
			return;
		const float fs = ImGui::GetFontSize();
		char duty[4][24], arp[4][24];
		for (int i = 0; i < 4; i++) {
			std::snprintf(duty[i], sizeof(duty[i]), UI_TEXT(fce_cc_duty_fmt, "Duty %d"), i + 1);
			std::snprintf(arp[i], sizeof(arp[i]), UI_TEXT(fce_cc_arp_fmt, "Arpeggio %d"), i + 1);
		}
		struct row { int param; const char *name; };
		const row wave[] = {
			{ vb::FC_P_WAVE, UI_TEXT(fce_wave, "Wave") }, { vb::FC_P_DUTY_LEN, UI_TEXT(fce_duty_len, "Duty steps") },
			{ vb::FC_P_DUTY_FRAMES, UI_TEXT(fce_duty_frames, "Frames per step") },
			{ vb::FC_P_DUTY1, duty[0] }, { vb::FC_P_DUTY2, duty[1] }, { vb::FC_P_DUTY3, duty[2] }, { vb::FC_P_DUTY4, duty[3] },
		};
		const row volume[] = {
			{ vb::FC_P_DECAY, UI_TEXT(fce_decay, "Fade while held") }, { vb::FC_P_FLOOR, UI_TEXT(fce_floor, "Stops at step") },
			{ vb::FC_P_RELEASE, UI_TEXT(fce_release, "Fade after release") },
		};
		const row pitch[] = {
			{ vb::FC_P_ARP_LEN, UI_TEXT(fce_arp_len, "Arpeggio steps") }, { vb::FC_P_ARP_FRAMES, UI_TEXT(fce_arp_frames, "Frames per step") },
			{ vb::FC_P_ARP1, arp[0] }, { vb::FC_P_ARP2, arp[1] }, { vb::FC_P_ARP3, arp[2] }, { vb::FC_P_ARP4, arp[3] },
			{ vb::FC_P_SWEEP, UI_TEXT(fce_sweep, "Starts off pitch by") }, { vb::FC_P_SWEEP_FRAMES, UI_TEXT(fce_sweep_frames, "Reaches pitch in") },
			{ vb::FC_P_VIB_DEPTH, UI_TEXT(fce_vib, "Vibrato") }, { vb::FC_P_VIB_DELAY, UI_TEXT(fce_vib_delay, "Vibrato starts after") },
		};
		const auto column = [&](const char *title, const row *rows, size_t n) {
			ImGui::TableNextColumn();
			ImGui::SeparatorText(title);
			for (size_t k = 0; k < n; k++) {
				const int now = fb::cc_map().cc[rows[k].param];
				ImGui::PushID(rows[k].param);
				ImGui::SetNextItemWidth(fs * 5.5f);
				int x = now;
				if (ImGui::InputInt("##cc", &x, 1, 10)) {
					// 使えない番号は、動かした向きへ飛ばす。端まで行ったら 0（割り当てない）か、元のまま
					const int dir = x >= now ? 1 : -1;
					x = std::clamp(x, 0, 119);
					while (x > 0 && x < 120 && !vb::fc_cc_allowed(x))
						x += dir;
					if (x >= 120)
						x = now;
					fb::assign_cc(br, rows[k].param, std::max(x, 0));
				}
				ImGui::SameLine();
				if (now)
					ImGui::TextUnformatted(rows[k].name);
				else
					ImGui::TextDisabled("%s  (%s)", rows[k].name, UI_TEXT(fce_off, "off"));
				ImGui::PopID();
			}
		};
		if (ImGui::BeginTable("fccct", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableNextRow();
			column(UI_TEXT(fce_wave, "Wave"), wave, std::size(wave));
			column(UI_TEXT(fce_volume, "Volume"), volume, std::size(volume));
			column(UI_TEXT(fce_pitch, "Pitch"), pitch, std::size(pitch));
			ImGui::EndTable();
		}
		if (ImGui::SmallButton(UI_TEXT(fce_cc_defaults, "Default numbers")))
			fb::reset_cc(br);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::PushTextWrapPos(fs * 40.0f);
		ImGui::TextWrapped("%s", UI_TEXT(fce_cc_assign_note, "0 leaves a value without a control change. A number can move one value only: choosing one that is in use takes it away from the other value. Numbers the board or the MU part already listens to are skipped (0, 1, 6, 7, 10, 11, 32, 38, 64, 91, 93, 94, 96-101, and 120 up). Values are the editor's numbers as they are; semitone offsets are sent with 64 as 0."));
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::EndPopup();
	}

	// 絵の枠。中を描く関数に左上と右下を渡す
	template <typename F> static void picture(const char *id, float h, F &&draw)
	{
		const ImVec2 a = ImGui::GetCursorScreenPos();
		const ImVec2 size(std::max(8.0f, ImGui::GetContentRegionAvail().x), h);
		ImGui::InvisibleButton(id, size);
		const ImVec2 b(a.x + size.x, a.y + size.y);
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(a, b, IM_COL32(16, 20, 26, 255), 4.0f);
		dl->PushClipRect(a, b, true);
		draw(dl, a, b);
		dl->PopClipRect();
	}
	// 階段の線（横に進んでから縦に動く）
	static void stair(ImDrawList *dl, ImVec2 &prev, ImVec2 p, bool first)
	{
		if (!first) {
			dl->AddLine(prev, ImVec2(p.x, prev.y), line_color(), 1.5f);
			dl->AddLine(ImVec2(p.x, prev.y), p, line_color(), 1.5f);
		}
		prev = p;
	}

	// 波の形。矩形波はデューティの並びの順に 2 周期ずつ、三角波は 2 周期、ノイズは音源と同じ帰還シフトレジスタ
	static void draw_wave(ImDrawList *dl, ImVec2 a, ImVec2 b, const fc_banks::voice &v)
	{
		using voice = fc_banks::voice;
		const float w = b.x - a.x, h = b.y - a.y, mid = a.y + h * 0.5f, amp = h * 0.36f;
		dl->AddLine(ImVec2(a.x, mid), ImVec2(b.x, mid), axis_color());
		const int n = std::max(16, int(w));
		ImVec2 prev;
		if (v.wave == voice::NOISE || v.wave == voice::METAL) {
			u32 lfsr = 1;
			int at = -1;
			for (int i = 0; i <= n; i++) {
				const int step = int(double(i) / n * 96.0);
				for (; at < step; at++) {
					const u32 tap = v.wave == voice::METAL ? 6 : 1;
					const u32 fb = (lfsr ^ (lfsr >> tap)) & 1;
					lfsr = (lfsr >> 1) | (fb << 14);
				}
				stair(dl, prev, ImVec2(a.x + w * i / n, mid - ((lfsr & 1) ? amp : -amp)), i == 0);
			}
			return;
		}
		const int parts = v.wave == voice::SQUARE ? std::clamp<int>(v.duty_len, 1, 4) : 1;
		for (int i = 0; i <= n; i++) {
			const double t = double(i) / n * parts;
			const int k = std::min(parts - 1, int(t));
			const double ph = std::fmod((t - k) * 2.0, 1.0);       // 並びの 1 つにつき 2 周期
			double y;
			if (v.wave == voice::TRIANGLE) {
				const int s = int(ph * 32.0) & 31;
				y = (s < 16 ? s : 31 - s) / 7.5 - 1.0;
			} else {
				y = ph < smu2000::vboard::FC_DUTY[v.duty[k] & 3] ? 1.0 : -1.0;
			}
			stair(dl, prev, ImVec2(a.x + w * i / n, mid - float(y) * amp), i == 0);
		}
		// 並びの境目
		for (int k = 1; k < parts; k++)
			dl->AddLine(ImVec2(a.x + w * k / parts, a.y), ImVec2(a.x + w * k / parts, b.y), axis_color());
	}

	// 音量の動き。いちばん強く 1 秒押して離したとき（縦の線が離した所）
	static void draw_volume(ImDrawList *dl, ImVec2 a, ImVec2 b, const fc_banks::voice &v)
	{
		const float w = b.x - a.x, h = b.y - a.y, pad = h * 0.12f;
		const int frames = 80, hold = 60;
		const bool tri = v.wave == fc_banks::voice::TRIANGLE;
		const auto y_of = [&](int vol) { return b.y - pad - (h - pad * 2) * (tri ? (vol > 0 ? 1.0f : 0.0f) : vol / 15.0f); };
		int vol = 15;
		ImVec2 prev;
		stair(dl, prev, ImVec2(a.x, y_of(vol)), true);
		for (int f = 1; f <= frames; f++) {
			if (f > hold)
				vol = std::max(0, vol - std::max<int>(v.release, 1));
			else if (v.decay && f % v.decay == 0 && vol > v.floor)
				vol--;
			stair(dl, prev, ImVec2(a.x + w * f / frames, y_of(vol)), false);
		}
		const float off_x = a.x + w * float(hold) / frames;
		dl->AddLine(ImVec2(off_x, a.y), ImVec2(off_x, b.y), axis_color());
	}

	// 音程の動き。最初の 1 秒（横の線が鍵の高さ）。ビブラートは 6Hz の波で足す
	static void draw_pitch(ImDrawList *dl, ImVec2 a, ImVec2 b, const fc_banks::voice &v)
	{
		const float w = b.x - a.x, h = b.y - a.y, pad = h * 0.14f;
		const int frames = 60;
		const auto semis_at = [&](int f, double sub) {
			double s = v.arp_len > 1 ? v.arp[(f / std::max<int>(v.arp_frames, 1)) % v.arp_len] : 0.0;
			if (v.sweep_frames && f < v.sweep_frames)
				s += v.sweep * double(v.sweep_frames - f) / v.sweep_frames;
			if (v.vib_depth && f >= v.vib_delay)
				s += v.vib_depth / 100.0 * std::sin(6.283185307179586 * 6.0 * (f + sub) / 60.0);
			return s;
		};
		double lo = -1.0, hi = 1.0;
		for (int f = 0; f < frames; f++) {
			lo = std::min(lo, semis_at(f, 0.0) - 0.5);
			hi = std::max(hi, semis_at(f, 0.0) + 0.5);
		}
		const auto y_of = [&](double s) { return b.y - pad - float((s - lo) / (hi - lo)) * (h - pad * 2); };
		dl->AddLine(ImVec2(a.x, y_of(0.0)), ImVec2(b.x, y_of(0.0)), axis_color());
		// アルペジオとずれはコマごとの階段、ビブラートはなめらか。1 コマを 4 つに割って線でつなぐ
		ImVec2 prev;
		for (int i = 0; i <= frames * 4; i++) {
			const int f = std::min(frames - 1, i / 4);
			const ImVec2 p(a.x + w * i / (frames * 4), y_of(semis_at(f, (i % 4) / 4.0)));
			if (i) {
				if (i % 4 == 0)
					stair(dl, prev, p, false);
				else {
					dl->AddLine(prev, p, line_color(), 1.5f);
					prev = p;
				}
			} else {
				prev = p;
			}
		}
	}

	std::vector<std::string> m_list;
	double m_listed = -1;
	std::string m_note, m_name_for;
	char m_set_name[15] = {};
	int m_name_target = -1;
	u32 m_dropped = 0;                 // ここで動かして、コントロールチェンジから引き取ったばかりの欄
	int m_dropped_frames = 0;
};

} // namespace ui

#endif // S_MU2000_UI_FC_VOICE_EDITOR_H
