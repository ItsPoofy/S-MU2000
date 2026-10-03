// license:BSD-3-Clause
#include "sampling_editor.h"

#include "wav_in.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
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
			std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t *>(m_path)), std::ios::binary);
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
		br.request_overview(0);
		return;
	}
	const int num = sel->number;
	const u32 frames = sel->frames();
	const double rate = double(sp::SAMPLE_RATE);
	char title[64];
	std::snprintf(title, sizeof(title), "%s  %03d %s", UI_TEXT(smp_wave, "Waveform"), num, sel->name.c_str());
	heading(title);
	const float fs = ImGui::GetFontSize();
	const bool busy = m_view.rec_state != 0;

	// 選んだサンプルが替わったら（トリムで長さが変わったときも）、全体を表示し、鳴らす所を表から読む
	if (m_trim_for != num || m_trim_frames != frames) {
		m_trim_for = num;
		m_trim_frames = frames;
		m_start = sel->play_from;
		m_end = sel->play_to ? sel->play_to : frames;
		m_view0 = 0.0;
		m_view1 = double(frames);
		m_drag = 0;
		m_loop_on = sel->loop;
		m_loop_at = sel->loop_from;
	}
	// 鳴り始め S・鳴り終わり E・ループの頭 L をそろえる（src/sampling.cpp の fit_points と同じ）。
	// moved は今動かしたもの（1 = S、2 = E、4 = L）。L は S と E の 8 手前のあいだで、S や E に押されて動く
	auto fit = [&](int moved) {
		m_end = std::clamp(m_end, std::min(frames, 8u), frames);
		m_start = std::min(m_start, m_end >= 8 ? m_end - 8 : 0);
		const u32 lo = (m_start + 1) & ~1u, hi = m_end >= 8 ? (m_end - 8) & ~1u : 0;
		m_loop_at = std::clamp(m_loop_at & ~1u, std::min(lo, hi), hi);
		if (moved == 4 && m_loop_at < m_start)
			m_start = m_loop_at;
	};
	// 鳴らす所は変えたらすぐ表へ（波形は切らない）
	auto post_points = [&]() {
		const u32 f0 = m_start, f1 = m_end, at = m_loop_at;
		const bool on = m_loop_on;
		br.post([num, f0, f1, on, at](mu2000 &mu) {
			mu.sampling_points(num, f0, f1, on, at);
			return std::string();
		});
	};
	// 表示の範囲。いちばん細かくて 32 サンプル
	const double min_span = std::min(32.0, double(frames));
	auto clamp_view = [&]() {
		double span = std::clamp(m_view1 - m_view0, min_span, double(frames));
		m_view0 = std::clamp(m_view0, 0.0, double(frames) - span);
		m_view1 = m_view0 + span;
	};
	auto zoom_at = [&](double center, double factor) {
		const double span = std::clamp((m_view1 - m_view0) * factor, min_span, double(frames));
		const double t = (center - m_view0) / (m_view1 - m_view0);
		m_view0 = center - span * t;
		m_view1 = m_view0 + span;
		clamp_view();
	};
	clamp_view();

	// ---- 音量。ノーマライズは最大を -0.5 dB に。書き換えなので元に戻せない
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

	// ---- 鳴り始めと鳴り終わり（サンプル単位で打ち込める。Shift を押しながら +/- で 10ms）
	ImGui::BeginDisabled(busy);
	const int step_fast = int(sp::SAMPLE_RATE / 100);
	int st = int(m_start), en = int(m_end);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_trim_start, "Start"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	if (ImGui::InputInt("##tstart", &st, 1, step_fast)) {
		m_start = u32(std::max(st, 0));
		fit(1);
		post_points();
	}
	ImGui::SameLine();
	ImGui::Text("%.3f s", double(m_start) / rate);
	ImGui::SameLine(0, fs * 1.2f);
	ImGui::TextUnformatted(UI_TEXT(smp_trim_end, "End"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	if (ImGui::InputInt("##tend", &en, 1, step_fast)) {
		m_end = u32(std::max(en, 0));
		fit(2);
		post_points();
	}
	ImGui::SameLine();
	ImGui::Text("%.3f s (%.3f s)", double(m_end) / rate, double(m_end - m_start) / rate);

	// 前後の無音を除いて選ぶ（全体を音源の側で調べる。結果は次のコマ以降に届く）
	if (m_auto && m_auto->load() != ~u64(0)) {
		const u64 r = m_auto->load();
		if (r != ~u64(1)) {
			m_start = u32(r >> 32);
			m_end = u32(r);
			fit(1);
			post_points();
		}
		m_auto.reset();
	}
	if (ImGui::Button(UI_TEXT(smp_trim_auto, "Select without silence")) && !m_auto) {
		auto res = std::make_shared<std::atomic<u64>>(~u64(0));
		m_auto = res;
		br.post([num, res](mu2000 &mu) {
			u32 a = 0, b = 0;
			res->store(mu.sampling_bounds(num, 0.01, a, b) ? (u64(a) << 32 | b) : ~u64(1));
			return std::string();
		});
	}
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_trim_clear, "Select all"))) {
		m_start = 0;
		m_end = frames;
		fit(1);
		post_points();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(m_start == 0 && m_end == frames);
	const bool trim = ImGui::Button(UI_TEXT(smp_trim, "Trim"));
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_trim_note, "Cuts away the sound before the start and after the end to free sampling memory. Start and end already work without trimming."));
	if (trim) {
		const u32 f0 = m_start, f1 = m_end;
		std::string trimmed = UI_TEXT(smp_trimmed_fmt, "Sample %03d trimmed to %.2f s");
		br.post([num, f0, f1, trimmed](mu2000 &mu) {
			std::string e;
			if (!mu.sampling_trim(num, f0, f1, e))
				return e;
			char buf[120];
			std::snprintf(buf, sizeof(buf), trimmed.c_str(), num, double(f1 - f0) / sp::SAMPLE_RATE);
			return std::string(buf);
		});
	}
	ImGui::EndDisabled();

	// ---- ループ。鍵盤を押しているあいだ、ループの頭から鳴り終わり（E）までをくり返す（変えたらすぐ表へ）
	if (ImGui::Checkbox(UI_TEXT(smp_loop, "Loop"), &m_loop_on))
		post_points();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_loop_tip, "While a key is held, repeat from the loop point to the end point. Off plays from the start to the end once."));
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_loop_on);
	ImGui::TextUnformatted(UI_TEXT(smp_loop_at, "Loop point"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	int la = int(m_loop_at);
	if (ImGui::InputInt("##loopat", &la, 2, step_fast)) {
		m_loop_at = u32(std::max(la, 0));
		fit(4);
		post_points();
	}
	ImGui::SameLine();
	ImGui::Text("%.3f s", double(m_loop_at) / rate);
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	// ---- 試聴。始点から終点まで（音源を通さない生の音）。ループが入っていれば止めるまでくり返す
	const bool playing = m_view.preview_number == num;
	if (!playing) {
		if (ImGui::Button(UI_TEXT(smp_play, "Play"))) {
			const u32 f0 = m_start, f1 = m_end;
			const u32 lp = m_loop_on ? m_loop_at : ~0u;
			br.post([num, f0, f1, lp](mu2000 &mu) {
				mu.preview_start(num, f0, f1, lp);
				return std::string();
			});
		}
	} else if (ImGui::Button(UI_TEXT(smp_play_stop, "Stop playing"))) {
		br.post([](mu2000 &mu) {
			mu.preview_stop();
			return std::string();
		});
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_play_tip, "Plays from the start to the end point as recorded, without the voice's level, pan or pitch"));
	ImGui::SameLine(0, fs * 1.2f);

	// ---- 表示の拡大・縮小
	ImGui::TextUnformatted(UI_TEXT(smp_zoom, "Zoom"));
	ImGui::SameLine();
	const double center = (m_view0 + m_view1) * 0.5;
	if (ImGui::SmallButton("-"))
		zoom_at(center, 2.0);
	ImGui::SameLine();
	if (ImGui::SmallButton("+"))
		zoom_at(center, 0.5);
	ImGui::SameLine();
	if (ImGui::SmallButton(UI_TEXT(smp_zoom_all, "All"))) {
		m_view0 = 0.0;
		m_view1 = double(frames);
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(UI_TEXT(smp_zoom_sel, "Selection"))) {
		const double pad = std::max(8.0, double(m_end - m_start) * 0.05);
		m_view0 = double(m_start) - pad;
		m_view1 = double(m_end) + pad;
		clamp_view();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%.3f - %.3f s  (x%.0f)", m_view0 / rate, m_view1 / rate, double(frames) / (m_view1 - m_view0));

	// ---- 波形
	const bool zoomed = m_view1 - m_view0 < double(frames) - 0.5;
	const float bar_h = zoomed ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y - bar_h, fs * 3));
	ImGui::InvisibleButton("##wave", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
	                                     ImGuiButtonFlags_MouseButtonMiddle);
	const double span = m_view1 - m_view0;
	auto x_of = [&](double f) { return p.x + float((f - m_view0) / span * double(sz.x)); };
	auto f_of = [&](float x) { return m_view0 + double(x - p.x) / double(sz.x) * span; };
	ImGuiIO &io = ImGui::GetIO();
	if (!busy) {
		if (ImGui::IsItemHovered()) {
			// ホイールでマウスの所を中心に拡大・縮小
			if (io.MouseWheel != 0.0f)
				zoom_at(f_of(io.MousePos.x), io.MouseWheel > 0 ? 0.8 : 1.25);
			ImGui::SetTooltip("%s", UI_TEXT(smp_trim_tip, "Left-click sets the start, right-click the end. Shift-click sets the loop point. Drag a line with either button to move it. Wheel zooms, middle-drag scrolls."));
		}
		// 線（つまみ）の近くを押したら、どのボタンでもその線を動かす。それ以外は左で始点、右で終点をそこへ。
		// 中ボタンで表示を動かす
		if (ImGui::IsItemActivated()) {
			const float xs = x_of(double(m_start)), xe = x_of(double(m_end)), xl = x_of(double(m_loop_at));
			const float mx = io.MousePos.x, grab = fs * 0.5f;
			const float ds = std::fabs(mx - xs), de = std::fabs(mx - xe), dlp = m_loop_on ? std::fabs(mx - xl) : 1e9f;
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
				m_drag = 3;
			else if (io.KeyShift)
				m_drag = 4;   // Shift を押しながらならどのボタンでもループの頭（ループも入れる）
			else if (dlp <= grab && dlp < ds && dlp < de)
				m_drag = 4;
			else if (ds <= grab || de <= grab)
				m_drag = ds <= de ? 1 : 2;
			else
				m_drag = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 2 : 1;
		}
		if (m_drag && ImGui::IsItemActive()) {
			if (m_drag == 3) {
				const double df = -double(io.MouseDelta.x) / double(sz.x) * span;
				m_view0 += df;
				m_view1 += df;
				clamp_view();
			} else {
				const long f = std::lround(std::clamp(f_of(io.MousePos.x), 0.0, double(frames)));
				if (m_drag == 1)
					m_start = u32(std::clamp(f, 0L, long(m_end) - 8));
				else if (m_drag == 2)
					m_end = u32(std::clamp(f, long(m_start) + 8, long(frames)));
				else {
					m_loop_at = u32(f);
					m_loop_on = true;
				}
				fit(m_drag);
			}
		}
		// S・E・L は放したときに表へ
		if (ImGui::IsItemDeactivated() && (m_drag == 1 || m_drag == 2 || m_drag == 4))
			post_points();
		if (!ImGui::IsItemActive())
			m_drag = 0;
	}
	const u32 v0 = u32(std::floor(m_view0)), v1 = u32(std::min(double(frames), std::ceil(m_view1)));
	br.request_overview(num, v0, v1);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(p, ImVec2(p.x + sz.x, p.y + sz.y), true);
	dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255));
	const float mid = p.y + sz.y * 0.5f, half = sz.y * 0.5f - 2.0f;
	dl->AddLine(ImVec2(p.x, mid), ImVec2(p.x + sz.x, mid), IM_COL32(80, 90, 110, 255));
	auto y_of = [&](int v) { return mid - half * float(v) / 32768.0f; };
	const int nb = int(m_view.wave_hi.size());
	if (m_view.wave_number == num && nb > 0) {
		// 届いた見取り図の範囲（拡大の途中は前の範囲のこともある）で、区切りごとの位置を出す
		const double d0 = double(m_view.wave_from), d1 = double(m_view.wave_to ? m_view.wave_to : frames);
		const double per = (d1 - d0) / double(nb);
		const int cols = std::max(1, int(sz.x));
		if (double(nb) >= double(cols) * (d1 - d0) / span * 0.999) {
			// 1 列に区切りが 1 つ以上: 列ごとに最小と最大の縦線
			for (int x = 0; x < cols; x++) {
				const double fa = f_of(p.x + float(x)), fb = f_of(p.x + float(x + 1));
				int b0 = int(std::floor((fa - d0) / per)), b1 = int(std::ceil((fb - d0) / per));
				b0 = std::max(b0, 0);
				b1 = std::min(b1, nb);
				if (b1 <= b0)
					continue;
				int lo = 32767, hi = -32768;
				for (int b = b0; b < b1; b++) {
					lo = std::min(lo, int(m_view.wave_lo[size_t(b)]));
					hi = std::max(hi, int(m_view.wave_hi[size_t(b)]));
				}
				const bool clip = hi >= 32767 || lo <= -32768;
				dl->AddLine(ImVec2(p.x + float(x) + 0.5f, y_of(hi)), ImVec2(p.x + float(x) + 0.5f, std::max(y_of(lo), y_of(hi) + 1.0f)),
				            clip ? IM_COL32(235, 80, 70, 255) : IM_COL32(110, 200, 255, 255));
			}
		} else {
			// 拡大して 1 サンプルが何列にもなる: 点を線でつなぎ、点も打つ
			ImVec2 prev;
			for (int b = 0; b < nb; b++) {
				const double f = d0 + (double(b) + 0.5) * per;
				const ImVec2 pt(x_of(f), y_of(int(m_view.wave_hi[size_t(b)])));
				if (b)
					dl->AddLine(prev, pt, IM_COL32(110, 200, 255, 255), 1.5f);
				if (sz.x / float(span) > 6.0f)
					dl->AddCircleFilled(pt, 2.0f, IM_COL32(170, 225, 255, 255));
				prev = pt;
			}
		}
	}
	// 鳴らさない所を暗く。鳴り始めは緑、鳴り終わりは黄の線
	const float xs = x_of(double(m_start)), xe = x_of(double(m_end));
	if (m_start > 0 || m_end < frames) {
		dl->AddRectFilled(p, ImVec2(std::max(p.x, xs), p.y + sz.y), IM_COL32(0, 0, 0, 150));
		dl->AddRectFilled(ImVec2(std::min(p.x + sz.x, xe), p.y), ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(0, 0, 0, 150));
	}
	dl->AddLine(ImVec2(xs, p.y), ImVec2(xs, p.y + sz.y), IM_COL32(110, 230, 120, 255), m_drag == 1 ? 3.0f : 2.0f);
	dl->AddLine(ImVec2(xe, p.y), ImVec2(xe, p.y + sz.y), IM_COL32(255, 210, 90, 255), m_drag == 2 ? 3.0f : 2.0f);
	dl->AddText(ImVec2(xs + 3, p.y + 2), IM_COL32(110, 230, 120, 255), "S");
	dl->AddText(ImVec2(xe - fs * 0.8f, p.y + 2), IM_COL32(255, 210, 90, 255), "E");
	// ループするところ（ループの頭から鳴り終わりまで）を薄く塗り、頭に紫の線
	if (m_loop_on) {
		const float xl = x_of(double(m_loop_at)), xf = xe;
		dl->AddRectFilled(ImVec2(xl, p.y + sz.y - fs * 0.5f), ImVec2(xf, p.y + sz.y), IM_COL32(200, 120, 255, 120));
		dl->AddLine(ImVec2(xl, p.y), ImVec2(xl, p.y + sz.y), IM_COL32(200, 120, 255, 255), m_drag == 4 ? 3.0f : 2.0f);
		dl->AddText(ImVec2(xl + 3, p.y + sz.y - fs * 1.6f), IM_COL32(200, 120, 255, 255), "L");
	}
	if (playing) {
		const float xp = x_of(double(m_view.preview_pos));
		dl->AddLine(ImVec2(xp, p.y), ImVec2(xp, p.y + sz.y), IM_COL32(255, 255, 255, 230), 1.5f);
	}
	dl->PopClipRect();

	// 拡大しているときは、表示の位置を動かす棒
	if (zoomed) {
		float pos = float(m_view0);
		const float max_pos = float(double(frames) - span);
		ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderFloat("##scroll", &pos, 0.0f, max_pos, "")) {
			m_view0 = double(pos);
			m_view1 = m_view0 + span;
			clamp_view();
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
		m_coarse = cur.coarse;
		m_fine = cur.fine;
		m_attack = cur.attack;
		m_decay1 = cur.decay1;
		m_decay2 = cur.decay2;
		m_release = cur.release;
		m_level1 = cur.level1;
		m_level2 = cur.level2;
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

	// 音程。鍵 60 が録ったときの高さで、そこからずらす
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_coarse, "Pitch (semitones)"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderInt("##coarse", &m_coarse, -24, 24, "%+d"))
		m_dirty = true;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_fine, "Fine (cents)"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderInt("##fine", &m_fine, -64, 63, "%+d"))
		m_dirty = true;

	// エンベロープ。速さは大きいほど速く 0 は動かない、レベルは 127 が最大。左に形の目安、右に値
	ImGui::Spacing();
	ImGui::TextDisabled("%s", UI_TEXT(smp_env, "Envelope"));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_env_tip, "Rates run 0-63 (higher is faster, 0 stays put); levels 0-127. A key goes up at the attack rate, falls at decay 1 to level 1, then at decay 2 to level 2 and stays there while held (with the loop on, the sound keeps going). Release fades it out after the key is let go."));
	{
		const float gw = fs * 9, gh = ImGui::GetFrameHeightWithSpacing() * 6 - ImGui::GetStyle().ItemSpacing.y;
		const ImVec2 g0 = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(gw, gh));
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(g0, ImVec2(g0.x + gw, g0.y + gh), IM_COL32(16, 20, 26, 255));
		// 形の目安: 区間の幅は (64 - 速さ) に比べる（0 は動かないので長い平らな区間）、高さはレベル（dB を直線に）
		auto seg = [](int rate) { return rate <= 0 ? 0.0f : float(64 - rate) + 2.0f; };
		auto lv = [](int level) { return std::clamp(1.0f - float(127 - level) * 0.77f / 48.0f, 0.0f, 1.0f); };
		const float hold = 24.0f;
		const float wa = seg(m_attack), w1 = m_decay1 ? seg(m_decay1) : hold, w2 = m_decay2 ? seg(m_decay2) : 0.0f, wr = seg(m_release);
		const float l1 = m_decay1 ? lv(m_level1) : 1.0f, l2 = m_decay1 && m_decay2 ? lv(m_level2) : l1;
		const float total = (m_attack ? wa : hold) + w1 + w2 + hold + (m_release ? wr : hold);
		const float sx = (gw - 8) / total, top = g0.y + 4, bot = g0.y + gh - 4, hh = bot - top;
		float x = g0.x + 4;
		std::vector<ImVec2> pts = { ImVec2(x, bot) };
		x += (m_attack ? wa : hold) * sx;
		pts.push_back(ImVec2(x, m_attack ? top : bot));
		const float peak = m_attack ? 1.0f : 0.0f;
		x += w1 * sx;
		pts.push_back(ImVec2(x, bot - hh * peak * l1));
		x += w2 * sx;
		pts.push_back(ImVec2(x, bot - hh * peak * l2));
		x += hold * sx;
		pts.push_back(ImVec2(x, bot - hh * peak * l2));
		const float xoff = x;
		x += (m_release ? wr : hold) * sx;
		pts.push_back(ImVec2(x, m_release ? bot : bot - hh * peak * l2));
		dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(110, 200, 255, 255), 0, 2.0f);
		dl->AddLine(ImVec2(xoff, g0.y), ImVec2(xoff, g0.y + gh), IM_COL32(255, 210, 90, 160));
		ImGui::SameLine();
		ImGui::BeginGroup();
		const float lab2 = fs * 8;
		auto rate = [&](const char *name, const char *id, int &v, int max) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(name);
			ImGui::SameLine(lab2);   // BeginGroup の中ではまとまりの左端から
			ImGui::SetNextItemWidth(-1);
			if (ImGui::SliderInt(id, &v, 0, max))
				m_dirty = true;
		};
		rate(UI_TEXT(smp_env_attack, "Attack"), "##ar", m_attack, 63);
		rate(UI_TEXT(smp_env_decay1, "Decay 1"), "##d1r", m_decay1, 63);
		rate(UI_TEXT(smp_env_level1, "Level 1"), "##d1l", m_level1, 127);
		rate(UI_TEXT(smp_env_decay2, "Decay 2"), "##d2r", m_decay2, 63);
		rate(UI_TEXT(smp_env_level2, "Level 2 (sustain)"), "##d2l", m_level2, 127);
		rate(UI_TEXT(smp_env_release, "Release"), "##rr", m_release, 63);
		ImGui::EndGroup();
	}

	ImGui::Spacing();
	if (ImGui::Button(UI_TEXT(smp_apply, "Apply"), ImVec2(fs * 7, 0))) {
		sp::voice v;
		v.assigned = m_sample != 0;
		v.sample = m_sample;
		v.name = m_voice_name;
		v.level = m_level;
		v.pan = m_pan;
		v.coarse = m_coarse;
		v.fine = m_fine;
		v.attack = m_attack;
		v.decay1 = m_decay1;
		v.decay2 = m_decay2;
		v.release = m_release;
		v.level1 = m_level1;
		v.level2 = m_level2;
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
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(UI_TEXT(smp_play_hint_fmt, "Play it with bank MSB 16, LSB %d, program %d. Changes take effect when the voice is selected again."),
		                  m_bank, m_pgm);
}

} // namespace ui
