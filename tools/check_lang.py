"""Checks the menus' language tables (components/shared/lang/<code>.json) against the code.

The code holds the English text: Lang.T / Lang.F / Lang.N in the plugins (VamkitLang.cs), tr / mark in the launch
panel (lang.py). A table maps each of those texts to its language. This reports, per table:
  - a text of the code the table does not have (it would show in English);
  - a line of the table no code asks for;
  - a text whose {0}, {1} ... differ from the English ones (a crash or a wrong value when it is filled in).
Exit 0 when every table is complete and exact.

    python tools/check_lang.py                 check every table
    python tools/check_lang.py --keys          print the English texts, one JSON list
    python tools/check_lang.py --new ja        write components/shared/lang/ja.json with every text still in English,
                                               ready to be translated (add "@name": the language's own name)
"""
from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parent.parent
TABLES = REPO / "components" / "shared" / "lang"
CSHARP = re.compile(r'\bLang\.(?:T|F|N)\(\s*"((?:[^"\\]|\\.)*)"')
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "'": "'", "\\": "\\", "0": "\0"}
PLACE = re.compile(r"\{(\d+)\}")


def unescape(text):
    """A C# string literal's text."""
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        c = text[i + 1]
        if c == "u":
            out.append(chr(int(text[i + 2:i + 6], 16)))
            i += 6
        else:
            out.append(ESCAPES[c])
            i += 2
    return "".join(out)


def keys():
    """{English text: [where it is used]} of every plugin source and of the launcher."""
    found = {}
    for path in sorted((REPO / "components").rglob("*.cs")):
        if path.name == "VamkitLang.cs" or "tests" in path.parts:
            continue
        for match in CSHARP.finditer(path.read_text(encoding="utf-8-sig")):
            found.setdefault(unescape(match.group(1)), []).append(path.relative_to(REPO).as_posix())
    for path in sorted((REPO / "components" / "render" / "launcher").glob("*.py")):
        if path.name == "lang.py":
            continue
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8-sig"))):
            if isinstance(node, ast.Call) and node.args and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str):
                name = node.func.id if isinstance(node.func, ast.Name) else node.func.attr if isinstance(node.func, ast.Attribute) else ""
                if name in ("tr", "mark"):
                    found.setdefault(node.args[0].value, []).append(path.relative_to(REPO).as_posix())
    return found


def check(code, table, wanted):
    problems = []
    for key in wanted:
        if key not in table:
            problems.append(f"{code}: no line for {key!r}  ({wanted[key][0]})")
        elif set(PLACE.findall(key)) != set(PLACE.findall(table[key])):
            problems.append(f"{code}: {key!r} -> {table[key]!r}: the {{0}}, {{1}} ... differ")
    for key in table:
        if key not in wanted and not key.startswith("@"):
            problems.append(f"{code}: a line no code asks for: {key!r}")
    if not table.get("@name"):
        problems.append(f"{code}: no \"@name\" (the language's own name, shown in the launch panel)")
    return problems


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--keys", action="store_true", help="print the English texts as a JSON list")
    ap.add_argument("--new", metavar="CODE", help="write a table for a new language with every text still in English")
    args = ap.parse_args()
    wanted = keys()
    if args.keys:
        print(json.dumps(sorted(wanted), ensure_ascii=False, indent=1))
        return 0
    if args.new:
        path = TABLES / (args.new + ".json")
        if path.exists():
            print(f"{path} is there already")
            return 1
        table = {"@name": args.new}
        table.update((key, key.split("##", 1)[0]) for key in sorted(wanted))
        path.write_text(json.dumps(table, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        print(f"wrote {path.relative_to(REPO)}: {len(wanted)} texts to translate; then add it to the `lang` component's installs in manifest/components.toml")
        return 0
    problems = []
    tables = sorted(TABLES.glob("*.json"))
    for path in tables:
        try:
            table = json.loads(path.read_text(encoding="utf-8-sig"))
        except ValueError as error:
            problems.append(f"{path.stem}: not JSON: {error}")
            continue
        if not isinstance(table, dict) or not all(isinstance(v, str) for v in table.values()):
            problems.append(f"{path.stem}: not a flat object of texts")
            continue
        problems += check(path.stem, table, wanted)
    for line in problems:
        print(line)
    print(f"{len(problems)} problem(s) in {len(tables)} language table(s) against {len(wanted)} texts of the code")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
