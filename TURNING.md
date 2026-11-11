# Sam Turns on a Dime

Version 0.2.3 adds an optional turning-assistance switch under **QuickSaves >
Gameplay**. It starts off. ReShade remembers `SamTurnsOnADime` in the
`DirectorCutQuickSaves` section of ReShade.ini. Turning it off takes effect on
subsequent movement calls; it does not require reloading a save.

The switch removes positive slowdown from three angular-response multipliers
while Sam's heading and facing are calculated. It leaves cargo mass, carrying
capacity, walking/running speed settings, stamina and saved game data alone.
It applies to the local player on foot during normal play. Vehicles, pauses,
cutscenes and loading transitions retain their normal behavior.

**Gameplay validation is pending.** The isolated tests establish that these
angular multipliers return to their default response. They do not establish
that every cargo, balance, terrain or animation state uses only those three
values. This first version is experimental; identical behavior to zero cargo
in every situation is not yet established. Explicit zero turn locks and
authored responses above the default remain intact. The native game still
decides whether Sam is allowed to move at all when overloaded.

## Implementation and ownership

- `src/turning_policy.hpp` owns the temporary adjustment and exact restoration.
- `src/turning.cpp` installs two MinHook detours, checks the local player and
  world state, and counts eligible, excluded and changed calls.
- `src/turning_panel.cpp` owns the checkbox, ReShade preference and log reports.
- `src/addon.cpp` connects these to runtime startup, shutdown and the overlay.

The full executable hash is checked by `Game` before startup. These offsets
apply only to the original Steam build 13300582, SHA-256
`9d5306d7eb2f5d3135c63accc3e74b8f9b029f9d76112a6aae4a38fdbb4edd2b`.
The additional entry-byte checks reject already-modified functions rather than
stacking an unknown detour. Addresses below are relative to the loaded ds.exe.

`DSPlayerMover` is held at Sam entity + `0x54b0`. It holds the entity at `0x48`
and its DSPlayerEntity at `0x2c8`. The local player is obtained from the player
manager at RVA `0x7bc7568`, field `0x48`; its entity is at `0x70`, player index
at `0x88`, and vehicle identifier at `0x3bc`. The existing `no_ride` constant
identifies an on-foot player. All of these identities must agree.

The heading function at RVA `0x26d5bc0` consumes the mover's `0x318` and `0x31c`
floats when smoothing its two desired directions. The facing function at
`0x268b0e0` consumes `0x314` when interpolating the final orientation. Their
observed native interpolation rates use `dt * 10`, `dt * 15`, and `dt * 20`
respectively, multiplied by the corresponding value. Native code also applies
its own constraints and blending. The constructor sets all three multipliers
to 1, and the ordinary mover update resets them to 1. Parameter setters at
RVAs `0x273f790`, `0x273f7b0`, and `0x273f7d0` supply authored overrides.

Each detour substitutes 1 only for a finite multiplier strictly between 0 and
1, calls the original function, and restores the original bytes before it
returns to the movement update. The heading detour changes only `0x318/0x31c`;
the facing detour changes only `0x314`. It does not leave a low cargo weight in
the shared entity, which would affect other movement systems and inventory.
The exact cargo-to-animation-parameter path is not claimed to be fully mapped.

Only the native update thread touches the mover. The render thread changes an
atomic preference and reads atomic counters. Gate reads have a narrow Windows
exception handler for disappearing world pointers; the original game function
is outside that handler. There is no per-frame allocation, file output,
background memory scan or polling loop. Logs are reported at most once per
15 seconds from the render callback, and only after new calls occur.

Hooks remain pinned until process exit so a returning native call cannot enter
unloaded code. Runtime destruction disables their effect. A new runtime reuses
them and reapplies the saved preference. Startup keeps effects disabled until
both hooks are active. A partial activation failure leaves any live trampoline
as a pass-through and requires a restart, rather than freeing executing code.

## Verification

Build and run the usual CTest suite from the project folder:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`turning_scope_restoration` verifies the exact affected bytes, disabled
behavior, nested scopes, restoration after a C++ exception, zero locks,
boosts, invalid values and preservation of all other mover bytes.

The optional native fixture requires your own supported ds.exe:

```powershell
.\build\Release\turning_native_test.exe 'J:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT\ds.exe'
```

It verifies the executable hash, maps code into its own process without running
the game's entry point, resolves only Windows math imports, and supplies
synthetic player/mover objects. It does not attach to or modify a running game,
start Steam, read saves, initialize graphics or redistribute game code.
The test compares full mover results and facing output before/after installing
the actual detours. A 0.15 response must differ from 1, enabling must reproduce
the 1 response, and disabling must restore the 0.15 response at 30/60/144 Hz.
The fixture verifies that the complete synthetic entity, including position
and speed fields, remains untouched. It also checks vehicles, other movers,
scene flags, missing/unreadable world pointers, runtime reuse and counters.

With `IMGUI_TEST_SOURCE_DIR` configured as described in README.md, the real
ImGui panel test also checks checkbox clicks, default-off behavior, saved
preference scope, restart/resize lifecycle, failed-startup UI and toggle logs.

For the 0.2.3 candidate, all 18 CTest checks and the native fixture passed.
An indicative 50,000-iteration measurement of both native calls averaged
0.553 microseconds without hooks, 0.550 disabled and 0.550 enabled on this PC.
These are isolated math-fixture measurements, subject to timing noise, and do
not establish any change in game frame rate or simulation-thread contention.

## Gameplay check and diagnosis

After restarting the game, open Home > QuickSaves > Gameplay. Compare a turn
with the switch off/on using the same cargo, terrain and walking speed. Repeat
while stationary, walking, running, carrying a heavy load, and at the carrying
limit. Check that the checkbox survives a restart and display resize. Verify
vehicle controls and cutscenes remain ordinary. The user's live playtest is
still pending; offline verification does not replace these observations.

The usual Diagnostics log contains:

- `turning_ready`: hooks are installed and the saved on/off preference loaded.
- `turning_toggle`: the requested state changed.
- `turning_sample`: cumulative eligible calls, heading/facing adjustments and
  excluded calls. Two calls can occur per update; these are not frame counts.
- `turning_error`: the switch is unavailable, with a specific startup reason.

If eligible stays zero during normal movement, recheck the player identity and
world gates against this executable. If eligible grows but heading/facing stay
zero during a sluggish loaded turn, these three parameters are not the source
of that slowdown. Investigate that movement/animation path before broadening
the hook; do not patch shared cargo mass or speed to hide the symptom. If a
compatibility error appears, preserve the log and verify game integrity and
other movement mods. Do not relax the hash or byte checks to make it run.
