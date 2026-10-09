# The native effects, and using them outside S-MU2000

This folder holds two different things. Only one of them can be taken out and
used in another program.

| | What it is | Usable on its own? |
|---|---|---|
| `meg_fx*.h`, `meg_reverb.h` | The MU2000's effect programs written out as C++ of the same shape (generated from what the firmware loads into the effect DSP). This is what S-MU2000's light mode plays, and it matches the emulated DSP to about 0.01 dB. | **No.** Every coefficient and delay address is read at run time from what the firmware wrote. Without a running firmware there are no numbers to put in. |
| `util.h`, `reverb.h`, `blocks.h`, `blocks2.h`, `fx_slot.h` | Ordinary effect algorithms written by hand, one per family of XG effect types, driven by the XG parameters. An **approximation**: the same kind of effect with the same controls, not the same sound. | **Yes.** Standard library only. No firmware, no ROM. |

The rest of this page is about the second row.

## What is there

```
util.h      delay line, one-pole and biquad filters, LFO, DC blocker, clippers, envelope follower
reverb.h    reverb (pre-delay, early reflections, 8-line feedback delay network)
blocks.h    early_ref   early reflections, gate reverb, reverse gate
            delay_fx    delay L/C/R, L,R, cross, echo
            mod_fx      chorus, celeste, flanger, symphonic, ensemble, phaser
            rotary_fx   rotary speaker, tremolo, auto pan
            drive_fx    distortion, overdrive, amp simulator
            eq_fx       2-band / 3-band EQ
            wah_fx      auto wah (LFO or envelope driven)
            dyn_fx      compressor, noise gate
            lofi_fx     bit and sample-rate reduction
            master_eq   the 5-band master EQ
blocks2.h   ring_fx, slice_fx, isolator_fx, reso_fx, cancel_fx, enhancer_fx, pitch_fx, talk_fx
fx_slot.h   one XG effect: picks a block from the type (MSB/LSB) and feeds it the XG parameters
```

Everything is `float`, stereo, one sample at a time, in namespace `smu2000::dsp`.
Signals are around ±1. Nothing allocates in `process()`.

## Level 1: a block, with parameters in plain units

Every block has the same shape:

```cpp
#include "dsp/reverb.h"

smu2000::dsp::reverb rev;
smu2000::dsp::reverb::params p;   // defaults are sensible; change what you need
p.time        = 2.8f;             // seconds to fall by 60 dB
p.predelay_ms = 30.0f;
p.damp_hz     = 5000.0f;
rev.set_rate(48000.0f);           // once, before set_params
rev.set_params(p);                // again whenever a parameter changes

float out_l, out_r;
rev.process(in_l, in_r, out_l, out_r);   // per sample; the output is the effect's sound only
rev.reset();                              // clear the tails
```

The `params` struct of each class is its whole interface; the fields are named
for what they are (`l_ms`, `feedback`, `lpf_hz`, `threshold_db` ...) and each has
a default. State is private to the object, so two instances never interact.

Reverbs, early reflections and delays output the wet sound only (you mix the dry
sound yourself). The others return the processed signal, with their own
`dry_wet` where that makes sense.

## Level 2: an XG effect, by type and raw XG values

`fx_slot` is one effect unit as an XG device presents it:

```cpp
#include "dsp/fx_slot.h"

smu2000::dsp::fx_slot fx;
fx.set_rate(44100.0f);
fx.set_insertion(true);                 // true: the dry sound is mixed in by Dry/Wet (insertion use)
                                        // false: wet only (system reverb / chorus use)
int raw[16] = { /* the type's parameters, raw XG values, in table order */ };
fx.set(0x05 << 7 | 0x00, raw, 16);      // type = MSB << 7 | LSB; here DELAY LCR
fx.process(in_l, in_r, out_l, out_r, frames);   // or the one-sample overload
```

`xg/fx_params.h` is the table behind it: for each type, its parameters in order,
each with the name the MU2000 shows on its LCD, the accepted raw range, and how a
raw value reads (`xg::fx_find(type)`, then `fx.def()` on a slot). `fx_value()`
turns a raw value into the displayed number, which the mapping uses as the
physical quantity: `ReverbTime` 2.1 is 2.1 seconds, `LPF Cutoff` 6.3k is 6300 Hz.

One behaviour to know about: to save work, a slot stops producing output once
its input has been exactly zero for four seconds (`QUIET_LIMIT`). That cuts a
very long reverb tail short; raise the constant, or use the block directly, if
you need the whole of it.

Two things the table does **not** have:

- **Default values.** The real unit loads each type's defaults from its ROM.
  Bring your own (the XG specification's effect parameter tables list them), or
  start from the middle of each range as the example does.
- **Type names.** They are in the comments of `FX_DEFS` only; the emulator reads
  the names from the ROM.

## The example

`tools/fxdemo/fxdemo.cpp` is a complete program using both levels and nothing
else from this repository:

```bash
make fxdemo                       # or: g++ -std=c++20 -O2 -I src -I src/compat -o fxdemo tools/fxdemo/fxdemo.cpp
build/fxdemo --list               # the types and their parameters
build/fxdemo 01 00 hall.wav       # HALL 1 over a built-in test phrase
build/fxdemo 05 00 out.wav in.wav "Dry/Wet=80" "FB Level=90"
build/fxdemo 41 00 chorus.wav --send
build/fxdemo --block out.wav      # level 1: a delay into a reverb
make check-fx                     # every type at the ends and the middle of its ranges; fails on NaN or a blow-up
```

## Taking it into another project

Copy these, keeping the folder layout (the includes are relative to `src/`):

```
src/dsp/util.h  reverb.h  blocks.h  blocks2.h          level 1: these four are enough
src/dsp/fx_slot.h  src/xg/fx_params.h                  level 2
src/compat/mamecompat.h                                only for the u8/u16 type names fx_params.h uses
```

and compile with `-I src -I src/compat`. If you would rather not carry
`mamecompat.h`, the only things `fx_params.h` needs from it are
`using u8 = uint8_t; using u16 = uint16_t;`.

The licence is BSD-3-Clause, as for the rest of the repository.

## How close is it?

Close in kind, not in sound. The reverb is a generic feedback delay network, not
Yamaha's; the chorus is a textbook modulated delay; and so on. The parameters
mean what they say, so a patch that asks for a 2.1 s hall with the highs rolled
off at 6.3 kHz gets one, but it will not null against a real MU2000 or against
the emulation. For the families and what each is approximated with, see
`fx_slot::kind_of()` and `apply()` in `fx_slot.h`; the notes there (in Japanese)
say which parts were matched against measurements and which were not.

If you need the real sound, that is what S-MU2000 itself is for: it runs the
effect DSP's program as the firmware wrote it.
