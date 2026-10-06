// license:BSD-3-Clause
//
// サンプリングの窓の「ライブラリ」のタブ（sampling_editor::library_pane）。
// 作り込んだサンプル音色を 1 つずつ PC のファイルに取っておき（src/voice_lib.h）、分類して、好きなものを選んで
// 好きな枠へ戻す。サンプリングのメモリは電源を切ると消えるので、その代わりの置き場。
//
//   置き場   設定のフォルダーの voices/（その下のフォルダーが分類）。1 音色 = 1 ファイル（.smuvoice）
//   保存     音色のタブで選んでいる枠を、鳴らしているサンプルの波形ごと取り出して書く
//   ロード   エミュレーターへ: 選んだ枠へ戻す（要るサンプルは、同じものが無ければ空きへ足す）
//            実機へ（MIDI OUT）: その音色を書く SysEx（機種 0x68 のパラメータチェンジ 334 通）を送る。
//            **内蔵ウェーブだけの音色に限る**（サンプルの波形を実機のメモリへ書き足す道はまだ無い）
//   試聴     内蔵ウェーブだけの音色は、借りた枠（Bank# 1 の 128 番）で鳴らせる（内蔵音色のタブと同じやり方）

#include "sampling_editor.h"

#include "compat/paths.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace ui {

namespace sp = smu2000::sampling;
namespace vl = smu2000::voicelib;

namespace {

constexpr int BORROW_SLOT = 255;
constexpr const char *EXT = ".smuvoice";

std::filesystem::path u8path(const std::string &s)
{
	return std::filesystem::path(reinterpret_cast<const char8_t *>(s.c_str()));
}

std::string u8str(const std::filesystem::path &p)
{
	const std::u8string s = p.u8string();
	return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

std::string lib_dir()
{
	const std::string base = smu2000::config_dir();
	return base.empty() ? std::string() : smu2000::join(base, "voices");
}

bool read_file(const std::string &path, std::vector<u8> &out)
{
	std::ifstream f(u8path(path), std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

bool write_file(const std::string &path, const std::vector<u8> &data)
{
	std::error_code ec;
	std::filesystem::create_directories(u8path(path).parent_path(), ec);
	std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
	if (!f)
		return false;
	f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
	return bool(f);
}

// ファイルやフォルダーの名前に使えない字を除く
std::string safe_name(std::string s)
{
	for (char &c : s)
		if (u8(c) < 0x20 || std::strchr("\\/:*?\"<>|", c))
			c = '_';
	while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
		s.pop_back();
	while (!s.empty() && s.front() == ' ')
		s.erase(s.begin());
	return s;
}

std::string lower(std::string s)
{
	for (char &c : s)
		c = char(std::tolower(u8(c)));
	return s;
}

std::string file_for(const std::string &category, const std::string &name)
{
	std::string dir = lib_dir();
	const std::string cat = safe_name(category);
	if (!cat.empty())
		dir = smu2000::join(dir, cat);
	std::string stem = safe_name(name);
	if (stem.empty())
		stem = "voice";
	return smu2000::join(dir, stem + EXT);
}

} // namespace

// 置き場を読み直す
void sampling_editor::library_scan()
{
	m_lib.clear();
	m_lib_cats.clear();
	m_lib_scanned = true;
	const std::string dir = lib_dir();
	if (dir.empty())
		return;
	romwave_build();
	std::error_code ec;
	const std::filesystem::path root = u8path(dir);
	if (!std::filesystem::exists(root, ec))
		return;
	for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
		if (it.depth() > 1 || !it->is_regular_file(ec) || it->path().extension() != EXT)
			continue;
		std::vector<u8> data;
		const std::string path = u8str(it->path());
		if (!read_file(path, data))
			continue;
		vl::item item;
		std::string err;
		size_t frames = 0;
		if (!vl::load(data, item, err, false, &frames))
			continue;
		lib_entry e;
		e.path = path;
		e.category = it.depth() == 1 ? u8str(it->path().parent_path().filename()) : std::string();
		e.name = item.name();
		e.memo = item.memo;
		e.samples = int(item.samples.size());
		e.frames = frames;
		e.voice = item.voice;
		for (int k = 0; k < 4; k++)
			if (item.el_sample[size_t(k)] >= 0)
				e.el_sample[size_t(k)] = item.samples[size_t(item.el_sample[size_t(k)])].name;
		for (int k = 0; k < 4; k++) {
			if (!item.el_on(k))
				continue;
			e.elements++;
			std::string w;
			if (item.el_sample[size_t(k)] >= 0)
				w = "\"" + item.samples[size_t(item.el_sample[size_t(k)])].name + "\"";
			else if (item.el_rom_wave(k) >= 0)
				w = "W" + std::to_string(item.el_rom_wave(k));
			if (!w.empty())
				e.waves += (e.waves.empty() ? "" : " + ") + w;
		}
		m_lib.push_back(std::move(e));
	}
	std::sort(m_lib.begin(), m_lib.end(), [](const lib_entry &a, const lib_entry &b) {
		return a.category != b.category ? a.category < b.category : lower(a.name) < lower(b.name);
	});
	for (const lib_entry &e : m_lib)
		if (!e.category.empty() && std::find(m_lib_cats.begin(), m_lib_cats.end(), e.category) == m_lib_cats.end())
			m_lib_cats.push_back(e.category);
}

void sampling_editor::library_pane(bridge &br)
{
	m_lib_drawn = true;
	if (!m_lib_scanned)
		library_scan();
	const float fs = ImGui::GetFontSize();
	const int slot = m_bank * 128 + (m_pgm - 1);

	// ---- 取り出しを頼んだ音色が届いたら、ファイルに書く
	if (m_lib_job && m_lib_job->done.load(std::memory_order_acquire)) {
		std::shared_ptr<lib_job> job = std::move(m_lib_job);
		if (!job->err.empty()) {
			m_lib_note = job->err;
		} else {
			job->item.set_name(job->name);
			job->item.memo = job->memo;
			if (write_file(job->path, vl::save(job->item))) {
				m_lib_note = std::string(UI_TEXT(lib_saved, "Saved to the library: ")) + job->name;
				library_scan();
				for (size_t i = 0; i < m_lib.size(); i++)
					if (m_lib[i].path == job->path)
						m_lib_sel = int(i);
				m_lib_edit_for = -1;
			} else {
				m_lib_note = std::string(UI_TEXT(lib_cannot_write, "Cannot write: ")) + job->path;
			}
		}
	}

	// ---- 左: 分類・絞り込み・一覧
	const float left_w = std::min(fs * 26.0f, ImGui::GetContentRegionAvail().x * 0.48f);
	ImGui::BeginChild("lib_left", ImVec2(left_w, 0));
	{
		ImGui::SetNextItemWidth(fs * 9);
		std::string cats = std::string(UI_TEXT(lib_cat_all, "All categories")) + '\0' + UI_TEXT(lib_cat_none, "(no category)") + '\0';
		for (const std::string &c : m_lib_cats)
			cats += c + '\0';
		m_lib_cat = std::clamp(m_lib_cat, 0, int(m_lib_cats.size()) + 1);
		ImGui::Combo("##cat", &m_lib_cat, cats.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-fs * 5.5f);
		ImGui::InputTextWithHint("##find", UI_TEXT(lib_find, "Name or note"), m_lib_find, sizeof(m_lib_find));
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(lib_rescan, "Reload"), ImVec2(-1, 0))) {
			library_scan();
			m_lib_sel = -1;
		}
		const std::string want = lower(m_lib_find);
		if (ImGui::BeginTable("lib_list", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(UI_TEXT(lib_col_name, "Voice"), ImGuiTableColumnFlags_WidthFixed, fs * 6.5f);
			ImGui::TableSetupColumn(UI_TEXT(lib_col_cat, "Category"), ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
			ImGui::TableSetupColumn(UI_TEXT(lib_col_waves, "Waves"));
			ImGui::TableHeadersRow();
			for (int i = 0; i < int(m_lib.size()); i++) {
				const lib_entry &e = m_lib[size_t(i)];
				if (m_lib_cat == 1 && !e.category.empty())
					continue;
				if (m_lib_cat >= 2 && e.category != m_lib_cats[size_t(m_lib_cat - 2)])
					continue;
				if (!want.empty() && lower(e.name).find(want) == std::string::npos && lower(e.memo).find(want) == std::string::npos)
					continue;
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Selectable(e.name.empty() ? "-" : e.name.c_str(), i == m_lib_sel, ImGuiSelectableFlags_SpanAllColumns))
					m_lib_sel = i;
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", e.category.c_str());
				ImGui::TableNextColumn();
				if (e.samples)
					ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.6f, 1.0f), "%s", e.waves.c_str());
				else
					ImGui::TextUnformatted(e.waves.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginChild("lib_right", ImVec2(0, 0));

	// ---- いまの音色を保存
	ImGui::TextUnformatted(UI_TEXT(lib_save_head, "Save the current voice"));
	ImGui::Separator();
	if (m_lib_save_for != slot || m_lib_save_src != m_voice_name) {
		// 枠が替わったか、その枠の音色の名前が替わったら（ロードした・音色のタブで書き直した）、名前の欄をそれに合わせる
		std::snprintf(m_lib_save_name, sizeof(m_lib_save_name), "%s", m_voice_name);
		m_lib_save_src = m_voice_name;
		m_lib_save_for = slot;
	}
	ImGui::TextDisabled(UI_TEXT(lib_from_fmt, "From sample voice Bank# %d number %d (the one chosen in the Voice tab)"), m_bank, m_pgm);
	const float lab = fs * 6.0f;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(lib_name, "Name"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 8);
	ImGui::InputText("##sname", m_lib_save_name, sizeof(m_lib_save_name));
	ImGui::SameLine();
	ImGui::TextUnformatted(UI_TEXT(lib_category, "Category"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 9);
	ImGui::InputTextWithHint("##scat", UI_TEXT(lib_category_hint, "e.g. Pads"), m_lib_save_cat, sizeof(m_lib_save_cat));
	if (!m_lib_cats.empty()) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 1.6f);
		if (ImGui::BeginCombo("##scats", "", ImGuiComboFlags_NoPreview)) {
			for (const std::string &c : m_lib_cats)
				if (ImGui::Selectable(c.c_str()))
					std::snprintf(m_lib_save_cat, sizeof(m_lib_save_cat), "%s", c.c_str());
			ImGui::EndCombo();
		}
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(lib_memo, "Note"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##smemo", m_lib_save_memo, sizeof(m_lib_save_memo));
	auto start_save = [&] {
		auto job = std::make_shared<lib_job>();
		job->name = m_lib_save_name;
		job->memo = m_lib_save_memo;
		job->path = file_for(m_lib_save_cat, m_lib_save_name);
		m_lib_job = job;
		// 音色のタブで触ったままの値（その枠を開いているときだけ）
		const bool dirty = m_dirty && m_loaded_slot == slot;
		sp::voice v;
		if (dirty) {
			stash_el();
			v.name = m_voice_name;
			v.el = m_els;
		}
		br.post([job, slot, dirty, v](mu2000 &mu) {
			std::string err;
			// 音色のタブで触ったままの値があれば、先に書き込む（試聴と同じ）
			if (dirty)
				mu.sampling_set_voice(slot, v, err);
			if (err.empty())
				mu.sampling_export_voice(slot, job->item, err);
			job->err = err;
			job->done.store(true, std::memory_order_release);
			return std::string();
		});
		if (dirty)
			m_dirty = false;
	};
	ImGui::BeginDisabled(m_lib_job != nullptr || !m_lib_save_name[0] || lib_dir().empty());
	if (ImGui::Button(UI_TEXT(lib_save, "Save to the library"))) {
		std::error_code ec;
		if (std::filesystem::exists(u8path(file_for(m_lib_save_cat, m_lib_save_name)), ec))
			ImGui::OpenPopup("###lib_over");
		else
			start_save();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(lib_save_tip, "Writes the voice to a file on this PC: all four elements with every setting, and the waveform of any sample it plays. The sampling memory is lost at power-off; the library is not."));
	const std::string over_title = std::string(UI_TEXT(lib_over_title, "Replace?")) + "###lib_over";
	if (ImGui::BeginPopupModal(over_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(UI_TEXT(lib_over_text, "A voice with this name is already in that category. Replace it?"));
		if (ImGui::Button("OK", ImVec2(fs * 6, 0))) {
			start_save();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(dlg_cancel, "Cancel"), ImVec2(fs * 6, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (!m_lib_note.empty())
		ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", m_lib_note.c_str());

	// ---- 選んだ音色
	ImGui::Spacing();
	ImGui::TextUnformatted(UI_TEXT(lib_sel_head, "Selected voice"));
	ImGui::Separator();
	if (m_lib_sel < 0 || m_lib_sel >= int(m_lib.size())) {
		ImGui::TextDisabled("%s", m_lib.empty() ? UI_TEXT(lib_empty, "The library is empty. Make a voice in the Voice tab and save it here.")
		                                        : UI_TEXT(lib_pick, "Pick a voice from the list."));
		ImGui::TextDisabled("%s", lib_dir().c_str());
		ImGui::EndChild();
		return;
	}
	const lib_entry sel = m_lib[size_t(m_lib_sel)];       // 写し（下で一覧を作り直すことがある）
	if (m_lib_edit_for != m_lib_sel) {
		std::snprintf(m_lib_edit_name, sizeof(m_lib_edit_name), "%s", sel.name.c_str());
		std::snprintf(m_lib_edit_cat, sizeof(m_lib_edit_cat), "%s", sel.category.c_str());
		std::snprintf(m_lib_edit_memo, sizeof(m_lib_edit_memo), "%s", sel.memo.c_str());
		m_lib_edit_for = m_lib_sel;
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(lib_name, "Name"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 8);
	ImGui::InputText("##ename", m_lib_edit_name, sizeof(m_lib_edit_name));
	ImGui::SameLine();
	ImGui::TextUnformatted(UI_TEXT(lib_category, "Category"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 9);
	ImGui::InputText("##ecat", m_lib_edit_cat, sizeof(m_lib_edit_cat));
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(lib_memo, "Note"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##ememo", m_lib_edit_memo, sizeof(m_lib_edit_memo));
	const bool edited = sel.name != m_lib_edit_name || sel.category != m_lib_edit_cat || sel.memo != m_lib_edit_memo;
	ImGui::BeginDisabled(!edited || !m_lib_edit_name[0]);
	if (ImGui::Button(UI_TEXT(lib_apply, "Save these changes"))) {
		// 名前・分類・覚え書きを書き直す。名前か分類が変われば、ファイルも移す
		std::vector<u8> data;
		vl::item item;
		std::string err;
		const std::string to = file_for(m_lib_edit_cat, m_lib_edit_name);
		std::error_code ec;
		if (to != sel.path && std::filesystem::exists(u8path(to), ec)) {
			m_lib_note = UI_TEXT(lib_exists, "A voice with that name is already in that category.");
		} else if (read_file(sel.path, data) && vl::load(data, item, err)) {
			item.set_name(m_lib_edit_name);
			item.memo = m_lib_edit_memo;
			if (write_file(to, vl::save(item))) {
				if (to != sel.path)
					std::filesystem::remove(u8path(sel.path), ec);
				library_scan();
				m_lib_sel = -1;
				for (size_t i = 0; i < m_lib.size(); i++)
					if (m_lib[i].path == to)
						m_lib_sel = int(i);
				m_lib_edit_for = -1;
				m_lib_note.clear();
			} else {
				m_lib_note = std::string(UI_TEXT(lib_cannot_write, "Cannot write: ")) + to;
			}
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(lib_delete, "Delete...")))
		ImGui::OpenPopup("###lib_del");
	const std::string del_title = std::string(UI_TEXT(lib_delete, "Delete...")) + "###lib_del";
	if (ImGui::BeginPopupModal(del_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text(UI_TEXT(lib_delete_fmt, "Delete \"%s\" from the library? The file is removed from this PC."), sel.name.c_str());
		if (ImGui::Button("OK", ImVec2(fs * 6, 0))) {
			std::error_code ec;
			std::filesystem::remove(u8path(sel.path), ec);
			library_scan();
			m_lib_sel = -1;
			m_lib_edit_for = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(dlg_cancel, "Cancel"), ImVec2(fs * 6, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// 中身
	ImGui::Spacing();
	for (int k = 0; k < 4; k++) {
		const size_t b = 12 + 84 * size_t(k);
		if (sel.voice.size() < b + 84)
			break;
		const bool on = (sel.voice[0] >> k) & 1;
		const bool smp = (sel.voice[b + 2] & 0x40) != 0;
		const int set = smp ? -1 : (sel.voice[b + 2] << 7) | (sel.voice[b + 3] & 0x7f);
		if (!smp && (set < 0 || set >= sp::ROM_WAVE_SETS))
			continue;                                         // 波形なし
		std::string what = smp ? std::string(UI_TEXT(lib_el_sample, "a sample")) + " \"" + sel.el_sample[size_t(k)] + "\"" : romwave_label(set);
		if (on)
			ImGui::Text(UI_TEXT(lib_el_fmt, "Element %d: %s"), k + 1, what.c_str());
		else
			ImGui::TextDisabled(UI_TEXT(lib_el_off_fmt, "Element %d (not played): %s"), k + 1, what.c_str());
	}
	if (sel.samples)
		ImGui::TextDisabled(UI_TEXT(lib_samples_fmt, "Carries %d sample(s), %.2f s in all"), sel.samples, double(sel.frames) / 44100.0);
	else
		ImGui::TextDisabled("%s", UI_TEXT(lib_no_samples, "Built-in waves only (no sample data)"));

	// ---- ロード
	ImGui::Spacing();
	ImGui::TextUnformatted(UI_TEXT(lib_load_head, "Load"));
	ImGui::Separator();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(lib_to, "To"));
	ImGui::SameLine();
	ImGui::RadioButton(UI_TEXT(lib_to_emu, "this emulator"), &m_lib_target, 0);
	ImGui::SameLine();
	ImGui::RadioButton(UI_TEXT(lib_to_hw, "a real MU2000 (MIDI out)"), &m_lib_target, 1);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Bank#");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 5);
	if (ImGui::InputInt("##lbank", &m_bank)) {
		m_bank = std::clamp(m_bank, 0, 1);
		m_loaded_slot = -1;
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(UI_TEXT(lib_number, "number"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 6);
	if (ImGui::InputInt("##lpgm", &m_pgm)) {
		m_pgm = std::clamp(m_pgm, 1, 128);
		m_loaded_slot = -1;
	}
	if (m_lib_target == 0) {
		if (ImGui::Button(UI_TEXT(lib_load, "Load into this slot"))) {
			std::vector<u8> data;
			auto item = std::make_shared<vl::item>();
			std::string err;
			if (read_file(sel.path, data) && vl::load(data, *item, err)) {
				const std::string done = UI_TEXT(lib_loaded_fmt, "Loaded from the library (%d sample(s) added)");
				br.post([item, slot, done](mu2000 &mu) {
					std::string e;
					int added = 0;
					if (!mu.sampling_import_voice(slot, *item, e, &added))
						return e;
					char buf[160];
					std::snprintf(buf, sizeof(buf), done.c_str(), added);
					return std::string(buf);
				});
				m_loaded_slot = -1;           // 音色のタブで読み直す
				m_dirty = false;
				m_lib_note.clear();
			} else {
				m_lib_note = std::string(UI_TEXT(dlg_cannot_fmt, "Cannot open: %s")) + " " + err;
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(lib_load_tip, "Writes the voice into the slot above, replacing what is there. Samples it needs are added to the sampling memory unless the same sample (same name and waveform) is already there."));
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(lib_open_voice, "Open in the Voice tab")))
			m_goto_tab = 1;
		// 試聴（内蔵ウェーブだけの音色）
		ImGui::BeginDisabled(sel.samples > 0);
		ImGui::Button(UI_TEXT(smp_audition, "Hold to play"), ImVec2(fs * 9, 0));
		const bool hold_on = ImGui::IsItemActivated(), hold_off = ImGui::IsItemDeactivated();
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", sel.samples > 0
			                  ? UI_TEXT(lib_audition_no, "A voice that carries samples has to be loaded first: listening would add its samples to the sampling memory.")
			                  : UI_TEXT(lib_audition_tip, "Plays the voice without loading it, on a borrowed slot (the last sample voice, Bank# 1 number 128) and part 1. What that slot held is put back when you leave this tab."));
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(UI_TEXT(smp_audition_key, "Key"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 6);
		if (ImGui::InputInt("##lkey", &m_audition_key))
			m_audition_key = std::clamp(m_audition_key, 0, 127);
		if (hold_on && sel.voice.size() == sp::VOICE_SIZE) {
			if (!m_pv_keep)
				m_pv_keep = std::make_shared<std::vector<u8>>();
			auto keep = m_pv_keep;
			const std::vector<u8> rec = sel.voice;
			m_pv_borrowed = true;
			br.post([keep, rec](mu2000 &mu) {
				if (keep->empty())
					mu.sampling_voice_raw(BORROW_SLOT, *keep);
				mu.sampling_set_voice_raw(BORROW_SLOT, rec);
				return std::string();
			});
			// 借りた枠は毎回選び直す（中身が替わったことを firmware に知らせる。内蔵音色のタブと同じ理由）
			m_pv_selected = false;
			m_pv_held = m_audition_key;
			const std::vector<u8> msg = { 0xb0, 0x00, 0x10, 0xb0, 0x20, 0x01, 0xc0, 0x7f, 0x90, u8(m_pv_held), 0x64 };
			br.send(msg);
		}
		if (hold_off && m_pv_held >= 0) {
			const std::vector<u8> msg = { 0x80, u8(m_pv_held), 0x40 };
			br.send(msg);
			m_pv_held = -1;
		}
	} else {
		// 実機へ: この音色を書く SysEx（334 通）を MIDI OUT へ。サンプルを鳴らす音色は送れない
		const bool busy = m_sx_job != nullptr || !m_sx_queue.empty();
		ImGui::BeginDisabled(sel.samples > 0 || busy || !xgui::out_ready() || sel.voice.size() != sp::VOICE_SIZE);
		if (ImGui::Button(UI_TEXT(lib_send, "Send to the real MU2000"))) {
			const std::vector<std::vector<u8>> msgs = sp::voice_sysex(slot, sel.voice.data());
			m_sx_queue.assign(msgs.begin(), msgs.end());
			m_sx_total = m_sx_queue.size();
			m_sx_wipe_wait = false;
			m_sx_next = ImGui::GetTime();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", sel.samples > 0
			                  ? UI_TEXT(lib_send_no, "Not yet: this voice plays samples, and writing sample waveforms into a real unit's memory from here is not done. Voices made of built-in waves can be sent.")
			                  : UI_TEXT(lib_send_tip, "Sends this voice to the MIDI out as SysEx (Yamaha model 0x68 parameter changes, 334 messages, a few seconds). It writes only the sample voice slot above on the real unit; nothing else in its memory is touched. The unit must be in a mode where sample voices can be selected (XG)."));
		if (!m_sx_queue.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled(UI_TEXT(smp_sx_sending_fmt, "Sending %d / %d"), int(m_sx_total - m_sx_queue.size()), int(m_sx_total));
		}
		if (xgui::out_ready())
			xgui::out_port_combo();
		else
			ImGui::TextDisabled("%s", UI_TEXT(lib_no_out, "No MIDI out is open (choose one from the panel's menu)."));
		ImGui::TextDisabled("%s", UI_TEXT(lib_hw_note, "The real unit's sampling memory is lost at power-off too: send the voice again after switching it on, or save it to a SmartMedia card on the unit."));
	}
	ImGui::Spacing();
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", sel.path.c_str());
	ImGui::PopTextWrapPos();
	ImGui::EndChild();
}

} // namespace ui
