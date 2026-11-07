# AGENTS.md — WZH Dinosaur

## Current speech / centered standby / developer policy (2026-10-19)

The latest user request requires neutral standby when no audio or requested
performance owns the motion lane. Return smoothly from the last commanded PWM
pose, including voice offsets, then settle and release; do not latch an
interrupted logical waypoint or keep an unbounded holding current. Protections
always release immediately. Keep this work in the motion task. See
`docs/wzh_dialog_developer_2026-10-19.md`.

Assistant text only supplies a bounded style hint; actual speaker PCM owns voice
energy, stresses, pauses and tail completion. Never execute controls from quoted
assistant text. Keep original XiaoZhi cloud transport and the double-click toggle.
The developer page uses the user-requested password 1 and server-checked expiring
tokens. External-audio Flash erase requires ERASE, exclusive maintenance and a
quiescent reader; never erase internal firmware/provisioning through this action.


## Current conversation-button policy (2026-10-19)

Power-key and BOOT-key double clicks use Application::ToggleConversation():
enter from Idle and fully exit from Connecting/Listening/Speaking. Single click
must not toggle chat in normal operation; keep BOOT provisioning and power-key
long-press shutdown. Prompt ordering, cancellation and stale-session fencing are
owned by Application/AudioService, never by the power polling task. See
`docs/wzh_double_click_conversation_2026-10-19.md`.

## Current exhibition policy (2026-10-19)

The user's latest explicit request supersedes the historical idle-quiet policy:
normal offline operation plays audio-first paired motion after 9–17 seconds of
quiet, measured from completion. Touch/voice/explicit commands immediately take
priority over an autonomous show. Keep stop/rest/quiet, conversation, maintenance,
and protection gates. Pandas exclude the 158 s ambience from automatic selection;
manual playback remains available. Dinosaur automatic ambience is a 12 s excerpt.
Use measured sound scores and the fair coverage selector; do not restore byte-
progress timing or compressed-packet-size loudness estimates. Every sound uses
its own duration/content score and eight phrase variations (dedicated sky-roar
paths remain in use). The latest all-product enrichment is documented in
`docs/wzh_lively_motion_2026-10-19.md`. See
`docs/wzh_audio_choreography_2026-10-19.md` for the current product contract.

## Scope and identity

These instructions apply to `main/boards/wzh_dinosaur/`.

- Product: five-axis dinosaur with articulated neck, head, and tail.
- Board directory: `wzh_dinosaur`.
- Build/reported type: `wzh-dinosaur`.
- Kconfig selector: `CONFIG_BOARD_TYPE_WZH_DINOSAUR`.
- Chip target: ESP32-S3.
- Board factory: `DECLARE_BOARD(WzhDinosaur)` in `wzh_dinosaur.cc`. Keep exactly one factory in a build.
- Product metadata lives in `wzh_product.cc`; changing identity, AP prefix, or Audio Hub product ID affects provisioning, OTA compatibility, and cloud association.

Read `README.md`, `CURRENT_STATUS.md`, `config.h`, `wzh_dinosaur.cc`, and the relevant controller/audio/power pair before editing. Use `XIAOZHI_RUNTIME.md` for the detailed event flow. The repository-root `AGENTS.md` also applies.

## Ownership map

- `wzh_dinosaur.cc`: board composition, initialization order, `NoDisplay` emotion bridge, state listeners, buttons, ES8311, MCP and Audio Hub registration.
- `dinosaur_controller.*`: all poses, keyframes, easing, autonomous scheduling, audio-reactive motion, emotion mapping, body commands, and the `dino_motion` task.
- `dinosaur_audio.*`: dinosaur action-to-sound selection only.
- `dinosaur_power.*`: power latch, ADC button, battery estimate, and shutdown sequence.
- `config.h`: this physical product's pins, servo defaults/limits, audio bus, power thresholds, and AP prefix.
- `config.json`: build identity and board-specific sdkconfig additions.
- `wzh_product.cc`: adapter used by `main/boards/common/wzh/`.
- `main/boards/common/wzh/`: shared Flash abstraction, audio store/player, Audio Hub, and audio MCP tools. A change there affects all three WZH products and must be validated on all three.

## Hardware contract

- ES8311 uses I2C0 and the single XiaoZhi `AudioService` path. Never install a second I2S driver or decoder for offline sound.
- Servos use 50 Hz LEDC PWM and are ordered `neck_tilt`, `neck_lean`, `head_turn`, `tail_ud`, `tail_lr`.
- GPIOs in that order are 17, 16, 15, 8, and 18. Servo power is GPIO4.
- Default pose is `70/90/90/90/90`.
- The user confirmed all five physical axis directions on 2026-09-13. Do not reinterpret IO17 as a lateral axis. IO17=0 is skyward roaring; IO17=180 is ground-level feeding. IO8's theoretical 0 is down but is prohibited by the 55-degree minimum.
- Current physical semantics: neck tilt `0=highest`, `70=default`, `180=lowest`; neck lean and head turn use `0=right`, `90=center`, `180=left`; tail-LR uses the opposite sense, `0=left`, `90=center`, `180=right`; tail-UD uses `55=lowest`, `90=default`, `180=highest`.
- Tail-UD has explicit `55..180` board limits. Other axes are finally clamped to `0..180`; real mechanical limits may be narrower and must be measured on hardware.
- Power latch is GPIO7; analog power key is GPIO6/ADC1 channel 5; battery divider is GPIO3/ADC1 channel 2.
- External audio Flash uses SPI2 on CS10/CLK9/MOSI47/MISO21. The shared layer owns JEDEC NOR/NAND detection, PNDA legacy compatibility, bad-block handling, and writes.

Never change these pins or axis meanings to support another dinosaur assembly. Add a unique board identity or variant instead. Do not copy three-axis panda poses into this five-axis engine.

## Motion character and runtime invariants

The intended product character is expressive, elegant, and animal-like—not fast or continuously busy:

- Preserve the product's original trigger cadence: autonomous performances start about 5–13 seconds after the previous one completes, and idle pulses recur about 5–14 seconds after the previous pulse completes.
- Full performances may use broad, expressive travel; control harshness through slower timing and smooth easing rather than globally shrinking every pose.
- Idle breathing must be clearly visible but slow. Do not add fast harmonics or drive servos directly from per-syllable audio attack spikes.
- Default idle behaviour is center-biased: each breathing cycle returns to the board default and pauses there; occasional offset poses are brief and followed by a longer centered dwell.
- Every moving keyframe must pass through the common duration limiter: at least 420 ms and no more than about 80 degrees/second average travel on the fastest-moving axis.
- Preserve the AudioService output-level callback into `FeedOutputAudioLevel()`. Real speaker PCM from online TTS, local Flash audio, and system prompts must all drive the same smooth multi-axis voice-motion layer.
- Offline autonomous performance is Flash-first: select the actual `animal` sound, classify it by filename/duration, wait briefly for that exact file to enter playback, then start its matched motion. Preserve the three short-Flash clips per enum category, per-family no-repeat selection, and stretch long-audio choreography only toward slower timing.
- The library currently contains 24 full clips, 24 general short-Flash clips, and seven dedicated sky-roar variants. Add spatially distinct stories instead of near-duplicate angle lists; never modify an old clip merely to increase the count.
- All axes that change within one keyframe share one interpolation progress and must start and arrive together. Do not add artificial head/neck/tail start delays.
- Preserve smooth action starts, direction changes, deliberate holds, and final stops. Consecutive same-direction `kSmooth` frames use clamped monotone Hermite tangents so motion flows through intermediate poses without stopping or overshooting; historical `kFastOut` frames may use only a mild early-arrival blend and remain subject to the same duration limiter.
- Every sound whose name contains `叫声音效合集`, plus sounds named `怒吼`, `长啸`, `咆哮`, or `roar`, uses the seven-clip sky-roar library regardless of number. Every variant must visibly raise neck tilt to `0..4` degrees before lateral detail; duration selects front, quick-answer, side-rise, high-sweep, or echo candidates. Disable random mirroring and lateral audio/living offsets, but retain each clip's limited, balanced authored lateral movement so roars remain varied rather than identical.
- Idle life pulses include glance, tail, perk, sniff, curious, greeting, double-take, and stretch. Keep them synchronized, slow, self-contained, returning toward center, and avoid selecting the same pulse twice in succession.
- The action queue intentionally keeps only the latest pending intent, and repeated actions are coalesced to prevent back-to-back performances.
- Keep 50 Hz servo PWM. Motion-target refresh rate and PWM carrier rate are different concepts; do not lower PWM frequency to make motion slower.

Additional invariants:

- `stories-v4` adds `affection-check-in`, `listen-confirm`, `roar-soft-answer`, and `roar-crown-arc`. Clamp resolved keyframe targets before interpolation as well as at final output. The living-offset envelope has zero endpoint slopes and duration-based attenuation below 900 ms; avoid reintroducing quick per-frame wiggles. Preserve short/long roar candidate separation and no-repeat selection.
- New full stories include `discover-canopy`, `happy-invite`, and `eat-graze`. Keep their physical-pose compile-time checks. Explicit 0/180 keyframes retain endpoints under amplitude variation; speaker-driven neck lift fades smoothly across tilt 150..180 to preserve ground feeding. Combination clearance still requires hardware validation.
- Roar revision `roar-onset-v3`: every roar's first frame is neck tilt 0 with all lateral axes at 90, about 900 ms from default under the shared speed limiter. Only subsequent content is stretched for long audio; onset and final centering are not stretched. Preserve the compile-time roar-pose checks and final-output `Roar pose` diagnostics.
- `self.dinosaur.test_servo(gpio)` is an explicit, queued assembly diagnostic for GPIO 17/16/15/8/18. Disable audio/living overlays while testing, use modest travel, and return to default. Never run it automatically. Reference GPIO definitions alone do not prove the direction of the current physical assembly.

- `kKeep` means an axis inherits the previous keyframe. It must not create artificial axis lag inside a keyframe; every changing axis in that frame shares one interpolation progress.
- Legacy `auto_run_data.cc` reverses only its internal `tail_ud`: convert with `physical_tail_ud = 180 - legacy_tail_ud`. Do not reverse neck tilt or any lateral axis; current keyframes always store direct physical servo angles.
- Audio and living offsets are output-only layers: never write them back into the logical base pose. Mirroring applies only when resolving a newly specified lateral-axis target; never mirror an inherited `kKeep` value twice.
- Mirroring may affect `neck_lean`, `head_turn`, and `tail_lr`; directional body commands must never be mirrored.
- All requested poses must pass through the final clamp/output path.
- Network, display, AudioService, and MCP callbacks only enqueue or update atomic/lightweight state. All interpolation/delays belong in `dino_motion`.
- Preserve bounded queues and never block the main loop or audio tasks with motion, Flash access, downloads, or erases.
- “Online” means an active conversation state, not merely connected Wi-Fi. Entering conversation stops local playback; idle resumes autonomous behaviour.
- Configuration, activation, and upgrade states intentionally suppress autonomous sound/motion.
- Shutdown order is local audio stop, default pose, servo power off, then power latch release.

When changing clips, keep action/clip arrays and enum indexes aligned. Estimate total duration and maximum per-axis travel, check all mirrored poses, and retain an unhurried return to the default pose.

## Public behaviour and data contracts

- MCP product tools use `self.dinosaur.*`. Audio and Hub tools are registered with prefix `dinosaur`.
- If an action name changes, update `DinoAction`, `ParseAction`, `ActionName`, `EmotionToAction`, MCP descriptions, keyword fallback, audio selection, and product documentation together.
- If a body action changes, update `DinoBodyAction`, `kBodyClips`, parser/name functions, keyword rules, and MCP descriptions together.
- `neutral`/`normal` are reset signals and must not trigger a performance.
- Audio entries are OGG/Opus. Dinosaur action selection excludes `ambient` entries and falls back to another non-ambient entry.
- Use `AudioStore::Snapshot()` for selection while the Hub may update the directory.
- NVS namespaces, PNDA/WZH3 formats, product IDs, and MCP names are persistent/external APIs. Add migration or backward compatibility when changing them.
- Keep `CURRENT_STATUS.md`, `XIAOZHI_RUNTIME.md`, and its HTML counterpart consistent when runtime timing or behaviour changes.

## Validation

Use ESP-IDF 6.0.2 when available, then run:

```sh
python3 -m unittest discover -s scripts/tests -v
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur
clang-format -i path/to/touched.cc path/to/touched.h
clang-format --dry-run -Werror path/to/touched.cc path/to/touched.h
```

For changes under `main/boards/common/wzh/`, also build `wzh-crawlpanda` and `wzh-tailredpanda`.

Hardware validation must cover all five servo directions and limits, mirrored clips, collision/current spikes, idle noise, power-key shutdown, battery reading, ES8311 capture/playback, NOR/NAND local sound, online TTS motion, conversation transitions, and Audio Hub sync. A successful build is not hardware validation.
