# Cutscene and movie bookmarks

Version 0.2.0 uses scene observations to block both capture methods and loading while a cutscene is playing. The older 0.1.0 format could store playback clocks. Those existing files can still be selected after returning to gameplay. The compatibility reader looks for the matching live scene and records any confirmed clocks in `scene_result`. New entries contain no scene clocks.

**An exact frame match describes the timeline or decoder.** Quest changes, one-time dialogue, audio mixing, child actors, and interactions within individual scenes still need an actual save, move forward, reload, and play-through test. The bookmark cannot start a movie or sequence that the restored game did not bring back.

## What the add-on recognizes

- An engine sequence must be playing, have no parent sequence, have its authored **IsCinematic** flag set, and belong to a cinematic game state. Its identity combines the live entity UUID and the sequence resource UUID. Its frame must fit the authored **StopFrame**. This prevents seeking a different scene that happens to share a frame number.
- A Bink movie must have active, unpaused playback and an open decoder. Its identity combines the exact filename sent to BinkOpen with its width, height, frame count and playback end. A different clip or language track with a different filename receives a different identity.
- The observer retains clocks for 150 milliseconds and keeps no saved pointer to a game object. The ReShade F5 callback reads the copied clocks. It never follows an engine pointer on the render thread.
- After loading, the add-on requires two observations of the same active owner. It uses the game's own jump function on the normal world thread for sequences, or its native seek wrapper during the matching movie's next update. The movie wrapper handles its pending asynchronous decode work. Frame one can only match when the game itself reaches it.
- The game can jump forward or backward. A requested seek is submitted once per matching identity. On another native update, the add-on must observe the requested frame or its immediate successor before it confirms continued playback. A different keyframe, a different resource, an out-of-range target, a simultaneously ambiguous movie, or a scene that never returns cannot produce a successful seek result. After six seconds unresolved clocks are reported as unconfirmed. A captured frame already reached naturally needs no seek.

## Maintaining this build

The public ReShade API supplies the interface and thumbnail. The cinematic bindings are private to Director's Cut Steam build **13300582**. The executable fingerprint is checked in `game.cpp`. Do not carry the RVAs across an update. The source also compares native entry instructions before installing either callback.

| Native entry, in hexadecimal | Evidence in the shipped game |
| --- | --- |
| 024ABBA0 | Sequence pre-update receives the live sequence, calls its base implementation and copies the next frame at +37C to the current frame at +378. A playing root has +341 set and no parent at +450. |
| 01DDE990 | Resolves the resource handle at sequence +58. The SequenceResource reflection table identifies IsCinematic at +1F5 and integer StopFrame at +158. Both the entity and its resource inherit ObjectUUID at +8. |
| 024A7E20 | The authored JumpEvent calls this with the target frame, blend and jump mode. Mode zero is a normal jump in either direction. The function updates both clocks, adjusts active event instances and processes child sequences. Mode one belongs to a return-label event. |
| 019C9E00 | Updates the live movie. It checks the application frame counter and active, pause and stop flags before working with the decoder at +248. Decoder offsets 0, 4, 8 and C hold width, height, frame count and decoder frame. |
| 019CB270 | Native movie seek waits for outstanding decode work, passes the frame to BinkGoto and restarts decode. It ignores frames below two. The open routine at 019CBA70 passes the player's filename at +268 to BinkOpen. |

Both callbacks first or subsequently run their original native function, preserving its arguments and the movie function's return value. MinHook is built into the add-on from the bundled x64 source; its license is in `vendor/minhook/LICENSE.txt`. The add-on disables these two callbacks when ReShade closes its runtime. The small native trampolines and add-on module stay resident until the game exits so a thread already returning from a callback cannot execute unloaded memory.

The automated scene tests use synthetic identities to verify matching, direction, bounds, confirmation, duplicate suppression and timeouts. For a new executable or a newly claimed scene, test at least two saved points inside the same scene. For each, check the matched clock in the log, watch the scene through its exit, and compare the next order objective, cargo and dialogue against an uninterrupted run.
