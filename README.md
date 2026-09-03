# DVHDR-DWM

Dynamic Dolby-Vision-style HDR tonemapping applied as a full-screen post-FX pass
inside DWM, so all on-screen content on the targeted display is governed against
the panel's true MaxFALL and rolled off below its safe luminance ceiling.

Three binaries:

- **`dvhdr.dll`** — injected payload. Lives inside `dwm.exe`. Hooks
  `COverlayContext::Present` and runs a six-pass shader (histogram +
  scene-cut-aware temporal adapt with a two-timescale ABL governor +
  edge-aware luminance base + BT.2390 tonemap with shadow lift, local
  contrast, ICtCp chroma and a hue-preserving gamut clip) over the
  back-buffer before scanout, only on monitors listed in
  `dvhdr.targets`. SDR content and non-targeted monitors pass through unmodified.
- **`dvhdrloader.exe`** — Task-Scheduler-friendly companion. Idempotent: a
  no-args run injects the DLL if absent, no-ops if already present. Run elevated
  (or as SYSTEM from Task Scheduler with highest privileges).
- **`dxgi.dll`** — ReShade-style per-game proxy. Drop it next to a game's
  executable and it loads in place of the system `dxgi.dll` (the application
  directory is searched first), forwards all 20 genuine DXGI exports to
  `C:\Windows\System32\dxgi.dll`, and hooks `IDXGISwapChain::Present`/`Present1`
  to run the **same** six-pass shader over the game's own back buffer — D3D11
  and D3D12. Handles HDR back buffers (scRGB FP16 / HDR10 R10G10B10A2) and SDR
  ones (gamma-encoded 8/10-bit): each swap chain is classified by the colour
  space the game declared through `SetColorSpace1`, else by format, where an
  undeclared 10-bit buffer counts as SDR just as DXGI treats it. SDR surfaces are decoded through
  their gamma curve against the live Windows SDR white level (see `[SDR]` in
  `dvhdr.ini`) and the output is held below that white. Every opaque swap chain
  in the process gets its own state (a video player's picture chain beside its
  UI toolkit's chain, on separate devices and threads); transparent overlay
  chains are passed through. Partial presentation (Chromium's dirty-rectangle
  presents) is handled by keeping the untonemapped page across presents and
  presenting whole frames. In a browser only the GPU process is hooked, and a
  video riding a YUV hardware overlay is beyond reach: launch Chrome with
  `--direct-composition-video-swap-chain-format=bgra` so SDR video overlays
  become RGB chains the proxy can treat (HDR video already uses RGB10A2), or
  `--disable-direct-composition` to fold video into the page chain. Built by the
  `dvhdrproxy` project; the shader source (`dvhdr_dwm.hlsl`) is shared with the
  DWM payload, so both stay in step.

The hook scaffolding (AOB scan, DirectFlip suppression, per-monitor keying via
`DeviceClipBox`) is forked from
[lauralex/dwm_lut](https://github.com/lauralex/dwm_lut).
The shader algorithm is the de-ReShaded twin of
`FFXIV-Dynamic-HDR/Shaders/DVHDR.fx`.

## Build

x64 Release in Visual Studio. Two external dependencies, both bundled or
resolved at solution-open time:

- `minhook` via **vcpkg** (manifest mode — declared in `dvhdr/vcpkg.json`).
  vcpkg integration must be run once per machine: from an elevated shell,
  `vcpkg integrate install`.
- `fxc.exe` (and its `d3dcompiler_47.dll`) for SM 5.0 shader bytecode
  compilation, **vendored under `tools/fxc/`** so the build does not depend
  on a Windows SDK fxc on `PATH`. A `CustomBuild` step in `dvhdr.vcxproj`
  invokes it seven times against `dvhdr_dwm.hlsl` (one per entry point —
  VS_Post, PS_BlurH, PS_BlurV, PS_Tonemap, CS_Clear, CS_Analyze, CS_Adapt)
  emitting bytecode headers into `$(IntDir)`. The DLL `#include`s these
  headers and creates each shader directly from the embedded array — no
  `D3DCompile` at runtime, no `d3dcompiler.dll` dependency inside `dwm.exe`.

## Usage

1. Build the solution. Output lands in `x64\Release\` —
   `dvhdrloader.exe`, `dvhdr.dll`, and (post-build copy) `dvhdr.ini`.
2. **Configure monitors** with display numbers (the same `Display 1, 2, …`
   Windows Settings shows): `dvhdrloader.exe -m 1` writes the chosen monitors
   to `HKLM\SOFTWARE\DVHDR-DWM\Monitors` and force-reinjects. `dvhdrloader.exe
   --list` enumerates available displays with their coordinates.
3. **Tune the shader** by editing `dvhdr.ini` — knobs live alongside the
   loader after build, get installed to `%SYSTEMROOT%\Temp\dvhdr.ini` on next
   inject. `dvhdrloader.exe --force` reloads after edits.
4. **Schedule with Task Scheduler** for persistence — run as SYSTEM with
   highest privileges, triggers "At startup" and "Every N minutes". The
   loader is idempotent: a no-args run injects only if the DLL is absent,
   so periodic ticks self-heal across `dwm.exe` restarts at no cost when
   already loaded.

`dvhdrloader.exe --status` reports whether the DLL is currently loaded.
`dvhdrloader.exe --force` unloads + reinjects (apply new INI / new DLL build).
`dvhdrloader.exe --unload` removes the DLL and deletes the installed files.
`dvhdrloader.exe --help` lists all flags.

## Game guard (protects G-Sync)

The DWM shader necessarily disables MPO / DirectFlip so its `Present` hook can
tonemap every composite - on 25H2 it does so globally, via dwm's internal
`OverlayTestMode`. That is exactly what strips a borderless or windowed game of
independent flip, and G-Sync with it. `dvhdrloader.exe --guard` is a persistent,
elevated watcher that resolves the conflict: while a game holds the foreground it
**unloads** the shader, returning `dwm.exe` to its clean launch state with MPO
re-enabled, and **re-injects** once the game exits.

Detection is the same heuristic OLEDSaver uses to protect G-Sync: a geometric
fullscreen / borderless test plus an ETW present-rate watch for windowed games
(a process presenting at game cadence that also looks like a game - installed
under a game store's folder, or carrying both a renderer and a game-input
library). Both are debounced over two ~2s scans so an alt-tab flash never cycles
the injection. Knobs live in the `[Guard]` section of `dvhdr.ini`.

Run it **instead of** a plain inject-at-logon task, since it owns the inject /
unload cycle itself: add one Task Scheduler entry running `dvhdrloader --guard`
at logon, "run only when logged on" with **highest privileges** (injecting into
`dwm.exe` needs elevation, and so does the ETW watch). It seeds its state at
startup (a game already running keeps dwm clean; an idle desktop is injected at
once) and, while active, re-checks every 15 s so it self-heals across `dwm.exe`
restarts. `dvhdrloader.exe --guard-stop` stops a running guard: a stop is a full
shutdown that **unloads** the shader (returning `dwm.exe` to its clean launch
state with MPO / DirectFlip re-enabled) and removes the installed payload, so
nothing is left injected without a watcher to manage it.

While running, the guard shows a tray icon whose status dot reads **green** while
the shader is injected and tonemapping and **red** while it is unloaded - paused
for a foreground game or manually disabled - so the current state is visible at a
glance. Hover for the same state as a tooltip.

The right-click menu carries an **Injection enabled** toggle (a left-click on the
icon flips it too): use it to manually unload the shader on demand and re-inject
it just as easily. A manual disable is an override that keeps the shader unloaded
regardless of what game detection sees; re-enabling hands control back to the
automatic watch. The menu's **Exit guard** item performs the same clean shutdown
as `--guard-stop` (unload the shader, restore MPO, remove the icon).

## Idle-screen dimmer (moved out)

The idle-screen dimmer that used to live here as `dvhdrloader --dim` has been
split into its own standalone application, **OLEDSaver**, so it no longer shares
a process with the DWM shader injection (which suppresses MPO overlay scanout and
can itself harm G-Sync). See the sibling `OLEDSaver` repository.

## Interaction with ApplyIccLut

The two tools operate at independent layers — ApplyIccLut writes
DisplayConfig HDR static metadata and the NV dither register and exits; DVHDR
lives inside DWM and shapes per-frame composites. They do not contend.

One coordination point: the value `ApplyIccLut --maxlum` writes is what Windows
tone-maps to before handing the back-buffer to DWM. With DVHDR active, keep it
at the panel's true peak (e.g. `--maxlum 1300`) and let DVHDR's `DisplayPeak`
knob be the effective ceiling — that is the design intent (dynamic per-scene
control). Setting `--maxlum` lower stacks a static clamp on top of DVHDR's
dynamic governor; less work for DVHDR but loses the rolloff character.

Robustness: ApplyIccLut's once-per-boot wake-up kick (`SetDisplayConfig
SDC_APPLY`) can reset DWM's D3D device. The DLL detects this on the next
`Present` (mismatched device pointer) and reinitialises its resources against
the new device, so the kick costs at most one frame of plain passthrough.
