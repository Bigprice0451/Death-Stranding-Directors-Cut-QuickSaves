# Native DLAA and Preset K

Enable DLSS and select Quality in Death Stranding's graphics settings. Then turn on the native DLAA option in QuickSaves. The game keeps displaying Quality. On the next normal graphics update, the game flushes its GPU work, asks NVIDIA for native DLAA sizing, and rebuilds the render targets. A short pause during this change is expected. Switching the option off follows the same path and returns to the game's Quality rendering resolution. Performance, Balanced and the other menu choices pass through unchanged.

The overlay has separate evidence for creation and for a rendered frame. A native measurement requires all of the following on one feature handle: the NGX create parameters read back mode 5 (DLAA) and hint 11 (Preset K), creation succeeded, the game successfully evaluated that handle, its real input rectangle matched the output size, the input resource could cover it, and the actual output texture had that size. Measurements expire after a short gap in game rendering. NVIDIA exposes the requested hint here. The specific neural model ultimately selected by the display driver is opaque to this code. No registry, NVIDIA App, or global driver profile is changed.

If the game defers a resize during a transition, the overlay waits. It can request the normal rebuild twice, then offers a manual display-settings reapply. If NVIDIA returns a scaled image for its DLAA sizing query, the add-on asks for regular Quality again. If the driver rejects the override during creation, the game recreates its normal Quality feature before the current rebuild returns.


The settings-snapshot detour is shared with the optional ultrawide control in `src/graphics_snapshot.cpp`. It passes the original call-site address to the DLAA callback, preserving the same fenced-refresh check. Only one detour owns that function. Ultrawide controls the output/image area; this DLAA option independently controls the internal NGX input size. See [ultrawide integration](ULTRAWIDE.md).

## Where to make changes

`src/dlaa_ngx.hpp` contains the NVIDIA-side rules and frame checks. The setting that the menu calls Quality is 2. The game rejects modes above 4 in its own manager, so only the query inside the renderer and the NGX create call receive 5. The DLAA-specific preset hint is an unsigned integer. The create call saves and restores the game's previous mode and preset on the shared parameter block. The game calculates its real rendering scale from the sizing result before allocating textures.

`src/dlaa.cpp` connects those rules to the verified Steam executable (build 13300582). The UI stores only the preference. Hooks run on the game's normal render paths. There are no game-owned resource references kept past the lifetime of a callback. The retained opaque handle is used only to compare identity, and is removed after a successful release. Graphics hooks can stay active across a ReShade swapchain rebuild.

Engine locations for this exact executable, expressed as offsets from `ds.exe`:

* `0x2642F08` calls the previous-settings snapshot at `0x1CD4BB0`. Only this caller receives a change to byte `+0x20` in its temporary copy. The game compares it to the real option at `0x26430DD`, flushes its queues at `0x264312B`, writes the real value back to the copy at `0x26433E2`, and rebuilds its targets at `0x26433E6`.
* Inside that rebuild, `0x1CD5385` calls the display-size handler `0x19A81A0`. It normally skips the NGX update for an unchanged display resolution. For the current pending toggle, call its native configure operation with the current device command context and the force flag. The game then uses the resulting scale to allocate the render targets. No graphics work is submitted from the overlay thread.
* `0x19894C0` is the native configure operation. `0x1980640` obtains the optimal internal size. The game stores it in its renderer manager at `+0x18/+0x1C`. `0x38F38F0` forwards feature creation to NGX. Request the preset there only when the dimensions, renderer manager, feature type and thread-local sizing transaction match. `0x197AF90` submits the real color/output resources and the active input rectangle. `0x38F3CE0` releases the feature.

`Game` verifies the complete executable hash before passing the module base to this component. An update requires rechecking both snapshot call sites, the original graphics options, queue flush, resource allocation, all NGX call signatures and the actual frame layout before accepting a new hash. Preserve the original app's command ordering; creating a new feature from a render overlay callback can leave already-recorded GPU work referring to released resources.

## Verification

The normal test suite builds `tests/dlaa_test.cpp`. It covers scaled and rejected size answers, readback failures, real surface bounds, alternate quality choices, disabled overrides, failed creation, and restoration of previous hints.

`tests/dlaa_gpu_smoke.cpp` is an optional standalone RTX test. It opens its own D3D12 device, runs the same sizing and creation code, and submits real evaluations for both DLAA and ordinary Quality. It requires a current NVIDIA NGX SDK, a signed `nvngx_dlss.dll` and an RTX adapter. In an x64 Visual Studio developer prompt, from a disposable folder on D:, adapt the directories below to the SDK, source tree and runtime location. The first argument to the executable is the directory containing the runtime DLL, the second is a writable log folder, and the optional last two numbers are the requested resolution. The program checks that the requested DLL actually loaded.

```bat
cl /nologo /EHsc /O2 /std:c++17 /MT /I"D:\SDK\DLSS\include" /I"D:\Mod Projects\Death Stranding QuickSave\src" "D:\Mod Projects\Death Stranding QuickSave\tests\dlaa_gpu_smoke.cpp" "D:\SDK\DLSS\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" dxgi.lib d3d12.lib advapi32.lib shell32.lib version.lib ole32.lib user32.lib
dlaa_gpu_smoke.exe "D:\DLSS-Runtime" "D:\NGX-Test" 3360 1440
```

With an RTX 4090, driver 617.42 and NVIDIA DLL 310.9.16.0, both evaluations completed on the GPU. At 3360 by 1440, DLAA rendered 3360 by 1440 with K=11 submitted. Switching off restored mode 2 with default hint 0 and an input of 2240 by 960. At 1280 by 720, the corresponding inputs were 1280 by 720 and 853 by 480. The smoke test exercises the public NGX runtime in its own process. The QuickSaves readout provides the separate in-game check after installation.

NVIDIA documents the quality and preset numbers in the [NGX definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h). The [Streamline DLSS implementation](https://github.com/NVIDIA-RTX/Streamline/blob/main/source/plugins/sl.dlss/dlssEntry.cpp) demonstrates submitting the per-mode preset as an unsigned integer.
