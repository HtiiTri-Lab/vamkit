"""Compile every embedded reconstruction/composition shader before a game run.
python -I build_composite_shaders.py (VAMKIT_FXC: the Windows SDK bin/<version>/x64 folder, or fxc.exe); the
compiled objects go to <repo>/out/gen/rt-core/composite-check."""
import os,re,subprocess
from pathlib import Path
repo=next(p for p in Path(__file__).resolve().parents if (p/'components').is_dir())
oc=repo/'components/render/opencomposite/OpenOVR/Compositor'
source=(oc/'vam_rr.inl').read_text()
out=repo/'out/gen/rt-core/composite-check';out.mkdir(parents=True,exist_ok=True)
fxc=os.environ.get('VAMKIT_FXC')
if not fxc:raise SystemExit('set VAMKIT_FXC to the Windows SDK bin/<version>/x64 folder (fxc.exe)')
fxc=Path(fxc) if fxc.lower().endswith('.exe') else Path(fxc)/'fxc.exe'
for symbol,entries in [('rrShader',[(e,'cs_5_0') for e in ('Prepare','Detail','DetailPt','Composite')]+[('OverlayVs','vs_5_0'),('OverlayPs','ps_5_0'),('HandoffPs','ps_5_0')]),
                       ('rrPathGuideShader',[('main','cs_5_0')])]:
    body=re.search(r'const char\* '+symbol+r'\s*=\s*R"\((.*?)\)";',source,re.S).group(1)
    path=out/(symbol+'.hlsl');path.write_text(body)
    for entry,target in entries:
        subprocess.run([str(fxc),'/nologo','/WX','/O3','/T',target,'/E',entry,'/Fo',str(out/(symbol+'-'+entry+'.cso')),str(path)],check=True)
        print(symbol,entry,'compiled')
ui=(oc/'dx11_vam_dlss.cpp').read_text()
body=re.search(r'constexpr char uiShader\[\]\s*=\s*R"\((.*?)\)";',ui,re.S).group(1)
path=out/'uiShader.hlsl';path.write_text(body)
for entry,target in [('vertexMain','vs_5_0'),('pixelMain','ps_5_0')]:
    subprocess.run([str(fxc),'/nologo','/WX','/O3','/T',target,'/E',entry,'/Fo',str(out/('uiShader-'+entry+'.cso')),str(path)],check=True)
    print('UI',entry,'compiled')
