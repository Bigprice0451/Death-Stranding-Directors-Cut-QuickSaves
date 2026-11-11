# Repair Vehicles While Charging

Open **Home > QuickSaves > Vehicle repair** and enable **Repair Vehicles While
Charging**. The separate switch starts off and is saved in ReShade.ini under
`[DirectorCutQuickSaves]`, key `RepairVehiclesWhileCharging`. It survives a game
restart and ReShade runtime recreation without changing any other preference.

The selected rate is **1% of maximum vehicle condition per five seconds at a
working charger**, applied smoothly using the game's simulation time. For a
vehicle with 1,000 maximum condition, five repair seconds add 10 condition.
Ordinary damage and timefall still apply, so the final visible gain can be
smaller. Turning the switch off stops further repair; previously gained
condition remains part of the vehicle and is saved normally by the game.

The feature applies to loaded motorbikes, trucks and roadsters connected to a
working generator selected by the game's own battery update. **A full battery
does not stop generator repair.** Leaving its range, losing generator power or
disabling the switch stops it. An unfilled battery must receive charge from that
generator during the update; a full battery remains eligible while the generator
is available. Its native capacity and overcharge allowance remain unchanged.
Other charging volumes still require actual positive transfer. Passive battery
recovery and special race replenishment do not count.

## Why full-battery repair was fixed

Live diagnosis on November 11, 2026 found the affected vehicle at approximately
**703 / 7,500 condition and 3,000 / 3,000 battery**. The game continued calling
the generator transfer routine, but cleared its charging indicator because the
battery was full. The original mod required the battery to increase, so its few
initial repair updates stopped. The corrected gate recognizes that working
generator connection at full capacity. It also measures transfer inside the
generator call, so battery consumption earlier in the update cannot hide real
charging.

The installed 0.2.5 candidate was then observed in the same campaign beside the
generator. Vehicle actor 122880 remained at 3,000 / 3,000 battery while condition
rose from 703 to 864.375, continued through 2,585.358 and 6,189.657, and reached
7,500 / 7,500. The log changed from `reason=repaired` to `reason=condition_full`
and stopped condition writes at the cap. This verifies the affected live
full-battery case; it does not establish every generator or vehicle variant.

Zero-condition vehicles and vehicles with pending fatal damage are excluded.
Repair cannot exceed the native maximum. Cargo, battery capacity and transfer,
ownership, movement, physics, maximum condition, pending damage and the separate
corrosion/wear parameter remain under the game's normal control. This repairs
remaining condition; it does not perform the garage's complete refurbishment.

## Implementation and evidence

This implementation supports only Steam build **13300582**, ds.exe SHA-256
`9d5306d7eb2f5d3135c63accc3e74b8f9b029f9d76112a6aae4a38fdbb4edd2b`.
`Game` verifies the complete executable before the panel starts the feature.
The hook also checks its exact entry bytes, so a conflicting mod fails closed.
Never relax these checks to support a new game update.

The following addresses are hexadecimal RVAs (offsets from the loaded ds.exe).
They were established by offline x64 disassembly and Ghidra 12.1.4 decompilation
of that executable, then exercised with its original native battery and charger
code in the isolated fixture. They are not symbols from a game SDK.

- **2F5C190** is the shared vehicle battery update. Its Windows x64 arguments
  are `(battery, float simulation_delta, vehicle_entity, driver)`. The ordinary
  entity update at **2F78DC0**, call site **2F79368**, supplies entity +898 as
  battery, the time step in XMM1 and that entity in R8. The thunk at **2F5BD20**
  jumps to it unchanged.
- Vehicle activation at **2F68B70** stores the actor pointer at entity +8A8,
  links entity +898 to actor +B0 and links actor +68 to entity +20. The three
  accepted actor vtables are **3E26070**, **3E26A48**, and **3E27790**. Actor +20
  must be active and actor +24 must be 2. These reciprocal links and actor ID
  are checked before and after the original battery call.
- **2CF6F00** is the native generator transfer, with arguments
  `(generator, battery, float simulation_delta, vehicle_entity)`. It checks its
  battery limit before availability and clears battery +14 at full capacity.
  The second hook observes this exact native call within the validated battery
  update. A thread-local context matches both battery and entity, restores any
  outer context after a nested update, and retains no pointers after returning.
  It runs the original transfer exactly once and measures battery +8 immediately
  before and after it. An unfilled battery requires a positive transfer; at the
  native full threshold the available connection is enough. That threshold is
  battery +C times 1, or the resource +4DC multiplier for generator levels over 2.
- **2C69110** is the exact-build, read-only generator availability predicate.
  It reads generator level/resource/condition fields without changing state.
  The mod calls it even at full battery, where the original transfer's early
  return would skip it. A negative generator +A8 flag, missing resource, invalid
  or nonpositive configured charge rate, invalid full threshold or unavailable
  generator rejects repair. The configured rate is resource +4B4 plus the native
  level index. These checks do not consume power, lower the battery or force a
  charge animation merely to keep repair running.
- **2F5C190** also handles registered charging volumes. When no generator call
  was observed, these retain the original gate: battery +48 bit 8, battery +14
  equal to 1, and positive battery change during the update. Nearby
  `DSChargeParkingComponent` / `BatterySpeed` metadata suggests parking charging;
  that facility mapping remains an inference. The branch is identified
  statically; the isolated original-code fixture exercises generators, not
  parking-volume discovery or individual facilities. Source selection comes
  from native charging code, not a guessed radius or wall-clock timer.
- A different branch calls **2F6BE90**, counts particular wheel-contact surface
  IDs, and replenishes battery from vehicle resource +150. Native resource
  metadata at VA **144CA2D38** names +150 `RaceBatteryChargeSpeed`, while +14C is
  `RaceBatteryConsumptionSpeedByAccel`. Resource +138 is independently named
  `BatteryNaturalCureSpeed`. These race and passive branches are intentionally
  excluded. Powered-road operation was not established by this investigation:
  reducing consumption alone does not count, and support for a separate road
  charge source is not claimed. Investigate its actual transfer path before
  adding support; never broaden the gate to any battery increase.
- Vehicle initialization at **2F75070**, writes **2F75277/2F7527F**, initializes
  entity +6E8 (remaining condition) and +6EC (maximum) from the same vehicle
  resource +12C. Native damage update **2F87B40** uses component base entity
  +670: +78 is current condition, +7C maximum, and +80 pending damage. It
  subtracts pending damage and clamps condition to the maximum. The surrounding
  **2F916D0** detects crossing zero and performs the normal destroyed-vehicle
  callbacks. Authored vehicle condition setup at **2D6D160** also writes entity
  +6E8 directly. The mod changes only that current-condition float, after the
  native battery call and using its latest value. It neither clears pending
  damage nor calls the general Entity Heal path, which belongs to another
  health system.

`src/charging_repair.cpp` owns both detours and validates object lifetime and
world state. It runs the original battery and generator functions exactly once,
then uses `src/charging_repair_policy.hpp` to calculate the capped addition.
Only the native update thread touches the vehicle. The overlay communicates
through atomic settings and counters; it does not retain entity pointers.
A small, coherent diagnostic observation is published at most four times per
second through a nonblocking lock attempt. If the overlay is reading it, the
native update skips publishing that sample and continues; it never waits for
the overlay. Counter values are individually atomic, not a single-time snapshot.
There is no time accumulator, vehicle registry or delayed repair work. A world
change, pause, cutscene, load, invalid value or simulation step over one second
is excluded; a late frame cannot grant a catch-up repair.

`src/charging_repair_panel.cpp` owns the saved preference, checkbox and local
diagnostics. `src/charging_repair_diagnostics.hpp` contains the heartbeat timing,
reason names, sample formatting and current-status text. The callback module is
pinned while the native hooks exist. A
runtime shutdown disables the effect without removing executing code, and a
new runtime reuses the hook and loads the saved preference. No global MinHook
shutdown is performed because other features share it.

## Integration and verification

The add-on calls `charging_repair_panel_start(game.base())` only after its
existing executable check succeeds, `charging_repair_panel_stop()` when that
runtime closes, `charging_repair_panel_tick()` with the regular overlay update,
and `draw_charging_repair_panel()` when drawing the QuickSaves panel. The native
source links the existing `cinematic_hooks` MinHook library; the panel uses the
same ReShade and ImGui headers as the existing panels.

From the project directory:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\charging_repair_native_test.exe 'J:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT\ds.exe'
```

- `tests/charging_repair_test.cpp` covers the selected rate at
  20/30/60/144/240 Hz, stop/restart without accumulated credit, disabling,
  full/decreasing battery with a verified working source, exclusion without
  that source, destroyed/overfull/invalid condition, pending fatal damage,
  caps and invalid/discontinuous time steps.
- `tests/charging_repair_panel_test.cpp` uses actual ImGui checkbox input and
  a ReShade preference fixture to verify default off, save/reload, runtime
  suspension, independence from other settings and an unavailable toggle.
  It also checks zero-update heartbeats, deterministic 15-second scheduling,
  complete reason/value reporting, current versus stale UI status, and visible
  log-write failure/recovery without repeated error spam.
- `tests/charging_repair_native_test.cpp` verifies the original executable hash
  and maps it into its **own process** without launching the game or resolving
  its dependencies. It supplies synthetic world services and objects while
  executing the original **2F5C190** battery update and **2CF6F00** generator
  transfer. It compares those native results with the actual installed detour
  at 30/60/144 Hz; tests on/off, full and nearly full batteries, uninterrupted
  repair after reaching capacity, and consumption before a positive generator
  transfer. Full-battery negative cases cover unavailable/disabled generators,
  zero or invalid charge rates and leaving the generator, including rejection
  counters. Further cases cover an unfilled battery receiving no power and a
  full overcharged battery at an upgraded generator. It also checks all accepted
  actor classes, invalid links, scene
  gates, destroyed condition and caps, and compares every fixture actor/entity
  byte except current condition. This
  demonstrates preservation of the supplied cargo/battery/physics bytes. It
  does not observe a live campaign, real charger discovery, rendering or cargo
  simulation, and does not attach to the running game.

The policy, native fixture and real ImGui panel check passed during development.
The panel check is part of the normal suite when `IMGUI_TEST_SOURCE_DIR` is configured.
The live observation above verifies repair of the affected full-battery bike
through its maximum condition. Visual gauge presentation, other generator/
facility variants and repair-specific save/load round trips still require
their own gameplay acceptance. An isolated fixture is not proof of those
additional live results.

## Diagnosis and manual acceptance

The normal local Diagnostics log records `charging_repair_ready`,
`charging_repair_toggle`, `charging_repair_error`, and `charging_repair_sample`.
While enabled, a sample is written immediately and every 15 seconds, even if
native update counts stay zero or unchanged. This heartbeat distinguishes an
idle/stalled hook from missing telemetry. No disk I/O occurs inside a vehicle
update. A failed write appears in the panel and once in the ReShade log per
failure streak; the warning clears after a successful write.

Each sample includes cumulative `charging`, `repairs`, `excluded`, and separate
`reason_*` counts; the latest reason and actor ID; battery/capacity, condition/
maximum, pending damage, simulation delta and native flags. `observed_at_ms` and
`repaired_at_ms` use Windows uptime, with `observed_age_ms` and `repair_age_ms`
expressing age at logging. Age -1 means no such observation. These are separate
from the log record's UTC timestamp. A sample with `valid=0` must not be treated
as complete vehicle-state evidence.

`charging` counts eligible external charging connections, including a working
generator at full battery. `repairs` counts actual condition writes. A vehicle
at maximum condition can increase the first count without the second. The UI
uses the latest observation and stops reporting it as current after three
seconds; a historical successful repair no longer leaves a permanent success
message. The displayed values belong to the latest sampled vehicle, identified
by `actor_id` in the log, not necessarily the vehicle Sam occupies.

For a manual check, start with a damaged, usable vehicle at a working generator.
Compare the switch off and on over five simulation seconds: condition should
gain 1% of its maximum before ordinary damage is applied. Repeat with an empty,
nearly full and full battery. Repair should continue after the battery reaches
capacity. Leave the generator, disable it or turn off the mod switch: repair
must stop, with no catch-up on return. Repeat with another supported vehicle.
Check the saved checkbox after restarting, ordinary cargo/handling and a
save/load round trip. A destroyed vehicle must stay destroyed.

If `charging` stays zero at a generator, inspect `reason_no_charger`,
`reason_charger_unavailable`, scene/lifetime exclusions and the latest sample's
age. A growing `reason_condition_full` explains a healthy vehicle; destroyed,
invalid-condition and pending-fatal-damage reasons explain other refused writes.
If all counts remain unchanged while heartbeats continue, the native hook has
not observed new updates. If the heartbeat itself stops or a write warning
appears, check the overlay runtime, Diagnostics folder and available disk space.
For unsupported charger types, preserve the log and inspect the exact native
source before broadening the gate. A compatibility
error requires checking game identity and conflicting mods, not bypassing the
entry signature. Offline tests and telemetry are evidence about the listed
paths, not a substitute for those player-visible checks.
