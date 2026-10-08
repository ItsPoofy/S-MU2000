// license:BSD-3-Clause
//
// VST3: パラメータで届いたプログラムチェンジを、バンクセレクトの後ろに並べる。
//
// VST3 のホストは、MIDI のコントロールチェンジとプログラムチェンジをパラメータの変化として渡してくる
// （kIsProgramChange の印が付いたパラメータ）。曲の同じ所に「CC0 → CC32 → プログラムチェンジ」と書いてあっても、
// ホストによっては順番が保たれない:
//   ・REAPER は、プログラムチェンジを処理ブロックの頭（位置 0）で渡し、CC は曲の中の本当の位置で渡す。
//     そのまま時刻順に流すと、プログラムチェンジがバンクセレクトより先になり、前のバンクの音色が選ばれる
//   ・位置が同じでも、ホストがパラメータを渡す順番（プログラムチェンジの列が先、など）はホスト次第
// XG のバンクセレクトは、次のプログラムチェンジで効く。だから、同じ処理ブロックに同じ口・同じチャンネルの
// バンクセレクトがあれば、プログラムチェンジはその後ろでなければ意味が通らない。
//
// ここでやること（パラメータから作ったメッセージだけが対象。ホストがイベントとして渡した MIDI は触らない）:
//   1. 同じ時刻のなかの順番を決める印（rank）を付ける: バンクセレクト → プログラムチェンジ → そのほか
//   2. プログラムチェンジより後ろの位置にバンクセレクト（CC0・CC32）があれば、プログラムチェンジをその位置へ移す。
//      基準はバンクセレクトだけ（モジュレーションやエクスプレッションのカーブが後ろまで続いていても、
//      プログラムチェンジを引きずらない。引きずると、間の音符が前の音色で鳴る）。
//      ほかのプログラムチェンジがそのバンクセレクト以降にあるなら、そのバンクセレクトはそちらのものなので移さない
// （原案と REAPER での確認は doranarasi さん。プルリクエスト #143）

#ifndef S_MU2000_VST3_PC_ORDER_H
#define S_MU2000_VST3_PC_ORDER_H

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smu2000::vst3 {

// 同じ時刻のなかの順番。小さいほうが先
enum : uint8_t {
	RANK_RESET   = 0,      // リセットの SysEx（is_reset_sysex。issue #51）
	RANK_BANK    = 1,      // パラメータで届いたバンクセレクト
	RANK_PROGRAM = 2,      // パラメータで届いたプログラムチェンジ
	RANK_OTHER   = 3,      // そのほか全部
};

// msgs の先頭から count 個がパラメータから作ったメッセージ。M は off（ブロックの中の位置）・port・n（バイト数）・
// b[]（バイト）・sysex（SysEx なら nullptr でない）・rank を持つ
template <typename M>
void order_program_changes(std::vector<M> &msgs, size_t count)
{
	if (count > msgs.size())
		count = msgs.size();
	const auto is_bank = [](const M &m) {
		return m.sysex == nullptr && (m.b[0] & 0xf0) == 0xb0 && (m.b[1] == 0 || m.b[1] == 32);
	};
	const auto is_program = [](const M &m) { return m.sysex == nullptr && (m.b[0] & 0xf0) == 0xc0; };
	bool any_program = false;
	for (size_t i = 0; i < count; i++) {
		M &m = msgs[i];
		if (is_bank(m)) {
			m.rank = RANK_BANK;
		} else if (is_program(m)) {
			m.rank = RANK_PROGRAM;
			any_program = true;
		}
	}
	if (!any_program)
		return;
	for (size_t i = 0; i < count; i++) {
		M &pc = msgs[i];
		if (!is_program(pc))
			continue;
		const uint8_t ch = pc.b[0] & 15;
		// 後ろにあるバンクセレクトのうち、いちばん遅いもの
		bool found = false;
		auto target = pc.off;
		for (size_t k = 0; k < count; k++) {
			const M &m = msgs[k];
			if (is_bank(m) && m.port == pc.port && (m.b[0] & 15) == ch && m.off > pc.off && (!found || m.off > target)) {
				target = m.off;
				found = true;
			}
		}
		if (!found)
			continue;
		// そのバンクセレクト以降に別のプログラムチェンジがあるなら、そちらのもの
		bool taken = false;
		for (size_t k = 0; k < count && !taken; k++)
			taken = k != i && is_program(msgs[k]) && msgs[k].port == pc.port && (msgs[k].b[0] & 15) == ch && msgs[k].off >= target;
		if (!taken)
			pc.off = target;
	}
}

} // namespace smu2000::vst3

#endif // S_MU2000_VST3_PC_ORDER_H
