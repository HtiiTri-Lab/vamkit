"""Which material inputs each PT-supported stock shader actually reads, against what PT captures.

Read-only. For every supported shader name, take the lit pixel programs (ForwardBase / ForwardAdd, all keyword
variants) from the installed assets and z_sha / p_sha_mat bundles; list the $Globals members and textures they read.
Engine globals (lights, matrices, Skyshop sky) are separated from per-material inputs. A material input that some lit
variant reads but RtScene.MaterialFor never captures is an omission candidate.

    python -I material_coverage.py [out.json]   (VAMKIT_VAM; default <repo>/out/gen/skin/audit/coverage.json)
"""
import json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from shader_sources import serialized_sources, S, ROOT, GEN

SUPPORTED = re.compile(r'^(Standard|Standard \(Specular setup\)|Custom/Subsurface/.*|Custom/Hair/.*|Marmoset/.*|Unlit/.*|'
                       r'Legacy Shaders/(Diffuse|Specular|Transparent/Diffuse|Transparent/Specular)|.*Custom/Mirror.*)$')
ENGINE = re.compile(r'^(_LightColor0|_WorldSpaceLightPos0|_LightTexture0|_LightTextureB0|_ShadowMapTexture|_LightShadowData|'
                    r'unity_.*|_ProjectionParams|_ScreenParams|_ZBufferParams|_Time|_SinTime|_CosTime|_WorldSpaceCameraPos|'
                    r'_LightPositionRange|_LightProjectionParams|_ShadowOffsets.*|_SH\d|_SkyMatrix|_InvSkyMatrix|_SkySize|'
                    r'_SkyMin|_SkyMax|_ExposureIBL|_ExposureLM|_SpecCubeIBL|_DiffCubeIBL|_SkyCubeIBL|_UniformOcclusion|'
                    r'_BlendWeightIBL|_SpecCubeIBL1|_DiffCubeIBL1|_SH1_\d|_SkyMatrix1|_InvSkyMatrix1|_SkySize1|_SkyMin1|'
                    r'_SkyMax1|_EmissionLM|_LightTexture.*|_Shadow.*|_CameraDepthTexture|_GrabTexture|glstate_.*|_Object2World|'
                    r'_World2Object|_LightMatrix0|_LightAsQuad|_MainTex_TexelSize|.*_TexelSize|_TessEdge|_Tess|_TessPhong|'
                    r'_Displacement|_DispTex|_MaxTess|_MinTess|_TessDistance|_Phong)$')

def lit_programs(summary):
    for sub in summary['subshaders']:
        for ps in sub['passes']:
            mode = ps['tags'].get('LIGHTMODE', '').upper()
            if mode in ('FORWARDBASE', 'FORWARDADD', 'ALWAYS', ''):
                for prog in ps['stages']['progFragment']:
                    yield mode or 'ALWAYS', prog

def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else GEN / 'audit/coverage.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    rt = (ROOT / 'skin/RtScene.cs').read_text(encoding='utf-8')
    body = rt[rt.index('private static MatInfo MaterialFor'):rt.index('private static bool[] SkinNoOcclude')]
    captured = set(re.findall(r'"(_[A-Za-z0-9]+)"', body))
    captured |= {'_GlossTex', '_SpecTex', '_SpecGlossMap', '_MetallicGlossMap'}  # surfaceName / vamGloss
    result = {}
    for source, data in serialized_sources():
        for name, summary, blobs, pid in S.load(r'.*', data):
            if not SUPPORTED.match(name) or name in result:
                continue
            members, textures, keywords = {}, {}, set()
            for mode, prog in lit_programs(summary):
                kw = tuple(prog['keywords'])
                keywords.update(kw)
                for cb in prog['cbuffers']:
                    if cb['name'] != '$Globals':
                        continue
                    for m in cb['members']:
                        members.setdefault(m, set()).add(mode)
                for t in prog.get('textures', []):
                    tn = t[0] if isinstance(t, (list, tuple)) else t.get('name', str(t)) if isinstance(t, dict) else str(t)
                    textures.setdefault(tn, set()).add(mode)
            inputs = sorted(k for k in set(members) | set(textures) if not ENGINE.match(k))
            result[name] = {
                'source': source,
                'material_inputs': inputs,
                'missed': [k for k in inputs if k not in captured and not k.endswith('_ST')],
                'missed_st': [k for k in inputs if k.endswith('_ST') and k[:-3] not in captured],
                'keywords': sorted(keywords),
            }
    out.write_text(json.dumps(result, indent=1, sort_keys=True), encoding='utf-8')
    missed = {}
    for name, r in result.items():
        for k in r['missed']:
            missed.setdefault(k, []).append(name)
    print('shaders', len(result), 'captured names', len(captured))
    for k, names in sorted(missed.items(), key=lambda kv: -len(kv[1])):
        print('%-28s %3d  %s' % (k, len(names), ', '.join(names[:4]) + (' ...' if len(names) > 4 else '')))

if __name__ == '__main__':
    main()
