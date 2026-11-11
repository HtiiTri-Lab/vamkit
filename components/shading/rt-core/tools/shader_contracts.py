"""Extract actual stock shader render states for the PT material adapters.
Reads Unity's serialized metadata, including blending/culling/depth and property
names. No scene launches and no screenshot-based classification.
    python -I shader_contracts.py [out.json]   (VAMKIT_VAM)
-> <repo>/out/gen/skin: shader-contracts.json and PathShaderStates.cs (the checked-in components/shading/skin/
PathShaderStates.cs is a copy of that; copy it over by hand when the installed engine assets change).
"""
import json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from shader_sources import serialized_sources,S,GEN

def extract():
    nodes=S.P.load_type_tree(); result=[]
    for path,data in serialized_sources():
        endian,types,objects=S.B.read_serialized(data)
        for pid,start,size,kind in objects:
            if types[kind][0]!=48: continue
            tree=types[kind][1]
            value,_=S.P.parse(tree.nodes if tree else nodes,data,start,endian); parsed=value['m_ParsedForm']
            name=parsed['m_Name'];rows=[]
            for sub in parsed['m_SubShaders']:
                for p in sub['m_Passes']: rows.append(p.get('m_State',{}))
            result.append({'name':name,'source':str(path),'properties':parsed.get('m_PropInfo',{}),'passes':rows})
    return result

def generate(data,target):
    lines=['// Generated from stock Unity shader render states by tools/path_tracing/shader_contracts.py.',
           '// Refresh when the installed engine assets change; runtime overrides are read from Material.',
           'using UnityEngine;','internal static class PathShaderStates {',
           '  internal struct State { public int source,destination,cull; public bool known; }',
           '  private static int Value(Material m,string property,int fallback) { return property.Length>0 && m.HasProperty(property)?(int)m.GetFloat(property):fallback; }',
           '  internal static State Read(Material m,string name) { State s=new State(); s.known=true; switch(name) {']
    seen=set()
    for row in data:
        if row['name'] in seen: continue
        seen.add(row['name']); passes=row['passes']
        if not passes: continue
        p=next((p for p in passes if any(t['first'].upper()=='LIGHTMODE' and t['second'].upper()=='FORWARDBASE' for t in p.get('m_Tags',{}).get('tags',[]))),passes[0])
        values=[p['rtBlend0']['srcBlend'],p['rtBlend0']['destBlend'],p['culling']]
        expressions=[]
        for v in values:
            prop=v['name'] if v['name']!='<noninit>' else ''
            expressions.append(f'Value(m,{json.dumps(prop)},{int(v["val"])})')
        lines.append(f'    case {json.dumps(row["name"])}: s.source={expressions[0]};s.destination={expressions[1]};s.cull={expressions[2]};break;')
    lines+=['    default: s.known=false;break;','  } return s; }','}']
    target.write_text('\n'.join(lines)+'\n',encoding='utf-8')

if __name__=='__main__':
    output=Path(sys.argv[1]) if len(sys.argv)>1 else GEN/'shader-contracts.json'; data=extract()
    output.write_text(json.dumps(data,indent=2),encoding='utf-8')
    generate(data,GEN/'PathShaderStates.cs')
    print(json.dumps({'shaders':len(data),'file':str(output)}))
    for row in data:
        if row['name'] in ['Custom/Subsurface/AlphaMaskComputeBuff','Marmoset/Transparent/Simple Glass/Specular IBLComputeBuff','Custom/Subsurface/TransparentComputeBuff','Standard']:
            p=row['passes'][0];b=p['rtBlend0']
            print(json.dumps({'name':row['name'],'blend':[b['srcBlend'],b['destBlend']], 'cull':p['culling'],'zwrite':p['zWrite']}))
