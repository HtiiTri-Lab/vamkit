"""Group PT-supported stock shaders by their lit ForwardBase pixel program, with blend/cull/zwrite per pass.

Read-only. ComputeBuff variants share pixel bytecode, so one semantic review per group covers every member.
    python -I family_groups.py [out.json]   (VAMKIT_VAM; default <repo>/out/gen/skin/audit/groups.json; reads
    out/gen/skin/shader-contracts.json, which shader_contracts.py writes)
"""
import hashlib, json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from shader_sources import serialized_sources, S, GEN
from material_coverage import SUPPORTED, ENGINE

out = Path(sys.argv[1]) if len(sys.argv) > 1 else GEN / 'audit/groups.json'
out.parent.mkdir(parents=True, exist_ok=True)
contracts = {x['name']: x for x in json.loads((GEN / 'shader-contracts.json').read_text(encoding='utf-8'))}
groups, seen = {}, set()
for source, data in serialized_sources():
    for name, summary, blobs, pid in S.load(r'.*', data):
        if not SUPPORTED.match(name) or name in seen:
            continue
        seen.add(name)
        key, info = None, None
        for sub in summary['subshaders']:
            for ps in sub['passes']:
                if ps['tags'].get('LIGHTMODE', '').upper() not in ('FORWARDBASE', 'ALWAYS', ''):
                    continue
                progs = ps['stages']['progFragment']
                if not progs:
                    continue
                p = next((p for p in progs if 'MARMO_LINEAR' in p['keywords'] and 'DIRECTIONAL' in p['keywords']),
                         next((p for p in progs if 'DIRECTIONAL' in p['keywords']), progs[0]))
                code = blobs[p['blob']]
                key = hashlib.sha1(code).hexdigest()[:16] if code else 'none'
                members = sorted(k for c in p['cbuffers'] if c['name'] == '$Globals' for k in c['members'] if not ENGINE.match(k))
                info = {'keywords': p['keywords'], 'textures': sorted(t for t in p['textures'] if not ENGINE.match(t)), 'members': members}
                break
            if key:
                break
        passes = []
        for p in contracts.get(name, {}).get('passes', []):
            b = p.get('rtBlend0', {})
            passes.append({'name': p.get('m_Name'), 'blend': [b.get('srcBlend', {}).get('val'), b.get('destBlend', {}).get('val')],
                           'zwrite': (p.get('zWrite') or {}).get('val') if isinstance(p.get('zWrite'), dict) else p.get('zWrite'),
                           'cull': (p.get('culling') or {}).get('val') if isinstance(p.get('culling'), dict) else p.get('culling')})
        g = groups.setdefault(key or 'none', {'program': info, 'shaders': []})
        g['shaders'].append({'name': name, 'passes': passes})
out.write_text(json.dumps(groups, indent=1), encoding='utf-8')
print('shaders', len(seen), 'groups', len(groups))
for k, g in sorted(groups.items(), key=lambda kv: kv[1]['shaders'][0]['name']):
    print(k, len(g['shaders']), g['shaders'][0]['name'], (g['program'] or {}).get('textures'))
