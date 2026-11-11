"""Find a Unity 2018.1 Shader (class 48) type tree in VaM's StreamingAssets bundles
and save it (json) for parsing type-tree-less resources.assets. The tree is read out of
the user's own VaM, so it is generated into <repo>/out/gen/shader-lift/shader_tt.json
and never kept in the repo (parse_shader.load_type_tree makes it on first use).
usage: python -I find_shader_tt.py [<StreamingAssets dir> [<out.json>]]
(defaults: %VAMKIT_VAM%/VaM_Data/StreamingAssets, <repo>/out/gen/shader-lift/shader_tt.json)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bundle_audit as B  # noqa: E402


def default_out():
    d = HERE
    while not os.path.isdir(os.path.join(d, "components")):
        up = os.path.dirname(d)
        if up == d:
            raise SystemExit("find_shader_tt.py: no folder holding components/ above " + HERE)
        d = up
    return os.path.join(d, "out", "gen", "shader-lift", "shader_tt.json")


def default_root():
    v = os.environ.get("VAMKIT_VAM")
    if not v:
        raise SystemExit("set VAMKIT_VAM to the VaM install folder (the one holding VaM_Data)")
    root = os.path.join(v, "VaM_Data", "StreamingAssets")
    if not os.path.isdir(root):
        raise SystemExit("VAMKIT_VAM=%s has no VaM_Data/StreamingAssets folder" % v)
    return root


def find(root):
    """(nodes, bundle, inner file) of the first Shader type tree, smallest bundles first; None if there is none"""
    files = sorted((os.path.getsize(os.path.join(root, f)), f) for f in os.listdir(root)
                   if os.path.isfile(os.path.join(root, f)) and not f.endswith((".manifest", ".meta")))
    for size, name in files:
        if size > 80_000_000:
            continue
        buf = open(os.path.join(root, name), "rb").read()
        if not buf.startswith(b"UnityFS"):
            continue
        try:
            nodes = B.read_unityfs(buf)
        except Exception as e:  # noqa: BLE001
            print("skip", name, e)
            continue
        for inner, data in nodes.items():
            if inner.endswith((".resS", ".resource")):
                continue
            try:
                E, types, objs = B.read_serialized(data)
            except Exception:  # noqa: BLE001
                continue
            for cid, tt in types:
                if cid == 48 and tt:
                    return tt.nodes, name, inner
    return None


def write(nodes, out):
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("[\n" + ",\n".join(json.dumps(list(n), ensure_ascii=False) for n in nodes) + "\n]\n")


def generate(root=None, out=None):
    """Writes the type tree json (default places) and returns its path"""
    root, out = root or default_root(), out or default_out()
    found = find(root)
    if not found:
        raise SystemExit("no shader type tree found in " + root)
    nodes, name, inner = found
    write(nodes, out)
    print("shader type tree: found in", name, inner, len(nodes), "nodes ->", out)
    return out


if __name__ == "__main__":
    generate(sys.argv[1] if len(sys.argv) > 1 else None, sys.argv[2] if len(sys.argv) > 2 else None)
