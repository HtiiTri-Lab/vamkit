"""Matched managed/native identities for each stock shader's actual PS programs.
-> <repo>/out/gen/skin: PathShaderPrograms.cs, path_program_sets.h, program-sets.json. python -I build_program_sets.py (VAMKIT_VAM)"""
import hashlib,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from shader_sources import serialized_sources,S,GEN
sets={}
for source,data in serialized_sources():
    for name,summary,blobs,pid in S.load(r'.*',data):
        programs=sets.setdefault(name,set())
        for sub in summary['subshaders']:
            for stage in sub['passes']:
                for variant in stage['stages']['progFragment']:
                    code=blobs[variant['blob']]
                    if code:programs.add(int.from_bytes(hashlib.sha1(code).digest()[:8],'big'))
names=sorted(sets);flat=[];ranges=[(0,0)]
cs=['// Generated together with path_program_sets.h by build_program_sets.py.',
    'internal static class PathShaderPrograms { internal static uint Id(string name) { switch(name) {']
for index,name in enumerate(names,1):
    hashes=sorted(sets[name]);ranges.append((len(flat),len(hashes)));flat.extend(hashes)
    cs.append('case '+json.dumps(name)+':return '+str(index)+'u;')
cs+=['default:return 0;','} } }']
(GEN/'PathShaderPrograms.cs').write_text('\n'.join(cs)+'\n',encoding='utf-8')
header='// Generated together with PathShaderPrograms.cs.\n'
header+='static const uint64_t kPathShaderPrograms[]={\n'+''.join('0x%016xull,\n'%h for h in flat)+'};\n'
header+='static const uint32_t kPathShaderProgramRanges[][2]={\n'+''.join('{%d,%d},\n'%r for r in ranges)+'};\n'
(GEN/'path_program_sets.h').write_text(header,encoding='utf-8')
(GEN/'program-sets.json').write_text(json.dumps({i+1:{'name':n,'programs':[format(h,'016x') for h in sorted(sets[n])]} for i,n in enumerate(names)},indent=2),encoding='utf-8')
print('shader program sets',len(names),'PS entries',len(flat))
