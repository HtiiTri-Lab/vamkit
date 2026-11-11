"""Checks manifest/components.toml and manifest/thirdparty.toml against the tree. Python standard library only.

Without options (needs nothing built):
  - every component folder, generator, native build script and managed source file exists;
  - every id under `needs` is an item of thirdparty.toml, every id under `uses` is a component;
  - every third_party/<id> folder a component's build scripts, generators and tests read is in its `needs`
    (a `needs` list is what `vamkit build` checks before it starts a component);
  - every test command's script inside the repository exists.
With --built (after `vamkit build`):
  - every generated source of a managed recipe exists;
  - every file an `installs` entry copies exists.

    python -I tools/check_manifest.py [--built]        lists what is wrong; exit 1 when anything is
"""
import re
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
READS = re.compile(r"(?:third_party|VAMKIT_THIRD_PARTY\}?)[/\\]+([a-z0-9][a-z0-9-]*)")
SCRIPTS = ("*.cmd", "*.py", "*.ps1", "*.cmake", "CMakeLists.txt")


def third_party_read(folder):
    """The third_party/<id> folders the build files directly inside one component name."""
    found = {}
    for pattern in SCRIPTS:
        for path in folder.rglob(pattern):
            rel = path.relative_to(folder).as_posix()
            if rel.startswith("third_party/"):   # a component's own vendored code, not the fetched folder
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for name in READS.findall(text):
                found.setdefault(name, rel)
    return found


def main():
    built = "--built" in sys.argv[1:]
    components = tomllib.loads((REPO / "manifest" / "components.toml").read_text(encoding="utf-8"))["component"]
    items = {i["id"] for i in tomllib.loads((REPO / "manifest" / "thirdparty.toml").read_text(encoding="utf-8"))["item"]}
    ids = {c["id"] for c in components}
    wrong = []

    def need(ok, cid, text):
        if not ok:
            wrong.append(f"{cid}: {text}")

    for c in components:
        cid, folder = c["id"], REPO / c["path"]
        need(folder.is_dir(), cid, f"no folder {c['path']}")
        for script in c.get("generate", []):
            need((REPO / script).is_file(), cid, f"no generator {script}")
        if c.get("native"):
            need((REPO / c["native"]).is_file(), cid, f"no native build script {c['native']}")
        managed = c.get("managed")
        if managed:
            for source in managed["sources"]:
                need((folder / source).is_file(), cid, f"no source {source}")
            for source in managed.get("shared", []):
                need((REPO / "components" / "shared" / source).is_file(), cid, f"no shared source {source}")
            for resource, _ in managed.get("resources", []):
                need((folder / resource).is_file(), cid, f"no resource {resource}")
            if built:
                for generated in managed.get("generated", []):
                    need((REPO / generated).is_file(), cid, f"not generated: {generated}")
        for other in c.get("needs", []):
            need(other in items, cid, f"needs {other}, which thirdparty.toml does not have")
        for other in c.get("uses", []):
            need(other in ids, cid, f"uses {other}, which is not a component")
        if folder.is_dir():
            for name, where in sorted(third_party_read(folder).items()):
                if name in items:
                    need(name in c.get("needs", []), cid, f"{where} reads third_party/{name}, which is not in its needs")
        for command in c.get("tests", []):
            for arg in command:
                if arg.startswith("{repo}/") and not arg.startswith("{repo}/out/"):
                    need((REPO / arg[len("{repo}/"):]).exists(), cid, f"test names {arg}, which does not exist")
        if built:
            for source, _ in c.get("installs", []):
                need((REPO / source).is_file(), cid, f"installs {source}, which the build did not make")

    for line in wrong:
        print(line)
    print(f"{len(wrong)} problem(s) in the manifest" + ("" if built else " (run with --built after a build to check its outputs too)"))
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
