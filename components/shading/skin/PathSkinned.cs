using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using HarmonyLib;
using UnityEngine;

// Animated CUAs use the same draw/material bridge as rooms. Only bone matrices
// are calculated each frame on CPU; positions and normals are skinned by RtCore.
// Registry: CUA load callbacks plus one startup seed, never a timed scene scan.
internal static partial class RtScene
{
    private sealed class PathSkinEntry {
        public SkinnedMeshRenderer renderer;public Mesh mesh;public MeshEntry data;
        public Material[] materials;public Matrix4x4[] bind;public Transform[] bones;
        public ComputeBuffer weights;public IntPtr weightPointer,rows;
        public ShapeKnot[][] shapes;public int frameCount,rowFloats;
        public bool invalid;public int id;
        public string unavailable;  // 0.23.1: the status text, built once
    }
    private struct ShapeKnot {public float weight;public int frame;}
    private struct ShapeDelta {public int vertex,frame;public Vector3 position,normal,tangent;}
    private static readonly List<PathSkinEntry> pathSkins=new List<PathSkinEntry>();
    private static readonly HashSet<int> pathSkinIds=new HashSet<int>();
    private static string pathSkinError="";
    private static int pathSkinOffFrame=-1;
    private static GameObject pathSkinFixture;
    private static Mesh pathSkinFixtureMesh;
    private static Material pathSkinFixtureMaterial;
    // Explicit test agent fixture: verifies Unity's non-readable mesh getters,
    // registration and target weights. Never created during normal gameplay.
    public static string PathSkinFixture(bool enabled) {
        if(!enabled) {
            if(pathSkinFixture!=null)UnityEngine.Object.Destroy(pathSkinFixture);
            if(pathSkinFixtureMesh!=null)UnityEngine.Object.Destroy(pathSkinFixtureMesh);
            if(pathSkinFixtureMaterial!=null)UnityEngine.Object.Destroy(pathSkinFixtureMaterial);
            pathSkinFixture=null;pathSkinFixtureMesh=null;pathSkinFixtureMaterial=null;return "removed";
        }
        if(pathSkinFixture!=null)return "exists";
        pathSkinFixture=new GameObject("PT explicit CUA fixture");pathSkinFixture.transform.position=new Vector3(-20,10,10);
        var renderer=pathSkinFixture.AddComponent<SkinnedMeshRenderer>();
        Mesh mesh=pathSkinFixtureMesh=new Mesh();mesh.name="PT unreadable target fixture";
        mesh.vertices=new Vector3[]{new Vector3(-.5f,-.5f,0),new Vector3(.5f,-.5f,0),new Vector3(.5f,.5f,0),new Vector3(-.5f,.5f,0)};
        mesh.normals=new Vector3[]{Vector3.back,Vector3.back,Vector3.back,Vector3.back};
        mesh.uv=new Vector2[]{Vector2.zero,Vector2.right,Vector2.one,Vector2.up};mesh.triangles=new int[]{0,2,1,0,3,2};
        BoneWeight[] bw=new BoneWeight[4];for(int i=0;i<4;i++)bw[i].weight0=1;mesh.boneWeights=bw;
        mesh.bindposes=new Matrix4x4[]{Matrix4x4.identity};
        Vector3[] delta=new Vector3[]{Vector3.right*.1f,Vector3.right*.1f,Vector3.right*.1f,Vector3.right*.1f};
        mesh.AddBlendShapeFrame("target",100,delta,new Vector3[4],new Vector3[4]);mesh.RecalculateBounds();
        renderer.sharedMesh=mesh;renderer.bones=new Transform[]{pathSkinFixture.transform};renderer.rootBone=pathSkinFixture.transform;
        renderer.updateWhenOffscreen=true;pathSkinFixtureMaterial=new Material(Shader.Find("Standard"));renderer.sharedMaterial=pathSkinFixtureMaterial;
        renderer.SetBlendShapeWeight(0,50);mesh.UploadMeshData(true);PathCuaLoaded(renderer);
        return "readable="+mesh.isReadable+" weights="+mesh.boneWeights.Length+" bindposes="+mesh.bindposes.Length+" targets="+mesh.blendShapeCount;
    }
    private static bool pathShadowsDisabled;
    private static UnityEngine.ShadowQuality savedPathShadows;
    private static bool pathRasterPrepared;
    private static readonly HashSet<Material> pathInstancedMaterials=new HashSet<Material>();
    public static void RestorePathScene() {PathSceneMode(false);PathRasterMode(false);}
    private static void PathRasterMode(bool enabled) {
        pathRasterPrepared=enabled;
        if(!enabled&&pathInstancedMaterials.Count>0) {
            foreach(Material material in pathInstancedMaterials)if(material!=null)material.enableInstancing=true;
            pathInstancedMaterials.Clear();
        }
    }
    private static void PreparePathMaterialDraw(Material material) {
        // DXR already owns the physical instances. Keep the remaining raster
        // depth/forward draws individually reconcilable, including first handoff.
        if(pathRasterPrepared&&material.enableInstancing) {
            pathInstancedMaterials.Add(material);material.enableInstancing=false;
        }
    }
    private static void PathSceneMode(bool enabled) {
        if(enabled==pathShadowsDisabled)return;
        if(enabled) {savedPathShadows=QualitySettings.shadows;QualitySettings.shadows=UnityEngine.ShadowQuality.Disable;}
        else if(QualitySettings.shadows==UnityEngine.ShadowQuality.Disable)QualitySettings.shadows=savedPathShadows;
        pathShadowsDisabled=enabled;
    }
    private static void SetupPathSkins(Harmony harmony) {
        Type loader=Type.GetType("CustomUnityAssetLoader, Assembly-CSharp",true);
        PatchAll(harmony,loader,"DoLoadedCallbacks",typeof(RtScene).GetMethod("PathCuaLoaded",BindingFlags.Public|BindingFlags.Static));
        UnityEngine.Object[] found=Resources.FindObjectsOfTypeAll(loader);
        for(int i=0;i<found.Length;i++) {
            Component c=found[i] as Component;if(c!=null && c.gameObject.scene.IsValid())PathCuaLoaded(c);
        }
    }
    public static void PathCuaLoaded(Component __instance) {
        if(__instance==null)return;
        contentRevision++;rigidDirty=true;
        SkinnedMeshRenderer[] found=__instance.GetComponentsInChildren<SkinnedMeshRenderer>(true);
        for(int i=0;i<found.Length;i++)RegisterPathSkin(found[i]);
    }
    private static void RegisterPathSkin(SkinnedMeshRenderer renderer) {
        if(renderer==null || !renderer.gameObject.scene.IsValid())return;
        int id=renderer.GetInstanceID();if(!pathSkinIds.Add(id))return;
        pathSkins.Add(new PathSkinEntry{renderer=renderer,id=id});
    }
    private static void ReleasePathSkin(PathSkinEntry e) {
        if(e.weights!=null)e.weights.Release();e.weights=null;e.weightPointer=IntPtr.Zero;
        if(e.rows!=IntPtr.Zero)Marshal.FreeHGlobal(e.rows);e.rows=IntPtr.Zero;
        e.mesh=null;e.data=null;e.invalid=false;
        e.shapes=null;e.frameCount=e.rowFloats=0;
    }
    private static unsafe float[] PathSkinData(PathSkinEntry e,Mesh mesh,BoneWeight[] weights) {
        int vertices=mesh.vertexCount;
        var deltas=new List<ShapeDelta>();int[] counts=new int[vertices];
        e.shapes=new ShapeKnot[mesh.blendShapeCount][];e.frameCount=0;
        Vector3[] dp=e.shapes.Length>0?new Vector3[vertices]:null,dn=e.shapes.Length>0?new Vector3[vertices]:null,dt=e.shapes.Length>0?new Vector3[vertices]:null;
        for(int shape=0;shape<e.shapes.Length;shape++) {
            var knots=new List<ShapeKnot>();bool zero=false;
            for(int f=0;f<mesh.GetBlendShapeFrameCount(shape);f++) {
                float weight=mesh.GetBlendShapeFrameWeight(shape,f);zero|=weight==0;
                int index=e.frameCount++;knots.Add(new ShapeKnot{weight=weight,frame=index});
                mesh.GetBlendShapeFrameVertices(shape,f,dp,dn,dt);
                for(int v=0;v<vertices;v++)if(dp[v].sqrMagnitude>0 || dn[v].sqrMagnitude>0 || dt[v].sqrMagnitude>0) {
                    deltas.Add(new ShapeDelta{vertex=v,frame=index,position=dp[v],normal=dn[v],tangent=dt[v]});counts[v]++;
                }
            }
            if(!zero)knots.Add(new ShapeKnot{weight=0,frame=-1});
            knots.Sort(delegate(ShapeKnot a,ShapeKnot b){return a.weight.CompareTo(b.weight);});
            e.shapes[shape]=knots.ToArray();
        }
        // CSR deltas: only vertices affected by a target occupy storage.
        // This conversion runs once at bind; each frame uploads scalar weights only.
        int floats=checked(vertices*(e.frameCount>0?10:8)+deltas.Count*12);
        float[] data=new float[floats];
        GCHandle pin=GCHandle.Alloc(weights,GCHandleType.Pinned);
        try {Marshal.Copy(pin.AddrOfPinnedObject(),data,0,vertices*8);}finally{pin.Free();}
        if(e.frameCount==0)return data;
        int[] offsets=new int[vertices];int at=vertices*10;
        fixed(float* p=data) {
            for(int v=0;v<vertices;v++) {
                offsets[v]=at;*((uint*)(p+vertices*8+v*2))=(uint)(at*4);
                *((uint*)(p+vertices*8+v*2+1))=(uint)counts[v];at+=counts[v]*12;
            }
            for(int k=0;k<deltas.Count;k++) {
                ShapeDelta d=deltas[k];int o=offsets[d.vertex];offsets[d.vertex]+=12;
                *((uint*)(p+o))=(uint)d.frame;p[o+1]=d.position.x;p[o+2]=d.position.y;p[o+3]=d.position.z;
                p[o+4]=d.normal.x;p[o+5]=d.normal.y;p[o+6]=d.normal.z;
                p[o+8]=d.tangent.x;p[o+9]=d.tangent.y;p[o+10]=d.tangent.z;
            }
        }
        return data;
    }
    private static unsafe void PathShapeWeights(PathSkinEntry e,float* output) {
        for(int f=0;f<e.frameCount;f++)output[f]=0;
        for(int s=0;s<e.shapes.Length;s++) {
            ShapeKnot[] knots=e.shapes[s];if(knots.Length<2)continue;
            float weight=e.renderer.GetBlendShapeWeight(s);if(float.IsNaN(weight)||float.IsInfinity(weight))continue;
            int hi=1;while(hi<knots.Length-1 && weight>knots[hi].weight)hi++;
            ShapeKnot a=knots[hi-1],b=knots[hi];float span=b.weight-a.weight;
            if(span<=0)continue;float t=(weight-a.weight)/span;
            if(a.frame>=0)output[a.frame]=1-t;if(b.frame>=0)output[b.frame]=t;
        }
    }
    private static bool BindPathSkin(PathSkinEntry e,Mesh mesh,int frame) {
        ReleasePathSkin(e);e.mesh=mesh;e.invalid=true;
        // Unity exposes bind poses/weights separately from the vertex read flag.
        // Use the actual getters; the rest vertices stay in native GPU buffers.
        e.bind=mesh.bindposes;e.bones=e.renderer.bones;
        BoneWeight[] weights=e.bind.Length>0?mesh.boneWeights:new BoneWeight[mesh.vertexCount];
        if(e.bind.Length==0){e.bind=new Matrix4x4[]{Matrix4x4.identity};e.bones=new Transform[]{e.renderer.transform};for(int v=0;v<weights.Length;v++)weights[v].weight0=1;}
        if(weights.Length!=mesh.vertexCount||e.bind.Length!=e.bones.Length||e.bones.Length>16384)
            throw new InvalidOperationException("CUA skin data unavailable: "+mesh.name);
        e.data=MeshFor(mesh,frame,true);if(e.data==null)return false;UvFor(mesh,e.data);
        float[] packed=PathSkinData(e,mesh,weights);
        e.weights=new ComputeBuffer(packed.Length,4,ComputeBufferType.Raw);e.weights.SetData(packed);e.weightPointer=e.weights.GetNativeBufferPtr();
        e.rowFloats=e.bones.Length*12+e.frameCount;
        e.rows=Marshal.AllocHGlobal(checked(Slots*e.rowFloats*4));e.materials=e.renderer.sharedMaterials;e.invalid=false;
        return true;
    }
    private static unsafe int AddPathSkins(int mask,int frame,byte* draws,int n) {
        if(!VaMSkinTransmission.RtWanted()) {
            if(pathSkinOffFrame<0)pathSkinOffFrame=frame;
            // The renderer reports the switch after retiring CUA owners. Keep
            // main-thread rows past the queued scene-record slots, then release.
            if(frame-pathSkinOffFrame>=Slots+2)for(int i=0;i<pathSkins.Count;i++)
                if(pathSkins[i].mesh!=null)ReleasePathSkin(pathSkins[i]);
            return n;
        }
        pathSkinOffFrame=-1;pathSkinError="";
        for(int i=pathSkins.Count-1;i>=0;i--) {
            PathSkinEntry e=pathSkins[i];SkinnedMeshRenderer r=e.renderer;
            if(r==null){ReleasePathSkin(e);pathSkinIds.Remove(e.id);pathSkins.RemoveAt(i);continue;}
            if(!r.enabled||!r.gameObject.activeInHierarchy||(mask&(1<<r.gameObject.layer))==0)continue;
            if(!VaMSkinTransmission.PathSceneActive && (r.shadowCastingMode==UnityEngine.Rendering.ShadowCastingMode.Off || r.GetComponentInParent<Canvas>()!=null))continue;
            Mesh mesh=r.sharedMesh;if(mesh==null)continue;
            MeshEntry currentData;
            bool changed=meshes.TryGetValue(mesh,out currentData) && (currentData.refresh || currentData!=e.data);
            if(e.mesh!=mesh || changed || (e.data!=null && e.data.vertexCount!=mesh.vertexCount)) {
                e.unavailable=null;
                try {if(!BindPathSkin(e,mesh,frame))continue;}
                catch(Exception error){pathSkinError=mesh.name+": "+error.Message;e.invalid=true;continue;}
            }
            if(e.invalid){pathSkinError=e.unavailable??(e.unavailable=mesh.name+": skin data unavailable");continue;}e.data.lastUsed=frame;
            ReadSharedMaterials(r,ref e.materials);
            bool propertyBlock=r.HasPropertyBlock();
            float* rows=(float*)e.rows.ToPointer()+(frame%Slots)*e.rowFloats;
            PathShapeWeights(e,rows+e.bones.Length*12);
            bool valid=true;
            for(int b=0;b<e.bones.Length;b++) {
                if(e.bones[b]==null){valid=false;break;}
                Matrix4x4 m=e.bones[b].localToWorldMatrix*e.bind[b];
                for(int y=0;y<3;y++)for(int x=0;x<4;x++)rows[b*12+y*4+x]=m[y,x];
            }
            if(!valid){e.mesh=null;continue;}
            Matrix4x4 identity=Matrix4x4.identity;
            uint shadowOnly=VaMSkinTransmission.PathSceneActive && r.shadowCastingMode==UnityEngine.Rendering.ShadowCastingMode.ShadowsOnly?DrawShadowOnly:0u;
            shadowOnly|=CameraOnly(r,mesh,e.id);
            if(VaMSkinTransmission.PathSceneActive && r.shadowCastingMode==UnityEngine.Rendering.ShadowCastingMode.Off)shadowOnly|=DrawNoCast;
            for(int s=0;s<e.data.subMeshCount&&n<MaxDraws;s++) {
                Material material=s<e.materials.Length?e.materials[s]:null;if(material==null)continue;
                MatInfo info=MaterialFor(material,frame);
                if(propertyBlock && (info.pathFlags&32)==0 && PathNoteNew(e.id,7))PathNote(r.name+": MaterialPropertyBlock not read, the material's own values are used");
                int at=n;n=Write(draws,n,e.id,e.data.vertexBuffer,IntPtr.Zero,e.data,s,32u|shadowOnly,0,(uint)mesh.vertexCount,ref identity,info,r.gameObject.layer);
                if(n==at)continue;byte* d=draws+at*DrawBytes;
                *(long*)(d+432)=e.weightPointer.ToInt64();*(long*)(d+440)=(long)rows;*(uint*)(d+448)=(uint)e.bones.Length;
                *(uint*)(d+452)=(uint)e.frameCount;
            }
        }
        return n;
    }
}
