// license:BSD-3-Clause
//
// サンプリングの窓の「内蔵ウェーブ」のタブ（sampling_editor::romwave_pane）。
// 内蔵の波形の組（0-502）を一覧にして、選んだものを**そのまま**鳴らし、波形を出し、
// どの音色・どのドラムの打が使っているかを見せる（xg/wave_catalog.h）。
// 気に入ったものは、音色のタブで編集している要素に使える。
//
// 鳴らすのは波形そのもの（フィルターもエンベロープも通さない、録った高さのまま）。音源の声は使わず、
// 試聴の道（mu2000::preview_pcm）で鳴らすので、パートの設定は何も変わらない。

#include "sampling_editor.h"

#include "xg/voices.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ui {

namespace {

// 実機の液晶の楽器の絵（16 × 16）。px は 1 点の大きさ
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

const char *format_name(int f)
{
	switch (f) {
	case 0: return "16 bit";
	case 1: return "12 bit";
	case 2: return "8 bit";
	}
	return UI_TEXT(rw_fmt_packed, "8 bit packed");
}

// 長さ。1 周期だけの波形（数十サンプル）から数秒のものまであるので、サンプル数と、短いものはミリ秒・長いものは秒で
std::string length_text(u32 frames)
{
	char b[64];
	const double sec = double(frames) / 44100.0;
	if (sec < 0.1)
		std::snprintf(b, sizeof(b), "%u smp (%.2f ms)", frames, sec * 1000.0);
	else if (sec < 1.0)
		std::snprintf(b, sizeof(b), "%u smp (%.0f ms)", frames, sec * 1000.0);
	else
		std::snprintf(b, sizeof(b), "%u smp (%.2f s)", frames, sec);
	return b;
}

} // namespace

// 組の名前代わり（番号と、使っている音色。音色が無ければドラムの打）
std::string sampling_editor::romwave_label(int w)
{
	std::string s = "W" + std::to_string(w);
	if (w < 0 || w >= int(m_rw_cat.size()))
		return s;
	const xg::wave_set_info &i = m_rw_cat[size_t(w)];
	size_t n = 0;
	for (const xg::wave_voice_use &u : i.voices) {
		if (n == 4)
			break;
		s += (n++ ? ", " : "  ") + u.name;
	}
	for (const xg::wave_drum_use &d : i.drums) {
		if (n == 4)
			break;
		s += (n++ ? ", " : "  ") + d.key_name;
	}
	if (i.voices.size() + i.drums.size() > 4)
		s += ", ...";
	return s;
}

void sampling_editor::romwave_build()
{
	if (m_rw_built)
		return;
	const xg::voice_rom *vr = xgui::voices();
	if (!vr || !vr->ok())
		return;
	m_rw_cat = xg::wave_catalog(*vr);
	m_rw_built = true;
}

void sampling_editor::romwave_pane(bridge &br)
{
	m_rw_drawn = true;
	romwave_build();
	const xg::voice_rom *vr = xgui::voices();
	if (!m_rw_built || !vr) {
		ImGui::TextDisabled("%s", UI_TEXT(smp_not_ready, "Waiting for the MU2000 to start..."));
		return;
	}
	const float fs = ImGui::GetFontSize();
	const int count = int(m_rw_cat.size());
	m_rw_sel = std::clamp(m_rw_sel, 0, count - 1);

	// ---- 取り出しを頼んだ波形が届いたか
	if (m_rw_job && m_rw_job->done.load(std::memory_order_acquire)) {
		if (m_rw_job->set == m_rw_sel && m_rw_job->zone == m_rw_zone) {
			m_rw_pcm = std::move(m_rw_job->pcm);
			m_rw_pcm_set = m_rw_job->set;
			m_rw_pcm_zone = m_rw_job->zone;
			if (m_rw_play_wanted)
				m_rw_start = true;
		}
		m_rw_job.reset();
		m_rw_play_wanted = false;
	}

	// ---- 左: 絞り込みと一覧
	const float left_w = std::min(fs * 24.0f, ImGui::GetContentRegionAvail().x * 0.45f);
	bool picked = false;
	ImGui::BeginChild("rw_left", ImVec2(left_w, 0));
	{
		ImGui::SetNextItemWidth(fs * 11);
		ImGui::InputTextWithHint("##find", UI_TEXT(rw_find, "Voice, drum or W number"), m_rw_find, sizeof(m_rw_find));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		const std::string kinds = std::string(UI_TEXT(rw_kind_all, "All")) + '\0' + UI_TEXT(rw_kind_voice, "Used by voices") + '\0' +
		                          UI_TEXT(rw_kind_drum, "Drums only") + '\0' + UI_TEXT(rw_kind_unused, "Not used") + '\0';
		ImGui::Combo("##kind", &m_rw_kind, kinds.c_str());
		const std::string want = lower(m_rw_find);

		std::vector<int> shown;
		shown.reserve(size_t(count));
		for (int w = 0; w < count; w++) {
			const xg::wave_set_info &i = m_rw_cat[size_t(w)];
			if (m_rw_kind == 1 && i.voices.empty())
				continue;
			if (m_rw_kind == 2 && (!i.voices.empty() || i.drums.empty()))
				continue;
			if (m_rw_kind == 3 && i.used())
				continue;
			if (!want.empty()) {
				bool hit = lower("W" + std::to_string(w)) == want || std::to_string(w) == want;
				for (size_t k = 0; !hit && k < i.voices.size(); k++)
					hit = lower(i.voices[k].name).find(want) != std::string::npos;
				for (size_t k = 0; !hit && k < i.drums.size(); k++)
					hit = lower(i.drums[k].key_name).find(want) != std::string::npos ||
					      lower(i.drums[k].kit).find(want) != std::string::npos;
				if (!hit)
					continue;
			}
			shown.push_back(w);
		}
		ImGui::TextDisabled(UI_TEXT(rw_count_fmt, "%d of %d waves"), int(shown.size()), count);

		if (ImGui::BeginTable("rw_list", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
		                      ImVec2(0, 0))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed, 18.0f);
			ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, fs * 2.8f);
			ImGui::TableSetupColumn(UI_TEXT(rw_col_used, "Used by"));
			ImGui::TableHeadersRow();
			// 上下の矢印で前後へ（一覧に的があるとき）
			int step = 0;
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsAnyItemActive()) {
				if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
					step = 1;
				if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
					step = -1;
			}
			if (step && !shown.empty()) {
				auto it = std::find(shown.begin(), shown.end(), m_rw_sel);
				int at = it == shown.end() ? 0 : int(it - shown.begin()) + step;
				at = std::clamp(at, 0, int(shown.size()) - 1);
				if (shown[size_t(at)] != m_rw_sel) {
					m_rw_sel = shown[size_t(at)];
					picked = true;
					m_rw_scroll = true;
				}
			}
			ImGuiListClipper clip;
			clip.Begin(int(shown.size()), std::max(ImGui::GetTextLineHeightWithSpacing(), 18.0f));
			if (m_rw_scroll) {
				auto it = std::find(shown.begin(), shown.end(), m_rw_sel);
				if (it != shown.end())
					clip.IncludeItemByIndex(int(it - shown.begin()));
			}
			while (clip.Step())
				for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
					const int w = shown[size_t(r)];
					const xg::wave_set_info &i = m_rw_cat[size_t(w)];
					ImGui::PushID(w);
					ImGui::TableNextRow(0, 18.0f);
					ImGui::TableNextColumn();
					u16 rows[16];
					if (vr->icon_rows(i.icon, rows))
						draw_icon(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 1.0f, rows,
						          i.icon == xg::voice_rom::ICON_DRUM ? IM_COL32(255, 200, 130, 255) : IM_COL32(170, 230, 150, 255));
					ImGui::Dummy(ImVec2(16, 16));
					ImGui::TableNextColumn();
					char num[16];
					std::snprintf(num, sizeof(num), "W%d", w);
					if (ImGui::Selectable(num, w == m_rw_sel, ImGuiSelectableFlags_SpanAllColumns)) {
						if (w != m_rw_sel)
							picked = true;
						else
							m_rw_play_again = true;       // 同じ行をもう 1 度押したら鳴らし直す
						m_rw_sel = w;
					}
					if (m_rw_scroll && w == m_rw_sel) {
						ImGui::SetScrollHereY(0.5f);
						m_rw_scroll = false;
					}
					ImGui::TableNextColumn();
					std::string used;
					size_t n = 0;
					for (const xg::wave_voice_use &u : i.voices) {
						if (n == 3)
							break;
						used += (n++ ? ", " : "") + u.name;
					}
					for (const xg::wave_drum_use &d : i.drums) {
						if (n == 3)
							break;
						used += (n++ ? ", " : "") + d.key_name;
					}
					if (i.voices.size() + i.drums.size() > 3)
						used += " ...";
					if (used.empty())
						ImGui::TextDisabled("%s", UI_TEXT(rw_unused, "(not used)"));
					else if (i.voices.empty())
						ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.6f, 1.0f), "%s", used.c_str());
					else
						ImGui::TextUnformatted(used.c_str());
					ImGui::PopID();
				}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// ---- 右: 選んだ組
	const xg::wave_set_info &info = m_rw_cat[size_t(m_rw_sel)];
	if (picked)
		m_rw_zone = 0;
	m_rw_zone = std::clamp(m_rw_zone, 0, std::max(0, int(info.zones.size()) - 1));
	ImGui::BeginChild("rw_right", ImVec2(0, 0));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	{
		// 絵（3 倍）と番号
		const ImVec2 p = ImGui::GetCursorScreenPos();
		u16 rows[16];
		const bool has_icon = vr->icon_rows(info.icon, rows);
		dl->AddRectFilled(p, ImVec2(p.x + 56, p.y + 56), IM_COL32(16, 20, 26, 255), 4.0f);
		if (has_icon)
			draw_icon(dl, ImVec2(p.x + 4, p.y + 4), 3.0f, rows,
			          info.icon == xg::voice_rom::ICON_DRUM ? IM_COL32(255, 200, 130, 255) : IM_COL32(170, 230, 150, 255));
		ImGui::Dummy(ImVec2(56, 56));
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::Text("W%d", m_rw_sel);
		ImGui::TextDisabled(UI_TEXT(rw_summary_fmt, "%d key zones, used by %d voices and %d drum sounds"), int(info.zones.size()),
		                    int(info.voices.size()), int(info.drums.size()));
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", UI_TEXT(rw_noname, "Waves have no names in the ROM; the names here are the voices that use them."));
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
	}

	// 使っている音色とドラムの打（横に 2 つ）
	const float list_h = fs * 8.5f;
	const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	ImGui::BeginChild("rw_voices", ImVec2(half, list_h), ImGuiChildFlags_Borders);
	ImGui::TextUnformatted(UI_TEXT(rw_voices, "Voices that use it"));
	ImGui::Separator();
	if (info.voices.empty())
		ImGui::TextDisabled("%s", UI_TEXT(rw_none, "(none)"));
	for (const xg::wave_voice_use &u : info.voices) {
		if (u.msb >= 0) {
			ImGui::TextUnformatted(u.name.c_str());
			ImGui::SameLine(fs * 7.0f);
			ImGui::TextDisabled("MSB %d  LSB %d  #%d", u.msb, u.lsb, u.prog + 1);
		} else {
			ImGui::TextDisabled("%s", u.name.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", UI_TEXT(rw_inner_tip, "A voice record that no XG bank selects directly (used inside kits or other modes)"));
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("rw_drums", ImVec2(0, list_h), ImGuiChildFlags_Borders);
	ImGui::TextUnformatted(UI_TEXT(rw_drums, "Drum sounds that use it"));
	ImGui::Separator();
	if (info.drums.empty())
		ImGui::TextDisabled("%s", UI_TEXT(rw_none, "(none)"));
	for (const xg::wave_drum_use &d : info.drums) {
		ImGui::TextUnformatted(d.key_name.c_str());
		ImGui::SameLine(fs * 8.0f);
		if (d.more_kits)
			ImGui::TextDisabled(UI_TEXT(rw_kit_more_fmt, "%s key %d (+%d kits)"), d.kit.c_str(), d.key, d.more_kits);
		else
			ImGui::TextDisabled(UI_TEXT(rw_kit_fmt, "%s key %d"), d.kit.c_str(), d.key);
	}
	ImGui::EndChild();

	// 鍵の区切り
	bool zone_picked = false;
	if (info.zones.size() > 1) {
		const float zh = std::min(fs * 7.5f, ImGui::GetTextLineHeightWithSpacing() * float(info.zones.size() + 1) + 6.0f);
		if (ImGui::BeginTable("rw_zones", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
		                      ImVec2(0, zh))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(UI_TEXT(rw_col_keys, "Keys"));
			ImGui::TableSetupColumn(UI_TEXT(rw_col_base, "Root key"));
			ImGui::TableSetupColumn(UI_TEXT(rw_col_len, "Length"));
			ImGui::TableSetupColumn(UI_TEXT(rw_col_loop, "Loop"));
			ImGui::TableSetupColumn(UI_TEXT(rw_col_fmt, "Format"));
			ImGui::TableHeadersRow();
			for (int z = 0; z < int(info.zones.size()); z++) {
				const xg::wave_zone &zn = info.zones[size_t(z)];
				ImGui::PushID(z);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char keys[32];
				std::snprintf(keys, sizeof(keys), "%d - %d", zn.key_lo, zn.key_hi);
				if (ImGui::Selectable(keys, z == m_rw_zone, ImGuiSelectableFlags_SpanAllColumns)) {
					if (z != m_rw_zone)
						zone_picked = true;
					else
						m_rw_play_again = true;
					m_rw_zone = z;
				}
				ImGui::TableNextColumn();
				ImGui::Text("%d", zn.base_key);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(length_text(zn.frames()).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(zn.backwards() ? UI_TEXT(rw_loop_back, "backwards")
				                       : zn.loops() ? UI_TEXT(rw_loop_yes, "loops") : UI_TEXT(rw_loop_no, "one shot"));
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(format_name(zn.format()));
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- 波形を頼む
	const xg::wave_zone *zone = info.zones.empty() ? nullptr : &info.zones[size_t(m_rw_zone)];
	const bool have = zone && m_rw_pcm_set == m_rw_sel && m_rw_pcm_zone == m_rw_zone;
	if (zone && !have && !m_rw_job) {
		auto job = std::make_shared<rw_job>();
		job->set = m_rw_sel;
		job->zone = m_rw_zone;
		const xg::wave_zone z = *zone;
		m_rw_job = job;
		br.post([job, z](mu2000 &mu) {
			job->pcm = mu.rom_wave_pcm(z.start, z.loop, z.address);
			job->done.store(true, std::memory_order_release);
			return std::string();
		});
	}
	if (picked || zone_picked) {
		// 前の波形を鳴らしっぱなしにしない
		if (m_rw_playing && m_view.preview_number == -1)
			br.post([](mu2000 &mu) {
				mu.preview_stop();
				return std::string();
			});
		m_rw_playing = false;
		m_rw_start = false;
		// 選んだら鳴らす（波形が届いてから）
		m_rw_play_wanted = m_rw_auto;
	}
	if (m_rw_play_again) {
		m_rw_play_again = false;
		if (have)
			m_rw_start = true;
	}

	// ---- 鳴らす・止める・使う
	const bool playing = m_rw_playing && m_view.preview_number == -1;
	if (m_rw_playing && m_view.preview_number != -1 && ImGui::GetTime() > m_rw_play_at + 0.5)
		m_rw_playing = false;                 // 1 度きりの波形が鳴り終わった
	ImGui::BeginDisabled(!zone);
	if (!playing) {
		if (ImGui::Button(UI_TEXT(smp_play, "Play"), ImVec2(fs * 6, 0))) {
			if (have)
				m_rw_start = true;
			else
				m_rw_play_wanted = true;
		}
	} else if (ImGui::Button(UI_TEXT(smp_play_stop, "Stop playing"), ImVec2(fs * 6, 0))) {
		br.post([](mu2000 &mu) {
			mu.preview_stop();
			return std::string();
		});
		m_rw_playing = false;
		m_rw_start = false;
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(rw_play_tip, "Plays the wave alone, as recorded: no filter, no envelope, at its root key. A looping wave goes on until stopped."));
	ImGui::SameLine();
	ImGui::Checkbox(UI_TEXT(rw_auto, "Play when picked"), &m_rw_auto);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(rw_auto_tip, "Plays each wave as you pick it. With the list focused, the up and down arrow keys step through the waves."));
	ImGui::SameLine();
	char use[64];
	std::snprintf(use, sizeof(use), UI_TEXT(rw_use_fmt, "Use in element %d"), m_cur_el + 1);
	if (ImGui::Button(use)) {
		m_sample = 0;
		m_rom_wave = m_rw_sel;
		m_dirty = true;
		m_goto_tab = 1;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(rw_use_tip, "Sets this wave on the element being edited in the Voice tab and goes there. There, pitch, filter, envelope and LFO apply to it like a sample."));
	if (zone) {
		ImGui::TextDisabled(UI_TEXT(rw_zone_fmt, "keys %d-%d  root %d  %s  %s  %s"), zone->key_lo, zone->key_hi, zone->base_key,
		                    length_text(zone->frames()).c_str(),
		                    zone->backwards() ? UI_TEXT(rw_loop_back, "backwards")
		                    : zone->loops() ? UI_TEXT(rw_loop_yes, "loops") : UI_TEXT(rw_loop_no, "one shot"),
		                    format_name(zone->format()));
	}
	if (m_rw_start && have && !m_rw_pcm.empty()) {
		m_rw_start = false;
		std::vector<s16> pcm = m_rw_pcm;
		const u32 loop = zone->loops() ? zone->loop_at() : ~0u;
		br.post([pcm, loop](mu2000 &mu) mutable {
			mu.preview_pcm(std::move(pcm), loop);
			return std::string();
		});
		m_rw_playing = true;
		m_rw_play_at = ImGui::GetTime();
	}

	// ---- 波形。上 = 全体（紫の線がループの頭、白い線が鳴らしている位置）、下 = ループの頭のまわりを拡大
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float total_h = std::max(fs * 8.0f, avail.y - 4.0f);
	const float top_h = total_h * 0.62f, bot_h = total_h - top_h - 6.0f;
	auto frame = [&](ImVec2 p, ImVec2 sz) {
		dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255), 4.0f);
		dl->AddLine(ImVec2(p.x, p.y + sz.y * 0.5f), ImVec2(p.x + sz.x, p.y + sz.y * 0.5f), IM_COL32(80, 90, 110, 255));
		dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + sz.x + 1, p.y + sz.y + 1), IM_COL32(110, 125, 150, 255), 4.0f, 0, 1.5f);
	};
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const ImVec2 sz(avail.x, top_h);
		ImGui::Dummy(sz);
		frame(p, sz);
		if (have && !m_rw_pcm.empty()) {
			const size_t n = m_rw_pcm.size();
			const float mid = p.y + sz.y * 0.5f, halfh = sz.y * 0.5f - 2.0f;
			const int cols = std::max(1, int(sz.x));
			if (n < size_t(cols) * 2 && n > 1) {
				// 点の数より短い波形は 1 サンプルずつ線で結ぶ
				ImVec2 prev;
				for (size_t k = 0; k < n; k++) {
					const ImVec2 q(p.x + sz.x * float(k) / float(n - 1), mid - halfh * float(m_rw_pcm[k]) / 32768.0f);
					if (k)
						dl->AddLine(prev, q, IM_COL32(110, 200, 255, 255));
					prev = q;
				}
			}
			for (int x = 0; n >= size_t(cols) * 2 && x < cols; x++) {
				const size_t a = size_t(x) * n / size_t(cols), b = std::max(a + 1, size_t(x + 1) * n / size_t(cols));
				int lo = 32767, hi = -32768;
				for (size_t k = a; k < b && k < n; k++) {
					lo = std::min(lo, int(m_rw_pcm[k]));
					hi = std::max(hi, int(m_rw_pcm[k]));
				}
				if (hi < lo)
					continue;
				const float y0 = mid - halfh * float(hi) / 32768.0f;
				dl->AddLine(ImVec2(p.x + float(x) + 0.5f, y0),
				            ImVec2(p.x + float(x) + 0.5f, std::max(mid - halfh * float(lo) / 32768.0f, y0 + 1.0f)),
				            IM_COL32(110, 200, 255, 255));
			}
			if (zone->loops()) {
				const float xl = p.x + sz.x * float(double(zone->loop_at()) / double(n));
				dl->AddLine(ImVec2(xl, p.y), ImVec2(xl, p.y + sz.y), IM_COL32(200, 120, 255, 255), 2.0f);
			}
			if (playing) {
				const float xp = p.x + sz.x * float(std::min(1.0, double(m_view.preview_pos) / double(n)));
				dl->AddLine(ImVec2(xp, p.y), ImVec2(xp, p.y + sz.y), IM_COL32(255, 255, 255, 230), 1.5f);
			}
		} else if (zone) {
			dl->AddText(ImVec2(p.x + 8, p.y + 6), IM_COL32(150, 160, 180, 255), UI_TEXT(rw_loading, "Reading the wave..."));
		}
	}
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const ImVec2 sz(avail.x, std::max(fs * 2.0f, bot_h));
		ImGui::Dummy(sz);
		frame(p, sz);
		if (have && !m_rw_pcm.empty()) {
			// ループする波形はループの 1 周（長ければ頭から 2000 サンプル）、1 度きりは頭の 2000 サンプル。1 サンプルずつ線で結ぶ
			const size_t n = m_rw_pcm.size();
			size_t a = zone->loops() ? std::min<size_t>(zone->loop_at(), n - 1) : 0;
			size_t len = std::min<size_t>(zone->loops() ? n - a : n, 2000);
			const float mid = p.y + sz.y * 0.5f, halfh = sz.y * 0.5f - 2.0f;
			if (len > 1) {
				ImVec2 prev;
				for (size_t k = 0; k < len; k++) {
					const ImVec2 q(p.x + sz.x * float(k) / float(len - 1), mid - halfh * float(m_rw_pcm[a + k]) / 32768.0f);
					if (k)
						dl->AddLine(prev, q, IM_COL32(150, 235, 170, 255));
					prev = q;
				}
			}
			char cap[96];
			std::snprintf(cap, sizeof(cap), zone->loops() ? UI_TEXT(rw_zoom_loop_fmt, "from the loop start, %d samples")
			                                              : UI_TEXT(rw_zoom_head_fmt, "the first %d samples"), int(len));
			dl->AddText(ImVec2(p.x + 8, p.y + 4), IM_COL32(150, 160, 180, 255), cap);
		}
	}
	ImGui::EndChild();
}

} // namespace ui
