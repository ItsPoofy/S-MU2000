// license:BSD-3-Clause
//
// 軽量モードの入り口: エフェクトの口 7 つ（native_fx）。doc/native-dsp.md を見よ。
// 口 1 つぶん（種類と値から C++ のエフェクトを選んで鳴らす fx_slot）は fx_slot.h に分けてある
// （そちらは MU のエミュレーション無しで使える。src/dsp/README.md）。
//
// **実機（MEG）の再現ではない。** 種類ごとの系統（残響・ディレイ・揺れ・歪み・EQ・ダイナミクス・
// ローファイ）に合わせて、似た掛かり方の C++ の作りを当てているだけで、同じ音にはならない。
// 正しさが要るときは今までどおり MEG を回す（既定はそちら）。
//
// 値は XG の生の数（0-127 など）で渡す。意味（秒・ミリ秒・Hz・dB）は xg/fx_params.h の表から引く。

#ifndef S_MU2000_DSP_FX_NATIVE_H
#define S_MU2000_DSP_FX_NATIVE_H

#pragma once

#include "fx_slot.h"
#include "meg_fx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace smu2000::dsp {

// 7 つの口（リバーブ・コーラス・バリエーション・インサーション 1-4）をまとめたもの
class native_fx
{
public:
	enum slot_id { REVERB = 0, CHORUS = 1, VARIATION = 2, INS1 = 3, INS2 = 4, INS3 = 5, INS4 = 6, SLOTS = 7 };

	void set_rate(float rate)
	{
		for (auto &s : m_slot)
			s.set_rate(rate);
		m_meq.set_rate(rate);
	}

	master_eq &meq() { return m_meq; }

	void set(slot_id id, int type, const int *raw, int count)
	{
		m_slot[id].set_insertion(id >= INS1);
		m_slot[id].set(type, raw, count);
	}
	void reset() { for (auto &s : m_slot) s.reset(); for (auto &s : m_mfx) s.reset(); }

	fx_slot &slot(slot_id id) { return m_slot[id]; }

	// MEG と同じ作りのエフェクト（リバーブ・コーラス・バリエーション・インサーション 1 の口ごと）。
	// firmware が置いたプログラムの形を知っていれば、その口はこちらで鳴らす
	meg_fx_slot &mfx(slot_id id) { return m_mfx[id]; }

	void process(slot_id id, float l, float r, float &ol, float &orr)
	{
		// インサーションは音がそこを通るので、種類が無い・分からないときは素通し。
		// 送り（リバーブなど）は、種類が無ければ何も出さない
		if (id >= INS1 && m_slot[id].current() == fx_slot::kind::none) {
			ol = l;
			orr = r;
			return;
		}
		m_slot[id].process(l, r, ol, orr);
	}

	// 送りに対する戻りの量（リバーブ・コーラス・バリエーション）
	void set_return(slot_id id, float gain) { m_return[id] = gain; }
	float ret(slot_id id) const { return m_return[id]; }

private:
	fx_slot   m_slot[SLOTS];
	master_eq m_meq;
	meg_fx_slot m_mfx[INS1 + 1];
	// 送りに対する戻りの量。インサーションは、送りの目盛りが乾いた音と違うので実測で合わせた
	// （THRU を掛けて、MEG のときと同じ大きさになる値。doc/native-dsp.md）
	float   m_return[SLOTS] = { 0.6f, 0.6f, 0.6f, 0.31f, 0.31f, 0.31f, 0.31f };
};

} // namespace smu2000::dsp

#endif // S_MU2000_DSP_FX_NATIVE_H
