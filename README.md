# Death Stranding Director's Cut QuickSaves

Version 0.2.0 supports the Windows Steam build 13300582. Extract the contents of the release ZIP beside ds.exe. The package includes a full add-on-enabled ReShade 6.8.0 runtime built from the upstream source. [Player instructions](release_assets/READ%20ME.txt) cover installation, the controls, diagnostics, and removal.

## Controls and storage

- **Quick Save** defaults to F5. Add or replace any number of independent keyboard shortcuts under Quick Save keys. A single new press makes one request even if two bindings go down together. Typing into a menu field and assigning a key are excluded.
- **Auto Save** has its own bar on the same menu, a 1–60 minute gameplay timer, and a separate archive. It defaults to off with a 10-minute interval. ReShade saves the preference. Both shelves can retain 1–100 entries. The initial limits are 100 Quick Saves and 15 Auto Saves. A reduction shows how many older entries it will retire. Unreadable files are preserved.
- **Gameplay restrictions** apply to all new mod captures. A cutscene, movie or loading transition holds new captures and loads. A blocked keypress cannot create a later save labeled with an earlier screen. The world reader still requires an open delivery in the matching game world. Saves from the original local playtest remain readable.
- **Graphics** has an independent Force DLAA + Preset K switch. Leave the game's DLSS option on Quality. The overlay reports real render dimensions when available. Preset K is a hint sent to NGX. [Graphics implementation and hardware verification](DLAA.md)
- New installs use the Windows Saved Games folder under DirectorCutQuickSaves, with Local AppData as a fallback. Existing local playtest installations reuse their D-drive archive. Within the root, QuickSaves and AutoSaves each contain a separate folder per Steam account. The stock save files stay with the game.
- The signed DLSS runtime is downloaded directly from NVIDIA only as needed when the graphics switch is enabled. The helper verifies the pinned SHA-256, signature, version and size. A running game finishes with its current library, then the verified file is installed after exit. The toggle can cancel the pending change.

## Finding a failed operation

The overlay's **Diagnostics** section shows the log path. The current file is **Logs\\quicksaves.jsonl** under the selected data root. It uses one JSON object per line. The previous 2 MiB of history are kept in **quicksaves.previous.jsonl** when the log rolls over. Both files stay local. If the game stops before the tab appears, check **ReShade.log** in the game folder for the add-on's registration and executable compatibility messages.

To review a playtest, start at the time of the problem. Use the process ID and request number to follow one press across threads. The entry number follows the same save across restarts.

- **requested**, **busy_input** and **request_expired** distinguish an accepted keypress, a second press during an operation, and a request that could not reach a regular game update.
- **capture_begin**, **capture_complete** and **saved** mark the native writer and the disk commit. The last record includes the slot, thumbnail byte count and complete record checksum. The game can finish its capture before a disk error occurs. `Auto Save` in the detail field identifies the timed shelf.
- **load_begin**, **load_phase** and **world_loaded** show the selected world and native reader progress. The last record contains Sam's observed coordinates and distance from F5 when available. Its details include the difference from the native decoded coordinates and any world-position override.
- **order_comparison** and **cargo_count_comparison** report a match, mismatch or unavailable sample. **scene_result** reports observed clocks after loading. **scene_unconfirmed** records a cinematic without an identifiable saved clock. **request_failed** gives the specific error. A crash may leave the preceding stage as the last durable event.

The entry number, shelf, Steam account and digest identify a selected save when a numbered file is reused.

## Maintaining the project

- `src/addon.cpp` owns F5, thumbnails, the ReShade panel and the handoff to the game thread. `src/session.cpp` owns the operation from button press to confirmation. Disk work runs on a background thread.
- `src/game.cpp` checks the complete executable hash before enabling build-specific code. `src/private_capture.cpp` creates an independent writer job and waits for its completion. `src/private_load.cpp` gives the normal world reader a private image and retains it until every native reader releases it. See [capture](NATIVE_CAPTURE.md) and [restore](NATIVE_RESTORE.md) for engine ownership and recovery details.
- `src/cinematic.cpp` observes scene clocks and seeks only matching live scenes. The matching rules, movie behavior and remaining playtest questions are in [cutscene bookmarks](CINEMATIC_BOOKMARKS.md).
- `src/private_record.cpp` keeps the PNG, optional observations, scene clocks, and actual game bytes together. `src/library.cpp` verifies each selected file and publishes completed writes atomically. `src/thumbnail_image.cpp` and `src/thumbnails.cpp` handle images and GPU texture lifetime.
- `src/delivery_sample.cpp` reads order identifiers, per-order delivery counts, and the number of items the game's baggage manager lists as carried by Sam. `src/telemetry.cpp` owns the local log location, event format, flushing and rotation. Missing observations are recorded as unavailable.
- `vendor` contains the matching ReShade and ImGui headers and the MinHook x64 code with their licenses. Every native address applies to this one executable. Recheck the addresses, layouts, and ownership against a changed game build before changing the compatibility check.

- `src/quick_keys.cpp` owns rebinding, input capture and conflict handling. `src/save_browser.cpp` renders both shelves. `src/autosave.hpp` owns the gameplay timer.
- `src/dlss_install.cpp` implements the optional signed runtime download. `src/graphics_panel.cpp` connects it to the toggle. `src/dlaa.cpp` applies and observes the game's Quality settings locally.
- `src/data_root.cpp` selects the user's data directory. `make-release.ps1` emits a tested allowlist of public package files.

The earlier manual-slot bridge in `src/restore.cpp` and its historical recovery test can help identify artifacts from the withdrawn prototype. The current CMake target has no dependency on that code.

## Rebuild and verify

Use the x64 Visual Studio 2022 C++ build tools, a Windows SDK, and CMake. From this project folder on D, run the following in PowerShell.

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_RESHADE_ADDON=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\tests\thumbnail_test_graphics.ps1 -ReShadeDll 'J:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT\dxgi.dll'
~~~

Generate the public ZIP with **.\\make-release.ps1 -ReShadeDll <path-to-source-built-ReShade64.dll>**. Its six entries include the runtime, add-on, optional updater, instructions, and notices. Release inputs and sources are recorded in [packaging notes](release_assets/PACKAGING-SOURCES.md).

The release add-on is **build\\Release\\DirectorCutQuickSaves.addon64**. Use the entire ZIP for an installation, including the updater folder. The automated tests use generated files, simulated world jobs, and a separate Direct3D 12 window. Those tests cover ring rotation, integrity, private-buffer lifetime, load rollback, thumbnails, scene matching, order comparison inputs and local logging. A campaign playtest is required to establish which live world and cinematic state survives a round trip.

## Source download

Extract the source ZIP into a working folder and run the rebuild commands above. The source includes the mod, updater, tests, build files and the dependency headers needed to compile them. To recreate the player ZIP, take `dxgi.dll` from the matching player release or build the pinned upstream ReShade revision in the packaging notes. To export this source snapshot again from a Git checkout, run `git archive --format=zip --prefix=QuickSaves-0.2.0-Source/ -o "Death Stranding Directors Cut - QuickSaves 0.2.0 - Source.zip" v0.2.0`.
