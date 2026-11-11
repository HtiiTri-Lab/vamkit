r"""shaders\mouth.hlsl -> <repo>\out\gen\skin\mouth_shaders.h (native\mouth_gpu.cpp). python -I gen_mouth.py (VAMKIT_FXC)"""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import skin_shaders as S
root=Path(__file__).resolve().parents[1]
src=(root/'shaders/mouth.hlsl').read_text()
with (Path(S.gen_dir())/'mouth_shaders.h').open('w') as f:
    f.write('#pragma once\n')
    for entry in ('Prepare','Refit','Trace'):
        code=S.compile_hlsl(src,entry,'cs_5_0',name='mouth.hlsl')
        f.write('static const unsigned char kMouth%s[]={%s};\n'%(entry,','.join(str(b) for b in code)))
