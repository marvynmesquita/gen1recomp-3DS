#!/usr/bin/env python3
"""ROM-free regression tests for source-build version detection and routing."""

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase, main, mock

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_rom_data  # noqa: E402


class BuildRomDataCliTest(TestCase):
    def run_builder(self, sha1, *extra):
        manifest = {"romSha1": sha1, "symbols": {}}
        rom = SimpleNamespace(sha1=sha1)
        with mock.patch.object(build_rom_data, "RomImage", return_value=rom), \
                mock.patch.object(build_rom_data, "load_manifest", return_value=manifest), \
                mock.patch.object(build_rom_data, "build") as build, \
                mock.patch.object(build_rom_data.os, "makedirs"), \
                redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            result = build_rom_data.main([
                "--rom", "fixture.gb", "--only", "constants", *extra])
        return result, build

    def test_blue_rom_selects_blue_manifest_and_cache_paths(self):
        result, build = self.run_builder(
            build_rom_data.CANONICAL_BLUE_SHA1)

        self.assertEqual(result, 0)
        args = build.call_args.args
        self.assertEqual(args[3], "blue/data/generated")
        self.assertEqual(args[4], "blue/assets/generated")

    def test_red_rom_keeps_historical_root_paths(self):
        result, build = self.run_builder(build_rom_data.CANONICAL_RED_SHA1)

        self.assertEqual(result, 0)
        args = build.call_args.args
        self.assertEqual(args[3], "data/generated")
        self.assertEqual(args[4], "assets/generated")

    def test_explicit_output_paths_are_preserved(self):
        result, build = self.run_builder(
            build_rom_data.CANONICAL_YELLOW_SHA1,
            "--out", "/tmp/custom-data", "--assets", "/tmp/custom-assets")

        self.assertEqual(result, 0)
        args = build.call_args.args
        self.assertEqual(args[3], "/tmp/custom-data")
        self.assertEqual(args[4], "/tmp/custom-assets")

    def test_unknown_rom_is_rejected_before_build(self):
        unknown = "0" * 40
        result, build = self.run_builder(unknown)

        self.assertEqual(result, 1)
        build.assert_not_called()


if __name__ == "__main__":
    main()
