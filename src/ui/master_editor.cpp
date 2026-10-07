// license:BSD-3-Clause

#include "board_view.h"
#include "master_editor.h"

#include "driver.h"
#include "fx_icons.h"
#include "overview.h"
#include "user_boards.h"
#include "ui/texts.h"
#include "xg_state.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui {

using namespace xgui;

namespace {

// 見出し 1 行。区画の頭に
void heading(const char *text)
{
	ImGui::TextUnformatted(text);
	ImGui::Separator();
}

// 選ぶ値（EQ の種類、バリエーションの接続など）の箱
void choice_combo(const char *id, const char *key, xg::model &m, bridge &br)
{
	const xg::param &p = P(key);
	int v = p.min;
	const bool known = m.get(p, 0, v);
	if (ImGui::BeginCombo(id, known ? xg::format(p, v).c_str() : "--")) {
		for (int i = p.min; i <= p.max; i++)
			if (ImGui::Selectable(p.choices[i - p.min], known && i == v))
				br.send(m.set(p, 0, i));
		ImGui::EndCombo();
	}
	help_tip(key);
}

// エフェクトの種類の箱。品書きの形（分類 → 系統 → LSB 違い）で選ぶ
void type_combo(const char *id, const std::vector<xg::fx_type> &types, const char *key, xg::model &m, bridge &br)
{
	const xg::param &p = P(key);
	int type = 0;
	const bool known = m.get(p, 0, type);
	if (begin_fx_combo(id, known ? type : -1, ImGuiComboFlags_HeightLarge)) {
		int chosen = 0;
		if (fx_type_menu(types, known ? type : -1, chosen)) {
			br.send(m.set(p, 0, chosen));
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndCombo();
	}
}

// バリエーションを掛けるパートの箱（接続が INSERTION のとき）。64 パートの後ろに AD1・AD2、127 が OFF
void part_combo(const char *id, const char *key, xg::model &m, bridge &br)
{
	const xg::param &p = P(key);
	int part = 127;
	const bool known = m.get(p, 0, part);
	const std::string now = !known ? "--" : part < XG_PARTS + 2 ? part_name(part) : "OFF";
	if (ImGui::BeginCombo(id, now.c_str(), ImGuiComboFlags_HeightLarge)) {
		if (ImGui::Selectable("OFF", known && part >= XG_PARTS + 2))
			br.send(m.set(p, 0, 127));
		for (int i = 0; i < XG_PARTS + 2; i++) {
			if (i % 16 == 0)
				ImGui::Separator();
			if (ImGui::Selectable(part_name(i).c_str(), known && i == part))
				br.send(m.set(p, 0, i));
		}
		ImGui::EndCombo();
	}
}

} // namespace


void master_editor::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	(void)ram;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("master_editor", nullptr, wf);
	ImGui::PopStyleVar();

	float &zoom = master_zoom();
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
	const float fs = ImGui::GetFontSize();
	const ImGuiStyle &st = ImGui::GetStyle();

	// ---- 上の帯。表示の大きさと説明は右端へ
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("MASTER");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - fs * 18);
	if (ImGui::SmallButton("-"))
		set_master_zoom(zoom - 0.1f);
	ImGui::SameLine();
	ImGui::Text("%d%%", int(std::lround(zoom * 100)));
	ImGui::SameLine();
	if (ImGui::SmallButton("+"))
		set_master_zoom(zoom + 0.1f);
	ImGui::SameLine();
	help_checkbox();

	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float row_h = ImGui::GetFrameHeightWithSpacing();
	// 上の段は、左の区画（システム・SysEx・架空のボード）が巻かずに収まる行数で高さを決める
	// （システムエフェクトの表は 区画の見出し + 表の見出し + 7 行で、それより低い）
	const float top_h = std::min(avail.y * 0.6f, row_h * 12.0f + fs * 1.5f);

	// ---- 上の左: システム
	const float sys_w = std::min(fs * 22.0f, avail.x * 0.35f);
	if (ImGui::BeginChild("system", ImVec2(sys_w, top_h), ImGuiChildFlags_Borders)) {
		heading(UI_TEXT(me_sys, "System"));
		ImGui::PushItemWidth(-fs * 6.5f);
		param_slider("system.master_volume", 0, m, br);
		param_slider("system.master_tune", 0, m, br);
		param_slider("system.transpose", 0, m, br);
		ImGui::PopItemWidth();
		ImGui::Spacing();
		ImGui::TextDisabled("%s", UI_TEXT(me_tune_note, "Tune moves in 0.1 cent steps,\ntranspose in semitones"));
		ImGui::Spacing();
		sysex_pane(ram, br);
		ImGui::Spacing();
		board_pane(br);
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// ---- 上の右: システムエフェクト（リバーブ・コーラス・バリエーション）
	if (ImGui::BeginChild("effects", ImVec2(0, top_h), ImGuiChildFlags_Borders)) {
		heading(UI_TEXT(me_fx, "System effects"));
		const ImGuiTableFlags tf = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV;
		if (ImGui::BeginTable("fx", 4, tf)) {
			ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthFixed, fs * 6.5f);
			ImGui::TableSetupColumn(UI_TEXT(sys_reverb, "Reverb"));
			ImGui::TableSetupColumn(UI_TEXT(sys_chorus, "Chorus"));
			ImGui::TableSetupColumn(UI_TEXT(sys_variation, "Variation"));
			ImGui::TableHeadersRow();

			// 1 行ずつ。無い所は空けておく
			auto row = [&](const char *what, const char *rev, const char *cho, const char *var) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(what);
				for (const char *key : { rev, cho, var }) {
					ImGui::TableNextColumn();
					if (!key)
						continue;
					ImGui::SetNextItemWidth(-FLT_MIN);
					param_slider(key, 0, m, br, "##v");
				}
			};

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(UI_TEXT(fx_kind, "Type"));
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			type_combo("##revtype", xg::rev_types(), "reverb.type", m, br);
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			type_combo("##chotype", xg::cho_types(), "chorus.type", m, br);
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			type_combo("##vartype", xg::ins_types(), "variation.type", m, br);

			// 種類ごとのパラメータは、エフェクトの窓（インサーションと同じ窓の REV・CHO・VAR）で
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(UI_TEXT(me_params, "Parameters"));
			for (int slot : { 5, 6, 7 }) {
				ImGui::TableNextColumn();
				ImGui::PushID(slot);
				if (ImGui::Button(UI_TEXT(me_knobs_open, "Open knobs...")))
					request_fx(slot);
				ImGui::PopID();
			}

			row(UI_TEXT(me_back, "Return"), "reverb.return", "chorus.return", "variation.return");
			row(UI_TEXT(me_pan, "Pan"), "reverb.pan", "chorus.pan", "variation.pan");
			row(UI_TEXT(me_to_rev, "To reverb"), nullptr, "chorus.to_reverb", "variation.to_reverb");
			row(UI_TEXT(me_to_cho, "To chorus"), nullptr, nullptr, "variation.to_chorus");

			// バリエーションの接続。INSERTION のときは戻り量と送りは使われず、掛けるパートに直に入る
			int conn = 1;
			const bool known_conn = m.get(P("variation.connect"), 0, conn);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(UI_TEXT(fx_connect, "Connection"));
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			choice_combo("##varconn", "variation.connect", m, br);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(UI_TEXT(fx_part, "Part"));
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(known_conn && conn != 0);
			ImGui::SetNextItemWidth(-FLT_MIN);
			part_combo("##varpart", "variation.part", m, br);
			ImGui::EndDisabled();
			if (known_conn && conn != 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", UI_TEXT(me_insert_note, "Only used when connected as INSERTION"));
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();

	// ---- 下: マスター EQ。マルチパートのボードが挿さっているときは、タブで「ボードのパート」に切り替えられる
	bool show_board = false;
	if (board_multi_kind() && ImGui::BeginTabBar("bottom")) {
		if (ImGui::BeginTabItem(UI_TEXT(me_master_eq, "Master EQ")))
			ImGui::EndTabItem();
		if (ImGui::BeginTabItem(UI_TEXT(me_board_parts, "Board parts (port E)"))) {
			show_board = true;
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	if (show_board) {
		if (ImGui::BeginChild("boardparts", ImVec2(0, 0), ImGuiChildFlags_Borders))
			board_parts_pane(m, br);
		ImGui::EndChild();
	} else if (ImGui::BeginChild("eq", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(UI_TEXT(me_master_eq, "Master EQ"));
		ImGui::SameLine(0, fs * 1.5f);
		ImGui::TextUnformatted(UI_TEXT(fx_kind, "Type"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 8);
		choice_combo("##eqtype", "master_eq.type", m, br);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(me_eq_type_note, "Picking a type rewrites the 5 bands to that type's values"));
		for (int band : { 1, 5 }) {
			char key[24], label[32];
			std::snprintf(key, sizeof(key), "master_eq.shape%d", band);
			std::snprintf(label, sizeof(label), UI_TEXT(me_band_peak_fmt, "Make band %d peak"), band);
			int shape = 0;
			const bool known = m.get(P(key), 0, shape);
			bool peak = shape == 1;
			ImGui::SameLine(0, fs * 1.5f);
			ImGui::BeginDisabled(!known);
			if (ImGui::Checkbox(label, &peak))
				br.send(m.set(P(key), 0, peak ? 1 : 0));
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", band == 1 ? UI_TEXT(me_band_shape1, "Band 1 shape. Off is low-shelf, on is peak")
				                                  : UI_TEXT(me_band_shape5, "Band 5 shape. Off is high-shelf, on is peak"));
		}
		ImGui::Separator();

		// 棒の表（見出し + ゲイン・周波数・Q）の高さを残して、残りを特性の絵に
		const float table_h = row_h * 4.0f + st.CellPadding.y * 8.0f;
		const float plot_w = ImGui::GetContentRegionAvail().x;
		const float plot_h = std::max(fs * 5.0f, ImGui::GetContentRegionAvail().y - table_h - st.ItemSpacing.y);
		overview::master_eq_plot(m, br, plot_w, plot_h, true);

		if (ImGui::BeginTable("bands", 6, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthFixed, fs * 4.5f);
			for (int b = 1; b <= 5; b++) {
				char name[16];
				std::snprintf(name, sizeof(name), UI_TEXT(me_band_fmt, "Band %d"), b);
				ImGui::TableSetupColumn(name);
			}
			ImGui::TableHeadersRow();
			const char *const WHAT[3] = { UI_TEXT(me_w_gain, "Gain"), UI_TEXT(me_w_freq, "Freq"), UI_TEXT(me_w_q, "Q (width)") };
			static const char *const KEY[3] = { "master_eq.gain%d", "master_eq.freq%d", "master_eq.q%d" };
			for (int r = 0; r < 3; r++) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(WHAT[r]);
				for (int b = 1; b <= 5; b++) {
					ImGui::TableNextColumn();
					char key[24];
					std::snprintf(key, sizeof(key), KEY[r], b);
					ImGui::SetNextItemWidth(-FLT_MIN);
					param_slider(key, 0, m, br, "##v");
				}
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
	} else {
		ImGui::EndChild();
	}

	ImGui::PopFont();
	ImGui::End();
}

// ---- .syx の書き出し・読み込み（issue #35）
//
// 書き出し  いまの XG の値を SysEx にしてファイルへ。「既定と違うものだけ」なら、頭に XG System On を
//           置き、そのあと既定値（XG System On の直後の値）と違うところだけを並べる。曲の頭に
//           貼るのに向く。外すと全部（プラグインの「XG の値だけ」の状態と同じもの）
// 読み込み  ファイルの SysEx を 1 通ずつ音源へ流す。リセットの後は 200ms 待つ（実機も受け付けない間がある）
void master_editor::sysex_pane(const xg_snapshot &ram, bridge &br)
{
	const bool ready = ram.serial != 0;

	// 既定値ができたら書き出す
	if (m_export_waiting && br.have_defaults()) {
		m_export_waiting = false;
		const std::unique_ptr<xg_snapshot> base = std::make_unique<xg_snapshot>();
		br.read_defaults(*base);
		std::vector<u8> out = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
		const std::vector<u8> diff = setup_diff_messages(ram, *base);
		out.insert(out.end(), diff.begin(), diff.end());
		xgui::ask_save_file(std::move(out));
	}

	// 読み込んだものを流す。輪に入りきらなければ次のコマで続き
	std::vector<u8> opened;
	if (xgui::take_opened_file(opened)) {
		m_import = std::move(opened);
		m_import_at = 0;
		m_import_hold_until = 0;
		size_t n = 0;
		for (u8 b : m_import)
			n += b == 0xf0;
		char note[64];
		std::snprintf(note, sizeof(note), n ? UI_TEXT(note_sysex_busy_fmt, "Loading (%zu SysEx messages)") : UI_TEXT(note_sysex_idle, "No SysEx found"), n);
		xgui::set_file_note(note);
	}
	while (m_import_at < m_import.size() && br.audio_ms() >= m_import_hold_until) {
		if (m_import[m_import_at] != 0xf0) {        // SysEx の外は飛ばす
			m_import_at++;
			continue;
		}
		size_t end = m_import_at + 1;
		while (end < m_import.size() && m_import[end] != 0xf7 && !(m_import[end] & 0x80))
			end++;
		if (end >= m_import.size() || m_import[end] != 0xf7) {   // 閉じていない。そこから先を探し直す
			m_import_at = end;
			continue;
		}
		const size_t len = end + 1 - m_import_at;
		if (len >= 4096) {                         // 輪より大きいものは送れない
			m_import_at = end + 1;
			continue;
		}
		if (!br.send(m_import.data() + m_import_at, len))
			break;
		if (driver::is_reset(m_import.data() + m_import_at + 1, len - 2))
			m_import_hold_until = br.audio_ms() + 200;
		m_import_at = end + 1;
		if (m_import_at >= m_import.size())
			xgui::set_file_note(UI_TEXT(note_imported, "Imported"));
	}
	if (m_import_at >= m_import.size() && !m_import.empty()) {
		m_import.clear();
		m_import_at = 0;
	}

	ImGui::SeparatorText(UI_TEXT(me_sysex_title, "SysEx (.syx)"));
	const bool can = xgui::file_dialogs() && ready && !m_export_waiting && m_import.empty();
	ImGui::BeginDisabled(!can);
	if (ImGui::Button(UI_TEXT(me_export, "Export..."))) {
		if (!m_diff_only) {
			xgui::ask_save_file(setup_messages(ram));
		} else {
			m_export_waiting = true;
			br.request_defaults();              // できたら上で書き出す
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(me_import, "Import...")))
		xgui::ask_open_file();
	ImGui::EndDisabled();
	if (!xgui::file_dialogs() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(me_no_dialog, "No file dialog on this platform yet"));
	ImGui::Checkbox(UI_TEXT(me_diff_only, "Only non-defaults"), &m_diff_only);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(me_diff_only_tip, "On: start with XG System On, then only what differs from defaults (for pasting at a song start).\n"
		                                            "Off: write all XG values.\n"
		                                            "Defaults are read by saving the machine, playing XG System On and restoring.\n"
		                                            "Sound may glitch for a moment the first time"));
	if (m_export_waiting)
		ImGui::TextDisabled("%s", UI_TEXT(me_reading_defaults, "Reading defaults..."));
	else if (!xgui::file_note().empty())
		ImGui::TextDisabled("%s", xgui::file_note().c_str());
}

// 架空のプラグインボードを挿す・外す。実在しないボードを挿したことにして、選んだパートの MIDI で鳴らす。
// 音は MU のミキサーとエフェクトを通り、そのパートの音量・パン・リバーブ／コーラスの送りが効く（src/vboard.h）
void master_editor::board_pane(bridge &br)
{
	ImGui::SeparatorText(UI_TEXT(me_board_title, "Imaginary plug-in board"));
	const std::string kinds = std::string(UI_TEXT(me_board_none, "(none)")) + '\0' + UI_TEXT(me_board_fc, "FC board (8-bit console sounds)") + '\0' +
	                          UI_TEXT(me_board_fc16, "FC board, 16 parts on port E") + '\0' +
	                          UI_TEXT(me_board_dls, "DLS board, 16 parts on port E") + '\0' +
	                          UI_TEXT(me_board_user, "Your own board (waves you made)") + '\0' +
	                          UI_TEXT(me_board_user16, "Your own board, 16 parts on port E") + '\0';
	// 差込口は実機と同じ 3 つ（PLG-1〜3）。1 段ずつ
	for (int slot = 0; slot < mu2000::PLG_SLOTS; slot++) {
		ImGui::PushID(slot);
		board_slot_pane(br, slot, kinds);
		ImGui::PopID();
	}
}

// 差込口 1 つぶん。挿すボードと、挿すパート（1 パートのボード）。その下に、いまの様子
void master_editor::board_slot_pane(bridge &br, int slot, const std::string &kinds)
{
	const float fs = ImGui::GetFontSize();
	ImGui::AlignTextToFramePadding();
	ImGui::Text("PLG-%d", slot + 1);
	ImGui::SameLine();
	bool changed = false;
	ImGui::SetNextItemWidth(-fs * 6.5f);
	const bool kind_changed = ImGui::Combo("##board", &m_board_kind[slot], kinds.c_str());
	changed |= kind_changed;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(me_board_tip, "A board that never existed, plugged in for fun. As with a real board, it plays on the part chosen below while that part is on the board's bank (MSB 90, LSB 0; plugging it in here selects it), in place of that part's own voice. On any other bank the part plays its own voice. Plugging or unplugging restarts the MU, as boards go in with the power off. Its sound goes through the MU's mixer and effects: the part's volume, expression, pan and reverb / chorus sends apply. It answers the MU's plug-in board check: the MU lists it under UTIL > PLG, PartAssign there moves it, the display names its voices and [AUDITION] plays it.\n\nFC board, program change 1-16:\n 1 square (duty 1/2)   2 square (1/4)   3 square (1/8)   4 triangle\n 5 noise   6 metallic noise   7 duty sweep   8 octave arpeggio\n 9-16 the same, fading while held\nPitch bend and the mod wheel (vibrato) work. Up to 8 notes.\n\nThe 16-part FC board is a multi-part board, like the real PLG100-XG: it does not borrow a part. It is a tone generator of its own on a fifth MIDI port, port E, after the MU's ports A-D: 16 channels, each with its own program, volume (CC7), expression (CC11), pan (CC10) and reverb / chorus sends (CC91 / CC93) into the MU's effects. The MU only lists its name under UTIL > PLG; its parts are not on the display and its voices cannot be chosen from the panel (the same on a real MU). Play it from MIDI IN E (in the port menu), the fifth port of a MIDI file, or after the cable message F5 05.\n\nThe DLS board is the same kind of board with a different tone generator: it plays a DLS sound bank that you choose (for example Windows' gm.dls), 16 channels on port E, channel 10 for drums, voices picked by bank select and program change."));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	ImGui::BeginDisabled(mu2000::board_is_multi(m_board_kind[slot]));       // 16 パートのボードは本体のパートを借りない
	if (ImGui::InputInt("##boardpart", &m_board_part[slot])) {
		m_board_part[slot] = std::clamp(m_board_part[slot], 1, 64);
		changed = true;
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(me_board_part, "Part"));
	// 下の様子の文は、欄の幅で折り返す
	ImGui::PushTextWrapPos(0.0f);
	struct wrap_end { ~wrap_end() { ImGui::PopTextWrapPos(); } } wrap_guard;
	const double now = ImGui::GetTime();
	if (changed) {
		const int kind = m_board_kind[slot], part = m_board_part[slot] - 1;
		br.post([kind, part, slot](mu2000 &mu) {
			// 実機と同じで、ボードは「割り当てたパートでボードのバンクを選んだとき」だけ鳴る。ここで挿したときは、
			// 手間を省いてそのパートにボードのバンクを選んでおく。外した・動かしたときは、元のパートをふつうのバンクに戻す
			// （ボードのバンクのままだと、内蔵の音源では無音になる）
			const auto select = [&mu](int p, int msb, int lsb) {
				const u8 ch = u8(p % 16);
				for (int b : { 0xb0 | ch, 0, msb, 0xb0 | ch, 32, lsb, 0xc0 | ch, 0 })
					mu.midi_in(u8(b), p / 16);
			};
			const int old_kind = mu.virtual_board_kind(slot), old_part = mu.virtual_board_part(slot);
			// 挿す・外すときは、このあと本体を起動し直す（下の request_restart）。ボードのバンクは起動し終えてから選ぶ。
			// 外すときは、ボードのバンクのままだと内蔵の音源では無音なので、先にふつうのバンクへ戻しておく
			if (old_kind && mu.virtual_board_playing(slot) && (!kind || old_part != part))
				select(old_part, 0, 0);
			mu.set_virtual_board(kind, part, slot);
			if (kind && old_kind && old_part != part)
				select(part, mu2000::board_bank_msb(slot), mu2000::VBOARD_BANK_LSB);
			return std::string();
		});
		m_board_touched[slot] = now;
		m_board_seen[slot]->store(-1);
		// 挿した・外した: 実機と同じく電源を入れ直す。本体（firmware）がボードを探すのは起動のときだけで、
		// 見つけていないと液晶は Silence のまま、[AUDITION] の音もボードへ送ってこない
		if (kind_changed) {
			if (mu2000::board_is_multi(m_board_kind[slot]))
				for (int o = 0; o < mu2000::PLG_SLOTS; o++)
					if (o != slot && mu2000::board_is_multi(m_board_kind[o]))
						m_board_kind[o] = 0;
			m_board_booting[slot] = m_board_kind[slot] != 0;
			m_board_touched[slot] = now + 1.0;           // 起動し直しが始まるまでの古い返事を読まない
			br.request_restart();
		}
	}
	// MU の側の様子をときどき聞く。前の回から挿さったままのボードと、MU のメニューで変えたパートに欄を合わせる
	// （自分で変えた直後は、古い返事で戻さないよう少し待つ）
	if (now - m_board_asked[slot] > 0.5) {
		m_board_asked[slot] = now;
		br.post([seen = m_board_seen[slot], slot](mu2000 &mu) {
			seen->store((mu.virtual_board_assigned(slot) ? mu.virtual_board_part(slot) + 1 : 0) | (mu.virtual_board_known(slot) ? 0x100 : 0) |
			            (mu.virtual_board_playing(slot) ? 0x200 : 0) | (mu.midi_ready() ? 0x400 : 0) |
			            (mu.virtual_board_kind(slot) << 11));
			return std::string();
		});
	}
	const int seen = m_board_seen[slot]->load();
	if (m_board_booting[slot] && (seen < 0 || now - m_board_touched[slot] < 1.0 || (seen & 0x500) != 0x500)) {
		ImGui::TextDisabled("%s", UI_TEXT(me_board_booting, "Restarting the MU so that it finds the board..."));
		return;
	}
	if (seen < 0 || now - m_board_touched[slot] < 1.0)
		return;
	// 設定から挿さった状態で始まったとき（この窓はまだ「なし」のまま）
	if (!m_board_booting[slot] && !m_board_kind[slot] && (seen & 0x3800))
		m_board_kind[slot] = (seen >> 11) & 7;
	if (!m_board_kind[slot])
		return;
	// 起動し直しが済んだ（本体がボードを見つけて MIDI を受け始めた）。そのパートにボードのバンクを選ぶ
	if (m_board_booting[slot]) {
		m_board_booting[slot] = false;
		m_board_touched[slot] = now;
		m_board_seen[slot]->store(-1);
		br.post([slot](mu2000 &mu) {
			if (!mu.virtual_board_kind(slot) || mu2000::board_is_multi(mu.virtual_board_kind(slot)))
				return std::string();
			const int p = mu.virtual_board_part(slot);
			const u8 ch = u8(p % 16);
			for (int b : { 0xb0 | ch, 0, mu2000::board_bank_msb(slot), 0xb0 | ch, 32, int(mu2000::VBOARD_BANK_LSB), 0xc0 | ch, 0 })
				mu.midi_in(u8(b), p / 16);
			return std::string();
		});
		return;
	}
	if (m_board_kind[slot] == mu2000::VBOARD_DLS)
		board_dls_pane(br);
	if (m_board_kind[slot] == mu2000::VBOARD_USER || m_board_kind[slot] == mu2000::VBOARD_USER16)
		board_user_pane(br);
	if (mu2000::board_is_multi(m_board_kind[slot])) {
		ImGui::TextDisabled("%s", (seen & 0x100) ? UI_TEXT(me_board_port_e, "Listed under UTIL > PLG. Plays from MIDI port E (the fifth port): 16 channels")
		                                         : UI_TEXT(me_board_unknown, "The MU has not noticed it yet: click the POWER switch and restart the MU to list it under UTIL > PLG"));
		return;
	}
	if ((seen & 0xff) && (seen & 0xff) != m_board_part[slot])
		m_board_part[slot] = seen & 0xff;
	if ((seen & 0xff) && !(seen & 0x200))
		ImGui::TextDisabled(UI_TEXT(me_board_idle, "Silent now: that part is on another bank. Select bank MSB %d, LSB 0 there to hear the board"), mu2000::board_bank_msb(slot));
	if (!(seen & 0x100))
		ImGui::TextDisabled("%s", UI_TEXT(me_board_unknown, "The MU has not noticed it yet: click the POWER switch and restart the MU to list it under UTIL > PLG"));
	else if (!(seen & 0xff))
		ImGui::TextDisabled("%s", UI_TEXT(me_board_off, "Listed under UTIL > PLG. PartAssign is off there, so the board is silent"));
	else
		ImGui::TextDisabled("%s", UI_TEXT(me_board_known, "Listed under UTIL > PLG. PartAssign there moves it too (parts 1-16)"));
}


// ボードのチャンネルをインサーションへ通す欄（-・1-4・V）。選んだエフェクトがどのパートにも割り当たっていなければ、
// 音が通らないので、A/D パート 1 に割り当てておく（A/D INPUT を使っていなければ、そのエフェクトはボード専用になる）
static void board_insert_combo(int ch, const mu2000::board_part &p, xg::model &m, bridge &br)
{
	static const char *const marks[6] = { "-", "1", "2", "3", "4", "V" };
	static const char *const keys[6] = { nullptr, "insertion1.part", "insertion2.part", "insertion3.part", "insertion4.part", "variation.part" };
	ImGui::SetNextItemWidth(-1);
	if (ImGui::BeginCombo("##ins", marks[std::min<int>(p.insert, 5)], ImGuiComboFlags_NoArrowButton)) {
		for (int s = 0; s < 6; s++)
			if (ImGui::Selectable(marks[s], p.insert == s)) {
				board_view::set_insert(br, ch, s);
				int who = 0;
				if (s && m.get(P(keys[s]), 0, who) && who == 127)
					br.send(m.set(P(keys[s]), 0, 64));
			}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(me_bp_insert_tip, "Send this channel through one of the MU's insertion effects (1-4), or through the variation effect when its connection is Insertion (V), instead of straight to the output. If the effect is not assigned to any part it gets assigned to A/D part 1, so that it passes sound."));
}

// マルチパートのボードの 16 チャンネル。様子は音声の糸に聞き、動かした値は口 E へ MIDI で送る
// （ボードは MIDI で動くので、外から送ったのと同じ結果になる）
void master_editor::board_parts_pane(xg::model &m, bridge &br)
{
	const float fs = ImGui::GetFontSize();
	br.post([info = m_board_parts](mu2000 &mu) {
		mu2000::board_part now[16];
		mu.board_parts(now);
		std::lock_guard<std::mutex> g(info->lock);
		std::copy(std::begin(now), std::end(now), std::begin(info->part));
		info->valid = true;
		return std::string();
	});
	mu2000::board_part part[16];
	{
		std::lock_guard<std::mutex> g(m_board_parts->lock);
		if (!m_board_parts->valid)
			return;
		std::copy(std::begin(m_board_parts->part), std::end(m_board_parts->part), std::begin(part));
	}
	const bool dls = board_multi_kind() == mu2000::VBOARD_DLS;
	const auto send = [&br](std::initializer_list<int> bytes) {
		u8 b[8];
		size_t n = 0;
		for (int x : bytes)
			b[n++] = u8(x);
		br.send_port(mu2000::MIDI_PORTS, b, n);
	};
	ImGui::TextDisabled("%s", UI_TEXT(me_board_parts_note, "The board's own 16 channels on port E. Variation, chorus and reverb sends go into the MU's effects; Ins puts a channel through an insertion effect. Changes here are sent to the board as MIDI."));
	if (!ImGui::BeginTable("bparts", dls ? 11 : 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_ch, "Ch"), 0, fs * 1.6f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_voice, "Voice"), 0, fs * 8.5f);
	if (dls)
		ImGui::TableSetupColumn(UI_TEXT(me_bp_bank, "Bank"), 0, fs * 4.5f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_program, "Program"), 0, fs * 5.0f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_volume, "Volume"), 0, fs * 6.5f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_pan, "Pan"), 0, fs * 6.5f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_insert, "Ins"), 0, fs * 2.6f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_variation, "Variation"), 0, fs * 6.5f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_reverb, "Reverb"), 0, fs * 6.5f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_chorus, "Chorus"), 0, fs * 6.5f);
	ImGui::TableSetupColumn(UI_TEXT(me_bp_level, "Level"), ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableHeadersRow();
	for (int ch = 0; ch < 16; ch++) {
		const mu2000::board_part &p = part[ch];
		ImGui::PushID(ch);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%d", ch + 1);
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(p.name[0] ? p.name : (p.drum ? UI_TEXT(me_bp_drum, "(drums)") : "-"));
		if (dls) {
			ImGui::TableNextColumn();
			int msb = p.msb;
			ImGui::SetNextItemWidth(-1);
			ImGui::BeginDisabled(p.drum);
			if (ImGui::InputInt("##msb", &msb, 0, 0)) {
				msb = std::clamp(msb, 0, 127);
				send({ 0xb0 | ch, 0, msb, 0xb0 | ch, 32, p.lsb, 0xc0 | ch, p.program });
			}
			ImGui::EndDisabled();
		}
		ImGui::TableNextColumn();
		int prog = p.program + 1;
		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputInt("##prog", &prog)) {
			prog = std::clamp(prog, 1, 128);
			send({ 0xb0 | ch, 0, p.msb, 0xb0 | ch, 32, p.lsb, 0xc0 | ch, prog - 1 });
		}
		const auto slider = [&](const char *id, int value, int cc) {
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1);
			int v = value;
			if (ImGui::SliderInt(id, &v, 0, 127))
				send({ 0xb0 | ch, cc, v });
		};
		slider("##vol", p.vol, 7);
		slider("##pan", p.pan, 10);
		ImGui::TableNextColumn();
		board_insert_combo(ch, p, m, br);
		slider("##var", p.var, 94);
		slider("##rev", p.rev, 91);
		slider("##cho", p.cho, 93);
		ImGui::TableNextColumn();
		ImGui::ProgressBar(std::min(1.0f, std::sqrt(p.level) * 1.4f), ImVec2(-1, 0), "");
		ImGui::PopID();
	}
	ImGui::EndTable();
}

// オリジナルのボード: 置き場（設定のフォルダーの boards）のボードから選ぶ。中身はサンプリングの窓の「波形を作る」で作る
void master_editor::board_user_pane(bridge &br)
{
	namespace ub = user_boards;
	const float fs = ImGui::GetFontSize();
	const double now = ImGui::GetTime();
	if (m_ub_listed < 0 || now - m_ub_listed > 1.0) {
		m_ub_listed = now;
		m_ub_list = ub::list();
	}
	std::shared_ptr<const ub::board> cur = ub::current();
	const std::string stem = ub::current_stem();
	ImGui::SetNextItemWidth(fs * 11);
	if (ImGui::BeginCombo("##ubfile", cur ? cur->name : UI_TEXT(me_board_user_none, "(no board)"))) {
		for (const std::string &s : m_ub_list)
			if (ImGui::Selectable(s.c_str(), s == stem) && s != stem) {
				std::string err;
				m_ub_note = ub::open(br, s, err) ? std::string() : err;
			}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(me_board_user_tip, "A board of your own: waves made on the Sampling window's \"Make a wave\" tab, one per program number. Program change picks the wave; the display shows the names you gave. Only waves computed there can go in (no built-in waves, no recordings). Boards are kept in the \"boards\" folder of the settings folder, one file each."));
	ImGui::SameLine();
	if (!m_ub_note.empty())
		ImGui::TextDisabled(UI_TEXT(me_board_dls_error_fmt, "Could not load: %s"), m_ub_note.c_str());
	else if (!cur)
		ImGui::TextDisabled("%s", UI_TEXT(me_board_user_empty, "No board yet: make one in Sampling > Make a wave"));
	else
		ImGui::TextDisabled(UI_TEXT(me_board_user_count_fmt, "%d programs"), cur->count());
}

// DLS のボード: 読むファイルを選ぶ。選んだら音声の糸で読ませ、結果（音色と波形の数、または読めなかった訳）を出す
void master_editor::board_dls_pane(bridge &br)
{
	const float fs = ImGui::GetFontSize();
	std::string picked;
	if (xgui::file_dialogs()) {
		if (ImGui::Button(UI_TEXT(me_board_dls_open, "DLS file...")))
			xgui::ask_open_dls();
	} else {
		ImGui::SetNextItemWidth(-fs * 6);
		ImGui::InputTextWithHint("##dlspath", UI_TEXT(me_board_dls_path, "Path of a DLS file"), m_board_dls_input, sizeof(m_board_dls_input));
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(me_board_dls_load, "Load")) && m_board_dls_input[0])
			picked = m_board_dls_input;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(me_board_dls_tip, "A DLS sound bank of your own, for example gm.dls in Windows' System32\\drivers folder. No sound bank comes with S-MU2000. The file is only read; its place is remembered."));
	xgui::take_opened_dls(picked);
	if (!picked.empty())
		br.post([info = m_board_dls, picked](mu2000 &mu) {
			std::string err;
			const bool ok = mu.load_board_dls(picked, err);
			std::lock_guard<std::mutex> g(info->lock);
			info->error = ok ? std::string() : err;
			return std::string();
		});
	// いま読んであるもの（設定から読んだものも含む）を聞いておく
	br.post([info = m_board_dls](mu2000 &mu) {
		std::lock_guard<std::mutex> g(info->lock);
		info->path = mu.board_dls_path();
		info->instruments = mu.board_dls_instruments();
		info->waves = mu.board_dls_waves();
		return std::string();
	});
	std::string path, error;
	int instruments = 0, waves = 0;
	{
		std::lock_guard<std::mutex> g(m_board_dls->lock);
		path = m_board_dls->path;
		error = m_board_dls->error;
		instruments = m_board_dls->instruments;
		waves = m_board_dls->waves;
	}
	ImGui::SameLine();
	if (!error.empty()) {
		ImGui::TextDisabled(UI_TEXT(me_board_dls_error_fmt, "Could not load: %s"), error.c_str());
	} else if (path.empty()) {
		ImGui::TextDisabled("%s", UI_TEXT(me_board_dls_empty, "No file yet: the board is silent"));
	} else {
		const size_t slash = path.find_last_of("/\\");
		ImGui::TextDisabled(UI_TEXT(me_board_dls_loaded_fmt, "%s: %d instruments, %d waves"),
		                    path.substr(slash == std::string::npos ? 0 : slash + 1).c_str(), instruments, waves);
	}
}

} // namespace ui
