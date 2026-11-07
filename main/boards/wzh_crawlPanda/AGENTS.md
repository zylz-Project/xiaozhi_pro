# AGENTS.md — WZH Crawl Panda

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

These instructions apply to `main/boards/wzh_crawlPanda/`.

- Product: crawling panda with one head servo and two front-arm servos.
- Board directory: `wzh_crawlPanda` (case-sensitive).
- Build/reported type: `wzh-crawlpanda`.
- Kconfig selector: `CONFIG_BOARD_TYPE_WZH_CRAWLPANDA`.
- Chip target: ESP32-S3.
- Board factory: `DECLARE_BOARD(WzhCrawlPanda)` in `wzh_crawlPanda.cc`. Keep exactly one factory in a build.
- Product metadata lives in `wzh_product.cc`; changing identity, AP prefix, or Audio Hub product ID affects provisioning, OTA compatibility, and cloud association.

Read `README.md`, `config.h`, `wzh_crawlPanda.cc`, and the relevant controller/audio/power pair before editing. The repository-root `AGENTS.md` also applies.

## Ownership map

- `wzh_crawlPanda.cc`: board composition, initialization order, `NoDisplay` emotion bridge, state listeners, buttons, ES8311, MCP and Audio Hub registration.
- `panda_controller.*`: all poses, clips, behaviour families, autonomous scheduling, audio-reactive motion, emotion mapping, body commands, and the `panda_motion` task.
- `panda_audio.*`: crawling-panda action-to-sound selection only.
- `panda_power.*`: power latch, ADC button, battery estimate, and shutdown sequence.
- `config.h`: this physical product's pins, servo directions/defaults/limits, audio bus, power thresholds, and AP prefix.
- `config.json`: build identity and board-specific sdkconfig additions.
- `wzh_product.cc`: adapter used by `main/boards/common/wzh/`.
- `main/boards/common/wzh/`: shared Flash abstraction, audio store/player, Audio Hub, and audio MCP tools. A change there affects all three WZH products and must be validated on all three.

## Hardware contract

- ES8311 uses I2C0 and the single XiaoZhi `AudioService` path. Never install a second I2S driver or decoder for offline sound.
- Servos use 50 Hz LEDC PWM and are ordered `head`, `left_arm`, `right_arm`.
- Head: GPIO17, `0=right`, `90=front`, `180=left`.
- Left arm: GPIO15; crawling direction decreases angle (`180 -> 0`).
- Right arm: GPIO16; crawling direction increases angle (`0 -> 180`).
- Software defaults are `90/90/90`; the user's 2026-10-21 request narrows head yaw to `30..150` (front `90`, +/-60); arms remain `8..172`. Apply the head range to every motion source and final output. Treat these as installation-specific values.
- Servo power is GPIO4. Power latch is GPIO7; analog power key is GPIO6/ADC1 channel 5; battery divider is GPIO3/ADC1 channel 2.
- External audio Flash uses SPI2 on CS10/CLK9/MOSI47/MISO21. The shared layer owns JEDEC detection, PNDA legacy compatibility, NAND bad-block handling, and writes.

Never change these pins or mechanical meanings to support another assembly. Add a new board identity or variant instead. Never infer physical left/right from another WZH product.

## Runtime invariants

- Constructor order matters: set AP prefix, initialize I2C, power, local audio, controller, shutdown hook, TTS level listener, MCP/Hub tools, state listener, then buttons.
- “Online” means an active conversation state, not merely connected Wi-Fi. Entering conversation stops local playback; idle resumes autonomous motion and sound.
- Configuration, activation, and upgrade states intentionally suppress autonomous sound/motion.
- Network, display, AudioService, and MCP callbacks must only enqueue or update atomic/lightweight state. Keep interpolation and delays in `panda_motion`.
- Preserve bounded queues. Do not block the main loop or audio tasks with servo sequences, Flash operations, downloads, or erases.
- All output angles must pass through the controller's final clamping/output path.
- Shutdown order is local audio stop, default pose, servo power off, then power latch release.

The controller uses `Pose(head, left_arm, right_arm)`, keyframe clips, behaviour families, shuffle bags, mirroring, and 20 ms updates. When editing motion:

- Preserve opposite arm direction semantics and verify crawling force does not fight the mechanism.
- Directional commands (`look_left`, `look_right`) must not be randomly mirrored.
- Keep clip IDs, `kClips`, `kFamily`, `Behaviour::kCount`, body clip IDs, and lookup functions aligned.
- Avoid short alternating frames, excessive audio-envelope gain, or large idle tremor that can buzz the servos.

## Public behaviour and data contracts

- MCP product tools use `self.panda.*`. Audio and Hub tools are also registered with prefix `panda`.
- If an action name changes, update `PandaAction`, `ParseAction`, `ActionName`, `EmotionToAction`, MCP descriptions, keyword fallback, audio selection, and `README.md` together.
- If a body action changes, update `PandaBodyAction`, body clip mapping, parser/name functions, keyword rules, and MCP descriptions together.
- `neutral`/`normal` are reset signals and must not trigger a performance.
- Audio entries are OGG/Opus with `animal` or `ambient` category. Use `AudioStore::Snapshot()` for selection while the Hub may update the directory.
- NVS namespaces, PNDA/WZH3 formats, product IDs, and MCP names are persistent/external APIs. Add migration or backward compatibility when changing them.

## Validation

Use ESP-IDF 6.0.2 when available, then run:

```sh
python3 -m unittest discover -s scripts/tests -v
python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda
clang-format -i path/to/touched.cc path/to/touched.h
clang-format --dry-run -Werror path/to/touched.cc path/to/touched.h
```

For changes under `main/boards/common/wzh/`, also build `wzh-dinosaur` and `wzh-tailredpanda`.

Hardware validation must cover head direction, both arm directions, crawling traction, mechanical limits/collisions, servo-current resets, power-key shutdown, battery reading, ES8311 capture/playback, local Flash sound, online TTS motion, conversation transitions, and Audio Hub sync. A successful build is not hardware validation.
