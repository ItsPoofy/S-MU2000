// license:BSD-3-Clause
// Exercises processing, persistence and the actual ImGui settings widgets.
#include "ui/settings.h"
#include "ui/font_file.h"
#include "ui/settings_view.h"
#include "ui/output_limiter.h"
#include "imgui_internal.h"
#include <iostream>
#include <map>
#include <stdexcept>

static void require(bool ok, const char *why)
{
	if (!ok) throw std::runtime_error(why);
}

struct item { ImRect rect; ImGuiWindow *window; bool disabled; };
static std::map<ImGuiID, ImRect> rectangles;
static std::map<std::string, item> items;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *ctx, ImGuiID id, const ImRect &rect, const ImGuiLastItemData *data)
{
	rectangles[id] = data ? data->NavRect : rect;
	if (!ctx->CurrentWindow || ctx->CurrentWindow->IDStack.empty()) return;
	// BeginCombo supplies ItemAdd but does not emit an ItemInfo hook.
	for (const char *label : {"Resampler", "Driver", "Language", "言語", "Stream sample rate", "Left output channel", "Right output channel", "##device"})
		if (id && id == ctx->CurrentWindow->GetID(label))
			items.try_emplace(label, item{rectangles[id], ctx->CurrentWindow, bool(ctx->LastItemData.ItemFlags & ImGuiItemFlags_Disabled)});
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *ctx, ImGuiID id, const char *label, ImGuiItemStatusFlags)
{
	items.try_emplace(label, item{rectangles[id], ctx->CurrentWindow, bool(ctx->LastItemData.ItemFlags & ImGuiItemFlags_Disabled)});
}
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID) { return nullptr; }

static void processing()
{
	ui::output_limiter limiter;
	double l = 2, r = 1;
	limiter.process(l, r, true);
	require(std::abs(l - 0.98) < 1e-9 && std::abs(r - 0.49) < 1e-9, "Limiter attack/stereo linking");
	for (int i = 0; i < 44100; i++) { l = r = 0.5; limiter.process(l, r, true); }
	require(l > 0.4999, "Limiter release");
	l = 2; r = -1; limiter.process(l, r, false);
	require(l == 2 && r == -1, "Limiter bypass must preserve samples");
	for (auto quality : {ui::resampler_quality::sinc, ui::resampler_quality::linear, ui::resampler_quality::nearest})
	for (int rate : {8000, 44100, 48000, 96000, 192000}) {
		ui::audio_stream_renderer renderer;
		renderer.configure(rate, quality);
		u64 generated = 0;
		std::vector<s16> output(size_t(rate) * 6);
		renderer.render(output.data(), unsigned(rate), 6, 4, 2,
			[&](s16 *dst, unsigned n) {
				for (unsigned i = 0; i < n; i++, generated++) {
					dst[i * 2] = s16(10000 * std::sin(2 * 3.141592653589793 * 440 * generated / 44100));
					dst[i * 2 + 1] = -dst[i * 2];
				}
			}, ui::audio_stream_renderer::pcm16);
		unsigned crossings = 0;
		for (int i = 1; i < rate; i++) {
			for (int ch : {0, 1, 3, 5}) require(output[size_t(i) * 6 + ch] == 0, "Unused channel contains audio");
			require(std::abs(int(output[size_t(i) * 6 + 4]) + output[size_t(i) * 6 + 2]) <= 1, "Stereo routing");
			if (output[size_t(i - 1) * 6 + 4] <= 0 && output[size_t(i) * 6 + 4] > 0) crossings++;
		}
		require(crossings >= 439 && crossings <= 441, "Resampling changed pitch");
		require(generated > 44090 && generated < 44180, "Resampling advanced the MU clock incorrectly");
	}
}

static void persistence()
{
	ui::remembered in, out;
	in.audio_out = "USB = audio";
	in.audio.exclusive = true;
	in.audio.stream = {96000, 256, 4, 5};
	in.audio_routes = {{"USB = audio", 4, 5}, {"HDMI", 1, 0}};
	in.audio.stream.quality = ui::resampler_quality::linear;
	in.limiter = true; in.native_fx = 2; in.native_engine = 1;
	ui::apply_settings(ui::collect_settings(in), out);
	require(out.audio == in.audio && out.audio_out == in.audio_out && out.limiter && out.native_fx == 2 && out.native_engine == 1, "Audio preferences round trip");
	require(out.audio_routes.size() == 2 && out.audio_routes[0].device == in.audio_out && out.audio_routes[0].left == 4 && out.audio_routes[1].right == 0, "Per-device routing round trip");
	ui::remembered invalid;
	ui::apply_settings({{"audio_rate", "48000garbage"}, {"audio_buffer", "-1"}, {"audio_left", "3"}, {"audio_right", "3"}}, invalid);
	require(ui::valid_audio_request(invalid.audio.stream) && invalid.audio.stream.sample_rate == 0 && invalid.audio.stream.left == 0, "Malformed settings validation");
	ui::menu_state menu;
	for (const auto &groups : {ui::menu_ports(menu), ui::menu_card(menu), ui::menu_phones(menu), ui::menu_power(menu), ui::menu_ain_only({}, "")}) {
		const auto &last = groups.back();
		require(last.title.empty() && last.items.size() == 2 && last.items[0].separator
			&& last.items[1].id == ui::ID_SETTINGS && last.items[1].enabled, "Quick menu lacks a separated Settings shortcut");
	}
}

static void lazy_fonts()
{
	ImGui::CreateContext();
	auto *atlas = ImGui::GetIO().Fonts;
	ImFontConfig cfg; cfg.SizePixels = 16;
	auto *primary = atlas->AddFontDefaultVector(&cfg);
	cfg.MergeMode = true; cfg.GlyphRanges = cjk_fullwidth_ranges;
	atlas->AddFontDefaultVector(&cfg);
	cfg.MergeMode = false; cfg.GlyphRanges = nullptr;
	auto *other = atlas->AddFontDefaultVector(&cfg);
	const int english_sources = atlas->Sources.Size;
	ensure_cjk_ui_fonts(atlas);
	require(atlas->Sources.Size == english_sources, "English startup loaded Japanese ranges");
	ui::set_lang(ui::lang::ja);
	ensure_cjk_ui_fonts(atlas);
	require(atlas->Sources.Size == english_sources + 1 && primary->Sources.Size == 3 && other->Sources.Size == 1,
		"Language change did not merge Japanese into the original UI font");
	ensure_cjk_ui_fonts(atlas);
	ui::set_lang(ui::lang::en); ensure_cjk_ui_fonts(atlas);
	ui::set_lang(ui::lang::ja); ensure_cjk_ui_fonts(atlas);
	require(atlas->Sources.Size == english_sources + 1, "Language changes loaded duplicate fonts");
	atlas->Build();
	ImGui::DestroyContext();
	ui::set_lang(ui::lang::en);
}

static void interface()
{
	ImGui::CreateContext();
	auto &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(760, 560);
	io.DeltaTime = 1.0f / 60;
	io.Fonts->Build();
	GImGui->TestEngineHookItems = true;
	ui::settings_state state;
	state.ready = state.connected = true;
	state.outputs = {"Speakers", "USB"};
	state.stream = {{44100, 48000}, {"Output 1", "Output 2", "Output 3", "Output 4"}, 44100};
	ui::audio_output_config applied;
	int calls = 0, command = 0;
	ui::settings_actions actions;
	actions.audio = [&](auto config) { applied = config; calls++; };
	actions.language = [](int value) { ui::set_lang(ui::lang(value)); };
	actions.command = [&](int id) { command = id; };
	actions.input = [](auto) {};
	actions.volume = [](float) {};
	actions.limiter = [](bool) {};
	ui::settings_view view(state, actions);
	xg::model model;
	ui::xg_snapshot snapshot;
	ui::bridge bridge;
	const auto frame = [&] {
		items.clear(); rectangles.clear();
		ImGui::NewFrame(); view.draw(model, snapshot, bridge); ImGui::Render();
		require(ImGui::GetDrawData()->TotalVtxCount > 0, "Settings window produced no drawing");
	};
	frame(); frame();
	const auto click = [&](const std::string &label) {
		require(items.contains(label), ("Missing widget: " + label).c_str());
		auto target = items.at(label);
		if (!target.window->ClipRect.Contains(target.rect.GetCenter())) {
			ImGui::SetScrollY(target.window, target.window->Scroll.y + target.rect.Min.y - target.window->ClipRect.Min.y - 20);
			frame(); target = items.at(label);
		}
		const auto center = target.rect.GetCenter();
		io.AddMousePosEvent(center.x, center.y); frame();
		io.AddMouseButtonEvent(0, true); frame();
		io.AddMouseButtonEvent(0, false); frame();
		frame();
	};
	click("Stream sample rate"); click("48000 Hz");
	require(calls == 1 && applied.preferences.stream.sample_rate == 48000, "Rate selection did not reach controller");
	require(!items.contains("Apply audio settings") && !items.contains("Revert"), "Manual audio apply controls remained");
	state.busy = true; frame(); click("Stream sample rate");
	require(calls == 1, "Audio controls remain active during reopen");
	state.busy = false; state.audio = applied; frame();
	click("Stream sample rate"); click("44100 Hz");
	require(calls == 2, "Audio edits did not apply immediately");
	state.busy = true; frame(); state.busy = false; state.error = "Unsupported format"; frame();
	click("Stream sample rate"); click("48000 Hz");
	require(calls == 2, "Failed audio edit did not restore the displayed selection");
	click("Resampler"); click("Linear");
	require(calls == 3 && applied.preferences.stream.quality == ui::resampler_quality::linear, "Resampler selection did not apply immediately");
	state.audio = applied; frame();
	click("Emulation");
	for (const char *label : {"Play effects in C++", "Lighten heavy MIDI", "Play without the firmware"}) {
		const auto center = items.at(label).rect.GetCenter();
		io.AddMousePosEvent(center.x, center.y);
		for (int i = 0; i < 45; i++) frame();
		bool tooltip = false;
		for (const auto *window : GImGui->Windows)
			tooltip |= (window->Flags & ImGuiWindowFlags_Tooltip) && window->Active && !window->Hidden;
		require(tooltip, (std::string("No hover help for: ") + label).c_str());
	}
	click("Play effects in C++");
	require(command == ui::ID_NATIVE_FX, "Emulation setting did not reach controller");
	click("Lighten heavy MIDI");
	require(command == ui::ID_THIN_BENDS, "MIDI lightening setting did not reach controller");
	click("General"); click("Language"); click("日本語");
	require(ui::get_lang() == ui::lang::ja && items.contains("一般"), "General language selection did not translate the interface");
	click("言語"); click("English");
	require(ui::get_lang() == ui::lang::en, "English language selection");
	click("Audio"); frame();
	// Refreshing the list removes disconnected endpoints from the actual popup.
	state.outputs = {"HDMI"}; click("##device");
	require(items.contains("HDMI") && !items.contains("USB") && !items.contains("Speakers"), "Device popup did not refresh");
	ImGui::DestroyContext();
}

int main()
{
	try {
		ui::init_lang("en");
		processing(); persistence(); lazy_fonts(); interface();
		std::cout << "Settings processing, persistence and ImGui interactions: PASS\n";
	} catch (const std::exception &e) {
		std::cerr << e.what() << '\n'; return 1;
	}
}
