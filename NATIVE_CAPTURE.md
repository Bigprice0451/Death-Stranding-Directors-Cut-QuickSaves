# Independent quicksave capture in Director's Cut

F5 captures the world into a 12 MiB buffer owned by the mod. It uses the game's serializer and job scheduler. The mod supplies its own request, controller, queue, and completion counter. The capture never asks the storage service to open a slot or rotate the game's autosaves. An isolated contract test exercises the implementation. Actual gameplay and cinematic continuity require an in-game round trip.

## Build identification

These observations apply to the 64-bit Steam executable for app 1850570, build 13300582, version 1.004.0, size 86,479,872 bytes. Its SHA-256 is `9d5306d7eb2f5d3135c63accc3e74b8f9b029f9d76112a6aae4a38fdbb4edd2b`. Addresses below are hexadecimal offsets from the loaded base of this executable. A game update requires another inspection.

## Game slots and the retry point

The game's exported script command `DSCreateSaveGame` at `365C560` puts a request on the game's save queue. The registered types are Manual (1), Quick (2), Automatic (4), and the combined filter All (15). The slot enum includes slots 0 through 22, the automatic selection value −1, and memory −2. The name resolver at `3635630` reads the built-in categories. The inspected game-facing requests carry a category and slot number. No private namespace or folder registration was identified in these paths.

The memory option has a gameplay purpose. `DSCreateSaveGameForRetry` at `365C5C0` requests an automatic save to memory −2. The game exposes separate commands to test for and clear its retry data. The clear operation at `3628F60` clears the header of the buffer referenced by controller offset `C8`. Reusing this point would make the mod and the game compete for the same retry state even if no file changed on disk.

## Where the game creates the snapshot

The ordinary save worker at `36297C0` receives a writer context in RCX and a request in RDX. The request field at `2C` chooses a buffer pointer in the context starting at `B8`. The worker clears a 512-byte header and builds a bounded memory stream at `3629A09`. The stream starts at buffer offset `200` and can hold `BFFE00` bytes. The full native buffer is 12 MiB.

The worker serializes several live game systems and the world. It writes a single-precision position at buffer offsets `1E0`, `1E4`, and `1E8`. It writes the payload length at `1FC`, computes a 16-byte digest of the payload at `1EC`, and sets the request state at `0C` to 3 when it finishes successfully. These offsets describe the in-memory snapshot. A Steam `checkpoint.dat` has an outer container with its own metadata and image.

The normal file path selection and disk write happen in other routines. F5 supplies its own memory destination, waits through completion, and returns exactly the header and the used payload. It verifies version, world ID, length, recorded position, activity header size, and payload MD5 before the library receives the bytes.

## How F5 schedules a private capture

1. The ordinary world frame returns through `263D2AE` and `263A869`. The mod requires both return addresses before dispatch. Other Windows message pumps also run during streaming and shutdown. The capture immediately refuses when the game's own load, save, pending request, or completion fence is busy. It also checks the player entity and the services the serializer dereferences. A refusal produces no image.
2. A private controller holds one request, one destination bank at `B8`, and a private job counter at `A8`. Only 36 bytes of world header metadata from offsets `108` through `12B` are copied from the live controller. The live queue, bank pointers, and reference counts never enter the private controller.
3. Build 13300582's dispatcher at `3633640` runs once in state 1. The memory-only marker −2 tells it to skip disk-slot setup. Because the request is on the private controller and its retry flag is clear, it chooses the mod's bank zero. It sets state 2 and constructs a native job. The request retains one owner reference through completion.
4. The game starts the job at `362DB80` and records completion against the private counter. The game joins at `362DCF0`, the routine labeled `SaveGameSystem::OnEndUpdateRendering`. The main frame cannot advance during the join. The mod keeps the complete request and destination alive until the join returns. It never advances the dispatcher to the next stage that would perform storage work. It checks the final request state, retained reference, and released job and fence before accepting the result.

`src/private_capture.cpp` owns this sequence. `src/native_image.cpp` owns format validation, shared by capture and restore.

## Capture boundaries

The worker reads the live world. At `3629CAB` it invokes a callback for each entry supplied by the live player manager. At `3629CFA` and `3629D9A`, it constructs and publishes an event identified by the executable as `TelemetrySaveGame`. This behavior is part of the native serializer. The effects of every game-specific callback during an extra capture have not been verified. The mod adds no process-wide file hook.

Manual and timed capture in 0.2.0 both require ordinary gameplay. The add-on checks scene and transition state before taking a preview and again before entering the native writer. Entering a cutscene or movie blocks both kinds of save. The native writer still needs an instantiated player, a stable world and its services. It decides which game systems contribute data to the record.

`tests/private_capture_test.cpp` verifies the frame gate, four kinds of overlapping native work, single-bank ownership, request lifetime, and the exact prepare/start/join sequence using a simulated scheduler. It also checks abort, oversized output and a bad native checksum. These tests exercise the add-on's decisions without starting a game or reading a player's save.

## Checking Sam's position

Two native accessors give an independent measurement. The exported `GetLocalDSPlayerEntity` at `2757A30` resolves the standard local player selector. In this build, read the player-manager pointer at `base + 7BC7568`, the local player pointer at `manager + 48`, and the entity pointer at `player + 70`. The exported entity position accessor at `2362AF0` reads three little-endian doubles from `entity + C8`. Use a fresh pointer walk for each sample. A missing manager, player, or entity during loading makes the position unavailable. For an external observer, two consecutive matching reads from the same entity reduce the risk of mixing two simulation updates.

Acceptance requires an F5 capture, movement to a distinct second position, and a successful private load back to the captured position with the same order and cargo. Compare the native save files and the game's retry point around the F5 transaction in an isolated profile. Verify a later ordinary autosave and a normal in-game load as separate operations. No independent end-to-end run of this kind is recorded here.

## What the delivery check measures

`src/delivery_sample.cpp` observes two different pieces of the live world. Either observation can be unavailable. It reads from the current process, takes two samples at the same game update, and accepts them only if the relevant values match. The observations do not issue game commands.

The first tracks active orders. The game's `CalcProgressMissionCountHeavy` at `33C8B10` iterates three tables under the mission manager at `7C0AFF8`. Entries have a 160-byte stride. The table pointers start at manager offset `108`. Each entry exposes its 64-bit mission ID at `38` through the registered `GetMissionId` accessor at `1926C20`. The 16-bit value at `2C` is the mission state. The diagnostic includes entries in state 4 (Progress) with a nonzero ID. The game's broader category counters also include Missing and Consign in one table.

For each active order, `DSMissionInfo.GetDeliveryCount` at `333CE40` looks up its mission ID in the delivery manager at `7C0AFF0`, table offset `816F8`. The lookup at `2F28680` scans 40-byte entries, matches the 64-bit ID at `10`, and uses the byte at `24` from the first match. A missing entry returns zero. The diagnostic reports the number of active entries, the sum of their delivery counters, and a SHA-256 fingerprint of the sorted list of table numbers, mission IDs and delivery counters. Reordering the list leaves the fingerprint unchanged. The count covers only orders still in progress.

The second measures the number of distinct bags currently carried by Sam. The game's `DsIsBaggageCarriedByPlayer1` at `2ED9190` reads the manager at `7BE7980`, then its player inventory at offset `20220`. Its enumerator at `2C53340` walks the inventory's groups. Each group contributes the items in its active stacks using `2C53640`. The game's membership test makes a cargo actor ID from each item's value at `D4` by retaining the lowest 13 bits and adding the cargo category bits `6000`. The diagnostic counts distinct IDs, checks for any owned inventory locks and never enters those locks. Count zero means it observed an empty collection. Missing managers, an active inventory update, an unreadable array or an excessive list size make the count unavailable. The actor IDs are indices and their stability across a load is unverified, so only the count is used for that comparison. Check an individual parcel's description, placement and damage in the game's menus if those details matter for a test.

## When an ordinary save overlaps F5

The native controller advances queued requests across game updates. Finishing an active write may require the game to run a later update. Waiting indefinitely in the F5 callback could prevent that update. Delaying F5 until the next update could also record a different position or delivery state. The mod checks both the native operation flags and the controller's pending request, job and fence. A busy controller returns a failure immediately and creates no quicksave. For a cinematic, the worker still needs the instantiated category-3 player, entity, handle, world save data and live game services. The player check is required because the worker dereferences the entity at `3629AA1` even when no position was available. A scene can therefore be active while the world data needed for a checkpoint is temporarily absent.
