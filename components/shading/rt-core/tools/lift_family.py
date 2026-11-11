"""Lift the lit ForwardBase pixel program of stock shaders (any installed source) to HLSL, read-only.

    python -I lift_family.py <name regex> [out dir] [keyword ...]   (VAMKIT_VAM, VAMKIT_FXC; default out dir
    <repo>/out/gen/skin/audit/lifted)
Picks the variant with every requested keyword (MARMO_LINEAR is preferred when present), otherwise the first.
"""
import json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from shader_sources import serialized_sources, S, GEN
import asm_lift as A

pattern = re.compile(sys.argv[1])
out = Path(sys.argv[2]) if len(sys.argv) > 2 else GEN / 'audit/lifted'
want = set(sys.argv[3:])
out.mkdir(parents=True, exist_ok=True)
done = set()
for source, data in serialized_sources():
    for name, summary, blobs, pid in S.load(r'.*', data):
        if not pattern.match(name) or name in done:
            continue
        for sub in summary['subshaders']:
            for ps in sub['passes']:
                if ps['tags'].get('LIGHTMODE', '').upper() not in ('FORWARDBASE', 'ALWAYS', ''):
                    continue
                programs = ps['stages']['progFragment']
                if not programs:
                    continue
                pick = [p for p in programs if want <= set(p['keywords'])] or programs
                selected = next((p for p in pick if 'MARMO_LINEAR' in p['keywords']), pick[0])
                stem = name.replace('/', '_').replace(' ', '_')
                try:
                    text = A.Program(blobs[selected['blob']]).hlsl({}, '', precise=True)
                    (out / (stem + '.hlsl')).write_text(text, encoding='utf-8')
                except ValueError:  # an instruction the lifter lacks: keep the disassembly
                    (out / (stem + '.asm')).write_text(S.disassemble(blobs[selected['blob']]), encoding='utf-8')
                (out / (stem + '.json')).write_text(json.dumps(selected, indent=1), encoding='utf-8')
                print(name, selected['keywords'], sorted(selected['textures']))
                done.add(name)
                break
            if name in done:
                break
