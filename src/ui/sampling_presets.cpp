// license:BSD-3-Clause
//
// サンプリングの窓の「内蔵音色」のタブ（sampling_editor::preset_pane）。
// MU2000 の内蔵の音色（ROM の音色の記録）を一覧にして、要素ごとの中身（どの内蔵ウェーブを、どの鍵と強さで、
// どんな音量・パン・音程・フィルター・エンベロープ・LFO で鳴らしているか）を見せ、**要素を選んで鳴らす**。
//
// 鳴らすのは本物の音源。ROM の音色の要素とサンプル音色の要素は同じ 84 バイトの並びなので、音色の記録を
// サンプル音色の枠に写して（mu2000::sampling_copy_preset）、鳴らす要素の印だけを変える。
// 借りる枠は Bank# 1 の 128 番（いちばん最後）。借りる前の中身は取っておき、このタブを離れるか窓を閉じると戻す。

#include "sampling_editor.h"

#include "xg/voices.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ui {

namespace {

constexpr int BORROW_SLOT = 255;          // Bank# 1 の 128 番

void draw_icon(ImDrawList *dl, ImVec2 p, float px, const u16 rows[16], ImU32 col)
{
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
			if (rows[y] & (0x8000 >> x))
				dl->AddRectFilled(ImVec2(p.x + float(x) * px, p.y + float(y) * px),
				                  ImVec2(p.x + float(x + 1) * px, p.y + float(y + 1) * px), col);
}

std::string lower(std::string s)
{
	for (char &c : s)
		c = char(std::tolower(u8(c)));
	return s;
}

// 要素 1〜4 の色（表の印と、鍵 × 強さの図の枠で同じ色）
ImU32 el_color(int e, int alpha)
{
	static const int RGB[4][3] = { { 90, 190, 255 }, { 255, 170, 80 }, { 140, 230, 130 }, { 240, 130, 220 } };
	return IM_COL32(RGB[e & 3][0], RGB[e & 3][1], RGB[e & 3][2], alpha);
}

std::string pan_text(int p)
{
	if (p == 15)
		return "Scl";
	if (p == 7)
		return "C";
	return (p < 7 ? "L" : "R") + std::to_string(p < 7 ? 7 - p : p - 7);
}

std::string signed_text(int v)
{
	return (v > 0 ? "+" : "") + std::to_string(v);
}

} // namespace

void sampling_editor::preset_restore(bridge &br)
{
	if (m_pv_held >= 0) {
		const std::vector<u8> msg = { 0x80, u8(m_pv_held), 0x40 };
		br.send(msg);
		m_pv_held = -1;
	}
	if (!m_pv_borrowed)
		return;
	m_pv_borrowed = false;
	m_pv_selected = false;
	auto keep = m_pv_keep;
	br.post([keep](mu2000 &mu) {
		if (keep && !keep->empty()) {
			mu.sampling_set_voice_raw(BORROW_SLOT, *keep);
			keep->clear();
		}
		return std::string();
	});
}

// 内蔵ウェーブのタブなどから: その選び方（MSB・LSB・番号）の音色を開く
void sampling_editor::preset_open(int msb, int lsb, int prog)
{
	const xg::voice_rom *vr = xgui::voices();
	if (!m_pv_built && vr && vr->ok()) {
		m_pv_list = xg::preset_voices(*vr);
		m_pv_built = true;
	}
	for (size_t i = 0; i < m_pv_list.size(); i++)
		if (m_pv_list[i].msb == msb && m_pv_list[i].lsb == lsb && m_pv_list[i].prog == prog) {
			m_pv_sel = int(i);
			m_pv_find[0] = 0;
			m_pv_scroll = true;
			for (bool &b : m_pv_on)
				b = true;
			m_goto_tab = 3;
			return;
		}
}

void sampling_editor::preset_pane(bridge &br)
{
	m_pv_drawn = true;
	const xg::voice_rom *vr = xgui::voices();
	if (!m_pv_built && vr && vr->ok()) {
		m_pv_list = xg::preset_voices(*vr);
		m_pv_built = true;
	}
	romwave_build();
	if (!m_pv_built || m_pv_list.empty() || !vr) {
		ImGui::TextDisabled("%s", UI_TEXT(smp_not_ready, "Waiting for the MU2000 to start..."));
		return;
	}
	const float fs = ImGui::GetFontSize();
	const int count = int(m_pv_list.size());
	m_pv_sel = std::clamp(m_pv_sel, 0, count - 1);
	const u8 *rom = vr->data();
	bool picked = false;

	// ---- 左: 一覧
	const float left_w = std::min(fs * 21.0f, ImGui::GetContentRegionAvail().x * 0.4f);
	ImGui::BeginChild("pv_left", ImVec2(left_w, 0));
	{
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##find", UI_TEXT(pv_find, "Voice name"), m_pv_find, sizeof(m_pv_find));
		const std::string want = lower(m_pv_find);
		std::vector<int> shown;
		shown.reserve(size_t(count));
		for (int i = 0; i < count; i++)
			if (want.empty() || lower(m_pv_list[size_t(i)].name).find(want) != std::string::npos)
				shown.push_back(i);
		ImGui::TextDisabled(UI_TEXT(pv_count_fmt, "%d of %d voices"), int(shown.size()), count);
		if (ImGui::BeginTable("pv_list", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed, 18.0f);
			ImGui::TableSetupColumn(UI_TEXT(pv_col_name, "Voice"));
			ImGui::TableSetupColumn("MSB/LSB/#", ImGuiTableColumnFlags_WidthFixed, fs * 5.6f);
			ImGui::TableSetupColumn(UI_TEXT(pv_col_els, "El."), ImGuiTableColumnFlags_WidthFixed, fs * 2.4f);
			ImGui::TableHeadersRow();
			int step = 0;
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsAnyItemActive()) {
				if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
					step = 1;
				if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
					step = -1;
			}
			if (step && !shown.empty()) {
				auto it = std::find(shown.begin(), shown.end(), m_pv_sel);
				int at = it == shown.end() ? 0 : int(it - shown.begin()) + step;
				at = std::clamp(at, 0, int(shown.size()) - 1);
				if (shown[size_t(at)] != m_pv_sel) {
					m_pv_sel = shown[size_t(at)];
					picked = true;
					m_pv_scroll = true;
				}
			}
			ImGuiListClipper clip;
			clip.Begin(int(shown.size()), std::max(ImGui::GetTextLineHeightWithSpacing(), 18.0f));
			if (m_pv_scroll) {
				auto it = std::find(shown.begin(), shown.end(), m_pv_sel);
				if (it != shown.end())
					clip.IncludeItemByIndex(int(it - shown.begin()));
			}
			while (clip.Step())
				for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
					const int i = shown[size_t(r)];
					const xg::preset_voice &v = m_pv_list[size_t(i)];
					ImGui::PushID(i);
					ImGui::TableNextRow(0, 18.0f);
					ImGui::TableNextColumn();
					u16 rows[16];
					if (vr->icon_rows(v.icon, rows))
						draw_icon(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 1.0f, rows, IM_COL32(170, 230, 150, 255));
					ImGui::Dummy(ImVec2(16, 16));
					ImGui::TableNextColumn();
					if (ImGui::Selectable(v.name.c_str(), i == m_pv_sel, ImGuiSelectableFlags_SpanAllColumns)) {
						if (i != m_pv_sel)
							picked = true;
						m_pv_sel = i;
					}
					if (m_pv_scroll && i == m_pv_sel) {
						ImGui::SetScrollHereY(0.5f);
						m_pv_scroll = false;
					}
					ImGui::TableNextColumn();
					if (v.msb >= 0)
						ImGui::TextDisabled("%d/%d/%d", v.msb, v.lsb, v.prog + 1);
					else
						ImGui::TextDisabled("-");
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%d", v.elements);
					ImGui::PopID();
				}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	if (picked)
		for (bool &b : m_pv_on)
			b = true;

	// ---- 右: 選んだ音色
	const xg::preset_voice &v = m_pv_list[size_t(m_pv_sel)];
	const int n = v.elements;
	ImGui::BeginChild("pv_right", ImVec2(0, 0));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		u16 rows[16];
		dl->AddRectFilled(p, ImVec2(p.x + 56, p.y + 56), IM_COL32(16, 20, 26, 255), 4.0f);
		if (vr->icon_rows(v.icon, rows))
			draw_icon(dl, ImVec2(p.x + 4, p.y + 4), 3.0f, rows, IM_COL32(170, 230, 150, 255));
		ImGui::Dummy(ImVec2(56, 56));
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::TextUnformatted(v.name.c_str());
		if (v.msb >= 0)
			ImGui::TextDisabled(UI_TEXT(pv_bank_fmt, "MSB %d  LSB %d  program %d  -  %d elements"), v.msb, v.lsb, v.prog + 1, n);
		else
			ImGui::TextDisabled(UI_TEXT(pv_inner_fmt, "not selectable from an XG bank (used inside kits or other modes)  -  %d elements"), n);
		ImGui::EndGroup();
	}

	// 要素の表。行 = 項目、列 = 要素
	const u8 *el[4] = {};
	for (int e = 0; e < n; e++)
		el[e] = rom + v.rec + 12 + u32(e) * 84;
	if (ImGui::BeginTable("pv_els", 1 + n, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
	                                       ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthFixed, fs * 13.0f);
		for (int e = 0; e < n; e++) {
			char h[32];
			std::snprintf(h, sizeof(h), UI_TEXT(pv_el_fmt, "Element %d"), e + 1);
			ImGui::TableSetupColumn(h);
		}
		ImGui::TableHeadersRow();
		auto row = [&](const char *label, const std::function<void(int)> &cell) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", label);
			for (int e = 0; e < n; e++) {
				ImGui::TableNextColumn();
				ImGui::PushID(e);
				cell(e);
				ImGui::PopID();
			}
		};
		row(UI_TEXT(pv_row_play, "Play"), [&](int e) {
			ImGui::Checkbox("##on", &m_pv_on[e]);
			// 図の枠と同じ色の印
			ImGui::SameLine();
			const ImVec2 q = ImGui::GetCursorScreenPos();
			const float h = ImGui::GetFrameHeight();
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x, q.y + h * 0.25f), ImVec2(q.x + h * 1.2f, q.y + h * 0.75f), el_color(e, 255), 2.0f);
			ImGui::Dummy(ImVec2(h * 1.2f, h));
		});
		row(UI_TEXT(pv_row_wave, "Wave"), [&](int e) {
			const int set = xg::nv::wave_set(el[e]);
			char b[24];
			std::snprintf(b, sizeof(b), "W%d", set);
			if (ImGui::SmallButton(b)) {
				// 内蔵ウェーブのタブで、この波形を開く
				m_rw_sel = set;
				m_rw_zone = 0;
				m_rw_scroll = true;
				m_goto_tab = 2;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n%s", romwave_label(set).c_str(), UI_TEXT(pv_wave_tip, "Click to open this wave in the Built-in waves tab"));
		});
		row(UI_TEXT(pv_row_keys, "Keys"), [&](int e) { ImGui::Text("%d - %d", el[e][4], el[e][5]); });
		row(UI_TEXT(pv_row_vel, "Velocity"), [&](int e) { ImGui::Text("%d - %d", el[e][6], el[e][7]); });
		row(UI_TEXT(pv_row_level, "Level"), [&](int e) { ImGui::Text("%d", el[e][59]); });
		row(UI_TEXT(pv_row_pan, "Pan"), [&](int e) { ImGui::TextUnformatted(pan_text(el[e][69] & 15).c_str()); });
		row(UI_TEXT(pv_row_pitch, "Pitch (semitones / cents)"), [&](int e) {
			ImGui::Text("%s / %s", signed_text(int(el[e][17]) - 0x40).c_str(), signed_text(int(el[e][18]) - 0x40).c_str());
		});
		row(UI_TEXT(pv_row_filter, "Cutoff / resonance"), [&](int e) { ImGui::Text("%d / %d", el[e][37], el[e][35]); });
		row(UI_TEXT(pv_row_aeg, "Amp EG rates A/D1/D2/R"), [&](int e) {
			ImGui::Text("%d / %d / %d / %d", el[e][73], el[e][74], el[e][75], el[e][76]);
		});
		row(UI_TEXT(pv_row_aeg_lv, "Amp EG levels 1/2"), [&](int e) { ImGui::Text("%d / %d", el[e][77], el[e][78]); });
		row(UI_TEXT(pv_row_lfo, "LFO shape / speed"), [&](int e) {
			static const char *const SHAPE[] = { "saw", "tri", "S&H" };
			ImGui::Text("%s / %d", el[e][9] < 3 ? SHAPE[el[e][9]] : "?", el[e][11]);
		});
		row(UI_TEXT(pv_row_lfo_depth, "LFO to pitch/filter/amp"), [&](int e) { ImGui::Text("%d / %d / %d", el[e][14], el[e][15], el[e][16]); });
		row(UI_TEXT(pv_row_delay, "Delayed start"), [&](int e) {
			if (el[e][72])
				ImGui::Text("%.0f ms", double(xg::nv::elem_delay(el[e])) / 44.1);
			else
				ImGui::TextDisabled("-");
		});
		ImGui::EndTable();
	}

	// ---- 鳴らす
	ImGui::Spacing();
	if (ImGui::SmallButton(UI_TEXT(pv_all, "All")))
		for (bool &b : m_pv_on)
			b = true;
	for (int e = 0; e < n; e++) {
		ImGui::SameLine();
		char b[40];
		std::snprintf(b, sizeof(b), UI_TEXT(pv_only_fmt, "Only %d"), e + 1);
		if (ImGui::SmallButton(b))
			for (int k = 0; k < 4; k++)
				m_pv_on[k] = k == e;
	}
	int mask = 0;
	for (int e = 0; e < n; e++)
		if (m_pv_on[e])
			mask |= 1 << e;

	// 鳴らす・止める。音色を借りた枠へ写し（鳴らす要素の印つき）、パート 1 でその枠を選んで鍵を押す。
	// 枠を選び直すのは、音色か鳴らす要素が替わったときだけ（毎回選び直すと前の音の余韻が切れる）。
	// **替わったら必ず選び直す**: firmware は選んだときに要素の数などを覚えるので、写しただけだと、前の音色に
	// 無かった要素が鳴らない（2 要素の音色のあとの 4 Way EP で、要素 3 が鳴らなかった）
	auto note_off = [&] {
		if (m_pv_held < 0)
			return;
		const std::vector<u8> msg = { 0x80, u8(m_pv_held), 0x40 };
		br.send(msg);
		m_pv_held = -1;
	};
	auto note_on = [&](int key, int vel) {
		note_off();
		const u32 rec = v.rec;
		if (!m_pv_keep)
			m_pv_keep = std::make_shared<std::vector<u8>>();
		auto keep = m_pv_keep;
		m_pv_borrowed = true;
		br.post([keep, rec, mask](mu2000 &mu) {
			if (keep->empty())
				mu.sampling_voice_raw(BORROW_SLOT, *keep);     // 借りる前の中身
			std::string err;
			mu.sampling_copy_preset(BORROW_SLOT, rec, mask, err);
			return err;
		});
		std::vector<u8> msg;
		if (!m_pv_selected || m_pv_sel_rec != rec || m_pv_sel_mask != mask)
			msg = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x01, 0xc0, 0x7f };
		m_pv_selected = true;
		m_pv_sel_rec = rec;
		m_pv_sel_mask = mask;
		m_pv_held = key;
		msg.insert(msg.end(), { 0x90, u8(key), u8(vel) });
		br.send(msg);
	};

	// ---- 鍵 × 強さの図。横 = 鍵（左が低い）、縦 = 強さ（上が強い）。色の枠 = 各要素が鳴る範囲。
	// 押した所の鍵と強さで鳴らす。押したまま動かすと、鍵が替わるたびに鳴らし直す
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(fs * 9.0f, std::min(fs * 16.0f, ImGui::GetContentRegionAvail().y - fs * 7.0f)));
		ImGui::InvisibleButton("pv_map", sz);
		const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
		auto key_x = [&](float k) { return p.x + sz.x * k / 128.0f; };
		auto vel_y = [&](float vv) { return p.y + sz.y * (128.0f - vv) / 127.0f; };   // 強さ 1 が下の端、127 の上の辺が上の端
		dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255), 4.0f);
		// 黒鍵の列をうっすら、オクターブの線と名前（60 = C3）
		for (int k = 0; k < 128; k++) {
			const int pc = k % 12;
			if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
				dl->AddRectFilled(ImVec2(key_x(float(k)), p.y), ImVec2(key_x(float(k + 1)), p.y + sz.y), IM_COL32(255, 255, 255, 7));
			if (pc == 0) {
				dl->AddLine(ImVec2(key_x(float(k)), p.y), ImVec2(key_x(float(k)), p.y + sz.y), IM_COL32(90, 100, 120, k == 60 ? 255 : 120));
				char name[8];
				std::snprintf(name, sizeof(name), "C%d", k / 12 - 2);
				dl->AddText(ImVec2(key_x(float(k)) + 3, p.y + sz.y - fs - 2), IM_COL32(130, 140, 160, 255), name);
			}
		}
		for (int vv : { 32, 64, 96 })
			dl->AddLine(ImVec2(p.x, vel_y(float(vv))), ImVec2(p.x + sz.x, vel_y(float(vv))), IM_COL32(90, 100, 120, 70));
		// 要素の枠。鳴らさない要素は薄く
		for (int e = 0; e < n; e++) {
			const float inset = 2.0f + float(e) * 2.5f;      // 同じ範囲の枠が重なっても見分けられるように少しずつ内へ
			const ImVec2 a(key_x(float(el[e][4])) + inset, vel_y(float(el[e][7]) + 1.0f) + inset);
			const ImVec2 b(key_x(float(el[e][5]) + 1.0f) - inset, vel_y(float(el[e][6])) - inset);
			if (b.x <= a.x || b.y <= a.y)
				continue;
			dl->AddRectFilled(a, b, el_color(e, m_pv_on[e] ? 38 : 10), 3.0f);
			dl->AddRect(a, b, el_color(e, m_pv_on[e] ? 220 : 70), 3.0f, 0, m_pv_on[e] ? 1.5f : 1.0f);
			char num[4];
			std::snprintf(num, sizeof(num), "%d", e + 1);
			dl->AddText(ImVec2(a.x + 4 + float(e) * fs * 0.8f, a.y + 2), el_color(e, m_pv_on[e] ? 255 : 110), num);
		}
		dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + sz.x + 1, p.y + sz.y + 1), IM_COL32(110, 125, 150, 255), 4.0f, 0, 1.5f);
		if (active || hovered) {
			const ImVec2 m = ImGui::GetIO().MousePos;
			const int key = std::clamp(int((m.x - p.x) / sz.x * 128.0f), 0, 127);
			const int vel = std::clamp(int(128.0f - (m.y - p.y) / sz.y * 127.0f), 1, 127);
			if (active) {
				m_pv_key = key;
				m_pv_vel = vel;
				if (ImGui::IsItemActivated() || key != m_pv_held)
					note_on(key, vel);
			} else {
				static const char *const NOTE[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
				ImGui::SetTooltip(UI_TEXT(pv_map_tip_fmt, "key %d (%s%d)  velocity %d\nClick to play here; drag to slide across keys"), key, NOTE[key % 12], key / 12 - 2, vel);
			}
		}
		if (ImGui::IsItemDeactivated())
			note_off();
		// いまの鍵と強さ
		const ImVec2 c(key_x(float(m_pv_key) + 0.5f), vel_y(float(m_pv_vel) + 0.5f));
		dl->AddLine(ImVec2(c.x, p.y), ImVec2(c.x, p.y + sz.y), IM_COL32(255, 255, 255, 50));
		dl->AddLine(ImVec2(p.x, c.y), ImVec2(p.x + sz.x, c.y), IM_COL32(255, 255, 255, 50));
		dl->AddCircleFilled(c, m_pv_held >= 0 ? 5.0f : 3.5f, m_pv_held >= 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 210, 230, 200));
		ImGui::TextDisabled("%s", UI_TEXT(pv_map_note, "Across = key (low on the left), up = velocity. Coloured frames show where each element plays. Click anywhere to hear that key and velocity."));
	}

	ImGui::Button(UI_TEXT(smp_audition, "Hold to play"), ImVec2(fs * 9, 0));
	const bool hold_on = ImGui::IsItemActivated(), hold_off = ImGui::IsItemDeactivated();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(pv_play_tip, "Plays the checked elements through the real tone generator while held. It borrows the last sample voice (Bank# 1, number 128) and part 1: the voice is copied there with only the checked elements switched on. What that slot held is put back when you leave this tab or close the window."));
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_audition_key, "Key"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 6);
	if (ImGui::InputInt("##pvkey", &m_pv_key))
		m_pv_key = std::clamp(m_pv_key, 0, 127);
	ImGui::SameLine();
	static const char *const NOTE[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	ImGui::TextDisabled("%s%d", NOTE[m_pv_key % 12], m_pv_key / 12 - 2);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(pv_vel, "Velocity"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 8);
	ImGui::SliderInt("##pvvel", &m_pv_vel, 1, 127);
	// どの要素もこの鍵・強さでは鳴らない、を知らせる
	{
		bool any = false;
		for (int e = 0; e < n; e++)
			if (m_pv_on[e] && m_pv_key >= el[e][4] && m_pv_key <= el[e][5] && m_pv_vel >= el[e][6] && m_pv_vel <= el[e][7])
				any = true;
		if (!any)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", UI_TEXT(pv_silent, "No checked element plays at this key and velocity (see the Keys and Velocity rows)."));
	}
	if (hold_on)
		note_on(m_pv_key, m_pv_vel);
	if (hold_off)
		note_off();

	// ---- サンプル音色へ写す
	ImGui::Spacing();
	char copy[96];
	std::snprintf(copy, sizeof(copy), UI_TEXT(pv_copy_fmt, "Copy to sample voice Bank# %d number %d"), m_bank, m_pgm);
	if (ImGui::Button(copy)) {
		const int slot = m_bank * 128 + (m_pgm - 1);
		const u32 rec = v.rec;
		br.post([slot, rec](mu2000 &mu) {
			std::string err;
			mu.sampling_copy_preset(slot, rec, -1, err);
			return err;
		});
		m_loaded_slot = -1;       // 音色のタブで読み直す
		m_dirty = false;
		m_goto_tab = 1;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(pv_copy_tip, "Copies this whole voice (every element, with all its settings) into the sample voice chosen in the Voice tab, replacing what is there, and goes to that tab. From there it can be edited like any sample voice and its waves swapped for your own samples."));
	ImGui::EndChild();
}

} // namespace ui
