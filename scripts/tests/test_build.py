import importlib.util
import json
import os
import contextlib
import io
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)


class VersionTests(unittest.TestCase):
    def test_parse_and_match(self):
        self.assertEqual(build._parse_version("ESP-IDF v6.0.1"), (6, 0, 1))
        self.assertTrue(build._version_matches((5, 5, 4), "<6.0"))
        self.assertTrue(build._version_matches((6, 0, 1), ">=6.0"))
        with self.assertRaises(ValueError):
            build._version_matches((6, 0, 1), "~=6.0")

    def test_only_the_three_wzh_products_remain_buildable(self):
        # This fork keeps only the three WZH toys; the upstream board matrix was
        # deliberately removed. This pins that fact and the invariants that still
        # matter: unique names, a resolvable Kconfig symbol, and a CMake branch.
        for idf_version in ((5, 5, 4), (6, 0, 1), (6, 1, 0)):
            variants = build._collect_variants(idf_version=idf_version)
            names = [variant["full_name"] for variant in variants]
            self.assertEqual(len(names), len(set(names)), idf_version)

        idf6 = build._collect_variants(idf_version=(6, 0, 1))
        boards = {variant["board"] for variant in idf6}
        self.assertEqual(
            boards,
            {"wzh_crawlPanda", "wzh_dinosaur", "wzh_tailRedPanda"},
        )
        idf6_names = {variant["full_name"] for variant in idf6}
        for expected in (
            "wzh-crawlpanda",
            "wzh-crawlpanda-barge-in",
            "wzh-dinosaur",
            "wzh-dinosaur-barge-in",
            "wzh-tailredpanda",
            "wzh-tailredpanda-barge-in",
        ):
            self.assertIn(expected, idf6_names)
        self.assertEqual(
            {variant["type"] for variant in idf6},
            {"wzh-crawlpanda", "wzh-dinosaur", "wzh-tailredpanda"},
        )
        for variant in idf6:
            self.assertEqual(variant["target"], "esp32s3")
            self.assertTrue(
                variant["config"].startswith("CONFIG_BOARD_TYPE_WZH_"),
                variant,
            )

        for config_path in (ROOT / "main/boards").rglob("config.json"):
            config = json.loads(config_path.read_text(encoding="utf-8"))
            self.assertEqual(build._get_reported_type(config), config["type"])
            board = config_path.parent.relative_to(ROOT / "main/boards").as_posix()
            self.assertTrue(build._board_type_exists(board), config_path)
        # Only the three product directories plus the shared layer survive.
        board_dirs = sorted(
            path.name for path in (ROOT / "main/boards").iterdir() if path.is_dir()
        )
        self.assertEqual(
            board_dirs,
            ["common", "wzh_crawlPanda", "wzh_dinosaur", "wzh_tailRedPanda"],
        )
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('BOARD_TYPE MATCHES "^[a-z0-9.-]+$"', cmake)
        self.assertIn('BOARD_NAME MATCHES "^[a-z0-9.-]+$"', cmake)
        self.assertNotIn("CONFIG_BOARD_TYPE_BREAD_COMPACT", cmake)

    def test_reported_types_and_names_are_valid_and_unique(self):
        type_owners = {}
        name_owners = {}
        pair_owners = {}

        for config_path in sorted(
            (ROOT / "main/boards").rglob("config*.json")
        ):
            config = json.loads(config_path.read_text(encoding="utf-8"))
            board = config_path.parent.relative_to(
                ROOT / "main/boards"
            ).as_posix()
            board_type = build._get_reported_type(config)

            previous_board = type_owners.setdefault(board_type, board)
            self.assertEqual(
                previous_board,
                board,
                f"BOARD_TYPE {board_type!r} is used by multiple boards",
            )

            for build_config in config.get("builds", []):
                board_name = build._get_reported_name(build_config)
                self.assertNotIn(
                    board_name,
                    name_owners,
                    f"BOARD_NAME {board_name!r} is used by "
                    f"{name_owners.get(board_name)} and {config_path}",
                )
                name_owners[board_name] = config_path

                pair = (board_type, board_name)
                self.assertNotIn(
                    pair,
                    pair_owners,
                    f"reported board identity {pair!r} is duplicated",
                )
                pair_owners[pair] = config_path

    def test_language_and_wake_word_are_not_board_config_options(self):
        for config_path in sorted(
            (ROOT / "main/boards").rglob("config*.json")
        ):
            config = json.loads(config_path.read_text(encoding="utf-8"))
            for build_config in config.get("builds", []):
                for option in build_config.get("sdkconfig_append", []):
                    self.assertFalse(
                        option.startswith("CONFIG_LANGUAGE_")
                        or option.startswith("CONFIG_SR_WN_"),
                        f"{config_path}: configure language and wake word "
                        f"through menuconfig or build parameters, not {option}",
                    )

    def test_default_flash_options_are_not_repeated(self):
        def read_defaults(path):
            values = {}
            if not path.exists():
                return values
            for line in path.read_text(encoding="utf-8").splitlines():
                if line.startswith("CONFIG_") and "=" in line:
                    key, value = line.split("=", 1)
                    values[key] = value
            return values

        base_defaults = read_defaults(ROOT / "sdkconfig.defaults")
        for config_path in sorted(
            (ROOT / "main/boards").rglob("config*.json")
        ):
            config = json.loads(config_path.read_text(encoding="utf-8"))
            defaults = dict(base_defaults)
            defaults.update(
                read_defaults(
                    ROOT / f"sdkconfig.defaults.{config['target']}"
                )
            )
            selected_flash = next(
                (
                    key
                    for key, value in reversed(defaults.items())
                    if key.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_")
                    and value == "y"
                ),
                None,
            )
            for build_config in config.get("builds", []):
                for option in build_config.get("sdkconfig_append", []):
                    if "=" not in option:
                        continue
                    key, value = option.split("=", 1)
                    if key.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_"):
                        self.assertNotEqual(
                            (key, value),
                            (selected_flash, "y"),
                            f"{config_path}: {option} repeats the effective "
                            "project default",
                        )
                    elif key == "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME":
                        self.assertNotEqual(
                            defaults.get(key),
                            value,
                            f"{config_path}: {option} repeats the effective "
                            "project default",
                        )

    def test_chip_defaults_only_contain_target_overrides(self):
        def read_defaults(path):
            values = {}
            for line in path.read_text(encoding="utf-8").splitlines():
                if line.startswith("CONFIG_") and "=" in line:
                    key, value = line.split("=", 1)
                    values[key] = value
            return values

        base_defaults = read_defaults(ROOT / "sdkconfig.defaults")
        self.assertEqual(
            base_defaults["CONFIG_ESPTOOLPY_FLASHSIZE_16MB"],
            "y",
        )
        self.assertEqual(
            base_defaults["CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"],
            '"partitions/v2/16m.csv"',
        )

        expected_partitions = {
            "esp32": '"partitions/v2/4m.csv"',
            "esp32c3": '"partitions/v2/16m_c3.csv"',
            "esp32c5": '"partitions/v2/16m.csv"',
            "esp32c6": '"partitions/v2/16m_c3.csv"',
            "esp32p4": '"partitions/v2/16m.csv"',
            "esp32s3": '"partitions/v2/16m.csv"',
            "esp32s31": '"partitions/v2/16m.csv"',
        }
        for target, expected_partition in expected_partitions.items():
            target_defaults = read_defaults(
                ROOT / f"sdkconfig.defaults.{target}"
            )
            duplicate_values = {
                key
                for key, value in target_defaults.items()
                if base_defaults.get(key) == value
            }
            self.assertFalse(
                duplicate_values,
                f"sdkconfig.defaults.{target} repeats base defaults: "
                f"{sorted(duplicate_values)}",
            )

            effective_defaults = dict(base_defaults)
            effective_defaults.update(target_defaults)
            self.assertEqual(
                effective_defaults[
                    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"
                ],
                expected_partition,
            )

            if target == "esp32":
                self.assertEqual(
                    target_defaults[
                        "CONFIG_ESPTOOLPY_FLASHSIZE_4MB"
                    ],
                    "y",
                )
            else:
                self.assertFalse(
                    any(
                        key.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_")
                        for key in target_defaults
                    ),
                    f"sdkconfig.defaults.{target} should inherit 16MB",
                )

    def test_wzh_partition_table_fits_the_largest_product_image(self):
        # The dinosaur image is ~0x40d040 and did not fit the original 0x3f0000
        # app slots. Guard the enlarged layout so a later edit cannot silently
        # break the build again.
        rows = {}
        for line in (ROOT / "partitions/v2/16m.csv").read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [field.strip() for field in line.split(",")]
            rows[fields[0]] = fields

        app_size = int(rows["ota_0"][4], 16)
        self.assertEqual(int(rows["ota_1"][4], 16), app_size)
        self.assertGreaterEqual(app_size, 0x40D040, "dinosaur image must fit")

        ota_start = int(rows["ota_0"][3], 16)
        assets_offset = int(rows["assets"][3], 16)
        self.assertGreaterEqual(
            assets_offset,
            ota_start + 2 * app_size,
            "assets must not overlap the second app slot",
        )
        assets_size = int(rows["assets"][4], 16)
        # The audio/image assets currently use ~3.7 MB. Keep at least 7 MB so the
        # partition enlargement above cannot quietly starve it.
        self.assertGreaterEqual(
            assets_size,
            7 * 1024 * 1024,
            "the assets partition needs at least 7 MB",
        )
        self.assertLessEqual(assets_offset + assets_size, 16 * 1024 * 1024)


class BoardSelectionTests(unittest.TestCase):
    def setUp(self):
        self.variants = [
            {"board": "bread-compact-wifi", "name": "bread-compact-wifi", "full_name": "bread-compact-wifi"},
            {
                "board": "waveshare/esp32-c6-touch-amoled-2.06",
                "name": "esp32-c6-touch-amoled-2.06",
                "full_name": "waveshare-esp32-c6-touch-amoled-2.06",
            },
        ]

    def test_nested_manufacturer_board_path(self):
        selected = build._select_variants_for_changes(
            self.variants,
            ["main/boards/waveshare/esp32-c6-touch-amoled-2.06/config.h"],
        )
        self.assertEqual([item["board"] for item in selected], [self.variants[1]["board"]])

    def test_board_display_name_comes_from_kconfig_prompt(self):
        self.assertEqual(
            build._get_board_display_name("CONFIG_BOARD_TYPE_WZH_DINOSAUR"),
            "WZH Dinosaur",
        )
        self.assertEqual(
            build._get_board_display_name("CONFIG_BOARD_TYPE_WZH_CRAWLPANDA"),
            "WZH Crawl Panda",
        )

    def test_variant_name_disambiguates_shared_board_directory(self):
        # Two variants share one board directory here too (barge-in vs. plain),
        # and both must resolve to the same board symbol.
        board = "wzh_crawlPanda"
        for variant in ("wzh-crawlpanda", "wzh-crawlpanda-barge-in"):
            with self.subTest(variant=variant):
                self.assertEqual(
                    build._resolve_board_config(
                        board, "esp32s3", [], variant_name=variant
                    ),
                    "CONFIG_BOARD_TYPE_WZH_CRAWLPANDA",
                )

    def test_wzh_board_directories_resolve_their_kconfig_symbols(self):
        cases = {
            "wzh_crawlPanda": "CONFIG_BOARD_TYPE_WZH_CRAWLPANDA",
            "wzh_dinosaur": "CONFIG_BOARD_TYPE_WZH_DINOSAUR",
            "wzh_tailRedPanda": "CONFIG_BOARD_TYPE_WZH_TAILREDPANDA",
        }
        for board, expected in cases.items():
            with self.subTest(board=board):
                self.assertTrue(build._board_type_exists(board))
                config = json.loads(
                    (ROOT / "main/boards" / board / "config.json").read_text(
                        encoding="utf-8"
                    )
                )
                self.assertEqual(config["target"], "esp32s3")
                self.assertEqual(
                    build._resolve_board_config(
                        board,
                        config["target"],
                        config["builds"][0].get("sdkconfig_append", []),
                    ),
                    expected,
                )

    def test_removed_upstream_board_directories_are_gone(self):
        board_root = ROOT / "main/boards"
        for board in (
            "alientek/atk-dnesp32s3",
            "magiclick/c3",
            "xmini/c3",
            "xorigin/aipi-lite",
            "kevin/box-2",
            "labplus/ledong-v2",
            "lckfb/szpi-esp32s3",
            "wdmomo/esp32-cgc",
        ):
            with self.subTest(board=board):
                self.assertFalse((board_root / board).exists())
                self.assertFalse(build._board_type_exists(board))

    def test_common_and_core_changes_select_all(self):
        for path in (
            "main/boards/common/board.cc",
            "main/application.cc",
            "components/esp-ml307/src/at_modem.cc",
            "scripts/build_default_assets.py",
            "scripts/build.py",
        ):
            with self.subTest(path=path):
                self.assertEqual(
                    build._select_variants_for_changes(self.variants, [path]),
                    self.variants,
                )

    def test_docs_only_selects_none(self):
        self.assertEqual(
            build._select_variants_for_changes(self.variants, ["docs/readme.md"]),
            [],
        )


class BoardMenuTests(unittest.TestCase):
    def test_board_menu_is_sorted_and_matches_cmake(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        choice = kconfig.split("choice BOARD_TYPE\n", 1)[1].split(
            "endchoice\n", 1
        )[0]
        entries = re.findall(
            r'^    config (BOARD_TYPE_[A-Za-z0-9_]+)\n'
            r'        bool "([^"]+)"\n'
            r'        depends on (IDF_TARGET_[A-Za-z0-9_]+)$',
            choice,
            re.MULTILINE,
        )
        symbols = [symbol for symbol, _, _ in entries]
        labels = [label for _, label, _ in entries]

        self.assertEqual(len(symbols), len(set(symbols)))
        self.assertEqual(len(labels), len(set(labels)))
        self.assertEqual(
            set(symbols),
            set(
                re.findall(
                    r"(?:if|elseif)\(CONFIG_(BOARD_TYPE_[A-Za-z0-9_]+)\)",
                    cmake,
                )
            ),
        )

        def natural_key(label):
            label = re.sub(
                r"\s+\([^)]*[\u3400-\u9fff][^)]*\)$",
                "",
                label.casefold(),
            )
            return tuple(
                (1, float(part))
                if re.fullmatch(r"\d+(?:\.\d+)?", part)
                else (0, part)
                for part in re.split(r"(\d+(?:\.\d+)?)", label)
            )

        self.assertEqual(labels, sorted(labels, key=natural_key))
        for label in labels:
            if re.search(r"[\u3400-\u9fff]", label):
                self.assertRegex(
                    label,
                    r"^[^\u3400-\u9fff]+\([^)]*[\u3400-\u9fff][^)]*\)$",
                )

        for stale_label in (
            "ALIENTEK",
            "AMOLOED",
            "AIPI-Lite",
            "NOLOGO",
            "NULLLAB-AI-VOX3",
            "StopWatch",
            "electronBot",
            "ottoRobot",
        ):
            self.assertNotIn(stale_label, choice)

    def test_board_menu_only_lists_the_three_wzh_products(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        choice = kconfig.split("choice BOARD_TYPE\n", 1)[1].split(
            "endchoice\n", 1
        )[0]
        symbols = re.findall(r"^    config (BOARD_TYPE_[A-Za-z0-9_]+)$", choice, re.MULTILINE)
        self.assertEqual(
            symbols,
            [
                "BOARD_TYPE_WZH_CRAWLPANDA",
                "BOARD_TYPE_WZH_DINOSAUR",
                "BOARD_TYPE_WZH_TAILREDPANDA",
            ],
        )
        # Every menu entry is an ESP32-S3 toy, so the target default is explicit.
        self.assertEqual(choice.count("depends on IDF_TARGET_ESP32S3"), 3)
        self.assertIn(
            "default BOARD_TYPE_WZH_CRAWLPANDA if IDF_TARGET_ESP32S3", choice
        )
        # No leftover menu from the removed upstream boards.
        for stale in (
            "BOARD_TYPE_BREAD_COMPACT",
            "BOARD_TYPE_ESP_BOX",
            "BOARD_TYPE_XMINI",
            "BOARD_TYPE_LICHUANG",
        ):
            self.assertNotIn(stale, choice)


class InvalidConfigTests(unittest.TestCase):
    def test_duplicate_reported_identifiers_fail_collection(self):
        cases = (
            (
                ("type-a", "same-name"),
                ("type-b", "same-name"),
                "duplicate reported board name",
            ),
            (
                ("same-type", "name-a"),
                ("same-type", "name-b"),
                "duplicate reported board type",
            ),
        )
        for first, second, expected_error in cases:
            with self.subTest(expected_error=expected_error):
                with tempfile.TemporaryDirectory() as temp_dir:
                    boards = Path(temp_dir)
                    for directory, (board_type, board_name) in zip(
                        ("board-a", "board-b"),
                        (first, second),
                    ):
                        board_dir = boards / directory
                        board_dir.mkdir()
                        (board_dir / "config.json").write_text(
                            json.dumps({
                                "type": board_type,
                                "target": "esp32s3",
                                "builds": [{"name": board_name}],
                            }),
                            encoding="utf-8",
                        )
                    with mock.patch.object(build, "_BOARDS_DIR", boards):
                        with self.assertRaisesRegex(
                            ValueError,
                            expected_error,
                        ):
                            build._collect_variants(
                                idf_version=(6, 0, 1)
                            )

    def test_invalid_reported_identifiers_are_rejected(self):
        for value in (
            "bad_board",
            "Bad-Board",
            "bad board",
            "坏板子",
        ):
            with self.subTest(type=value):
                with self.assertRaisesRegex(ValueError, "only lowercase"):
                    build._get_reported_type({"type": value})
            with self.subTest(name=value):
                with self.assertRaisesRegex(ValueError, "only lowercase"):
                    build._get_reported_name({"name": value})

        for value in ("board", "board-1", "board.1", "1.2-3"):
            with self.subTest(valid=value):
                self.assertEqual(
                    build._get_reported_type({"type": value}),
                    value,
                )
                self.assertEqual(
                    build._get_reported_name({"name": value}),
                    value,
                )

    def test_missing_reported_type_fails_collection(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            boards = Path(temp_dir)
            board_dir = boards / "bad-board"
            board_dir.mkdir()
            (board_dir / "config.json").write_text(json.dumps({
                "target": "esp32s3",
                "builds": [{"name": "bad-board"}],
            }), encoding="utf-8")
            with mock.patch.object(build, "_BOARDS_DIR", boards):
                with self.assertRaisesRegex(ValueError, 'top-level "type"'):
                    build._collect_variants(idf_version=(6, 0, 1))

    def test_invalid_version_rule_fails_collection(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            boards = Path(temp_dir)
            board_dir = boards / "bad-board"
            board_dir.mkdir()
            (board_dir / "config.json").write_text(json.dumps({
                "type": "bad-board",
                "target": "esp32s3",
                "builds": [{
                    "name": "bad-board",
                    "idf_version": "~=6.0",
                }],
            }), encoding="utf-8")
            with mock.patch.object(build, "_BOARDS_DIR", boards):
                with self.assertRaisesRegex(ValueError, "Invalid ESP-IDF version expression"):
                    build._collect_variants(idf_version=(6, 0, 1))


class PreviewTargetTests(unittest.TestCase):
    def test_stage_marker_is_only_emitted_when_enabled(self):
        output = io.StringIO()
        with (
            mock.patch.dict(os.environ, {}, clear=True),
            contextlib.redirect_stdout(output),
        ):
            build._emit_build_stage("compiling")
        self.assertEqual(output.getvalue(), "")

        output = io.StringIO()
        with (
            mock.patch.dict(
                os.environ,
                {"XIAOZHI_BUILD_STAGES": "true"},
                clear=True,
            ),
            contextlib.redirect_stdout(output),
        ):
            build._emit_build_stage("compiling")
        self.assertEqual(output.getvalue(), "XIAOZHI_STAGE compiling\n")

    def test_merge_bin_enables_preview_mode(self):
        with mock.patch.object(build, "_run_idf") as run_idf:
            build.merge_bin(preview=True)

        run_idf.assert_called_once_with("merge-bin", preview=True)


class TargetConfigurationTests(unittest.TestCase):
    def test_sync_vscode_target_preserves_other_settings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            settings = Path(temp_dir) / "settings.json"
            settings.write_text(
                '{\n'
                '  "idf.customExtraVars": {\n'
                '    "IDF_TARGET": "esp32c3"\n'
                '  },\n'
                '  "editor.formatOnSave": true\n'
                '}\n',
                encoding="utf-8",
            )

            self.assertTrue(build._sync_vscode_target("esp32s3", settings))
            self.assertEqual(
                settings.read_text(encoding="utf-8"),
                '{\n'
                '  "idf.customExtraVars": {\n'
                '    "IDF_TARGET": "esp32s3"\n'
                '  },\n'
                '  "editor.formatOnSave": true\n'
                '}\n',
            )
            self.assertFalse(build._sync_vscode_target("esp32s3", settings))

    def test_sync_vscode_target_skips_missing_settings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            settings = Path(temp_dir) / "settings.json"
            self.assertFalse(build._sync_vscode_target("esp32s3", settings))

    def test_same_target_does_not_clean_build_directory(self):
        with (
            mock.patch.object(build, "_target_from_cmake_cache", return_value="esp32s3"),
            mock.patch.object(build, "_configured_target", return_value="esp32s3"),
            mock.patch.object(build, "_run_idf") as run_idf,
        ):
            build._prepare_target("esp32s3", preview=False)

        run_idf.assert_not_called()

    def test_changed_cmake_target_runs_fullclean_only(self):
        with (
            mock.patch.object(build, "_target_from_cmake_cache", return_value="esp32c3"),
            mock.patch.object(build, "_configured_target", return_value=None),
            mock.patch.object(build, "_run_idf") as run_idf,
        ):
            build._prepare_target("esp32s3", preview=True)

        run_idf.assert_called_once_with(
            "fullclean",
            preview=True,
        )

    def test_configure_build_uses_all_cmake_values_in_one_run(self):
        previous_cwd = Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                os.chdir(temp_dir)
                Path("sdkconfig").write_text(
                    'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_OLD_VARIANT=y\n',
                    encoding="utf-8",
                )
                Path("sdkconfig.defaults").write_text(
                    "CONFIG_PROJECT_DEFAULT=y\n",
                    encoding="utf-8",
                )

                with mock.patch.object(build, "_run_idf") as run_idf:
                    build._configure_build(
                        "esp32s3",
                        ["CONFIG_BOARD_TYPE_TEST=y", "CONFIG_FEATURE=y"],
                        "test-board",
                        preview=False,
                    )

                self.assertFalse(Path("sdkconfig").exists())
                self.assertIn(
                    "CONFIG_OLD_VARIANT=y",
                    Path("sdkconfig.old").read_text(encoding="utf-8"),
                )
                fragment = Path("build/xiaozhi-build.sdkconfig.defaults")
                self.assertEqual(
                    fragment.read_text(encoding="utf-8"),
                    "# Generated by scripts/build.py\n"
                    "CONFIG_BOARD_TYPE_TEST=y\n"
                    "CONFIG_FEATURE=y\n",
                )
                run_idf.assert_called_once_with(
                    "-DIDF_TARGET=esp32s3",
                    "-DSDKCONFIG_DEFAULTS="
                    "sdkconfig.defaults;build/xiaozhi-build.sdkconfig.defaults",
                    "-DBOARD_NAME=test-board",
                    "reconfigure",
                    preview=False,
                )
        finally:
            os.chdir(previous_cwd)

    def test_configure_build_replaces_stale_sdkconfig_backup(self):
        previous_cwd = Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                os.chdir(temp_dir)
                Path("sdkconfig").write_text(
                    'CONFIG_IDF_TARGET="esp32s3"\n',
                    encoding="utf-8",
                )
                Path("sdkconfig.old").write_text(
                    "CONFIG_USER_PREVIOUS_VALUE=y\n",
                    encoding="utf-8",
                )

                with mock.patch.object(build, "_run_idf"):
                    build._configure_build(
                        "esp32s3",
                        ["CONFIG_BOARD_TYPE_TEST=y"],
                        "test-board",
                        preview=False,
                    )

                self.assertFalse(Path("sdkconfig").exists())
                self.assertEqual(
                    Path("sdkconfig.old").read_text(encoding="utf-8"),
                    'CONFIG_IDF_TARGET="esp32s3"\n',
                )
        finally:
            os.chdir(previous_cwd)


class BuildOptionTests(unittest.TestCase):
    def test_supported_languages_match_kconfig_and_cmake(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(
            encoding="utf-8"
        )
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        supported_languages = build._collect_languages()
        kconfig_languages = set(
            re.findall(r"^\s+config LANGUAGE_([A-Z_]+)$", kconfig, re.MULTILINE)
        )
        expected_symbols = {
            language.replace("-", "_").upper()
            for language in supported_languages
        }
        self.assertEqual(kconfig_languages, expected_symbols)
        for language in supported_languages:
            symbol = language.replace("-", "_").upper()
            self.assertIn(f"CONFIG_LANGUAGE_{symbol}", cmake)
            self.assertTrue(
                (ROOT / "main/assets/locales" / language).is_dir(),
                language,
            )

    def test_languages_are_discovered_from_build_configuration(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            cmake = root / "CMakeLists.txt"
            kconfig = root / "Kconfig.projbuild"
            locales = root / "locales"
            (locales / "xx-YY").mkdir(parents=True)
            cmake.write_text(
                'if(CONFIG_LANGUAGE_XX_YY)\n'
                '    set(LANG_DIR "xx-YY")\n'
                'endif()\n',
                encoding="utf-8",
            )
            kconfig.write_text(
                "config LANGUAGE_XX_YY\n"
                '    bool "Test language"\n',
                encoding="utf-8",
            )

            self.assertEqual(
                build._collect_languages(cmake, kconfig, locales),
                ["xx-YY"],
            )

    def test_language_normalization_and_option(self):
        self.assertEqual(
            build._language_sdkconfig_option("EN_us"),
            ("en-US", "CONFIG_LANGUAGE_EN_US=y"),
        )
        with self.assertRaisesRegex(ValueError, "Unsupported language"):
            build._language_sdkconfig_option("xx-YY")

    def test_wake_words_are_read_from_esp_sr_kconfig(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            kconfig = Path(temp_dir) / "Kconfig.projbuild"
            kconfig.write_text(
                'config SR_WN_WN9S_HELLO\n'
                '    bool "Hello (wn9s_hello)"\n'
                '\n'
                'config SR_WN_WN9_JARVIS_TTS\n'
                '    bool "Jarvis (wn9_jarvis_tts)"\n',
                encoding="utf-8",
            )
            wake_words = build._collect_wake_words(kconfig)

        self.assertEqual(
            [item["model"] for item in wake_words],
            ["wn9s_hello", "wn9_jarvis_tts"],
        )
        self.assertEqual(
            [item["phrase"] for item in wake_words],
            ["Hello", "Jarvis"],
        )
        self.assertIn("esp32c5", wake_words[0]["targets"])
        self.assertNotIn("esp32c5", wake_words[1]["targets"])

    def test_missing_esp_sr_kconfig_has_actionable_error(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            missing = Path(temp_dir) / "missing-kconfig"
            with self.assertRaisesRegex(
                RuntimeError,
                "idf.py reconfigure",
            ):
                build._collect_wake_words(missing)

    def test_wake_word_alias_selects_target_engine(self):
        model, options, symbols = build._wake_word_sdkconfig_options(
            "nihaoxiaozhi",
            "esp32c5",
        )
        self.assertEqual(model, "wn9s_nihaoxiaozhi")
        self.assertIn("CONFIG_USE_ESP_WAKE_WORD=y", options)
        self.assertIn("CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y", options)
        self.assertEqual(
            symbols,
            [
                "CONFIG_USE_ESP_WAKE_WORD",
                "CONFIG_SR_WN_WN9S_NIHAOXIAOZHI",
            ],
        )

        model, options, symbols = build._wake_word_sdkconfig_options(
            "nihaoxiaozhi",
            "esp32s3",
        )
        self.assertEqual(model, "wn9_nihaoxiaozhi_tts")
        self.assertIn("CONFIG_USE_AFE_WAKE_WORD=y", options)
        self.assertIn("CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y", options)
        self.assertEqual(
            symbols,
            [
                "CONFIG_USE_AFE_WAKE_WORD",
                "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS",
            ],
        )

    def test_wake_word_disabled_removes_target_default_model(self):
        model, options, symbols = build._wake_word_sdkconfig_options(
            "disabled",
            "esp32s3",
        )
        self.assertEqual(model, "disabled")
        self.assertIn("CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n", options)
        self.assertIn("CONFIG_USE_AFE_WAKE_WORD=n", options)
        self.assertIn("CONFIG_USE_ESP_WAKE_WORD=n", options)
        self.assertIn("CONFIG_WAKE_WORD_DISABLED=y", options)
        self.assertEqual(symbols, ["CONFIG_WAKE_WORD_DISABLED"])

    def test_incompatible_wake_word_model_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "WakeNet9s"):
            build._wake_word_sdkconfig_options(
                "wn9_jarvis_tts",
                "esp32c6",
            )
        with self.assertRaisesRegex(ValueError, "Invalid wake word"):
            build._wake_word_sdkconfig_options("jarvis", "esp32s3")

    def test_user_options_override_board_options(self):
        merged = build._merge_sdkconfig_options(
            [
                "CONFIG_BOARD_TYPE_TEST=y",
                "CONFIG_USE_ESP_WAKE_WORD=n",
            ],
            [
                "CONFIG_USE_ESP_WAKE_WORD=y",
                "CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y",
            ],
        )
        self.assertEqual(
            merged,
            [
                "CONFIG_BOARD_TYPE_TEST=y",
                "CONFIG_USE_ESP_WAKE_WORD=y",
                "CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y",
            ],
        )

    def test_product_default_wake_word_and_cli_override_reach_configuration(self):
        for override, expected in [(None, 'WN9_XIAOLONGXIAOLONG_TTS'),
                                   ('wn9_jarvis_tts', 'WN9_JARVIS_TTS')]:
            with (
                self.subTest(override=override),
                mock.patch.object(build, '_prepare_target'),
                mock.patch.object(build, '_configure_build') as configure,
                mock.patch.object(build, '_validate_configured_symbols') as validate,
                mock.patch.object(build, '_run_idf'),
                mock.patch.object(build, 'merge_bin'),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                build.build_board('wzh_dinosaur', name_filter='wzh-dinosaur', wake_word=override)
                options = configure.call_args.args[1]
                self.assertIn('CONFIG_USE_AFE_WAKE_WORD=y', options)
                self.assertIn('CONFIG_USE_CUSTOM_WAKE_WORD=n', options)
                self.assertIn('CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n', options)
                self.assertEqual([option for option in options
                                  if option.startswith('CONFIG_SR_WN_') and option.endswith('=y')],
                                 [f'CONFIG_SR_WN_{expected}=y'])
                self.assertIn(f'CONFIG_SR_WN_{expected}', validate.call_args.args[0])

    def test_configured_build_options_are_verified(self):
        previous_cwd = Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                os.chdir(temp_dir)
                Path("sdkconfig").write_text(
                    "CONFIG_LANGUAGE_EN_US=y\n",
                    encoding="utf-8",
                )
                build._validate_configured_symbols(
                    ["CONFIG_LANGUAGE_EN_US"],
                    "--language",
                )
                with self.assertRaisesRegex(ValueError, "Kconfig rejected"):
                    build._validate_configured_symbols(
                        ["CONFIG_SR_WN_UNKNOWN"],
                        "--wake-word",
                    )
        finally:
            os.chdir(previous_cwd)


class VariantSelectionTests(unittest.TestCase):
    def setUp(self):
        self.variants = [
            {"board": "test-board", "name": "variant-a", "full_name": "variant-a"},
            {"board": "test-board", "name": "variant-b", "full_name": "variant-b"},
        ]

    def test_non_interactive_selection_requires_name(self):
        stdin = mock.Mock()
        stdin.isatty.return_value = False
        with (
            mock.patch.object(build.sys, "stdin", stdin),
            self.assertRaisesRegex(SystemExit, "2"),
        ):
            build._select_variant("test-board", self.variants)

    def test_interactive_selection_accepts_number(self):
        stdin = mock.Mock()
        stdin.isatty.return_value = True
        with (
            mock.patch.object(build.sys, "stdin", stdin),
            mock.patch("builtins.input", return_value="2"),
        ):
            selected = build._select_variant("test-board", self.variants)

        self.assertEqual(selected, "variant-b")


class CliTests(unittest.TestCase):
    def setUp(self):
        self.variants = [
            {
                "board": "wzh_crawlPanda",
                "name": "wzh-crawlpanda",
                "full_name": "wzh-crawlpanda",
            },
            {
                "board": "multi-board",
                "name": "variant-a",
                "full_name": "variant-a",
            },
            {
                "board": "multi-board",
                "name": "variant-b",
                "full_name": "variant-b",
            },
        ]

    def test_no_arguments_prints_help_without_building(self):
        output = io.StringIO()
        with (
            mock.patch.object(build, "build_board") as build_board,
            contextlib.redirect_stdout(output),
        ):
            build.main([])

        build_board.assert_not_called()
        self.assertIn("usage:", output.getvalue())
        self.assertIn("--list-boards", output.getvalue())
        self.assertIn("--list-languages", output.getvalue())
        self.assertIn("--list-wake-words", output.getvalue())

    def test_list_boards_prints_boards_and_multi_variants(self):
        output = io.StringIO()
        with (
            mock.patch.object(
                build,
                "_detect_idf_version_for_listing",
                return_value=(6, 0, 2),
            ),
            mock.patch.object(
                build,
                "_collect_variants",
                return_value=self.variants,
            ),
            contextlib.redirect_stdout(output),
        ):
            build.main(["--list-boards"])

        self.assertEqual(
            output.getvalue(),
            "wzh_crawlPanda\n"
            "multi-board\n"
            "  - variant-a\n"
            "  - variant-b\n",
        )

    def test_list_languages_supports_json(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            build.main(["--list-languages", "--json"])

        self.assertEqual(
            json.loads(output.getvalue()),
            build._collect_languages(),
        )

    def test_list_wake_words_supports_json(self):
        wake_words = [{
            "model": "wn9_jarvis_tts",
            "phrase": "Jarvis",
            "targets": ["esp32s3"],
        }]
        output = io.StringIO()
        with (
            mock.patch.object(
                build,
                "_collect_wake_words",
                return_value=wake_words,
            ),
            contextlib.redirect_stdout(output),
        ):
            build.main(["--list-wake-words", "--json"])

        self.assertEqual(json.loads(output.getvalue()), wake_words)

    def test_build_does_not_create_zip_by_default(self):
        with (
            mock.patch.object(build, "_detect_idf_version", return_value=(6, 0, 2)),
            mock.patch.object(build, "_board_type_exists", return_value=True),
            mock.patch.object(
                build,
                "_collect_variants",
                return_value=self.variants,
            ),
            mock.patch.object(build, "build_board") as build_board,
        ):
            build.main(["wzh_crawlPanda"])

        build_board.assert_called_once_with(
            "wzh_crawlPanda",
            config_filename="config.json",
            name_filter="wzh-crawlpanda",
            create_zip=False,
            language=None,
            wake_word=None,
            idf_version=(6, 0, 2),
        )

    def test_zip_flag_is_forwarded(self):
        with (
            mock.patch.object(build, "_detect_idf_version", return_value=(6, 0, 2)),
            mock.patch.object(build, "_board_type_exists", return_value=True),
            mock.patch.object(
                build,
                "_collect_variants",
                return_value=self.variants,
            ),
            mock.patch.object(build, "build_board") as build_board,
        ):
            build.main(["wzh_crawlPanda", "--zip"])

        self.assertTrue(build_board.call_args.kwargs["create_zip"])

    def test_language_and_wake_word_are_forwarded(self):
        with (
            mock.patch.object(build, "_detect_idf_version", return_value=(6, 0, 2)),
            mock.patch.object(build, "_board_type_exists", return_value=True),
            mock.patch.object(
                build,
                "_collect_variants",
                return_value=self.variants,
            ),
            mock.patch.object(build, "build_board") as build_board,
        ):
            build.main([
                "wzh_crawlPanda",
                "--language",
                "en-US",
                "--wake-word",
                "wn9_jarvis_tts",
            ])

        self.assertEqual(build_board.call_args.kwargs["language"], "en-US")
        self.assertEqual(
            build_board.call_args.kwargs["wake_word"],
            "wn9_jarvis_tts",
        )


class BoardSourceTests(unittest.TestCase):
    def test_relative_board_includes_exist(self):
        missing = []
        boards_dir = ROOT / "main/boards"
        for source in boards_dir.rglob("*"):
            if source.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                continue
            for line_number, line in enumerate(
                source.read_text(encoding="utf-8", errors="replace").splitlines(),
                1,
            ):
                match = re.match(
                    r'\s*#\s*include\s+"(\.\./[^"]+)"',
                    line,
                )
                if match and not (source.parent / match.group(1)).resolve().exists():
                    missing.append(
                        f"{source.relative_to(ROOT)}:{line_number}: {match.group(1)}"
                    )

        self.assertEqual(missing, [])


class ZipTests(unittest.TestCase):
    def test_zip_is_always_recreated(self):
        previous_cwd = Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                os.chdir(temp_dir)
                Path("build").mkdir()
                Path("build/merged-binary.bin").write_bytes(b"new firmware")
                Path("releases").mkdir()
                output = Path("releases/v1.2.3_test-board.zip")
                output.write_bytes(b"stale zip")

                build.zip_bin("test-board", "1.2.3")

                with build.zipfile.ZipFile(output) as archive:
                    self.assertEqual(
                        archive.read("merged-binary.bin"),
                        b"new firmware",
                    )
        finally:
            os.chdir(previous_cwd)


if __name__ == "__main__":
    unittest.main()
