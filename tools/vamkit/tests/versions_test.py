"""vamkit versions: the comparison, the reference file and the notes, without depending on this machine.

  python -I tools/vamkit/tests/versions_test.py

Checks that the comparison marks lines right, that collecting works on a machine with none of the tools (nothing found
must not raise), that the published reference holds versions only (no paths, user or machine names), and that every
document a note names exists.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import sys
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import versions  # noqa: E402

failures = []


def expect(what, ok):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


# The comparison: same, differs, new; info-only lines and groups only one side has are left out.
ours = {"Windows": {"d3d11on12.dll": "10.0.19041.5794", "dxgi.dll": "10.0.26100.9818", "hardware GPU scheduling": "on"},
        "VaM": {"Unity": "2018.1.9.52265"},
        "Kit": {"commit": "abc1234", "captured": "2026-11-13"}}
theirs = {"Windows": {"d3d11on12.dll": "10.0.26100.9818", "dxgi.dll": "10.0.26100.9818"},
          "Graphics": {"GPU": "NVIDIA GeForce RTX 5090 D"},
          "Kit": {"commit": "8cc6fc8", "captured": "2026-11-12"}}
rows = {(g, k): s for g, k, _, _, s in versions.compare(ours, theirs)}
expect("a changed version is 'differs'", rows.get(("Windows", "d3d11on12.dll")) == "differs")
expect("an equal version is 'same'", rows.get(("Windows", "dxgi.dll")) == "same")
expect("a line the reference lacks is 'new'", rows.get(("Windows", "hardware GPU scheduling")) == "new")
expect("a group the reference lacks is not compared", ("VaM", "Unity") not in rows)
expect("the commit and the date are not compared", ("Kit", "commit") not in rows and ("Kit", "captured") not in rows)
expect("differences() leaves out the same lines",
       sorted(k for _, k, *_ in versions.differences(ours, theirs)) == ["d3d11on12.dll", "hardware GPU scheduling"])

# Collecting on a machine where nothing is found: no tools, no versions, no command output.
bare = SimpleNamespace(file_version=lambda p: None, run=lambda a: "", repo=REPO, runtimes=lambda: [],
                       tools=lambda: {"VAMKIT_VCVARS": None, "VAMKIT_FXC": None, "VAMKIT_DXC": None, "VAMKIT_CSC": None})
try:
    found = versions.collect(bare, None)
    expect("collect() runs with nothing found", all(isinstance(v, str) for g in found.values() for v in g.values()))
    expect("collect() leaves VaM out without a VaM folder", "VaM" not in found)
except Exception as e:  # noqa: BLE001 - any exception is the failure being tested
    expect(f"collect() runs with nothing found ({type(e).__name__}: {e})", False)

# The published reference: present, the main groups, and versions only.
ref = versions.reference()
expect("docs/agent/reference-machine.json loads", isinstance(ref, dict))
ref = ref or {}
for group in ("Windows", "Graphics", "VR", "Hardware", "VaM", "Kit run-time files", "Build tools"):
    expect(f"the reference has the group {group}", group in ref)
values = [v for g in ref.values() for v in g.values()]
leaks = [v for v in values if re.search(r"[A-Za-z]:[\\/]|\\\\|/Users/|/home/", v)]
expect("the reference holds no paths", not leaks)
for variable in ("USERNAME", "COMPUTERNAME", "USERDOMAIN"):
    name = os.environ.get(variable)
    if name and len(name) > 2:
        expect(f"the reference does not hold this machine's {variable}", not any(name.lower() in v.lower() for v in values))

# Every document a note sends the reader to exists.
for (group, key), note in versions.NOTES.items():
    for named in re.findall(r"(?:docs|components|tools)/[\w./-]+", note):
        expect(f"note {group} / {key}: {named} exists", (REPO / named.rstrip(".,")).exists())

print(f"{len(failures)} failure(s)" if failures else "all passed")
sys.exit(1 if failures else 0)
