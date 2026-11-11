"""Canonical installed shader sources, including the game's shader bundles.
Shader names are not limited to the three always-loaded engine asset files.
No scene is started. Caller may additionally supply a specific asset bundle.
VaM is found through VAMKIT_VAM (skin_shaders.vam_root); GEN is <repo>/out/gen/skin, where every tool here writes.
"""
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]  # components/shading
sys.path.insert(0,str(ROOT/'skin/tools'))
import skin_shaders as S
GEN=Path(S.gen_dir())

def serialized_sources(extra=()):
    data=Path(S.vam_root())/'VaM_Data'
    for path in sorted(data.glob('*.assets'))+[data/'Resources/unity_builtin_extra']:
        yield str(path),path.read_bytes()
    # Shared shader dependencies named by StandaloneWindows64.manifest.
    for path in [data/'StreamingAssets/z_sha',data/'StreamingAssets/p_sha_mat']+list(extra):
        for member,blob in S.B.read_unityfs(Path(path).read_bytes()).items():
            if not member.endswith(('.resS','.resource')):yield str(path)+'/'+member,blob
