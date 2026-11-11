# Repair Vehicles While Charging

Open **Home > QuickSaves > Vehicle repair** and enable **Repair Vehicles While
Charging**. The separate switch starts off and is saved in ReShade.ini under
`[DirectorCutQuickSaves]`, key `RepairVehiclesWhileCharging`. It survives a game
restart and ReShade runtime recreation without changing any other preference.

The selected rate is **1% of maximum vehicle condition per five seconds of
actual charging**, applied smoothly using the game's simulation time. For a
vehicle with 1,000 maximum condition, five charging seconds add 10 condition.
Ordinary damage and timefall still apply, so the final visible gain can be
smaller. Turning the switch off stops further repair; previously gained
condition remains part of the vehicle and is saved normally by the game.

The feature applies to loaded motorbikes, trucks and roadsters whose native
battery update actually transfers charge at a working charger. Merely standing
near a generator is insufficient. Leaving its range, an unavailable generator,
or a full battery stops repair. Passive battery recovery and special race
replenishment do not count. A
generator's native overcharge allowance remains unchanged: repair can continue
while it actually transfers charge, and stops at its own full threshold.

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
- **2CF6F00** is the native generator transfer. It checks generator availability
  and its battery limit before setting battery +14 to 1 and adding charge to
  battery +8. A full or unavailable generator clears +14. **2F5C190** selects
  the actual charger and sets battery +48 bit 8 for that path; ordinary passive
  recovery clears it. Repair requires both indicators **and a positive battery
  increase during this exact call**. A separate registered charge-volume branch
  reads a resource charge rate and sets the same indicators after positive
  transfer. Nearby `DSChargeParkingComponent` / `BatterySpeed` metadata suggests
  parking charging; that facility mapping remains an inference. The transfer
  branch itself is identified statically;
  the isolated original-code fixture exercises generators, not parking-volume
  discovery or individual facilities. This is a native state/transfer check,
  not a position radius or wall-clock timer.
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

`src/charging_repair.cpp` owns the detour and validates object lifetime and
world state. It runs the original battery function exactly once, unchanged,
then uses `src/charging_repair_policy.hpp` to calculate the capped addition.
Only the native update thread touches the vehicle. The overlay communicates
through atomic settings and counters; it does not retain entity pointers.
There is no time accumulator, vehicle registry or delayed repair work. A world
change, pause, cutscene, load, invalid value or simulation step over one second
is excluded; a late frame cannot grant a catch-up repair.

`src/charging_repair_panel.cpp` owns the saved preference, checkbox and local
diagnostics. The callback module is pinned while the native hook exists. A
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
  passive/full/decreasing battery, destroyed/overfull/invalid condition,
  pending fatal damage, caps and invalid/discontinuous time steps.
- `tests/charging_repair_panel_test.cpp` uses actual ImGui checkbox input and
  a ReShade preference fixture to verify default off, save/reload, runtime
  suspension, independence from other settings and an unavailable toggle.
- `tests/charging_repair_native_test.cpp` verifies the original executable hash
  and maps it into its **own process** without launching the game or resolving
  its dependencies. It supplies synthetic world services and objects while
  executing the original **2F5C190** battery update and **2CF6F00** generator
  transfer. It compares those native results with the actual installed detour
  at 30/60/144 Hz; tests on/off, full/unavailable/leaving chargers, all accepted
  actor classes, invalid links, scene gates, destroyed condition and caps; and
  compares every fixture actor/entity byte except current condition. This
  demonstrates preservation of the supplied cargo/battery/physics bytes. It
  does not observe a live campaign, real charger discovery, rendering or cargo
  simulation, and does not attach to the running game.

The policy, native fixture and real ImGui panel check passed during development.
The panel check is part of the normal suite when `IMGUI_TEST_SOURCE_DIR` is configured.
The user's in-game playtest remains pending and is required to confirm the
visible condition gauge, particular generator/facility variants and campaign
behavior. The developer did not control, restart, inject into or modify the
running game for this feature.

## Diagnosis and manual acceptance

The normal local Diagnostics log records `charging_repair_ready`,
`charging_repair_toggle`, `charging_repair_error`, and `charging_repair_sample`.
Samples contain cumulative `charging`, `repairs` and `excluded` update counts
and are written at most every 15 seconds while enabled. No disk I/O occurs
inside a vehicle update. `charging` counts real positive native transfers;
`repairs` counts actual condition writes. A charging vehicle at maximum
condition can increase the first count without increasing the second.

For a manual check, start with a damaged, usable vehicle and an unfilled
battery. Charge it with the switch off, then on. Observe five simulation
seconds while the battery increases; condition should gain 1% of its maximum
before ordinary damage is applied. Leave the charger, fill the battery,
disable the switch, and repeat with another supported vehicle. Repair should
stop for each noncharging/disabled interval, with no catch-up on return. Check
the saved checkbox after restarting, ordinary cargo/handling, and a save/load
round trip. A destroyed vehicle must stay destroyed.

If `charging` stays zero despite visible charge transfer, preserve the log and
inspect the exact native source/indicators for that charger type before
broadening the gate. If `charging` rises but `repairs` does not, check remaining
condition, maximum, pending damage and time-step exclusions. A compatibility
error requires checking game identity and conflicting mods, not bypassing the
entry signature. Offline tests and telemetry are evidence about the listed
paths, not a substitute for those player-visible checks.
