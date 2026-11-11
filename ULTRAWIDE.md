# Force 3440 x 1440 Ultrawide

This independent QuickSaves checkbox requests a 3440 by 1440 game image on a
monitor running at that resolution. It starts off and is saved in ReShade.ini
as `DirectorCutQuickSaves / ForceUltrawide3440x1440`. It replaces the resolution
in normal game display requests and checks for drift after Alt+Tab returns.
It does not change the Windows desktop mode or take focus while you are away.

The game retains its other graphics choices, including window mode, HDR, frame
limit and upscaler selection. DLAA + Preset K remains separate: without DLAA,
the selected upscaler may render internally below the 3440x1440 output. This
option controls the game image area and output sizing, not the DLSS scale.

Turning the switch off restores the underlying game resolution on the next
focused display update. Initially this is the current in-memory renderer size,
which can differ from a stale last-written settings.cfg. Subsequent ordinary
graphics requests update that choice. The native settings writer records that
underlying resolution, so an unsupported custom size is not left in
settings.cfg after removing the mod. Changes made to the game's resolution
while the switch is on become the underlying choice, while the override keeps
requesting 3440x1440. The menu may still name a standard resolution; the mod's
image/output readout shows the measured sizes separately.

## Why the native display path is necessary

The inspected running game had a 3440x1440 DXGI output but a 3360x1440 renderer
and a 7/3 aspect ratio. That leaves 80 horizontal pixels outside the game image.
Its saved settings also contained 2520x1080. Stretching the final output alone
would not correct the game's own image area, projection or resource sizes.

`src/ultrawide.cpp` runs through the same graphics application function as the
game's normal settings update. This function manages window state and native
resource reconstruction in engine order. The mod supplies a private copy of
the full request with only width and height changed. It never independently
resizes a live DXGI surface or changes renderer dimensions from the overlay.

Native findings apply only to Steam build 13300582, SHA-256
`9d5306d7eb2f5d3135c63accc3e74b8f9b029f9d76112a6aae4a38fdbb4edd2b`.
Addresses below are relative to ds.exe. The host checks the full file identity
and the component checks entry bytes before installing hooks.

- `0x2642EC0`: normal display/settings poll; arguments are renderer and user
  settings, and its return value is a Boolean change indicator. Its normal
  native application call is at `0x2643422`.
- `0x2642010`: graphics application, returning a Boolean change result. Its
  136-byte request contains 34 signed integers. Width/height are the first two.
  Other fields are copied intact. The constructor's unspecified value is -1.
- `0x26447C0`: reads all current native graphics options into that request.
- `0x1CD5130`: rebuilds renderer resources and passes the display dimensions to
  NGX through `0x19A81A0`. The existing DLAA hook still handles that stage.
- `0x19A8220`: the native output-recreation path writes the aspect at device
  `+0x5BA1F8` from requested width divided by height; it does not clamp that
  division to 7/3. No shared aspect constant or shader is patched.
- The device pointer is at `0x4F6D430`, renderer pointer at `0x578C148`.
  Renderer `+0x3F8/+0x3FC` gives the image size. Device `+0x5BA1D0` points to
  the current surface, whose packed dimensions are at `+0x34`; the no-surface
  fallback is device `+0x5BA188/+0x5BA18C`.

## Shared snapshot and restoration

DLAA and ultrawide both need `0x1CD4BB0`, the small native settings snapshot
function. `src/graphics_snapshot.cpp` owns its single detour and passes the
original caller address to each component. This avoids competing hooks on the
same function and keeps the DLAA refresh behavior independent of ultrawide.

The native snapshot stores width/height in its first eight bytes.
`ultrawide_settings_snapshot` substitutes the remembered game choice only for
the native configuration writer's call returning to `0x2645034`. Normal game
rendering and other snapshot callers continue seeing the actual forced size.
The writer derives its window rectangle normally; window mode is unchanged.
The underlying size is one atomically published 64-bit value, so a reader
cannot combine width and height from separate choices.

An Alt+Tab can change the window/viewport independently of cached render size.
When a repair is necessary, a thread-local flag invalidates the first width
in the native apply routine's temporary comparison snapshot (return address
`0x26447E6`). The requested dimensions stay valid. This makes the original
function perform its normal rebuild even when its cached dimensions already
match. The flag is consumed once and cleared when the call returns. This
comparison-only change never reaches a live render resource or saved setting.

All application work occurs inside the intercepted native poll. The render
overlay writes only the requested Boolean. The poll is guarded against
concurrent/reentrant mod work; native game calls still pass through. A resize
requires the actual game window to be foreground and not minimized. Enabling
also requires its monitor to be 3440x1440. The repair budget permits three
attempts at least one second apart, then waits for a new toggle, focus return
or normal game-settings request. This prevents an endless GPU rebuild loop
when the game or display driver rejects a request.

A thread-specific `WH_CALLWNDPROC` observer records focus-loss/minimize messages
without changing them. Its generation counter rearms retries on return even if
the native display poll was completely suspended while the game was minimized.
The observer is reused across swapchain resets and replaced only if the owning
window thread changes. It never sends input or changes focus. When the option
is off and no restoration is pending, the native poll takes a short pass-through
path without querying Windows display state.

Hooks and their module stay pinned through runtime recreation. Overlay
destruction during a resize does not disable the resolution override. Startup
reuses existing hooks and refreshes the window identity and saved preference.
Failed startup leaves the toggle unavailable with a diagnostic message.

## Rendering-history return contract

The native poll returns its change indicator in AL (`movzx eax,dil` at
`0x264353F`). Its caller tests that value at `0x263F1EE`. A true result reaches
`0x3643C70`, which asks the renderer to invalidate temporal history through
`0x1D04620`. DLSS then resets its accumulated frame history.

Versions before 0.2.6 incorrectly declared the poll and its hook as `void`.
Hook cleanup could leave an unrelated nonzero value in AL. Live diagnosis of
the affected scene measured 181 history invalidations in 181 frames from this
caller, and 213 consecutive DLSS evaluations with their reset counter at one.
The recordings showed intermittent missing clouds. Turning the ultrawide
checkbox off did not remove the hook, so it could not avoid the bad return.

Both function signatures must remain `bool`. Every pass-through returns the
native result, including unavailable/reentrant/disabled paths. The normal path
retains that result through observation and cleanup. If an additional repair
is needed, it always calls the native apply function, then combines its Boolean
result with the poll result. Do not short-circuit the apply call when the poll
already returned true, and do not suppress genuine history resets globally.

## Tests and troubleshooting

`ultrawide_resolution_policy` checks replacements for 1920x1080, 2520x1080,
3360x1440 and 3840x2160, preservation of every other request field, restoration,
rejection of pillarboxed/invalid measurements, retry spacing, exhausted
budgets, Alt+Tab rearming and immediate restoration after disabling.
`ultrawide_menu_preference` drives the shipping checkbox through real ImGui
input and checks saved preference scope, runtime recreation and failure UI.
Enable `IMGUI_TEST_SOURCE_DIR` as described in README.md for the menu test.

`ultrawide_native_poll_contract` compiles the production detour with private
native-memory fixtures and fake Windows display answers. It checks both
Boolean signatures, native false/true results through unavailable, reentrant,
disabled and active branches, and every native/extra-repair result combination.
It also verifies that native true never skips a necessary repair, and that
scope cleanup releases only its own ownership. It requires no game or window.

The optional `graphics_snapshot_native_test.exe <path-to-ds.exe>` verifies the
full executable hash and maps the original code in its own process without
starting the game. It executes the actual leaf snapshot and the production
MinHook detour. Three private call thunks reproduce the relevant native return
addresses, proving callback dispatch, configuration-copy preservation,
single-use comparison invalidation and separation from the DLAA refresh field.
Only the game's snapshot function executes; this is not a GPU-resize test.

Run `cmake --build build --config Release` and
`ctest --test-dir build -C Release --output-on-failure` from the project root.
These checks do not prove a live GPU resize or visual removal of side bars.
The user will perform the in-game acceptance test: turn it on, inspect the
image/output dimensions, Alt+Tab away and back, select a different resolution,
and turn it off. Also test DLAA on/off and a cutscene. A pre-rendered movie may
contain black pixels in the video itself; this option does not crop video.

The local Diagnostics log records `ultrawide_ready`, `ultrawide_toggle`,
`ultrawide_error` and changed `ultrawide_sample` observations. Samples contain
image, viewport and output sizes, aspect, focus, monitor eligibility, repair
count and pending restoration. There is at most one observation check per
second in the overlay and no per-frame file logging. A matching output size
alone is not reported as proof of a full game image.

If the switch remains waiting, check that Windows is using 3440x1440 on the
game's monitor, the game has focus, and the native poll has produced a recent
sample. Three attempts with mismatched dimensions indicate a deferred or
rejected resize; use the log before changing offsets or retry limits. Keep
the executable hash and entry-byte checks when porting to a new game build.

Research reference: Lyall's MIT-licensed
[DeathStrandingDCFix](https://codeberg.org/Lyall/DeathStrandingDCFix/src/branch/main/src/dllmain.cpp)
helped locate the game's resolution enumeration and validation. This feature
uses the separately traced native apply path instead of copying its menu-list
patches or shipping that mod.
