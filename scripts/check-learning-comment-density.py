#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check Chinese learning-comment coverage in C-family source files.

This is a lexical gate, not a semantic comment-quality checker.  It counts
physical lines after separating C/C++ comments from code while keeping string
and character literals from being mistaken for comment delimiters.  The
optional English-comment gate also verifies that every non-exempt English
comment unit is immediately followed by a Chinese comment unit.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


CJK_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")


@dataclass
class Gap:
    start: int
    end: int
    code_lines: int


@dataclass
class UntranslatedComment:
    start: int
    end: int
    excerpt: str


@dataclass
class Result:
    path: str
    code_lines: int
    comment_lines: int
    chinese_comment_lines: int
    density: float
    max_code_gap: int
    gaps: list[Gap]
    untranslated_english_comments: list[UntranslatedComment]
    passed: bool


@dataclass
class CommentUnit:
    start: int
    end: int
    start_offset: int
    end_offset: int
    text: str
    preprocessor_inline: bool
    code_inline: bool


def comment_units(source: str) -> list[CommentUnit]:
    """Extract C/C++ comment units without treating literals as comments."""
    units: list[CommentUnit] = []
    i = 0
    line = 1
    quote = ""
    escaped = False
    line_start = 0

    while i < len(source):
        ch = source[i]
        nxt = source[i : i + 2]

        if quote:
            if ch == "\n":
                line += 1
                line_start = i + 1
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = ""
            i += 1
            continue

        if ch in {'"', "'"}:
            quote = ch
            i += 1
            continue

        if nxt not in {"//", "/*"}:
            if ch == "\n":
                line += 1
                line_start = i + 1
            i += 1
            continue

        start_offset = i
        start_line = line
        prefix = source[line_start:i]
        preprocessor_inline = bool(prefix.strip().startswith("#"))
        code_inline = bool(prefix.strip()) and not preprocessor_inline

        if nxt == "//":
            end_offset = source.find("\n", i)
            if end_offset < 0:
                end_offset = len(source)
            text = source[i:end_offset]
            end_line = line
            i = end_offset
        else:
            end_offset = source.find("*/", i + 2)
            if end_offset < 0:
                end_offset = len(source)
            else:
                end_offset += 2
            text = source[i:end_offset]
            end_line = start_line + text.count("\n")
            line = end_line
            last_newline = text.rfind("\n")
            if last_newline >= 0:
                line_start = start_offset + last_newline + 1
            i = end_offset

        units.append(
            CommentUnit(
                start=start_line,
                end=end_line,
                start_offset=start_offset,
                end_offset=end_offset,
                text=text,
                preprocessor_inline=preprocessor_inline,
                code_inline=code_inline,
            )
        )

    return units


def is_exempt_english_comment(unit: CommentUnit) -> bool:
    """Return whether a comment is metadata/structural rather than prose."""
    if unit.preprocessor_inline:
        return True

    body = re.sub(r"^\s*(?://|/\*)|\*/\s*$", "", unit.text, flags=re.DOTALL)
    lines = [re.sub(r"^\s*\*?\s?", "", line).strip() for line in body.splitlines()]
    lines = [line for line in lines if line]
    if not lines:
        return True

    # Kernel named-argument tags such as `/* drop_lock= */ true` are code
    # structure rather than prose.  Keep the exemption deliberately narrow so
    # an explanatory trailing English comment still requires a translation.
    if len(lines) == 1 and re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_]*\s*=", lines[0]
    ):
        return True

    metadata = re.compile(
        r"^(?:SPDX-License-Identifier:|Copyright\b|Author:|Authors:)", re.IGNORECASE
    )
    return all(metadata.search(line) for line in lines)


def untranslated_english_comments(source: str) -> list[UntranslatedComment]:
    """Find English prose comments lacking an adjacent Chinese comment."""
    units = comment_units(source)
    missing: list[UntranslatedComment] = []

    for index, unit in enumerate(units):
        if CJK_RE.search(unit.text) or not re.search(r"[A-Za-z]{3}", unit.text):
            continue
        if is_exempt_english_comment(unit):
            continue

        translated = False
        if index + 1 < len(units):
            next_unit = units[index + 1]
            between = source[unit.end_offset : next_unit.start_offset]
            translated = not between.strip() and bool(CJK_RE.search(next_unit.text))

        if not translated:
            excerpt = " ".join(
                line.strip(" /*\t") for line in unit.text.splitlines() if line.strip(" /*\t")
            )
            missing.append(
                UntranslatedComment(
                    start=unit.start,
                    end=unit.end,
                    excerpt=excerpt[:120],
                )
            )

    return missing


def split_line(line: str, in_block: bool, quote: str) -> tuple[str, str, bool, str]:
    """Return code, comment and lexical state for the next physical line."""
    code: list[str] = []
    comment: list[str] = []
    i = 0
    escaped = False

    while i < len(line):
        if in_block:
            end = line.find("*/", i)
            if end < 0:
                comment.append(line[i:])
                break
            comment.append(line[i : end + 2])
            i = end + 2
            in_block = False
            continue

        ch = line[i]
        nxt = line[i : i + 2]
        if quote:
            code.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = ""
            i += 1
            continue

        if ch in {'"', "'"}:
            quote = ch
            code.append(ch)
            i += 1
        elif nxt == "//":
            comment.append(line[i:])
            break
        elif nxt == "/*":
            in_block = True
            comment.append(nxt)
            i += 2
        else:
            code.append(ch)
            i += 1

    return "".join(code), "".join(comment), in_block, quote


def analyze(
    path: Path, min_density: float, max_gap: int, require_english_translation: bool
) -> Result:
    code_lines = 0
    comment_lines = 0
    chinese_lines = 0
    in_block = False
    quote = ""
    current_gap_start = 0
    current_gap_end = 0
    current_gap_count = 0
    longest_gap = 0
    gaps: list[Gap] = []

    def finish_gap() -> None:
        nonlocal current_gap_start, current_gap_end, current_gap_count, longest_gap
        if current_gap_count:
            longest_gap = max(longest_gap, current_gap_count)
            if current_gap_count > max_gap:
                gaps.append(Gap(current_gap_start, current_gap_end, current_gap_count))
        current_gap_start = 0
        current_gap_end = 0
        current_gap_count = 0

    source_text = path.read_text(encoding="utf-8")
    for lineno, line in enumerate(source_text.splitlines(keepends=True), 1):
        code, comment, in_block, quote = split_line(line, in_block, quote)
        has_code = bool(code.strip())
        has_comment = bool(comment.strip())
        has_chinese = has_comment and bool(CJK_RE.search(comment))

        code_lines += has_code
        comment_lines += has_comment
        chinese_lines += has_chinese

        if has_chinese:
            finish_gap()
        elif has_code:
            if not current_gap_count:
                current_gap_start = lineno
            current_gap_end = lineno
            current_gap_count += 1

    finish_gap()
    untranslated = (
        untranslated_english_comments(source_text)
        if require_english_translation
        else []
    )
    density = chinese_lines / code_lines if code_lines else 0.0
    return Result(
        path=str(path),
        code_lines=code_lines,
        comment_lines=comment_lines,
        chinese_comment_lines=chinese_lines,
        density=density,
        max_code_gap=longest_gap,
        gaps=gaps,
        untranslated_english_comments=untranslated,
        passed=density >= min_density and not gaps and not untranslated,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check Chinese learning-comment density in source files."
    )
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument(
        "--min-density",
        type=float,
        default=0.20,
        help="minimum Chinese-comment/code-line ratio (default: 0.20)",
    )
    parser.add_argument(
        "--max-code-gap",
        type=int,
        default=10,
        help="maximum consecutive code lines without Chinese comments (default: 10)",
    )
    parser.add_argument(
        "--max-reported-gaps",
        type=int,
        default=20,
        help="maximum uncovered ranges printed per file (default: 20; JSON is complete)",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON")
    parser.add_argument(
        "--require-english-translation",
        action="store_true",
        help="fail when non-exempt English comments lack adjacent Chinese comments",
    )
    args = parser.parse_args()
    if not 0 <= args.min_density <= 1:
        parser.error("--min-density must be between 0 and 1")
    if args.max_code_gap < 0:
        parser.error("--max-code-gap must be non-negative")
    if args.max_reported_gaps < 0:
        parser.error("--max-reported-gaps must be non-negative")
    return args


def main() -> int:
    args = parse_args()
    missing = [str(path) for path in args.files if not path.is_file()]
    if missing:
        for path in missing:
            print(f"error: not a regular file: {path}", file=sys.stderr)
        return 2

    results = [
        analyze(path, args.min_density, args.max_code_gap, args.require_english_translation)
        for path in args.files
    ]
    if args.json:
        print(json.dumps([asdict(result) for result in results], ensure_ascii=False, indent=2))
    else:
        for result in results:
            status = "PASS" if result.passed else "FAIL"
            print(
                f"{status} {result.path}: code={result.code_lines} "
                f"comments={result.comment_lines} chinese={result.chinese_comment_lines} "
                f"density={result.density:.3f} max_gap={result.max_code_gap}"
            )
            for gap in result.gaps[: args.max_reported_gaps]:
                print(
                    f"  uncovered {gap.start}-{gap.end}: "
                    f"{gap.code_lines} code lines"
                )
            hidden = len(result.gaps) - args.max_reported_gaps
            if hidden > 0:
                print(f"  ... {hidden} more uncovered ranges (use --json for all)")
            for comment in result.untranslated_english_comments[
                : args.max_reported_gaps
            ]:
                print(
                    f"  untranslated comment {comment.start}-{comment.end}: "
                    f"{comment.excerpt}"
                )
            hidden_comments = (
                len(result.untranslated_english_comments) - args.max_reported_gaps
            )
            if hidden_comments > 0:
                print(
                    f"  ... {hidden_comments} more untranslated comments "
                    "(use --json for all)"
                )

    return 0 if all(result.passed for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
