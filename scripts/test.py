#!/usr/bin/env python3
"""Run the host regression modules affected by changed project files."""

import argparse
import ast
from fnmatch import fnmatchcase
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "scripts/tests"
WZH = "main/boards/common/wzh/"
BOARDS = "main/boards/wzh_*/"

# Each rule maps source paths to unittest module patterns. Overlap is intentional
# where a change crosses a subsystem boundary. Test module names omit '.py'.
RULES = (
    (
        ("scripts/build.py", "scripts/versions.py", "docker/*", "partitions/*",
         "sdkconfig.defaults*", "main/idf_component.yml", "CMakeLists.txt",
         "main/CMakeLists.txt", "main/Kconfig.projbuild", BOARDS + "config*.json",
         WZH + "wake_model_check.cmake"),
        ("test_build", "test_build_default_assets", "test_wzh_wake_model_check",
         "test_wzh_product_identity", "test_wzh_joint_voice", "test_official_asr_probe",
         "test_dino_keyword_barge", "test_voice_barge_in"),
    ),
    (
        ("scripts/build_default_assets.py", "scripts/gen_lang.py", "main/assets.*",
         "main/assets/locales/*", WZH + "wzh_tail_interrupt_phrases.inc"),
        ("test_build", "test_build_default_assets", "test_wzh_wake_model_check",
         "test_wzh_joint_voice", "test_wzh_onboarding_audio", "test_dino_keyword_barge"),
    ),
    (
        ("scripts/test.py", ".github/workflows/build.yml"),
        ("test_test_selection", "test_build"),
    ),
    (
        ("main/application.*", "main/device_state*", "main/boards/common/board.*",
         WZH + "wzh_conversation_*", WZH + "wzh_speech_intent.h"),
        ("test_activation_task_recovery", "test_wzh_application_interaction",
         "test_wzh_conversation_toggle", "test_wzh_ble_parity", "test_voice_barge_in",
         "test_dino_keyword_barge", "test_official_asr_probe", "test_voice_enrollment_command",
         "test_wzh_voice_preview_abort", "test_wzh_quiet_idle", "test_wzh_feedback",
         "test_wzh_product_readiness", "test_wzh_device_voice_enrollment",
         "test_wzh_miniprogram_voice_record", "test_firmware_runtime_optimizations"),
    ),
    (
        ("main/application.h", "main/boards/common/board.h"),
        ("test_wzh_onboarding_player", "test_wzh_shutdown_completion",
         "test_wzh_system_prompts", "test_wzh_voice_web"),
    ),
    (
        ("main/protocols/*", "main/ota.*", "main/assets.*", "main/mcp_server.*",
         WZH + "wzh_mcp_tools.*"),
        ("test_firmware_runtime_optimizations", "test_voice_barge_in",
         "test_wzh_application_interaction", "test_wzh_conversation_toggle",
         "test_wzh_ble_parity", "test_wzh_product_readiness"),
    ),
    (
        ("main/audio/audio_service.*", "main/audio/audio_playout_buffer.h",
         "main/audio/audio_source_gain.h", "main/audio/audio_codec.*", "main/audio/codecs/*",
         "main/audio/demuxer/*", "main/audio/fixed_queue.h", "main/audio/opus_stream_metadata.h",
         "main/audio/decoded_audio_stats.h", WZH + "wzh_audio_player.*",
         BOARDS + "*_audio.*", BOARDS + "audio_calibration.inc", "docs/wzh/data/*audio_calibration.json",
         "scripts/audio/build_*_audio_calibration.py"),
        ("test_audio_playout_buffer", "test_decoded_audio_stats",
         "test_es8311_output_gain", "test_firmware_runtime_optimizations",
         "test_product_audio_quality", "test_speaker_protection", "test_wzh_voice_tts_wav",
         "test_wzh_sound_scores", "test_wzh_user_pairs", "test_wzh_voice_capture_routing"),
    ),
    (
        ("main/audio/speaker_protection.h",),
        ("test_speaker_protection", "test_es8311_output_gain", "test_product_audio_quality"),
    ),
    (
        ("main/audio/audio_service.*", "main/audio/audio_playout_buffer.h",
         "main/audio/voice_enrollment.h", "main/audio/external_tts.h"),
        ("test_wzh_audio_enrollment_power", "test_wzh_application_interaction",
         "test_wzh_conversation_toggle"),
    ),
    (
        ("main/audio/audio_service.*", "main/audio/audio_engine.h", "main/audio/engines/*",
         "main/audio/wake_word*", "main/audio/echo_reference_guard.h", "main/audio/voice_barge_in.h"),
        ("test_afe_*", "test_neural_audio_frontend", "test_voice_barge_in",
         "test_dino_keyword_barge", "test_official_asr_probe", "test_wzh_joint_voice",
         "test_wzh_quiet_idle"),
    ),
    (
        ("main/audio/codecs/es8311_audio_codec.*", "main/audio/fixed_queue.h"),
        ("test_voice_barge_in",),
    ),
    (
        (BOARDS + "dinosaur_ml307_*", WZH + "voice_tts/voice_http.*",
         WZH + "voice_tts/voice_at_buffer.h"),
        ("test_wzh_dinosaur_ml307_http", "test_wzh_voice_at_buffer"),
    ),
    (
        ("main/audio/audio_debugger.*", "scripts/audio_debug*.py"),
        ("test_audio_debug_capture",),
    ),
    (
        (WZH + "voice_tts/*", WZH + "wzh_voice_services.*",
         "main/audio/voice_enrollment.h", "main/audio/external_tts.h"),
        ("test_wzh_ali_*", "test_wzh_voice_*", "test_voice_enrollment_command",
         "test_wzh_audio_enrollment_power", "test_wzh_device_voice_enrollment",
         "test_wzh_enrollment_psram_worker", "test_wzh_selection_memory_status"),
    ),
    (
        (WZH + "wzh_ble_frame.h", WZH + "wzh_ble_rx_stream.h"),
        ("test_wzh_ble_rx_stream", "test_wzh_ble_base64", "test_wzh_ble_parity"),
    ),
    (
        (WZH + "wzh_ble_service.*",),
        ("test_wzh_ble_*", "test_wzh_miniprogram_*", "test_miniprogram_device_lifecycle",
         "test_wzh_factory_reset_policy", "test_wzh_startup_notice",
         "test_wzh_selection_memory_status", "test_wzh_device_voice_enrollment",
         "test_wzh_onboarding_progress", "test_wzh_power_telemetry"),
    ),
    (
        ("wzh-miniprogram/*", WZH + "wzh_ble_service.*", WZH + "wzh_product.h"),
        ("test_miniprogram_device_lifecycle", "test_wzh_miniprogram_*", "test_wzh_ble_parity",
         "test_wzh_feedback", "test_wzh_product_identity"),
    ),
    (
        ("project_components/esp-wifi-connect/*", "components/esp-wifi-connect/*",
         "main/boards/common/wifi_board.*", "main/boards/common/blufi.*"),
        ("test_wzh_wifi_*", "test_wzh_miniprogram_wifi", "test_wzh_ble_parity",
         "test_wzh_portal_credentials", "test_wzh_portal_security", "test_wzh_onboarding_progress"),
    ),
    (
        (WZH + "wzh_web_*", WZH + "wzh_user_web*", WZH + "wzh_developer_session.h",
         "main/assets/wzh_console.html", "main/assets/wzh_ui/*", "scripts/pack_wzh_ui.py"),
        ("test_wzh_developer_console", "test_wzh_console_memory_*", "test_wzh_web_peer",
         "test_wzh_portal_security", "test_wzh_factory_reset_policy", "test_wzh_power_telemetry",
         "test_wzh_voice_web", "test_wzh_selection_memory_status"),
    ),
    (
        (WZH + "wzh_factory_reset_policy.h",),
        ("test_wzh_factory_reset_policy", "test_miniprogram_device_lifecycle"),
    ),
    (
        ("main/audio/audio_codec.*", WZH + "wzh_audio_store.*"),
        ("test_wzh_developer_console",),
    ),
    (
        ("project_components/esp-wifi-connect/include/wifi_manager.h",),
        ("test_wzh_onboarding_player",),
    ),
    (
        (WZH + "wzh_audio_hub.*", WZH + "wzh_audio_store.*", WZH + "wzh_flash*", WZH + "w25*",
         WZH + "wzh_mcp_tools.*", "main/settings.*"),
        ("test_wzh_product_readiness", "test_wzh_audio_parser_memory", "test_wzh_hub_lazy_task",
         "test_wzh_offline_psram_worker", "test_wzh_voice_flash_dispatcher",
         "test_wzh_console_memory_retry", "test_wzh_factory_reset_policy"),
    ),
    (
        (WZH + "wzh_audio_player.*",),
        ("test_wzh_audio_parser_memory", "test_wzh_feedback", "test_wzh_lively_motion",
         "test_wzh_offline_psram_worker", "test_wzh_quiet_idle"),
    ),
    (
        (WZH + "wzh_onboarding*", WZH + "wzh_system_prompt*", WZH + "wzh_startup*", WZH + "wzh_prompt_audio_gate.h",
         "scripts/cmake/*", "scripts/audio/*onboarding.py", "scripts/audio/prepare_*audio.py",
         "scripts/audio/prepare_wzh_system_prompts.py", "scripts/audio/build_wzh_conversation_sound.py",
         "main/assets/*prompt*", "main/assets/*onboarding*", "main/assets/wzh_system_voices/*",
         "main/assets/wzh_sounds/*", "main/assets/common/*", "docs/wzh/product/onboarding_voice_plan*"),
        ("test_wzh_onboarding_*", "test_wzh_system_prompts", "test_prepare_product_audio",
         "test_wzh_startup_greeting", "test_wzh_startup_notice", "test_wzh_feedback",
         "test_wzh_shutdown_completion", "test_miniprogram_device_lifecycle"),
    ),
    (
        (WZH + "wzh_pet_*", WZH + "wzh_motion_*", WZH + "wzh_*motion.h", WZH + "wzh_sound_*",
         WZH + "wzh_body_*", WZH + "wzh_interaction*", WZH + "wzh_user_pair*", WZH + "wzh_quiet_center.h",
         WZH + "wzh_posture_input.h", WZH + "wzh_touch_input.h", WZH + "wzh_tail_phrase.h",
         WZH + "wzh_servo_output.*", WZH + "wzh_sensors.*",
         BOARDS + "*_controller.*", BOARDS + "*_motion*.h", BOARDS + "wzh_user_pairs_catalog.inc",
         BOARDS + "assets/*", "scripts/motion/*", "scripts/audio/*_wzh_sound_scores.py",
         "scripts/audio/*wzh_choreography.py", "scripts/audio/import_wzh_user_pairs.py",
         "scripts/audio/audit_wzh_tail_balance.py", "scripts/audio/audit_wzh_vitality.py",
         "scripts/audio/build_wzh_motion_preview.py", "docs/wzh/data/reference_motion_manifest.json",
         "docs/wzh/data/sound_pack.json", "docs/wzh/data/user_pairs.json"),
        ("test_wzh_action_queue", "test_wzh_continuous_motion", "test_wzh_crawl_personality",
         "test_wzh_event_driven", "test_wzh_fluent_motion", "test_wzh_fluid_completion",
         "test_wzh_lively_motion", "test_wzh_motion_*", "test_wzh_offline_personality",
         "test_wzh_pet_behavior", "test_wzh_product_readiness", "test_wzh_quiet_idle",
         "test_wzh_reference_expression", "test_wzh_sound_*", "test_wzh_tail_balance",
         "test_wzh_user_pairs", "test_wzh_feedback", "test_wzh_protection"),
    ),
    (
        (WZH + "wzh_speech_intent.h", WZH + "wzh_startup_greeting.h"),
        ("test_wzh_event_driven",),
    ),
    (
        (WZH + "wzh_body_commands.h", WZH + "wzh_body_phrases.inc", WZH + "wzh_voice_motion.h"),
        ("test_wzh_joint_voice",),
    ),
    (
        (WZH + "wzh_sound_choreography.h", BOARDS + "*_controller.cc",
         BOARDS + "wzh_user_pairs_catalog.inc"),
        ("test_wzh_startup_greeting",),
    ),
    (
        (WZH + "wzh_sound_score.h",),
        ("test_product_audio_quality",),
    ),
    (
        (WZH + "wzh_power_startup.h",),
        ("test_wzh_user_pairs",),
    ),
    (
        (BOARDS + "*_power.*", BOARDS + "config.h", WZH + "wzh_*battery*", WZH + "wzh_idle_power.h",
         WZH + "wzh_power_*", WZH + "wzh_protection*", WZH + "wzh_shutdown*",
         "main/boards/common/adc_battery_monitor.*", "main/boards/common/*sleep_timer.*",
         "main/boards/common/*power*_timer.*"),
        ("test_wzh_battery_rest", "test_wzh_fast_battery_start", "test_wzh_power_telemetry",
         "test_wzh_protection", "test_wzh_shutdown_completion", "test_wzh_quiet_idle",
         "test_wzh_product_readiness", "test_wzh_audio_enrollment_power", "test_wzh_ble_parity",
         "test_wzh_startup_notice"),
    ),
    (
        (BOARDS + "wzh_*.cc", BOARDS + "config.h", "main/boards/common/board.*"),
        ("test_build", "test_es8311_output_gain", "test_wzh_product_identity",
         "test_wzh_action_queue", "test_wzh_feedback", "test_wzh_power_telemetry",
         "test_wzh_product_readiness", "test_wzh_startup_notice", "test_wzh_memory_profile",
         "test_wzh_device_voice_enrollment", "test_wzh_wifi_startup", "test_voice_barge_in"),
    ),
    (
        ("main/Kconfig.projbuild", BOARDS + "config*.json", WZH + "wzh_audio_hub.*",
         WZH + "wzh_web_console.*", WZH + "wzh_onboarding.*"),
        ("test_wzh_memory_profile", "test_wzh_console_memory_admission", "test_wzh_offline_psram_worker"),
    ),
)


def matches(value, patterns):
    return any(fnmatchcase(value, pattern) for pattern in patterns)


def git_paths(*args):
    data = subprocess.check_output(["git", "--no-optional-locks", *args], cwd=ROOT)
    return {os.fsdecode(path) for path in data.split(b"\0") if path}


def changed_paths(base):
    # Disabling rename detection returns both old and new paths for a move.
    tree = subprocess.check_output(
        ["git", "rev-parse", "--verify", "--end-of-options", f"{base}^{{tree}}"],
        cwd=ROOT, text=True,
    ).strip()
    tracked = git_paths("diff", "--name-only", "--no-renames", "-z", tree, "--")
    staged = git_paths("diff", "--cached", "--name-only", "--no-renames", "-z", tree, "--")
    untracked = git_paths("ls-files", "--others", "--exclude-standard", "-z")
    return tracked | staged | untracked


def add_importers(selected, test_paths):
    imports = {}
    python_paths = {path.stem: path for path in TESTS.glob("*.py")}
    python_paths.update(test_paths)
    for name, path in python_paths.items():
        try:
            tree = ast.parse(path.read_bytes(), filename=str(path))
        except SyntaxError:
            # Unittest reports syntax errors when it loads an affected module.
            continue
        dependencies = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                dependencies.update(alias.name for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                dependencies.add(node.module)
        imports[name] = dependencies
    while True:
        dependents = {name for name, dependencies in imports.items() if dependencies & selected}
        if dependents <= selected:
            return selected
        selected |= dependents


def select_tests(paths, test_paths):
    selected = set()
    changed_tests = set()
    unmapped = set()
    for path in paths:
        if Path(path).parent == Path("scripts/tests") and path.endswith(".py"):
            changed_tests.add(Path(path).stem)
            continue
        if path.endswith((".md", ".rst")) or path.startswith(("docs/wzh/archive/", "tmp/")):
            continue
        patterns = {pattern for sources, tests in RULES if matches(path, sources) for pattern in tests}
        if patterns:
            selected.update(name for name in test_paths if matches(name, patterns))
        elif not path.startswith("docs/"):
            unmapped.add(path)
    if changed_tests:
        selected.update(add_importers(changed_tests, test_paths) & test_paths.keys())
    return sorted(selected), sorted(unmapped)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--base", help="Git revision to compare with the worktree (default HEAD)")
    scope.add_argument("--all", action="store_true", help="explicitly run the complete suite")
    parser.add_argument("--list", action="store_true", help="show modules without running them")
    parser.add_argument("paths", nargs="*", help="project-relative paths to use instead of Git changes")
    args = parser.parse_args()
    if args.paths and (args.all or args.base):
        parser.error("choose explicit paths, --base or --all")
    os.chdir(ROOT)
    test_paths = {path.stem: path for path in TESTS.glob("test_*.py")}
    if args.all:
        selected, unmapped = sorted(test_paths), []
    else:
        try:
            paths = {Path(path).as_posix() for path in args.paths} if args.paths else changed_paths(args.base or "HEAD")
        except subprocess.CalledProcessError:
            parser.error("unable to read Git changes; check --base or pass project-relative paths")
        if any(Path(path).is_absolute() or ".." in Path(path).parts for path in paths):
            parser.error("paths must be relative to the project root")
        selected, unmapped = select_tests(paths, test_paths)
    for path in unmapped:
        print(f"No host test mapping: {path}", file=sys.stderr)
    print("\n".join([f"Selected {len(selected)} host test modules.", *selected]), flush=True)
    if args.list or not selected:
        return 0

    sys.dont_write_bytecode = True
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    sys.path.insert(0, str(TESTS))
    suite = unittest.defaultTestLoader.loadTestsFromNames(selected)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
