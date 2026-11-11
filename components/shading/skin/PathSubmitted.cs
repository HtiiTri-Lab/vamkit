using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

internal static partial class RtScene
{
    // Capture the common managed submission boundary, not a growing list of
    // DAZ/debug/cloth caller classes. Native procedural/indirect draws remain
    // guarded by the render-thread reconciler.
    private struct SubmittedKey : IEquatable<SubmittedKey> {
        public int mesh,material,sub,ordinal;
        public bool Equals(SubmittedKey b){return mesh==b.mesh&&material==b.material&&sub==b.sub&&ordinal==b.ordinal;}
        public override bool Equals(object b){return b is SubmittedKey&&Equals((SubmittedKey)b);}
        public override int GetHashCode(){unchecked{return ((mesh*397^material)*397^sub)*397^ordinal;}}
    }
    private struct SubmittedDraw {
        public Mesh mesh;public Material material;public Matrix4x4 matrix;
        public int layer,sub;public bool shadow,shadowOnly;public Camera camera;public bool propertyBlock;
        public SubmittedKey key;
    }
    private sealed class SubmittedEntry {public long owner;public int frame;}
    private static readonly List<SubmittedDraw> submittedDraws=new List<SubmittedDraw>();
    private static readonly Dictionary<SubmittedKey,int> submittedOrdinals=new Dictionary<SubmittedKey,int>();
    private static readonly Dictionary<SubmittedKey,SubmittedEntry> submittedOwners=new Dictionary<SubmittedKey,SubmittedEntry>();
    private static readonly List<SubmittedKey> staleSubmitted=new List<SubmittedKey>();
    private static readonly HashSet<Mesh> pathSpecialMeshes=new HashSet<Mesh>();
    private static long nextSubmittedOwner=0x100000000L;
    private static void SetupSubmittedDraws(Harmony harmony) {
        MethodInfo method=typeof(Graphics).GetMethod("Internal_DrawMesh",BindingFlags.Static|BindingFlags.NonPublic);
        if(method==null||method.GetMethodBody()==null)throw new MissingMethodException("Graphics.Internal_DrawMesh");
        harmony.Patch(method,null,new HarmonyMethod(typeof(RtScene).GetMethod("SubmittedMesh",BindingFlags.Public|BindingFlags.Static)),null,null);
    }
    public static void SubmittedMesh(Mesh __0,int __1,Matrix4x4 __2,Material __3,int __4,Camera __5,MaterialPropertyBlock __6,UnityEngine.Rendering.ShadowCastingMode __7) {
        if(!ready||!VaMSkinTransmission.RtWanted()||__0==null||__3==null)return;
        int frame=Time.frameCount;if(frame!=callsFrame)NewFrame(frame);
        SubmittedKey key=new SubmittedKey{mesh=__0.GetInstanceID(),material=__3.GetInstanceID(),sub=__1};
        int ordinal;submittedOrdinals.TryGetValue(key,out ordinal);submittedOrdinals[key]=ordinal+1;key.ordinal=ordinal;
        submittedDraws.Add(new SubmittedDraw{mesh=__0,sub=__1,matrix=__2,material=__3,layer=__4,camera=__5,propertyBlock=__6!=null,shadow=__7!=UnityEngine.Rendering.ShadowCastingMode.Off,shadowOnly=__7==UnityEngine.Rendering.ShadowCastingMode.ShadowsOnly,key=key});
    }
    private static unsafe int AddSubmittedDraws(int mask,int frame,byte* draws,int n) {

        for(int i=0;i<submittedDraws.Count&&n<MaxDraws;i++) {
            SubmittedDraw call=submittedDraws[i];
            if(!pathRegistryActive && !call.shadow)continue;
            if(call.mesh==null||call.material==null||pathSpecialMeshes.Contains(call.mesh)||(mask&(1<<call.layer))==0)continue;
            if(call.camera!=null&&!call.camera.stereoEnabled)continue;
            MeshEntry mesh=MeshFor(call.mesh,frame,true);if(mesh==null)continue;
            if(call.sub<0||call.sub>=mesh.subMeshCount){if(PathNoteNew(call.mesh.GetInstanceID(),11))PathNote(call.mesh.name+": DrawMesh of a submesh it does not have, not in PT");continue;}
            MatInfo mat=MaterialFor(call.material,frame);bool overlay=(mat.pathFlags&32)!=0;
            if(!overlay&&(call.propertyBlock||mat.shader.IndexOf("ComputeBuff",StringComparison.OrdinalIgnoreCase)>=0)) {
                if(PathNoteNew(call.mesh.GetInstanceID(),12))PathNote(call.mesh.name+" / "+mat.shader+": procedural DrawMesh geometry (compute buffers) not readable, not in PT");continue;
            }
            if(!overlay)WatchEye(call.material);  // DAZMesh hair and head items drawn directly (0.22.65)
            UvFor(call.mesh,mesh);SubmittedEntry entry;
            if(!submittedOwners.TryGetValue(call.key,out entry)) {
                entry=new SubmittedEntry{owner=nextSubmittedOwner++};submittedOwners.Add(call.key,entry);contentRevision++;
            }
            if(entry.frame!=0&&entry.frame<frame-1)contentRevision++;
            entry.frame=frame;
            // Directly submitted CPU-deformed meshes have no Renderer lifetime.
            // Copy the actual source buffer; do not freeze a presumed rest pose.
            n=Write(draws,n,entry.owner,mesh.vertexBuffer,IntPtr.Zero,mesh,call.sub,DrawRigid|64u|(pathRegistryActive&&call.shadowOnly?DrawShadowOnly:0u),0,(uint)mesh.vertexCount,ref call.matrix,mat,call.layer);
        }
        return n;
    }
    private static void SweepSubmitted(int frame) {
        staleSubmitted.Clear();
        foreach(KeyValuePair<SubmittedKey,SubmittedEntry> item in submittedOwners)if(frame-item.Value.frame>CacheFrames)staleSubmitted.Add(item.Key);
        for(int i=0;i<staleSubmitted.Count;i++)submittedOwners.Remove(staleSubmitted[i]);
    }
}
