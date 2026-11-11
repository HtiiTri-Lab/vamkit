"""Test of the launch panel's lang.py on the tables of the repository, and that it and VamkitLang.cs agree on which
Windows language is which table. Exit 0 when every check holds."""
from __future__ import annotations

from pathlib import Path
import re
import sys
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
sys.path.insert(0, str(REPO / "components" / "render" / "launcher"))
import lang  # noqa: E402

failed = 0


def check(ok, what):
    global failed
    if not ok:
        failed += 1
        print("FAIL ", what)


with tempfile.TemporaryDirectory() as temporary:
    lang.CHOICE = Path(temporary) / "profiles" / "language.json"      # never the launcher's own folder
    names = dict(lang.languages())
    check(names.get("en") == "English" and names.get("zh") == "中文", f"the languages are listed under their own names ({names})")
    lang.choose("zh")
    check(lang.chosen() == "zh" and lang.current() == "zh", "a choice is kept and used")
    check(lang.tr("Launch VaM") == "启动 VaM", "a text is shown in the language chosen")
    check(lang.tr("Off") == "关" and lang.tr("Off##choice") == "关闭", "one English word, two words by context")
    check(lang.tr("{0} upscaling", "XeSS") == "XeSS 超分", "values are filled in")
    check(lang.tr("not in any table##context") == "not in any table", "a text the table lacks stays English, without its context")
    check(lang.tr(lang.mark("Vanilla")) == "原版", "a marked text is translated when shown")
    lang.choose("en")
    check(lang.current() == "en" and lang.tr("Launch VaM") == "Launch VaM", "English is the text in the code")
    lang.choose("xx")
    check(lang.current() == "en", "a language without a table is English")
    lang.CHOICE.write_text("not json", encoding="utf-8")
    lang.load(again=True)
    check(lang.chosen() is None and lang.current() in names, "a choice file that cannot be read: Windows' language")

source = (REPO / "components" / "shared" / "lang" / "VamkitLang.cs").read_text(encoding="utf-8")
csharp = {int(number, 16): code for number, code in re.findall(r'case (0x[0-9A-Fa-f]+): return "([A-Za-z-]+)";', source)}
check(csharp == lang.PRIMARY, f"lang.py and VamkitLang.cs map Windows' languages alike ({csharp} / {lang.PRIMARY})")

print("ok" if not failed else f"{failed} check(s) failed")
sys.exit(1 if failed else 0)
