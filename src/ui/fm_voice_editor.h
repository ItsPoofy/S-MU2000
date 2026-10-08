// license:BSD-3-Clause
//
// FM ボードの音色エディタ（音色の窓で、FM ボードのチャンネルを出しているときの右の面）。
// そのチャンネルでいま選んでいるプログラムの音色（src/vboard_fm.h の fm_voice）を触る:
//   ・つなぎ方（8 通り）を絵で選ぶ。箱がオペレーター、塗ってある箱が「鳴る側」、線が変調の向き
//   ・オペレーター 1 のフィードバック、鳴り始めの音程の落ち幅、ノイズ
//   ・オペレーターごとに、周波数の比・大きさ（変調する側なら深さ）・アタック／ディケイ／サステイン／リリース
//   ・できあがる 1 周期の形と、倍音の分布
// 触った値はすぐ音源へ渡る（鳴っている音にも効く）。組（128 個の音色）は fm_banks.h が設定のフォルダーに保存する。
// 何も開いていない状態で触ると、初期の音色の写しから新しい組を作る

#ifndef S_MU2000_UI_FM_VOICE_EDITOR_H
#define S_MU2000_UI_FM_VOICE_EDITOR_H

#pragma once

#include "fm_banks.h"
#include "xg_ui.h"
#include "ui/texts.h"

#include "imgui.h"

#include <cmath>
#include <cstring>

namespace ui {

class fm_voice_editor
{
public:
	// program はそのチャンネルでいま選んでいるプログラム（0-127）
	void draw(int program, bridge &br)
	{
		namespace fb = fm_banks;
		namespace vb = smu2000::vboard;
		const float fs = ImGui::GetFontSize();
		ImDrawList *dl = ImGui::GetWindowDrawList();
		program &= 127;

		const double now = ImGui::GetTime();
		if (m_listed < 0 || now - m_listed > 1.0) {
			m_listed = now;
			m_list = fb::list();
		}
		std::shared_ptr<const fb::bank> cur = fb::current();
		fb::voice v = cur ? cur->prog[size_t(program)] : vb::fm_default_voice(program);
		bool changed = false;

		// ---- 組（128 個の音色のまとまり）
		{
			const std::string stem = fb::current_stem();
			ImGui::SetNextItemWidth(fs * 9);
			if (ImGui::BeginCombo("##fmset", cur ? cur->name : UI_TEXT(fme_default_set, "(initial voices)"))) {
				if (ImGui::Selectable(UI_TEXT(fme_default_set, "(initial voices)"), !cur) && cur) {
					fb::close(br);
					m_note.clear();
				}
				for (const std::string &s : m_list)
					if (ImGui::Selectable(s.c_str(), s == stem) && s != stem) {
						std::string err;
						m_note = fb::open(br, s, err) ? std::string() : err;
					}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered())
				xgui::hint("%s", UI_TEXT(fme_set_tip, "Voice set\nThe 128 voices of the FM board, kept as one file in the \"fmsets\" folder of the settings folder. \"(initial voices)\" is the built-in set; touching anything there starts a new set from a copy of it. Changes are saved as you go."));
			ImGui::SameLine();
			if (ImGui::SmallButton(UI_TEXT(fme_new_set, "New set"))) {
				fb::create(br);
				m_listed = -1;
				m_note.clear();
			}
			cur = fb::current();
			if (cur) {
				// 組の名前。変えるとファイルの名前も変わる
				const std::string path = fb::current_path();
				if (m_name_for != path && !ImGui::IsAnyItemActive()) {
					m_name_for = path;
					std::snprintf(m_set_name, sizeof(m_set_name), "%s", cur->name);
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(fs * 8);
				ImGui::InputText("##fmsetname", m_set_name, sizeof(m_set_name));
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					char clean[15];
					vb::fm_detail::clean_text(clean, reinterpret_cast<const u8 *>(m_set_name), 14, false);
					for (size_t n = std::strlen(clean); n && clean[n - 1] == ' '; n--)
						clean[n - 1] = 0;
					const std::string to = fb::file_for(clean);
					if (!clean[0] || (to != path && smu2000::is_file(to))) {
						m_note = UI_TEXT(fme_name_taken, "That name is empty or already used by another set");
					} else {
						m_note.clear();
						auto nb = std::make_shared<fb::bank>(*cur);
						std::snprintf(nb->name, sizeof(nb->name), "%s", clean);
						fb::commit(br, nb);
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
			if (ImGui::InputText("##fmname", name, sizeof(name))) {
				vb::fm_detail::clean_text(v.name, reinterpret_cast<const u8 *>(name), 8, true);
				changed = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(fme_reset_voice, "Initial voice"))) {
			v = vb::fm_default_voice(program);
			changed = true;
		}
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(fme_reset_tip, "Initial voice\nPuts the built-in voice of this program number back."));
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(fme_copy, "Copy"))) {
			m_clip = v;
			m_has_clip = true;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_has_clip);
		if (ImGui::SmallButton(UI_TEXT(fme_paste, "Paste"))) {
			v = m_clip;
			changed = true;
		}
		ImGui::EndDisabled();

		// ---- 左: つなぎ方と全体の値。右: できあがる形と倍音
		const ImVec2 alg_sz(fs * 4.6f, fs * 3.5f);
		const float left_w = alg_sz.x * 4.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
		ImGui::BeginGroup();
		ImGui::TextDisabled("%s", UI_TEXT(fme_alg, "Connection"));
		for (int a = 0; a < 8; a++) {
			if (a % 4)
				ImGui::SameLine();
			ImGui::PushID(a);
			const ImVec2 sz = alg_sz;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton("##alg", sz)) {
				v.alg = a;
				changed = true;
			}
			const bool hov = ImGui::IsItemHovered();
			dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(v.alg == a ? ImGuiCol_Header : hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
			if (v.alg == a)
				dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 3.0f, 0, 1.5f);
			draw_alg(dl, a, p, sz, fs, v.feedback > 0.0f);
			if (hov)
				xgui::hint("%s", UI_TEXT(fme_alg_tip, "Connection\nHow the four operators are wired. A filled box is an operator you hear; an outlined box modulates the one its line leads to (it changes the tone, not the level). The loop on operator 1 is its feedback."));
			ImGui::PopID();
		}
		ImGui::PushItemWidth(left_w - fs * 10.5f);
		changed |= ImGui::SliderFloat(UI_TEXT(fme_feedback, "Feedback (op 1)"), &v.feedback, 0.0f, 1.0f, "%.2f");
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(fme_feedback_tip, "Feedback\nOperator 1 modulating itself. A little turns its sine towards a sawtooth; a lot turns it to noise."));
		changed |= ImGui::SliderFloat(UI_TEXT(fme_drop, "Pitch drop (semitones)"), &v.drop, 0.0f, 36.0f, "%.0f");
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(fme_drop_tip, "Pitch drop\nThe note starts this many semitones high and falls to pitch in about 30 ms: the thump of a tom or a kick, or a zap."));
		changed |= ImGui::SliderFloat(UI_TEXT(fme_noise, "Noise"), &v.noise, 0.0f, 1.0f, "%.2f");
		if (ImGui::IsItemHovered())
			xgui::hint("%s", UI_TEXT(fme_noise_tip, "Noise\nRandom modulation added to the operators you hear: breath, hiss, the rattle of a snare."));
		ImGui::PopItemWidth();
		ImGui::EndGroup();

		ImGui::SameLine(0, fs * 0.8f);
		{
			// 1 周期の形（左）と倍音 1-32（右）
			float cyc[256];
			vb::fm_cycle(v, cyc, 256);
			const float w = std::max(fs * 10.0f, ImGui::GetContentRegionAvail().x);
			const float half = (w - fs * 0.5f) * 0.5f;
			const float h = alg_sz.y * 2.0f + ImGui::GetStyle().ItemSpacing.y * 4.0f + ImGui::GetTextLineHeight() + ImGui::GetFrameHeight() * 3.0f;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(w, h));
			const ImU32 back = IM_COL32(16, 20, 26, 255), line = IM_COL32(110, 200, 255, 255);
			dl->AddRectFilled(p, ImVec2(p.x + half, p.y + h), back, 4.0f);
			dl->AddLine(ImVec2(p.x, p.y + h * 0.5f), ImVec2(p.x + half, p.y + h * 0.5f), IM_COL32(80, 90, 110, 255));
			float peak = 1e-6f;
			for (float x : cyc)
				peak = std::max(peak, std::fabs(x));
			for (int i = 0; i + 1 < 256; i++)
				dl->AddLine(ImVec2(p.x + half * float(i) / 255.0f, p.y + h * 0.5f * (1.0f - 0.9f * cyc[i] / peak)),
				            ImVec2(p.x + half * float(i + 1) / 255.0f, p.y + h * 0.5f * (1.0f - 0.9f * cyc[i + 1] / peak)), line, 1.5f);
			const ImVec2 q(p.x + half + fs * 0.5f, p.y);
			dl->AddRectFilled(q, ImVec2(q.x + half, q.y + h), back, 4.0f);
			float mag[33] = {}, top = 1e-9f;
			for (int k = 1; k <= 32; k++) {
				double re = 0, im = 0;
				for (int i = 0; i < 256; i++) {
					const double ph = 6.283185307179586 * double(k) * double(i) / 256.0;
					re += double(cyc[i]) * std::cos(ph);
					im += double(cyc[i]) * std::sin(ph);
				}
				mag[k] = float(std::sqrt(re * re + im * im));
				top = std::max(top, mag[k]);
			}
			const float bw = half / 32.0f;
			for (int k = 1; k <= 32; k++) {
				const float db = 20.0f * std::log10(std::max(mag[k] / top, 1e-6f));
				const float t = std::clamp(1.0f + db / 60.0f, 0.0f, 1.0f);
				if (t > 0.0f)
					dl->AddRectFilled(ImVec2(q.x + bw * float(k - 1) + 0.5f, q.y + h * (1.0f - t)), ImVec2(q.x + bw * float(k) - 0.5f, q.y + h), line);
			}
		}

		// ---- オペレーター 4 つ
		if (ImGui::BeginTable("fmops", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableNextRow();
			for (int i = 0; i < 4; i++) {
				ImGui::TableNextColumn();
				ImGui::PushID(i);
				smu2000::vboard::fm_op &o = v.op[i];
				const bool car = vb::fm_is_carrier(v.alg, i);
				ImGui::TextColored(car ? ImVec4(1.0f, 0.82f, 0.36f, 1.0f) : ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "OP%d  %s", i + 1,
				                   car ? UI_TEXT(fme_op_carrier, "heard") : UI_TEXT(fme_op_mod, "modulates"));
				// 包絡線の絵（押している間 → 離す）
				{
					const ImVec2 p = ImGui::GetCursorScreenPos();
					const ImVec2 sz(ImGui::GetContentRegionAvail().x - fs * 0.3f, fs * 2.3f);
					ImGui::Dummy(sz);
					dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255), 3.0f);
					// 横の割り当て: 時間を t / (t + 0.4 秒) で縮めて、アタック・ディケイ・押している間・リリースを並べる
					const auto squash = [](float t) { return t / (t + 0.4f); };
					const float wa = 0.22f * squash(o.attack * 4.0f), wd = o.decay > 0.0f && o.sustain < 1.0f ? 0.3f * squash(o.decay) : 0.0f;
					const float wr = 0.25f * squash(o.release * 2.0f), wh = 1.0f - wa - wd - wr;
					const auto pt = [&](float x, float y) { return ImVec2(p.x + sz.x * x, p.y + sz.y * (1.0f - 0.9f * y) - 1.0f); };
					const float held = o.decay > 0.0f ? o.sustain : 1.0f;
					const ImVec2 pts[5] = { pt(0, 0), pt(wa, 1), pt(wa + wd, held), pt(wa + wd + std::max(0.02f, wh * 0.7f), held), pt(std::min(1.0f, wa + wd + std::max(0.02f, wh * 0.7f) + wr), 0) };
					dl->AddPolyline(pts, 5, car ? IM_COL32(255, 210, 90, 255) : IM_COL32(140, 190, 255, 255), 0, 1.5f);
				}
				ImGui::PushItemWidth(-fs * 4.6f);
				changed |= ImGui::DragFloat(UI_TEXT(fme_ratio, "Ratio"), &o.ratio, 0.01f, 0.25f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemHovered())
					xgui::hint("%s", UI_TEXT(fme_ratio_tip, "Ratio\nThis operator's frequency as a multiple of the note's. Whole numbers give harmonic tones; in-between values give the clang of bells and metal. Drag, or double-click to type."));
				changed |= ImGui::SliderFloat(UI_TEXT(fme_level, "Level"), &o.level, 0.0f, 1.0f, "%.2f");
				if (ImGui::IsItemHovered())
					xgui::hint("%s", car ? UI_TEXT(fme_level_car_tip, "Level\nHow loud this operator is.")
					               : UI_TEXT(fme_level_mod_tip, "Level\nHow deeply this operator modulates: more gives a brighter, harsher tone. Harder key strikes add to it."));
				changed |= ImGui::DragFloat(UI_TEXT(fme_attack, "Attack"), &o.attack, 0.002f, 0.0f, 3.0f, "%.3f s", ImGuiSliderFlags_AlwaysClamp);
				changed |= ImGui::DragFloat(UI_TEXT(fme_decay, "Decay"), &o.decay, 0.01f, 0.0f, 10.0f, o.decay <= 0.0f ? "-" : "%.2f s", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemHovered())
					xgui::hint("%s", UI_TEXT(fme_decay_tip, "Decay\nWhile the key is held, the time to fall to the Sustain level (\"-\" keeps the full level). On a modulator this makes the tone mellow as the note goes on."));
				changed |= ImGui::SliderFloat(UI_TEXT(fme_sustain, "Sustain"), &o.sustain, 0.0f, 1.0f, "%.2f");
				changed |= ImGui::DragFloat(UI_TEXT(fme_release, "Release"), &o.release, 0.005f, 0.0f, 10.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
				ImGui::PopItemWidth();
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		if (changed) {
			std::shared_ptr<fb::bank> nb;
			if (cur) {
				nb = std::make_shared<fb::bank>(*cur);
			} else {
				nb = fb::fresh();             // 初期の音色の写しから、新しい組を作る
				m_listed = -1;
			}
			nb->prog[size_t(program)] = v;
			fb::commit(br, nb);
		}
		// つまみを離したら、ファイルへ書く
		if (fb::unsaved() && !ImGui::IsAnyItemActive()) {
			if (!fb::save(br))
				m_note = UI_TEXT(fme_save_fail, "Could not write the voice set file");
			m_listed = -1;
		}
		if (!m_note.empty())
			ImGui::TextDisabled("%s", m_note.c_str());
	}

private:
	// つなぎ方の絵。箱の位置（横は -1〜1、縦は 0 が下の段）と、変調の線（どれからどれへ）
	static void draw_alg(ImDrawList *dl, int alg, ImVec2 p, ImVec2 sz, float fs, bool feedback)
	{
		struct pos { float x, y; };
		static const pos POS[8][4] = {
			{ { 0, 3 }, { 0, 2 }, { 0, 1 }, { 0, 0 } },
			{ { -0.5f, 2 }, { 0.5f, 2 }, { 0, 1 }, { 0, 0 } },
			{ { -0.5f, 1 }, { 0.5f, 2 }, { 0.5f, 1 }, { 0, 0 } },
			{ { -0.5f, 2 }, { -0.5f, 1 }, { 0.5f, 1 }, { 0, 0 } },
			{ { -0.5f, 1 }, { -0.5f, 0 }, { 0.5f, 1 }, { 0.5f, 0 } },
			{ { 0, 1 }, { -1, 0 }, { 0, 0 }, { 1, 0 } },
			{ { -1, 1 }, { -1, 0 }, { 0, 0 }, { 1, 0 } },
			{ { -1.5f, 0 }, { -0.5f, 0 }, { 0.5f, 0 }, { 1.5f, 0 } },
		};
		// 線: from → to（0 始まり）。-1 で終わり
		static const int EDGE[8][4][2] = {
			{ { 0, 1 }, { 1, 2 }, { 2, 3 }, { -1, -1 } },
			{ { 0, 2 }, { 1, 2 }, { 2, 3 }, { -1, -1 } },
			{ { 0, 3 }, { 1, 2 }, { 2, 3 }, { -1, -1 } },
			{ { 0, 1 }, { 1, 3 }, { 2, 3 }, { -1, -1 } },
			{ { 0, 1 }, { 2, 3 }, { -1, -1 }, { -1, -1 } },
			{ { 0, 1 }, { 0, 2 }, { 0, 3 }, { -1, -1 } },
			{ { 0, 1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
			{ { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
		};
		// 箱の大きさは、いちばん段の多いつなぎ方（縦に 4 つ）が収まるように決める
		const float rows = alg == 0 ? 4.0f : alg <= 3 ? 3.0f : alg == 7 ? 1.0f : 2.0f;
		const float box = std::min(fs * 0.8f, sz.y / 5.4f);
		const float step_y = rows > 1.0f ? std::min(box * 1.45f, (sz.y - box * 1.3f) / (rows - 1.0f)) : 0.0f, step_x = box * 1.35f;
		const float cx = p.x + sz.x * 0.5f, base = p.y + sz.y * 0.5f + step_y * (rows - 1.0f) * 0.5f;
		const auto center = [&](int i) { return ImVec2(cx + POS[alg][i].x * step_x, base - POS[alg][i].y * step_y); };
		const ImU32 wire = ImGui::GetColorU32(ImGuiCol_Text, 0.7f);
		for (const auto &e : EDGE[alg]) {
			if (e[0] < 0)
				break;
			dl->AddLine(center(e[0]), center(e[1]), wire, 1.5f);
		}
		for (int i = 0; i < 4; i++) {
			const ImVec2 c = center(i);
			const ImVec2 a(c.x - box * 0.5f, c.y - box * 0.5f), b(c.x + box * 0.5f, c.y + box * 0.5f);
			const bool car = smu2000::vboard::fm_is_carrier(alg, i);
			dl->AddRectFilled(a, b, car ? IM_COL32(255, 205, 90, 255) : IM_COL32(24, 30, 40, 255), 2.0f);
			dl->AddRect(a, b, car ? IM_COL32(255, 205, 90, 255) : IM_COL32(140, 190, 255, 255), 2.0f, 0, 1.5f);
			char n[2] = { char('1' + i), 0 };
			ImGui::PushFont(nullptr, box * 0.95f);
			const ImVec2 ts = ImGui::CalcTextSize(n);
			dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), car ? IM_COL32(20, 20, 20, 255) : IM_COL32(200, 225, 255, 255), n);
			ImGui::PopFont();
			if (i == 0 && feedback)       // フィードバックの輪
				dl->AddCircle(ImVec2(b.x + box * 0.05f, a.y - box * 0.05f), box * 0.3f, IM_COL32(140, 190, 255, 255), 12, 1.2f);
		}
	}

	std::vector<std::string> m_list;
	double m_listed = -1;
	std::string m_note, m_name_for;
	char m_set_name[15] = {};
	fm_banks::voice m_clip;
	bool m_has_clip = false;
};

} // namespace ui

#endif // S_MU2000_UI_FM_VOICE_EDITOR_H
