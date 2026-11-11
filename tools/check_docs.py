"""Checks that the documentation points at things that exist. Python standard library only.

For every Markdown file under docs/, tools/lab/ and components/ (README.md, VAMKIT.md), plus the front pages:
  - every relative link target exists;
  - every repo path written in backticks (components/..., tools/..., manifest/..., docs/...) exists, from the repo
    root, from the document's folder or inside some component.
Paths with a placeholder (<...>), a wildcard or a trailing "..." are skipped, and so is anything under out/ or
third_party/ (built or fetched on the user's machine).

    python -I tools/check_docs.py            lists what is missing; exit 1 when anything is
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ROOTS = ("components/", "tools/", "manifest/", "docs/")
LINK = re.compile(r"\]\(([^)#\s]+)(?:#[^)]*)?\)")
TICK = re.compile(r"`([^`\n]+)`")
SKIP = re.compile(r"[<>*{}$%]|\.\.\.|^out/|^third_party/")


def documents():
    for name in ("README.md", "README.zh.md", "AGENTS.md", "THIRD_PARTY.md", "CHANGELOG.md"):
        yield REPO / name
    yield from sorted((REPO / "docs").rglob("*.md"))
    yield from sorted((REPO / "tools" / "lab").rglob("*.md"))
    for name in ("README.md", "VAMKIT.md"):
        for path in sorted((REPO / "components").rglob(name)):
            # upstream documentation inside the two imported projects is theirs
            rel = path.relative_to(REPO).as_posix()
            if rel.count("/") <= 3:
                yield path


def main():
    missing = 0
    for doc in documents():
        if not doc.is_file():
            continue
        text = doc.read_text(encoding="utf-8", errors="replace")
        problems = []
        for target in LINK.findall(text):
            if "://" in target or target.startswith("mailto:"):
                continue
            if not (doc.parent / target).exists():
                problems.append("link  " + target)
        for token in TICK.findall(text):
            token = token.strip().replace("\\", "/")
            path = token.split()[0].rstrip(".,;:)")
            if not path.startswith(ROOTS) or SKIP.search(path):
                continue
            # a component's own documents name its files relative to the component (tools/gen_x.py)
            if not ((REPO / path).exists() or (doc.parent / path).exists() or any(REPO.glob("components/*/*/" + path))):
                problems.append("path  " + path)
        if problems:
            missing += len(problems)
            print(doc.relative_to(REPO).as_posix())
            for p in sorted(set(problems)):
                print("    " + p)
    print(f"{missing} reference(s) to things that do not exist", file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
