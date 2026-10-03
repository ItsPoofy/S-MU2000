// license:BSD-3-Clause
#include "sampling_editor.h"

#include "wav_in.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

namespace ui {

namespace sp = smu2000::sampling;

namespace {

// 見出し 1 行。区画の頭に
void heading(const char *text)
{
	ImGui::TextUnformatted(text);
	ImGui::Separator();
}

const char *pan_text(int pan, char *buf, size_t n)
{
	if (pan == 15)
		std::snprintf(buf, n, "%s", UI_TEXT(smp_pan_scaling, "Scaling"));
	else if (pan == 7)
		std::snprintf(buf, n, "C");
	else
		std::snprintf(buf, n, "%c%d", pan < 7 ? 'L' : 'R', pan < 7 ? 7 - pan : pan - 7);
	return buf;
}

// 16bit の絶対値を dBFS に（0 は -90）
float db(s32 peak)
{
	return peak <= 0 ? -90.0f : 20.0f * std::log10(float(peak) / 32768.0f);
}

void meter(const char *label, s32 peak, s32 trigger)
{
	const float fs = ImGui::GetFontSize();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::SameLine(fs * 3.5f);
	const float w = ImGui::GetContentRegionAvail().x - fs * 5;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float h = ImGui::GetFrameHeight() * 0.6f;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	auto x_of = [&](float d) { return p.x + w * std::clamp((d + 60.0f) / 60.0f, 0.0f, 1.0f); };
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg));
	const float d = db(peak);
	const ImU32 col = d > -1.0f ? IM_COL32(230, 70, 60, 255) : d > -12.0f ? IM_COL32(230, 200, 60, 255) : IM_COL32(90, 200, 110, 255);
	dl->AddRectFilled(p, ImVec2(x_of(d), p.y + h), col);
	if (trigger > 0) {
		const float tx = x_of(db(trigger));
		dl->AddLine(ImVec2(tx, p.y - 2), ImVec2(tx, p.y + h + 2), IM_COL32(255, 255, 255, 220), 2.0f);
	}
	ImGui::Dummy(ImVec2(w, h));
	ImGui::SameLine();
	if (peak > 0)
		ImGui::Text("%5.1f dB", d);
	else
		ImGui::TextUnformatted("  -inf");
}

s32 trigger_level(int trigger_db)
{
	return trigger_db >= 0 ? 0 : s32(std::lround(32768.0 * std::pow(10.0, trigger_db / 20.0)));
}

} // namespace

void sampling_editor::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	(void)m;
	(void)ram;
	br.get_sampling(m_view);
	if (m_view.serial != m_note_serial && !m_view.message.empty()) {
		// 仕事の結果は、次の写しにも同じ文が残るので、変わったときだけ受け取る
		if (m_view.message != m_note)
			m_note = m_view.message;
		m_note_serial = m_view.serial;
	}

	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("sampling_editor", nullptr, wf);
	ImGui::PopStyleVar();

	if (!m_view.ready) {
		ImGui::TextUnformatted(UI_TEXT(smp_not_ready, "Waiting for the MU2000 to start..."));
		ImGui::End();
		return;
	}

	br.request_overview(m_selected);

	// WAV のファイルの窓から読んだ中身
	std::vector<u8> opened;
	if (xgui::take_opened_wav(opened))
		import_wav(opened, br);

	const float fs = ImGui::GetFontSize();
	const float left_w = std::min(fs * 24.0f, ImGui::GetContentRegionAvail().x * 0.5f);
	if (ImGui::BeginChild("left", ImVec2(left_w, 0), ImGuiChildFlags_Borders)) {
		input_pane(br);
		ImGui::Separator();
		record_pane(br);
	}
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("right", ImVec2(0, 0))) {
		const float h = ImGui::GetContentRegionAvail().y * 0.28f;
		if (ImGui::BeginChild("samples", ImVec2(0, h), ImGuiChildFlags_Borders))
			samples_pane();
		ImGui::EndChild();
		if (ImGui::BeginChild("wave", ImVec2(0, h * 1.15f), ImGuiChildFlags_Borders))
			wave_pane(br);
		ImGui::EndChild();
		if (ImGui::BeginChild("assign", ImVec2(0, 0), ImGuiChildFlags_Borders))
			assign_pane(br);
		ImGui::EndChild();
	}
	ImGui::EndChild();
	ImGui::End();
}

void sampling_editor::input_pane(bridge &br)
{
	heading(UI_TEXT(smp_input, "Input"));
	// 録音デバイス。gui だけ（プラグインはホストの A/D Input バス）
	std::vector<std::string> names;
	std::string current;
	if (br.ain_devices(names, current)) {
		ImGui::TextUnformatted(UI_TEXT(smp_device, "Recording device"));
		ImGui::SetNextItemWidth(-1);
		const char *none = UI_TEXT(smp_device_none, "(none)");
		if (ImGui::BeginCombo("##ain", current.empty() ? none : current.c_str())) {
			br.request_ain_list();
			if (ImGui::Selectable(none, current.empty()))
				br.request_ain(-1);
			for (size_t i = 0; i < names.size(); i++)
				if (ImGui::Selectable(names[i].c_str(), names[i] == current))
					br.request_ain(int(i));
			ImGui::EndCombo();
		}
	} else {
		ImGui::TextWrapped("%s", UI_TEXT(smp_device_host, "Recording comes from the host's A/D Input bus."));
	}

	ImGui::TextUnformatted(UI_TEXT(smp_source, "Record from"));
	ImGui::RadioButton("AD1", &m_source, int(sp::source::ad1));
	ImGui::SameLine();
	ImGui::RadioButton("AD2", &m_source, int(sp::source::ad2));
	ImGui::SameLine();
	ImGui::RadioButton("AD1+2", &m_source, int(sp::source::both));

	const s32 trig = trigger_level(m_trigger_db);
	meter("AD1", m_view.peak[0], m_source != int(sp::source::ad2) ? trig : 0);
	meter("AD2", m_view.peak[1], m_source != int(sp::source::ad1) ? trig : 0);

	ImGui::TextUnformatted(UI_TEXT(smp_trigger, "Trigger"));
	ImGui::SetNextItemWidth(-1);
	ImGui::SliderInt("##trig", &m_trigger_db, -60, 0,
	                 m_trigger_db >= 0 ? UI_TEXT(smp_trigger_off, "Off (start at once)") : "%d dB");
}

void sampling_editor::record_pane(bridge &br)
{
	heading(UI_TEXT(smp_record, "Record"));
	const float fs = ImGui::GetFontSize();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_name, "Name"));
	ImGui::SameLine(fs * 4.5f);
	ImGui::SetNextItemWidth(fs * 8);
	ImGui::InputTextWithHint("##name", "take###", m_name, sizeof(m_name));

	const bool busy = m_view.rec_state != 0;
	const std::string name = m_name;
	if (!busy) {
		const bool can = m_view.free_frames > sp::SAMPLE_RATE / 10;
		ImGui::BeginDisabled(!can);
		ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 50, 50, 255));
		if (ImGui::Button(UI_TEXT(smp_record_start, "Record"), ImVec2(fs * 7, 0))) {
			const sp::source src = sp::source(m_source);
			const int trig = trigger_level(m_trigger_db);
			br.post([src, trig](mu2000 &mu) {
				mu.rec_start(src, trig, mu.sampling_free_frames());
				return std::string();
			});
		}
		ImGui::PopStyleColor();
		ImGui::EndDisabled();
	} else if (ImGui::Button(UI_TEXT(smp_stop, "Stop"), ImVec2(fs * 7, 0))) {
		// 止めたら、録れたものを firmware の表に足す
		std::string nothing = UI_TEXT(smp_nothing, "Nothing was recorded");
		std::string added = UI_TEXT(smp_added_fmt, "Added sample %03d (%.1f s)");
		br.post([name, nothing, added](mu2000 &mu) {
			const std::vector<s16> pcm = mu.rec_take();
			if (pcm.empty())
				return nothing;
			std::string err;
			const int n = mu.sampling_add(pcm.data(), pcm.size(), name, err);
			if (!n)
				return err;
			char buf[160];
			std::snprintf(buf, sizeof(buf), added.c_str(), n, double(pcm.size()) / sp::SAMPLE_RATE);
			return std::string(buf);
		});
	}
	ImGui::SameLine();
	if (m_view.rec_state == 1)
		ImGui::TextUnformatted(UI_TEXT(smp_waiting, "Waiting for the trigger..."));
	else if (m_view.rec_state == 2)
		ImGui::Text(UI_TEXT(smp_recording_fmt, "Recording %.1f s"), double(m_view.rec_frames) / sp::SAMPLE_RATE);
	ImGui::Text(UI_TEXT(smp_free_fmt, "%.1f s free"),
	            double(m_view.free_frames - std::min(m_view.free_frames, m_view.rec_frames)) / sp::SAMPLE_RATE);

	ImGui::Spacing();
	ImGui::TextWrapped("%s", UI_TEXT(smp_wav_note, "A WAV file can be imported instead: the channel picked under \"Record from\" is taken and converted to 44.1 kHz."));
	ImGui::BeginDisabled(busy);
	if (xgui::file_dialogs()) {
		if (ImGui::Button(UI_TEXT(smp_wav, "Import WAV...")))
			xgui::ask_open_wav();
	} else {
		ImGui::SetNextItemWidth(-fs * 6);
		ImGui::InputTextWithHint("##path", UI_TEXT(smp_wav_path, "WAV file path"), m_path, sizeof(m_path));
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(smp_wav_load, "Import")) && m_path[0]) {
			std::ifstream f(std::filesystem::u8path(m_path), std::ios::binary);
			std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (bytes.empty())
				m_note = UI_TEXT(smp_wav_fail, "Could not read the WAV file");
			else
				import_wav(bytes, br);
		}
	}
	ImGui::EndDisabled();

	if (!m_note.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", m_note.c_str());
	}
}

void sampling_editor::import_wav(const std::vector<u8> &bytes, bridge &br)
{
	smu2000::wav_data w;
	std::string err;
	if (!smu2000::parse_wav(bytes, w, err)) {
		m_note = err;
		return;
	}
	auto pcm = std::make_shared<std::vector<s16>>(
		smu2000::wav_for_sampling(w, sp::source(m_source), m_view.free_frames));
	if (pcm->empty()) {
		m_note = UI_TEXT(smp_nothing, "Nothing was recorded");
		return;
	}
	const std::string name = m_name;
	std::string added = UI_TEXT(smp_added_fmt, "Added sample %03d (%.1f s)");
	br.post([pcm, name, added](mu2000 &mu) {
		std::string e;
		const int n = mu.sampling_add(pcm->data(), pcm->size(), name, e);
		if (!n)
			return e;
		char buf[160];
		std::snprintf(buf, sizeof(buf), added.c_str(), n, double(pcm->size()) / sp::SAMPLE_RATE);
		return std::string(buf);
	});
}

void sampling_editor::samples_pane()
{
	heading(UI_TEXT(smp_samples, "Samples"));
	if (m_view.samples.empty()) {
		ImGui::TextDisabled("%s", UI_TEXT(smp_none, "No samples yet"));
		m_selected = 0;
		return;
	}
	// 選んだものが消えていたら、最後に足したものを選ぶ（録った直後にすぐ見られるように）
	bool found = false;
	for (const sp::sample &s : m_view.samples)
		found = found || s.number == m_selected;
	if (!found)
		m_selected = m_view.samples.back().number;
	if (ImGui::BeginTable("samples", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(UI_TEXT(smp_col_no, "No."), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(UI_TEXT(smp_name, "Name"));
		ImGui::TableSetupColumn(UI_TEXT(smp_col_len, "Length"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(UI_TEXT(smp_col_peak, "Peak"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();
		for (const sp::sample &s : m_view.samples) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			char id[16];
			std::snprintf(id, sizeof(id), "%03d", s.number);
			if (ImGui::Selectable(id, s.number == m_selected, ImGuiSelectableFlags_SpanAllColumns))
				m_selected = s.number;
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(s.name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%.2f s", double(s.frames()) / double(s.rate ? s.rate : sp::SAMPLE_RATE));
			ImGui::TableNextColumn();
			if (s.peak == 0)
				ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", UI_TEXT(smp_silent, "silent"));
			else if (s.peak > 0)
				ImGui::Text("%.1f dB", double(db(s.peak)));
		}
		ImGui::EndTable();
	}
}

void sampling_editor::wave_pane(bridge &br)
{
	const sp::sample *sel = nullptr;
	for (const sp::sample &s : m_view.samples)
		if (s.number == m_selected)
			sel = &s;
	if (!sel) {
		heading(UI_TEXT(smp_wave, "Waveform"));
		ImGui::TextDisabled("%s", UI_TEXT(smp_wave_pick, "Pick a sample in the list"));
		return;
	}
	char title[64];
	std::snprintf(title, sizeof(title), "%s  %03d %s", UI_TEXT(smp_wave, "Waveform"), sel->number, sel->name.c_str());
	heading(title);
	const float fs = ImGui::GetFontSize();

	// 音量。ノーマライズは最大を -0.5 dB に。書き換えなので元に戻せない
	const int num = sel->number;
	const int peak = sel->peak;
	std::string done = UI_TEXT(smp_gain_done_fmt, "Sample %03d: peak now %.1f dB");
	auto post_gain = [&](double gain) {
		br.post([num, gain, done](mu2000 &mu) {
			const int p = mu.sampling_gain(num, gain);
			if (p < 0)
				return std::string("no such sample");
			char buf[120];
			std::snprintf(buf, sizeof(buf), done.c_str(), num, p > 0 ? 20.0 * std::log10(p / 32768.0) : -90.0);
			return std::string(buf);
		});
	};
	const bool busy = m_view.rec_state != 0;
	ImGui::BeginDisabled(busy || peak <= 0);
	if (ImGui::Button(UI_TEXT(smp_normalize, "Normalize")))
		post_gain(32767.0 * std::pow(10.0, -0.5 / 20.0) / double(peak));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	ImGui::InputFloat("##gain", &m_gain_db, 1.0f, 6.0f, "%+.1f dB");
	m_gain_db = std::clamp(m_gain_db, -40.0f, 40.0f);
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_gain_apply, "Change volume")))
		post_gain(std::pow(10.0, double(m_gain_db) / 20.0));
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_gain_note, "Rewrites the sample in place. Turning it down and up again loses detail, and turning it up past full scale clips."));

	// トリム。波形をドラッグして残す所を選ぶ。選んだサンプルが替わったら全体に戻す
	if (m_trim_for != num) {
		m_trim_for = num;
		m_trim0 = 0.0f;
		m_trim1 = 1.0f;
		m_trim_drag = false;
	}
	const bool have_wave = m_view.wave_number == num && !m_view.wave_hi.empty();
	const double secs = double(sel->frames()) / sp::SAMPLE_RATE;
	const bool partial = m_trim0 > 0.0f || m_trim1 < 1.0f;
	ImGui::BeginDisabled(busy || !have_wave);
	if (ImGui::Button(UI_TEXT(smp_trim_auto, "Select without silence"))) {
		// 見取り図で、最大から 40 dB 下を超える最初と最後の区切りを選ぶ
		const int nb = int(m_view.wave_hi.size());
		int top = 0;
		for (int b = 0; b < nb; b++)
			top = std::max({ top, int(m_view.wave_hi[size_t(b)]), -int(m_view.wave_lo[size_t(b)]) });
		const int thr = std::max(1, top / 100);
		int first = -1, last = -1;
		for (int b = 0; b < nb; b++)
			if (m_view.wave_hi[size_t(b)] >= thr || -int(m_view.wave_lo[size_t(b)]) >= thr) {
				if (first < 0)
					first = b;
				last = b;
			}
		if (first >= 0) {
			m_trim0 = float(first) / float(nb);
			m_trim1 = float(last + 1) / float(nb);
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!partial);
	if (ImGui::Button(UI_TEXT(smp_trim, "Trim"))) {
		const u32 f0 = u32(std::lround(double(m_trim0) * sel->frames()));
		const u32 f1 = u32(std::lround(double(m_trim1) * sel->frames()));
		std::string trimmed = UI_TEXT(smp_trimmed_fmt, "Sample %03d trimmed to %.2f s");
		br.post([num, f0, f1, trimmed](mu2000 &mu) {
			std::string e;
			if (!mu.sampling_trim(num, f0, f1, e))
				return e;
			char buf[120];
			std::snprintf(buf, sizeof(buf), trimmed.c_str(), num, double(f1 - f0) / sp::SAMPLE_RATE);
			return std::string(buf);
		});
		m_trim_for = 0;              // 次の写しで全体に戻す
	}
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_trim_clear, "Select all"))) {
		m_trim0 = 0.0f;
		m_trim1 = 1.0f;
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("%.2f - %.2f s (%.2f s)", double(m_trim0) * secs, double(m_trim1) * secs,
	                    double(m_trim1 - m_trim0) * secs);

	// 波形。窓の幅の 1 列ごとに、見取り図の最小と最大を縦の線で
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, fs * 3));
	ImGui::InvisibleButton("##wave", sz);
	if (have_wave && !busy) {
		const float fx = std::clamp((ImGui::GetIO().MousePos.x - p.x) / sz.x, 0.0f, 1.0f);
		static float anchor = 0.0f;
		if (ImGui::IsItemActivated()) {
			anchor = fx;
			m_trim_drag = true;
		}
		if (m_trim_drag && ImGui::IsItemActive()) {
			m_trim0 = std::min(anchor, fx);
			m_trim1 = std::max(anchor, fx);
		}
		if (m_trim_drag && !ImGui::IsItemActive()) {
			m_trim_drag = false;
			if (m_trim1 - m_trim0 < 0.002f) {        // クリックだけなら全体に戻す
				m_trim0 = 0.0f;
				m_trim1 = 1.0f;
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_trim_tip, "Drag across the waveform to choose what to keep"));
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255));
	const float mid = p.y + sz.y * 0.5f, half = sz.y * 0.5f - 2.0f;
	dl->AddLine(ImVec2(p.x, mid), ImVec2(p.x + sz.x, mid), IM_COL32(80, 90, 110, 255));
	if (have_wave) {
		const int nb = int(m_view.wave_hi.size());
		const int cols = std::max(1, int(sz.x));
		for (int x = 0; x < cols; x++) {
			const int b0 = x * nb / cols, b1 = std::max(b0 + 1, (x + 1) * nb / cols);
			s16 lo = 0, hi = 0;
			for (int b = b0; b < b1 && b < nb; b++) {
				lo = std::min(lo, m_view.wave_lo[size_t(b)]);
				hi = std::max(hi, m_view.wave_hi[size_t(b)]);
			}
			const float y0 = mid - half * float(hi) / 32768.0f, y1 = mid - half * float(lo) / 32768.0f;
			const bool clip = hi >= 32767 || lo <= -32768;
			dl->AddLine(ImVec2(p.x + float(x) + 0.5f, y0), ImVec2(p.x + float(x) + 0.5f, std::max(y1, y0 + 1.0f)),
			            clip ? IM_COL32(235, 80, 70, 255) : IM_COL32(110, 200, 255, 255));
		}
		// 残さない所を暗く、境目に線
		if (m_trim0 > 0.0f || m_trim1 < 1.0f) {
			const float x0 = p.x + sz.x * m_trim0, x1 = p.x + sz.x * m_trim1;
			dl->AddRectFilled(p, ImVec2(x0, p.y + sz.y), IM_COL32(0, 0, 0, 150));
			dl->AddRectFilled(ImVec2(x1, p.y), ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(0, 0, 0, 150));
			dl->AddLine(ImVec2(x0, p.y), ImVec2(x0, p.y + sz.y), IM_COL32(255, 210, 90, 255), 2.0f);
			dl->AddLine(ImVec2(x1, p.y), ImVec2(x1, p.y + sz.y), IM_COL32(255, 210, 90, 255), 2.0f);
		}
	}
}

void sampling_editor::assign_pane(bridge &br)
{
	heading(UI_TEXT(smp_assign, "Voice assignment"));
	const float fs = ImGui::GetFontSize();
	const float lab = fs * 7.5f;

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Bank#");
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 5);
	ImGui::Combo("##bank", &m_bank, "000\0" "001\0");
	ImGui::SameLine();
	ImGui::TextUnformatted(UI_TEXT(smp_pgm, "Program"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 6);
	if (ImGui::InputInt("##pgm", &m_pgm))
		m_pgm = std::clamp(m_pgm, 1, 128);

	const int slot = m_bank * 128 + (m_pgm - 1);
	const sp::voice &cur = m_view.voices[size_t(slot)];
	if (slot != m_loaded_slot || !m_dirty) {
		// 選んだ音色の今の値を編集欄へ（触っている間は上書きしない）
		m_loaded_slot = slot;
		m_sample = cur.assigned ? cur.sample : 0;
		std::snprintf(m_voice_name, sizeof(m_voice_name), "%s", cur.name.c_str());
		m_level = cur.level;
		m_pan = cur.pan;
		m_dirty = false;
	}

	// サンプル
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_sample, "Sample"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	char label[32];
	const char *none = UI_TEXT(smp_sample_none, "(none)");
	std::string shown = none;
	for (const sp::sample &s : m_view.samples)
		if (s.number == m_sample) {
			std::snprintf(label, sizeof(label), "%03d %s", s.number, s.name.c_str());
			shown = label;
		}
	if (m_sample && shown == none) {
		std::snprintf(label, sizeof(label), "%03d ?", m_sample);
		shown = label;
	}
	if (ImGui::BeginCombo("##sample", shown.c_str())) {
		if (ImGui::Selectable(none, m_sample == 0)) {
			m_sample = 0;
			m_dirty = true;
		}
		for (const sp::sample &s : m_view.samples) {
			std::snprintf(label, sizeof(label), "%03d %s", s.number, s.name.c_str());
			if (ImGui::Selectable(label, s.number == m_sample)) {
				m_sample = s.number;
				m_dirty = true;
			}
		}
		ImGui::EndCombo();
	}

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_voice_name, "Voice name"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 10);
	if (ImGui::InputText("##vname", m_voice_name, sizeof(m_voice_name)))
		m_dirty = true;

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_level, "Level"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderInt("##level", &m_level, 0, 127))
		m_dirty = true;

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_pan, "Pan"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 8);
	char pbuf[24];
	if (ImGui::BeginCombo("##pan", pan_text(m_pan, pbuf, sizeof(pbuf)))) {
		for (int p = 0; p <= 15; p++)
			if (ImGui::Selectable(pan_text(p, pbuf, sizeof(pbuf)), p == m_pan)) {
				m_pan = p;
				m_dirty = true;
			}
		ImGui::EndCombo();
	}

	ImGui::Spacing();
	if (ImGui::Button(UI_TEXT(smp_apply, "Apply"), ImVec2(fs * 7, 0))) {
		sp::voice v;
		v.assigned = m_sample != 0;
		v.sample = m_sample;
		v.name = m_voice_name;
		v.level = m_level;
		v.pan = m_pan;
		std::string done = UI_TEXT(smp_voice_set_fmt, "Wrote Bank# %d, program %d");
		const int bank = m_bank, pgm = m_pgm;
		br.post([slot, v, done, bank, pgm](mu2000 &mu) {
			std::string err;
			if (!mu.sampling_set_voice(slot, v, err))
				return err;
			char buf[120];
			std::snprintf(buf, sizeof(buf), done.c_str(), bank, pgm);
			return std::string(buf);
		});
		m_dirty = false;
	}
	ImGui::SameLine();
	// 鳴らしてみる: パート 1 をこの音色にする（バンク MSB 16）。書いた値は音色を選び直したときに効く
	if (ImGui::Button(UI_TEXT(smp_select_part1, "Select on part 1"))) {
		const std::vector<u8> msg = { 0xb0, 0x00, 0x10, 0xb0, 0x20, u8(m_bank), 0xc0, u8(m_pgm - 1) };
		br.send(msg);
	}
	ImGui::TextWrapped(UI_TEXT(smp_play_hint_fmt, "Play it with bank MSB 16, LSB %d, program %d. Changes take effect when the voice is selected again."),
	                   m_bank, m_pgm);
}

} // namespace ui
