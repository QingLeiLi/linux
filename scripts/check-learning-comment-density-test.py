#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Regression tests for the learning-comment coverage checker."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CHECKER = Path(__file__).with_name("check-learning-comment-density.py")


class LearningCommentCoverageTest(unittest.TestCase):
    def run_checker(self, source: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "fixture.c"
            path.write_text(source, encoding="utf-8")
            return subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    "--min-density",
                    "0",
                    "--max-code-gap",
                    "100",
                    "--require-english-translation",
                    str(path),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

    def test_missing_translation_fails(self) -> None:
        result = self.run_checker("/* Explain the state transition. */\nint value;\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("untranslated comment 1-1", result.stdout)

    def test_adjacent_translation_passes(self) -> None:
        result = self.run_checker(
            "/* Explain the state transition. */\n"
            "/* 解释这一状态转换及其作用。 */\n"
            "int value;\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_code_between_translation_fails(self) -> None:
        result = self.run_checker(
            "/* Explain the state transition. */\n"
            "int value;\n"
            "/* 这段中文已经不再紧邻英文注释。 */\n"
        )
        self.assertEqual(result.returncode, 1)

    def test_metadata_literals_and_preprocessor_tags_are_exempt(self) -> None:
        result = self.run_checker(
            "// SPDX-License-Identifier: GPL-2.0\n"
            "#endif /* CONFIG_EXAMPLE */\n"
            'const char *text = "/* not a comment */";\n'
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_named_argument_tag_is_exempt_but_inline_prose_is_not(self) -> None:
        result = self.run_checker(
            "call(value, /* drop_lock= */ true);\n"
            "call(value,\n /* before_unmaps= */ false);\n"
            "int value; /* Explain the state transition. */\n"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("untranslated comment 4-4", result.stdout)


if __name__ == "__main__":
    unittest.main()
