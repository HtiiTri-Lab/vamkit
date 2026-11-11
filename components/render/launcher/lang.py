"""The words of the launch panel in the user's language; the in-game menus read the same tables (VamkitLang.cs).

English is the text in the code: tr("Launch VaM"). A language is one table, <VaM>/vamkit/lang/<code>.json: a flat
JSON object, English text -> that language's text, with "@name" the language's own name. A text the table does not
have stays English.

Which language: profiles/language.json {"language": "<code>"}, written by the panel's language row; without it,
Windows' display language (PRIMARY below), and English when that has no table.

One English word that needs two words in another language carries a context after "##": tr("Off##choice"); the
context is never shown. mark() names an English text kept in data and translated later with tr(variable);
tools/check_lang.py finds the keys by these two names.
"""
from __future__ import annotations

import ctypes
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent
# Installed: <VaM>/vamkit/lang beside <VaM>/vamkit/launcher. In the repository: components/shared/lang.
FOLDERS = (ROOT.parent / "lang", ROOT.parent.parent / "shared" / "lang")
CHOICE = ROOT / "profiles" / "language.json"
# The primary language of Windows' LANGID -> a table's code. VamkitLang.cs has the same list; a new table for a
# language that is not here adds its line to both.
PRIMARY = {0x04: "zh", 0x07: "de", 0x0A: "es", 0x0C: "fr", 0x10: "it", 0x11: "ja", 0x12: "ko", 0x16: "pt", 0x19: "ru"}
CODE = re.compile(r"[A-Za-z0-9_-]{1,24}")

_table = None
_code = "en"


def folder():
    return next((f for f in FOLDERS if f.is_dir()), FOLDERS[0])


def windows():
    """Windows' display language as a table's code, "en" when it is not in PRIMARY."""
    try:
        return PRIMARY.get(ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF, "en")
    except (AttributeError, OSError):
        return "en"


def read(code):
    """A language's table, {} when there is none or it cannot be read. Only texts are kept."""
    if code == "en" or not CODE.fullmatch(code):
        return {}
    try:
        found = json.loads((folder() / (code + ".json")).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return {}
    return {k: v for k, v in found.items() if isinstance(v, str)} if isinstance(found, dict) else {}


def languages():
    """[(code, the language's own name)]: English, then every table in the folder."""
    found = [("en", "English")]
    try:
        tables = sorted(folder().glob("*.json"))
    except OSError:
        tables = []
    for path in tables:
        table = read(path.stem)
        if table:
            found.append((path.stem, table.get("@name", path.stem)))
    return found


def chosen():
    """The language the user chose in the panel, None while Windows' is followed."""
    try:
        code = json.loads(CHOICE.read_text(encoding="utf-8-sig")).get("language")
    except (OSError, ValueError, AttributeError):
        return None
    return code if isinstance(code, str) and CODE.fullmatch(code) else None


def current():
    """The language in use: the chosen one, else Windows'; "en" when that has no table."""
    load()
    return _code


def load(again=False):
    global _table, _code
    if _table is not None and not again:
        return
    code = chosen() or windows()
    _table = read(code)
    _code = code if _table else "en"


def choose(code):
    """Keeps the user's choice for the panel and for the in-game menus of the next start, and uses it from now on."""
    CHOICE.parent.mkdir(parents=True, exist_ok=True)
    temporary = CHOICE.with_name(CHOICE.name + ".tmp")
    temporary.write_text(json.dumps({"language": code}) + "\n", encoding="utf-8")
    temporary.replace(CHOICE)
    load(again=True)


def tr(english, *values):
    """The text in the language in use, with {0}, {1} ... filled in from `values`."""
    load()
    text = _table.get(english)
    if text is None:
        text = english.split("##", 1)[0]
    return text.format(*values) if values else text


def mark(english):
    """Marks an English text that is kept as data and shown later through tr(variable)."""
    return english
