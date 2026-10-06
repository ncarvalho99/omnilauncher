#!/usr/bin/env python3
"""Write the newest CHANGELOG section into romfs, for the build to carry.

The Update tab shows what the installed version brought, and it has to do that
before any network call -- a console that never reaches GitHub still deserves an
answer. Deriving the file from CHANGELOG.md rather than keeping a second copy by
hand is deliberate: the review.json episode showed how quickly a hand-maintained
duplicate drifts from the thing it duplicates.

Run during release preparation, right after CHANGELOG.md gains its new section.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHANGELOG = ROOT / "CHANGELOG.md"
# A loose file at the romfs root is not copied into the install layout;
# only directories are. Hence notes/.
OUTPUT = ROOT / "romfs" / "notes" / "release-notes.txt"
# Matches what the dialog can render, so the console never has to strip Markdown
# it was not given: headings and bullets, nothing else. The budget is per
# language half: the dialog shows one of them, never both, and a shared budget
# silently swallowed the tail of the Portuguese half once the release notes grew.
MAX_CHARS = 6000
# Splits the English half from the Portuguese one for the console.
LANGUAGE_MARKER = "--pt-BR--"


def plain(line: str) -> str:
    line = line.replace("*", "").replace("`", "").replace("#", "")
    line = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", line)
    return line.strip()


def main() -> int:
    if not CHANGELOG.is_file():
        print(f"not found: {CHANGELOG}", file=sys.stderr)
        return 1

    text = CHANGELOG.read_text(encoding="utf-8")
    sections = re.split(r"\n#\s*(?:OmniLaunch|SwitchU)\s+", text)
    head = sections[0]
    version_match = re.match(r"#\s*(?:OmniLaunch|SwitchU)\s+(\S+)", head)
    if not version_match:
        print("could not read the version from the first heading", file=sys.stderr)
        return 1
    version = version_match.group(1)

    # A bullet in the changelog wraps over several lines; only the first starts
    # with "- ". Joining the continuations is what keeps a sentence whole.
    kept: list[str] = []
    total = 0
    pending = ""

    def flush() -> None:
        nonlocal pending, total
        if not pending:
            return
        if total + len(pending) <= MAX_CHARS:
            kept.append("- " + pending)
            total += len(pending)
        pending = ""

    for raw in head.splitlines():
        stripped = raw.strip()
        if stripped.startswith("# SwitchU") or stripped.startswith("# OmniLaunch"):
            continue
        if stripped == "---" or not stripped or stripped.startswith("<") or stripped.startswith("!["):
            flush()
            continue
        if stripped.startswith("## ") or stripped.startswith("### "):
            flush()
            title = plain(stripped)
            # The two halves are separated by a marker the console splits on, so
            # it can show the reader's own language without parsing headings.
            if title.lower().startswith(("portugu", "português")):
                kept.append(LANGUAGE_MARKER)
                # The budget is per half, so the second language starts fresh.
                total = 0
            elif not title.lower().startswith("english"):
                kept.append("\n" + title)
            continue
        if stripped.startswith("- "):
            flush()
            pending = plain(stripped[2:])
            continue
        if pending:
            pending += " " + plain(stripped)
    flush()

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(kept) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT.relative_to(ROOT)}: {version}, {len(kept)} lines, {total} chars")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
