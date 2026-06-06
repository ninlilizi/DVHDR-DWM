# DVHDR-DWM

Dynamic Dolby-Vision-style HDR tonemapping applied as a full-screen post-FX pass
inside DWM, so all on-screen content on the targeted display is governed against
the panel's true MaxFALL and rolled off below its safe luminance ceiling.

Three binaries:

- **`dvhdr.dll`** — injected payload. Lives inside `dwm.exe`. Hooks
  `COverlayContext::Present` and runs a six-pass shader (histogram + temporal
  adapt + separable luminance blur + BT.2390 tonemap with shadow lift + local
  contrast) over the back-buffer before scanout, only on monitors listed in
  `dvhdr.targets`. SDR content and non-targeted monitors pass through unmodified.
- **`dvhdrloader.exe`** — Task-Scheduler-friendly companion. Idempotent: a
  no-args run injects the DLL if absent, no-ops if already present. Run elevated
  (or as SYSTEM from Task Scheduler with highest privileges).
- **`dxgi.dll`** — ReShade-style per-game proxy. Drop it next to a game's
  executable and it loads in place of the system `dxgi.dll` (the application
  directory is searched first), forwards all 20 genuine DXGI exports to
  `C:\Windows\System32\dxgi.dll`, and hooks `IDXGISwapChain::Present`/`Present1`
  to run the **same** six-pass shader over the game's own back buffer — D3D11
  and D3D12. Engages only on HDR back buffers (scRGB FP16 / HDR10 R10G10B10A2);
  SDR games pass through untouched. Built by the `dvhdrproxy` project; the shader
  source (`dvhdr_dwm.hlsl`) is shared with the DWM payload, so both stay in step.

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
