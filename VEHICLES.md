# Vehicle state in private saves

These details apply to the fingerprinted Steam build 13300582. Both Quick Save and Auto Save run the same code.

## What the player sees

When the game captures the world, the add-on samples the loaded motorbike, truck and roadster entities within 100 world units of Sam. The vehicle he is riding is included regardless of distance. The game records vehicle identity, transform, life and power in its vehicle manager. The add-on compares that row with the visible vehicle and corrects a stale position or rotation in the private copy. The game handles cargo, physics, ownership and spawning through its original load sequence.

A missing native row, a vehicle undergoing removal, or an authored return-to-position rule requires different treatment. Missing or unrecognizable data is reported in Diagnostics. An authored rule keeps its game-defined behavior. The game can reattach Sam to a restored bike or truck. Its load routine clears the ride request for a roadster. No duplicate vehicle is spawned to simulate a successful reload.

## Where the values come from

All offsets below are hexadecimal relative to the supported executable. The ordinary vehicle update at 2F97CF0 gets its active entity through actor +68 and copies the world pose from entity +C8. The four unit pointers used by the native vehicle writer begin at vehicle manager +28. The current physical map is obtained by 262A860. Separately, 26E9180 reads the rider's actor ID. 1FFFFF means on foot. World and actor IDs alone can be reused, so capture correction also requires the unique persistent vehicle identifier and exactly one matching row on each side.

The private worker invokes the vehicle manager writer at 2FA65E0. It emits an indexed manager section with per-world and per-group lengths. Each active record has 74 hex bytes. The native pose occupies bytes 10 through 2B, with three floats followed by a four-float quaternion. 2F7CF20 obtains the position and uses 19C44D0 to convert the axes. Vehicle initialization at 2F70960 uses 2FBF610 to decode the same row. The independent social tracking system can keep the last parked location while Sam rides. It serves a different purpose and is never used to correct placement.

## Maintaining or debugging it

- `src/vehicle_live.cpp` samples the instantiated vehicles on the game thread only at capture and after loading. It follows the four unit arrays, checks array lengths and exact vehicle classes and follows the same entity link as the native update. It also reads the engine's life, current battery and capacity fields.
- `src/vehicle_archive.cpp` checks the native footer, section boundaries, group sizes and individual rows. It returns distinct results for a readable empty section, missing data, ambiguity and an unsupported format.
- `src/vehicle_check.cpp` adjusts only newly captured, uniquely matched private records with ordinary placement. It skips removal and authored rows. Only the 28 pose bytes and the native payload MD5 change. The normal archive writer then includes the result in the slot's SHA-256. Loading an existing file passes its original world image to the game.
- Vehicle cargo verification reads each available inventory twice and requires matching package IDs. The combined digest includes the vehicle identity, so moving a package between vehicles changes the digest. This observes package membership. Individual package damage and physics velocity are not independently verified. Optional seat and cargo fields in the outer slot description preserve compatibility with older saves.
- `vehicle_capture` logs observed vehicles, matched rows, adjusted poses, authored exceptions and unmatched identities. `vehicle_after_load` reports the number found, maximum position and orientation differences, changes in life or battery and the observed seat. A short retry allows for delayed bike or truck reattachment. `vehicle_cargo_after_load` compares the cargo signatures when available. These are observations after the world resumes, so movement or power use can affect a comparison.

The regression tests exercise multiple vehicles and worlds, a rider beyond the normal radius, changed records, malformed snapshots, missing or duplicated identities, item replacement, and exact-byte loading of older images. The native pose decoder was also exercised outside the game against 133 existing bike snapshots on the development machine. A campaign playtest is still needed to observe post-load physics, seat attachment and cargo in a particular mission.
