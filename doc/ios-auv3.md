# iOS AUv3 — architecture and technical study

An experiment: carry the AUv3 to iOS, and make the standalone app carry MIDI IN/OUT and
Audio IN/OUT while it is at it. The reason to expect this to be cheap is that the AUv3 is
already the most portable of the three plug-in surfaces — the sound engine has no platform
code in it at all. What is left is the UI toolkit, the build, and the standalone app.

Written in English; the rest of `doc/` is Japanese, so this may want translating to match.

## What is already portable

Measured by reading the sources, not assumed.

| piece | evidence | verdict |
|---|---|---|
| sound engine | `src/vst3/engine.cpp` (987 lines) includes only project headers and `<thread>`/`<chrono>`/`<cmath>` — no CoreAudio, no AVFoundation | **portable as-is** |
| AUv3 core | `src/auv3/audio_unit.mm` (587 lines) uses only `AUAudioUnit`, `AUAudioUnitBusArray`, `AUMIDIEventList`, `AUEventBlock`. No AppKit, no `AVCaptureDevice`, no `AudioComponent*` enumeration | **portable as-is** |
| UI | `src/ui/panel.cpp` is **ImGui** (`imgui.h`, `imgui_internal.h`, `ui/draw_imgui.h`); `view_mac.mm` is ImGui in a `CAMetalLayer`-hosted `NSView`; `ImGui_ImplMetal_*` is vendored | **already shared — see below** |
| JIT | `src/compat/exec_mem.h` already does arm64 `MAP_JIT` with write-protection toggling; `packaging/auv3-appex.entitlements` already grants `com.apple.security.cs.allow-jit`; the engine falls back to an interpreter when JIT is refused | **works, possibly slowly** |
| paths | `config_dir()` in `src/compat/paths.h` is `$HOME` + `Library/Application Support/S-MU2000` — the same shape on iOS | **likely portable, see risks** |
| MIDI | in: `AUMIDIEventList` → `feed_ump()` → legacy bytes → `engine::midi()`; out: engine → `ui::midi_split` → `MIDIOutputEventBlock` | **portable as-is** |

That is the good news, and it is better than expected: the AUv3 is already the *most*
platform-neutral of the three surfaces. VST3 drags in the SDK; AUv2 drags in AudioUnit
component registration; the AUv3 drags in almost nothing.

## The two real problems

### 1. The UI is ImGui already — the platform layer is thin

I initially wrote this study claiming the UI was AppKit and that `view_mac.mm` would have to
be ported to UIKit. **That was wrong.** `src/ui/panel.cpp` (1571 lines) includes `imgui.h`,
`imgui_internal.h` and `ui/draw_imgui.h`, and `src/vst3/view_mac.mm` (679 lines) is ImGui
inside a `CAMetalLayer`-hosted `NSView` — not a native AppKit panel. The macOS AUv3 and the
Linux build already share the same ImGui panel code.

So the split is already where a port wants it:

| layer | where | portable |
|---|---|---|
| panel, layout, svg, png, editor, effects, xg_ui | `src/ui/*`, `src/xg/model.cpp` (~7000 lines) | **yes — already ImGui, shared** |
| ImGui shell | `src/ui/imgui_shell.h` (288 lines) | **yes** |
| renderer | `ImGui_ImplMetal_*` from `third_party/imgui/backends/` | **yes — Metal is iOS's own GPU API** |
| host view | `src/vst3/view_mac.mm`: layer + `ImGui_ImplMetal_Init` + NSEvent → `io` | **no — this is the new file** |

The iOS view is one file: a `UIView` with a `CAMetalLayer`, the *same*
`ImGui_ImplMetal_Init` call, and touch/keyboard forwarded into ImGui's `io`. Metal
initialisation is the same API on both platforms, so most of the file is shared logic that
merely changes class. Upstream's `imgui_impl_ios.mm` (~150 lines, touch and safe-area) is
the only genuinely new third-party piece, and it can be vendored or reimplemented.

Per the owner's decision: **everything that is ImGui stays ImGui and shared.** No second UI
codebase, no native widget layer. That is not a compromise here — it is already the shape of
the code.

### 2. There is no third-party AUv3 host on iOS

### 2. There is no third-party AUv3 host on iOS — so the standalone is not a bonus

macOS has Logic, Ableton and GarageBand to load the plug-in. iOS effectively does not, so the
standalone app is *the only way anyone would hear this*.

Two facts shape the design:

- `src/auv3/main_app.mm` (521 lines) is an **installer and nothing else** — its own UI says
  "This app installs the AUv3 plug-in. It makes no sound", and it consists of an
  "Install ROMs…" button, an "Install from Application Support" button, and a
  `--install-roms` path. It hosts no audio. On iOS it becomes a document-picker import plus
  whatever the embed needs to register the `.appex`.
- The existing standalone is `src/gui.cpp` ("run the MU2000 behind a front panel that looks
  like the real machine"), with `--midi n --midi-b n --midi-c n --midi-d n --midiout n`. It
  runs the engine **directly — it is not a host** — and its platform layer is already
  factored per concern, with a Linux counterpart for several parts:

  | concern | macOS | Linux |
  |---|---|---|
  | audio out | `src/ui/audio_out_mac.cpp` (582) | WASAPI version |
  | audio in | `src/ui/audio_in_mac.cpp` (469) | — |
  | MIDI in | `src/ui/midi_in_mac.cpp` (160) | `src/ui/midi_in_linux.cpp` |
  | MIDI out | `src/ui/midi_out_mac.cpp` (256) | `src/ui/midi_out_linux.cpp` |
  | window | `src/ui/window_mac.mm` (637) | `gui_linux.cpp` |

**iOS needs exactly one file per row**, behind the same headers:

  - `midi_in_ios.cpp` / `midi_out_ios.cpp` — **CoreMIDI is the same API on iOS**, so these
    should be close to copies of the macOS ones.
  - `audio_out_ios.cpp` / `audio_in_ios.cpp` — `AVAudioSession` + RemoteIO in place of the
    CoreAudio HAL. This is the only genuinely rewritten audio code.
  - `window_ios.mm` — UIKit + the same `CAMetalLayer`/Metal/ImGui stack.

#### A design decision worth making deliberately

The standalone can either **host the AUv3** or **run the engine directly** the way
`gui.cpp` does. These are not equivalent:

- *Hosting the AUv3* gives the app and the plug-in **one audio path**. The app only wires
  CoreMIDI into the event list and the output into RemoteIO; the AUv3 renders. This is what
  makes the "could become the future macOS app too" story true — the same rendering code
  everywhere.
- *Running the engine directly* reuses more of the existing standalone scaffolding
  (`gui.cpp`'s device abstraction) but means **two audio paths** to keep correct on iOS.

Recommended: **host the AUv3**, and take only the MIDI wiring pattern from `gui.cpp`. The
device-layer files above are then needed only for the host app's own I/O, not for the engine.

## Everything else that has to be built

- **Build.** Makefile targets for `arm64-apple-ios`, a different `Info.plist`, an
  `app-extension` target embedding the `.appex` in the app bundle, and codesigning with
  entitlements. The existing `packaging/auv3-*.plist|entitlements` are macOS-shaped.
- **ROM delivery.** ROMs are Yamaha's and must never be baked into a shipped bundle — the
  Makefile says so explicitly and defaults `AUV3_ROMS` off for exactly that reason. The macOS
  build installs them from the container app. On iOS `NSOpenPanel` does not exist, so this
  becomes a Files/document-picker import. Same constraint, new UI.
- **Audio session.** `AVAudioSession` category and sample-rate policy are iOS-only
  concerns; the engine itself does not care.

## Risks, and which are cheap to retire early

Ranked by how much they would cost to discover late.

1. **App ↔ appex container sharing.** On macOS a sandboxed appex gets `$HOME` pointed at its
   own container, which is why `config_dir()` works unchanged there. **iOS does not work this
   way** — an extension that needs the app's files almost certainly needs an App Group
   container, and `paths.h` knows nothing about groups. If this is wrong, the extension sees
   an empty directory and the engine starts with no ROMs. *Retire it first: it is a one-file
   experiment and it invalidates the path layer for everything above it.*
2. **JIT entitlement on iOS.** `MAP_JIT` needs `com.apple.security.cs.allow-jit`, which is
   granted on iOS but is scrutinised at review. The interpreter fallback means the app still
   works without it, just slower — so this degrades rather than blocks, but it should be
   measured early because it decides whether audio comes from JIT or emulation.
3. **Metal/ImGui on iOS.** The backend is vendored; the iOS glue is not. Cheap to test,
   contained to the UI.
4. **AUv3 hosting on iOS.** Instantiating your own `AUAudioUnit` from the app is standard,
   but audio-session interaction and the render-loop callback shape are the kind of thing
   that only answers on device.

## Suggested order

The plan is ordered so that each step is worth something even if the next one is abandoned.

1. **Prove the container path.** App Group, shared `config_dir()`, engine finds its ROMs.
   Small, and everything else depends on it.
2. **Build the AUv3 for iOS** as an `.appex` with no UI. Does it compile, install, and
   report a sane `auval`-equivalent? This retires the entitlement and packaging risks.
3. **Standalone app that self-hosts it**, audio out only. Confirms hosting, audio session,
   and that sound actually comes out. This is the milestone that matters.
4. **MIDI IN/OUT** through CoreMIDI into the same event-list path, as four virtual cables
   like the real machine. Small once (3) works, because the AUv3 side already exists.
5. **Audio IN.** The AUv3 already declares 2-in/2-out; this is app-side wiring only.
6. **UI**: one new `src/vst3/view_ios.mm` — a `UIView` with a `CAMetalLayer`, the same
   `ImGui_ImplMetal_Init`, and touch/keyboard into ImGui's `io`. Everything above it is the
   shared ImGui panel, untouched.

Steps 1–3 are the risky ones and none of them involve the UI toolkit. Doing the UI first
would be starting on the part where the options are a genuine judgement call, before the
parts that are simply unknown.

## Milestone: the extension builds for iOS

    make ios-auv3                      # the .appex, no ROMs
    make ios-auv3 IOS_ROMS=roms        # ROMs baked into S-MU2000AU.appex/Resources/roms

Resulting binary: `Mach-O 64-bit executable arm64`, `LC_BUILD_VERSION platform 2` (iOS),
`minos 14.0`, `sdk 27.2`, ad-hoc signed, identifier `com.tarboh.smu2000.auv3.au.ios`.

**Zero compile errors.** Every iOS problem encountered was one of two things, and neither
was "this API does not exist on iOS":

1. **Not included** - the source list, and `src/mu2000.cpp` and the `ui::xgui` PC sources
   which the macOS rule passes separately.
2. **Guarded** - three things, in this order of discovery:
   - `pthread_jit_write_protect_np` is marked unavailable in the iOS SDK.
     `exec_mem.h` now gates the MAP_JIT write-protection dance on `!TARGET_OS_IPHONE`.
     iOS has no per-thread toggle, so the pages stay writable. The interpreter path never
     touches `exec_mem` and is fully working, so this is a compile fix, not a capability
     loss.
   - `audio_unit.mm`'s AppKit: `<Cocoa/Cocoa.h>`, `<CoreAudioKit/CoreAudioKit.h>` and
     `"view_controller.h"`; `requestViewControllerWithCompletionHandler:`; and
     `supportedViewConfigurations:` (`AUAudioUnitViewConfiguration`). All three behind
     `#if !TARGET_OS_IPHONE`.
   - `-framework CoreText`, which `src/ui/font_file.h` needs for the family-name -> file
     walk that finds the CJK face. The header compiled on iOS; only the link needed it.

**The extension is deliberately no-UI** (`com.apple.AudioUnit`, principal class NSObject -
see `src/auv3/factory_ios.mm`). Sound is identical either way; only the host's request for
a view never arrives. That bought a working iOS binary before any UIKit existed. Switching
back to `com.apple.AudioUnit-UI` means deleting the three guards and adding
`view_controller_ios` + `panel_uiview`.

**No paid account, so no App Groups.** Free provisioning does not support them, so the
extension cannot reach the app's files through a shared container. This turned out to cost
nothing: `engine.cpp` already searches `module_dir()/../Resources/roms`, and that is correct
for both layouts - macOS puts the binary in `Contents/MacOS`, iOS is flat, so the bundle's
`Resources` is one level up either way. ROMs are baked in for development; "import ROMs"
from the app can come later.

### Next: it needs a containing app

A bare `.appex` does not register on iOS - only extensions inside an app are discovered. The
next step is a deliberately thin host app: embed the `.appex`, let it register, load it,
pull audio. That also settles the open question of whether an iOS app can host its own
extension in-process (`doc/ios-auv3.md`, "A design decision worth making deliberately"),
and it is worth finding out with a 60-line app rather than the full standalone.

## Open questions for the owner

- Should the iOS app be a *different* UI codebase from the macOS one, or the same? The
  ImGui route makes "same logic, different view layer" cheap; the UIKit route does not.
- Is App Store distribution in scope, or is this sideloaded/development-only? It decides how
  hard the JIT entitlement is to keep.
- Does the MIDI IN need to be a full 4-cable virtual MIDI endpoint as on the real machine
  (`virtualMIDICableCount` returns 4), or is a single physical input enough for the app?