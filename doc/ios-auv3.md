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
  | audio out | `src/ui/audio_out_mac.cpp` (the AudioUnit, fed 44100 Hz) | WASAPI version |
  | audio in | `src/ui/audio_in_mac.cpp` | — |
  | MIDI in | `src/ui/midi_in_apple.cpp` (160) | `src/ui/midi_in_linux.cpp` |
  | MIDI out | `src/ui/midi_out_apple.cpp` (256) | `src/ui/midi_out_linux.cpp` |
  | window | `src/ui/window_mac.mm` (637) | `gui_linux.cpp` |

**iOS needs exactly one file per row**, behind the same headers:

  - `midi_in_ios.cpp` / `midi_out_ios.cpp` — **CoreMIDI is the same API on iOS**, so these
    should be close to copies of the macOS ones.
  - `audio_out_ios.mm` / `audio_in_ios.mm` — the answers to what the engine cannot ask,
    `session_ios.{h,mm}` — the `AVAudioSession` they ask them of, and
    `audio_core_ios.{h,mm}` — the render block, the input tap, the resampler and the
    ring, which both directions share.
    **What this study got wrong:** the audio was expected to be the one genuinely rewritten
    part, and it isn't. `AVAudioEngine`'s input and output nodes hand out the very `AudioUnit`
    a hand-written backend owns, so the workgroup read is the same property on both systems.
    That much is shared. What is *not* shared is the rest: iOS ships no public AudioHardware
    HAL (`AudioObject*` appears in no header, only in `CoreAudio.tbd`), so
    `audio_out_mac.cpp`/`audio_in_mac.cpp` cannot compile there at all and the back end is
    written from scratch against the session's route and ports.
    The macOS back end is left exactly as it was — an owned `AudioUnit` fed 44100 Hz with
    CoreAudio converting — so a shared render path is its own change, with its own review
    (`src/ui/audio_core_ios.{h,mm}` is what it would share).
  - `window_ios.mm` — UIKit + the same `CAMetalLayer`/Metal/ImGui stack.

#### A design decision worth making deliberately

The standalone can either **host the AUv3** or **run the engine directly** the way
`gui.cpp` does. These are not equivalent:

- *Hosting the AUv3* gives the app and the plug-in **one audio path**. The app only wires
  CoreMIDI into the event list and the output into RemoteIO; the AUv3 renders. This is what
  makes the "could become the future macOS app too" story true — the same rendering code
  everywhere.
- *Running the engine directly* reuses more of the existing standalone scaffolding
  (`gui.cpp`'s device abstraction) but means **two audio paths** to keep correct on iOS — one
  for the standalone and one for the extension (which, as an AUv3, takes the host's audio and
  has none of its own).

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
`minos 17.0`, ad-hoc signed, identifier `com.tarboh.smu2000.ios.app.auv3`.

(17.0, not 14.0: the AUv3 only needs 14 for UMP, but the current Xcode's libc++
no longer supports 14 as a deployment target. The extension id must prefix-extend
the containing app's - `com.tarboh.smu2000.ios.app.auv3` under `com.tarboh.smu2000.ios.app`
- or iOS refuses the install with "Mismatched bundle IDs".)

**Zero compile errors.** Every iOS problem encountered was one of two things, and neither
was "this API does not exist on iOS":

1. **Not included** - the source list, and `src/mu2000.cpp` and the `ui::xgui` PC sources
   which the macOS rule passes separately.
2. **Guarded** - three things, in this order of discovery:
   - `pthread_jit_write_protect_np` is marked unavailable in the iOS SDK.
     `exec_mem.h` gates it on `!TARGET_OS_IPHONE` - and without the toggle a
     MAP_JIT mapping succeeds but the first store faults with
     KERN_PROTECTION_FAILURE, so both JITs (SH2, SWP30) are compiled out on iOS
     and the interpreter runs. This is a fallback, not a capability loss.
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
extension cannot reach the app's files through a shared container. ROMs are baked
in beside the binary (`<bundle>/roms/`, engine candidate 3b) for the same reason
art goes to `<bundle>/art/real/` (layout step 4): subdirectories under the app's
`Resources/` break ad-hoc codesign ("bundle format unrecognized"), so the macOS
`Resources/roms` layout cannot be reused on iOS. "import ROMs" from the app can
come later.

### Next: it needs a containing app

A bare `.appex` does not register on iOS - only extensions inside an app are discovered. The
next step is a deliberately thin host app: embed the `.appex`, let it register, load it,
pull audio. That also settles the open question of whether an iOS app can host its own
extension in-process (`doc/ios-auv3.md`, "A design decision worth making deliberately"),
and it is worth finding out with a 60-line app rather than the full standalone.

## Build and install workflows

One bundle, two front ends, two SDKs. `CFBundleExecutable` switches between the
front ends and each target re-signs, so `ios-app` after `ios-standalone` (or the
reverse) just works - no deletion needed.

### Build options

| variable | default | alternatives |
|---|---|---|
| target | — | `ios-app` (smoke-test host), `ios-standalone` (the synth), `ios-auv3` (extension alone) |
| `IOS_SDK_NAME` | `iphoneos` (device, `build-ios/device/`) | `iphonesimulator` (`build-ios/simulator/`) - a different platform ID, not interchangeable |
| `IOS_ROMS` | empty (no ROMs) | `roms` (baked beside the binary: `<bundle>/roms/`, `<appex>/roms/`) |
| `IOS_DEBUG` | `1` (`-Og`, symbols) | `0` (`-O3`, for the interpreter-speed measurement) |
| `TEAM_ID` | `.ios-team-id`, else none (ad-hoc) | `ABCDE12345`; the ids, the identity and the profiles are all derived from it |
| `IOS_PROFILE` / `IOS_APPEX_PROFILE` | found by bundle id | a path, when several match or the wrong one wins |

`CODESIGN_ID` is *not* the iOS variable: it signs the macOS AUv3, and setting it
changed nothing here (and, before the iOS block was fenced, was silently
overwritten by the iOS side). For iOS the identity comes from the team.

```bash
# Simulator standalone with ROMs (the usual test loop)
rm -rf build-ios/simulator/S-MU2000.app   # only needed after renames; header edits rebuild via depfiles
make ios-standalone IOS_SDK_NAME=iphonesimulator IOS_ROMS=roms

# Device build. With TEAM_ID the identity and the profiles are found (see
# "Device install" below); without it the signature is ad-hoc and no profile is
# embedded, which only a jailbroken device will take.
make ios-standalone TEAM_ID=ABCDE12345 IOS_ROMS=roms
make ios-sign-info TEAM_ID=ABCDE12345     # what it found, and what is missing
```

A team with profiles but no signing certificate fails the build rather than
signing ad-hoc, which a device would refuse at install time long after the build
said nothing was wrong. To sign a device build ad-hoc on purpose (a jailbroken
device), pass an empty team: `make ios-standalone IOS_SDK_NAME=iphoneos IOS_TEAM_ID=`.

Header edits rebuild their dependents (iOS depfiles are `-include`d); a source
rename/delete leaves a stale `.d` pointing at the ghost - `find build-ios -name
'*.d' -delete` fixes that one case. `codesign` runs last in every target, after
binaries, plist, artwork and ROMs: signing earlier signs contents about to change.

The whole iOS block is inside `ifeq ($(PLATFORM),macos)` and
`ifneq ($(filter ios%,$(MAKECMDGOALS)),)`, so a Windows or Linux build reads none
of it - no `xcrun` and no `tools/ios_sign.py` per invocation - and neither does
`make all` on macOS. The one consequence worth knowing: the goal has to be spelled
on the command line, so an iOS target cannot be a prerequisite of another target,
and `make` with no goal builds nothing iOS.

### Simulator install and run

The short version, for the loop you run dozens of times a day:

```bash
make ios-install      # build, install and launch on the booted simulator
```

It picks the booted simulator itself (an iPhone if several are up, otherwise
whatever is booted), and prints the two commands worth having - `log stream` and
`screenshot` - because those need the device id and a second terminal. It also
sets `CFBundleExecutable` to `Standalone` and re-signs before installing, since
both front ends share one bundle and whichever was built last leaves its own name
there; without that, an `ios-app` first means `installd` refuses the bundle.

The same thing by hand, which is what to reach for when a step fails:

```bash
IPHONE=$(xcrun simctl list devices booted -j | /usr/bin/python3 -c \
  'import json,sys;print(next(d["udid"] for d in json.load(sys.stdin)["devices"].values() for d in d if "iPhone" in d["name"]))')

xcrun simctl install $IPHONE build-ios/simulator/S-MU2000.app
xcrun simctl launch --console-pty $IPHONE com.tarboh.smu2000.ios.app
xcrun simctl io $IPHONE screenshot /tmp/panel.png   # second shell: pixels without logs
```

Name the device: with two simulators booted, `booted` is ambiguous and installs
to the wrong one. The id is `com.tarboh.smu2000.ios.app`, which is
`packaging/ios-app-Info.plist`'s `CFBundleIdentifier` - `make ios-app-info` prints
it rather than trusting this line. `--console-pty` puts stdout, stderr and crashes
on one stream with no predicate to get wrong.

`make ios-install` is the same three commands (build, install, launch) with the
device picked the same way, for the loop where you want them every time.

Success looks like this, in order (each step logs before the next runs, so a
failure names itself):

```
[ios] scene willConnectToSession
[ios] ROM dir: .../S-MU2000.app/roms
[ios] layout: .../S-MU2000.app/art/real/panel.txt
[ios] booted
[ios] running: tap to press, hold a second finger for menus
```

Without a ROM set the first three lines are replaced by `[ios] no ROMs: the
panel comes up empty` and the import flow opens by itself (see "ROM images"
below) - that is the expected first launch, not a failure.

The audio lines are printed by the shared core and interleave wherever they land;
`audio running: 0-frame buffer` on the first of them is normal, since it prints
before the first render callback stores a count. The live numbers are on the
status line - `worst_ms` there is the interpreter-speed measurement.

### Device install

```bash
xcrun devicectl list devices
xcrun devicectl device install app --device <UDID> build-ios/device/S-MU2000.app
```

#### Signing: one team id in, the rest found

A bundle id belongs to a team, so the ids in `packaging/*.plist` are the upstream
author's and nobody else can sign them. Give `make` your team and it derives the
ids, finds your signing identity and finds your provisioning profiles:

```bash
make ios-sign-info TEAM_ID=ABCDE12345   # what it would use, and what is missing
make ios-team-id   TEAM_ID=ABCDE12345   # remember it in .ios-team-id, once
make ios-app       TEAM_ID=ABCDE12345   # signed .app, ready to install
```

With a team set, the ids become

```
com.tarboh.smu2000.ios.<TEAM>          the app
com.tarboh.smu2000.ios.<TEAM>.auv3     the AUv3, prefix-extending the app's
```

and they are written into the *copied* `Info.plists` inside `build-ios/`, never
into the checkout - a build leaves the tree as it found it.

The suffix is `.auv3` on the end, not a subdomain in front: Apple's rule is that
an extension's bundle id must *begin with* its containing app's
(`com.example.App.Ext`, never `ext.com.example.App`), which is what installd
checks when it reports `Mismatched bundle IDs`. Both ids still have to be
registered separately, and each needs its own profile. The team comes from
`TEAM_ID=`, the environment (`SMU2000_IOS_TEAM_ID`) or `.ios-team-id`, in that
order, and all three may be omitted once the file exists.

The two ids have to be **registered** before a profile can exist, which `make`
cannot do. The short way, with a throwaway Xcode project (nothing of it is
committed - `make` only reads the profiles out of the keychain and the profile
directory):

1. New iOS App project, empty, no storyboard, no tests. Set the app's bundle id
   to `com.tarboh.smu2000.ios.<TEAM>` and turn on automatic signing with your
   team.
2. Add a target of type **Audio Unit Extension**, bundle id
   `com.tarboh.smu2000.ios.<TEAM>.auv3` - the app's id with `.auv3` on the end,
   which is the rule (an extension id must *begin with* its container's, never be
   a subdomain in front of it). Both ids are needed separately registered, and
   each needs its own profile.
3. Build once for the device. Xcode registers both ids, issues both profiles and
   creates a development certificate for the team if there is none.
4. `make ios-sign-info TEAM_ID=<TEAM>` should then report an identity and both
   profiles; `make ios-app TEAM_ID=<TEAM>` builds the signed bundle.

Free provisioning (no paid account) hands out a wildcard profile, which the
tool accepts - it matches on the profile's own `application-identifier` **and**
its team, so another team's profile is never embedded. Entitlements are taken from
the profile itself, so nothing has to be written out by hand; the appex's own file
stays empty on purpose (`app-sandbox` is macOS-only, `allow-jit` is meaningless
where the JIT is compiled out, and App Groups need a paid account).

Two things that are about *devices* rather than ids, since they are the usual
next wall:

- **The device must be registered to the team.** That is what a profile's device
  list holds; installing the throwaway app on a device is one way to get it
  registered (Xcode asks, or it happens under automatic signing), but S-MU2000
  itself does not have to be installed there first. After adding a device,
  regenerate the profile so it lists it - `make ios-sign-info` prints each
  profile's expiry, and a device missing from it fails at install with
  "device is not included".
- **The certificate has to exist before the profile that allows it.** A profile
  lists the SHA-1s it accepts (`DeveloperCertificates`), so a profile made before
  the certificate does not accept it: `codesign` then fails with a message about
  the profile not including a signing certificate. If you made the profiles in the
  portal before creating the certificate, make them again afterwards - Xcode does
  this for you when automatic signing is on.

Without a team the device build signs ad-hoc and looks for no profile at all:
the shipped ids cannot be signed by anyone else, so a profile for them would be
one this build cannot use. `IOS_PROFILE=` / `IOS_APPEX_PROFILE=` override the
search when you do have one for those exact ids.

Troubleshooting by message, when an install is still refused:

| message | what it says | fix |
|---|---|---|
| `provisioning profile ... doesn't include ...` | the profile is not this team's, or not for this id | `make ios-sign-info` prints both ids it wants; check the profile's team |
| `Mismatched bundle IDs` | the appex id must prefix-extend the app's | `TEAM_ID` set for both, or override both profiles |
| `No profiles for '<team>' were found` | Xcode has not made one yet | register the ids (Xcode or the portal), then re-run |

### Troubleshooting (every one earned)

| symptom | cause | fix |
|---|---|---|
| `Mismatched bundle IDs` at install | the appex id must *begin with* the app's | `com.tarboh.smu2000.ios.app.auv3` under `com.tarboh.smu2000.ios.app` |
| `does not contain code ... iOS-simulator` | device build on a simulator | per-SDK trees; `IOS_SDK_NAME=iphonesimulator` |
| SIGTRAP `NoSceneLifecycleAdoption` | scene lifecycle mandatory | manifest + nil app delegate + scene delegate from plist |
| black screen, no log, no crash | `UISceneDelegateClassName` names a missing class | match it to the delegate; the marker `fprintf` first line tells called from never-called |
| `bundle format unrecognized` | any subdir under the app's `Resources/` (even empty, even `en.lproj`) | top-level `roms/`, `art/` - both verified signing |
| `invalid Info.plist` from codesign | stale plist beside a fresh binary | plists are separate make targets the sign depends on |
| `Missing bundle ID` | unreadable extension (above) or installing mid-build | fix the above; never inspect/install under `-j8` |
| `make` does nothing after edits | header deps (fixed) or stale `.d` after renames | depfiles included; `find build-ios -name '*.d' -delete` on renames |
| `-10863` instantiating the AUv3 | unexplained; note the appex ROMs used to sit where the engine never searches | retest with `appex/roms/` before assuming deeper |
| LCD frozen at 起動中 | nothing pumps post-boot (no audio yet) | display-link pump via `eng->fill` (pumps the bridge too, not just samples) |
| taps do nothing | panel hears app verbs, not `io` | `mouse_down/drag/up` like `window_mac.mm`; `io` only serves widgets |
| silence, no log | `make_audio` empty / `start_audio` never called | mirror `run()`; status `Starting...` means `!(out && produced())` |

### Bluetooth and network MIDI (standalone only)

CoreMIDI lists only connected endpoints, so USB MIDI appears on plug-in but
Bluetooth LE and network MIDI never do without setup (`ui/window_ios.mm`,
additive iOS-only files; the AUv3 needs nothing, the host routes MIDI):

- The context menu gains one appended group, "Bluetooth & network MIDI"
  (titles in `ui/texts.h`, localized like the rest): Connect Bluetooth
  MIDI... (Apple's `CABTMIDICentralViewController`), Advertise this
  device... (`CABTMIDILocalPeripheralViewController`), and a Network MIDI
  checkmark toggle. The app's own groups are untouched and render first.
- `Info.plist` carries `NSBluetoothAlwaysUsageDescription` (BLE pairing goes
  through CoreBluetooth; required since iOS 13) and
  `NSLocalNetworkUsageDescription` (the network session, since iOS 14).
- The toggle enables `MIDINetworkSession` with policy `Anyone`, persisted in
  `NSUserDefaults` (`smu_network_midi`) and applied at startup; the menu
  re-enumerates on every open, so newly paired/found endpoints appear with no
  extra refresh.
- The sheets present fullscreen, 0.4 s past the pick, clearing anything
  still presented first: presenting while the context menu is still
  dismissing force-loads the BT view mid-transition. The "already presenting
  `_UIContextMenuActionsOnlyViewController`" log line is that collision.
- Root cause of the `CALayerInvalidGeometry` (NaN-height table) crashes, all
  three traces: Apple's BT controllers build their table from deprecated
  `-[UIScreen applicationFrame]` (disassembled `loadView` proves it:
  `mainScreen` -> `applicationFrame` -> `initWithFrame:style:`, no other
  input), which returns a NaN-height rect on this runtime. Fixed by a
  one-time substitution returning `bounds` (`patch_application_frame` in
  `ui/window_ios.mm`), applied before presenting.
- Simulator cannot test pairing (no Bluetooth hardware): sheets present with
  an empty list. Proven working there; real-device pairing still owed.

## ROM images: imported, not bundled

The images are Yamaha's, so no build we hand out carries them (`IOS_ROMS` is empty
by default, exactly as `AUV3_ROMS` is on macOS). The user brings their own dump,
and the flow is the one the desktop already had, in `src/ui/rom_locate.h`:

- Shared, one implementation: the "here's what you need and how to dump it" text
  (`roms_needed_message`), the "that folder is not a set, missing …" answer
  (`roms_bad_message`), the validation (`smu2000::accept_roms_choice`, which also
  accepts the parent of a `roms/` folder) and the remembering
  (`remember_roms_dir` → `roms.txt`). `locate_roms_for_gui` is written from those,
  and so is iOS, so the platforms cannot drift.
- iOS-only: the picker (a document picker must be presented and answer later, so
  it cannot be the blocking `ask_roms_folder` seam the desktop implements) and the
  **copy**. The copy is forced: a picked folder is granted for that run only —
  iOS has no security-scoped bookmark (`NSURLBookmarkCreationWithSecurityScope` is
  `API_UNAVAILABLE(ios)`) and no `accessForFolder` API — so `install_roms()` into
  `config_dir()/roms` is what survives a relaunch. UIKit also refuses to import a
  folder at all ("folder import is not supported, use asCopy:false"), which is why
  the folder is picked in place and copied by hand.
- Two copies, one per process: no App Groups without a paid account, so the app
  and the extension have separate containers and separate `roms.txt`.
- A launch that finds no set opens the flow by itself, and installing boots the
  machine in place (no relaunch). `boot_machine()` in `src/ios/app.mm` exists for
  that second call.
- Entry points: the standalone's context menu, and the extension's panel menu —
  which `plug_window` leaves empty ("a plug-in has no settings of its own") and
  iOS now fills, since installing the images is the one setting it has.

## Open questions for the owner

- Should the iOS app be a *different* UI codebase from the macOS one, or the same? The
  ImGui route makes "same logic, different view layer" cheap; the UIKit route does not.
- Is App Store distribution in scope, or is this sideloaded/development-only? It decides how
  hard the JIT entitlement is to keep.
- Does the MIDI IN need to be a full 4-cable virtual MIDI endpoint as on the real machine
  (`virtualMIDICableCount` returns 4), or is a single physical input enough for the app?
- Should the macOS app that bundles the AUv3 (`src/auv3/main_app.mm`, today a ROM
  installer and a test harness) become a standalone S-MU2000 the way the iOS one is?
  The iOS port is the working reference for the parts macOS lacks: no XPC round
  trip for its own extension, and a panel the app owns rather than borrows.