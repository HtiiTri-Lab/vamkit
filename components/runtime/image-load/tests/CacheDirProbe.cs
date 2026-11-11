using System;using System.IO;using BepInEx;using HarmonyLib;using MVR.FileManagement;
// Test only: points VaM's texture cache at an empty folder named in BepInEx\config\cachedirprobe.txt.
[BepInPlugin("local.vam.cachedirprobe","Cache Dir Probe","0.1")]
public sealed class CacheDirProbe:BaseUnityPlugin{
 static string dir;
 void Awake(){string f=Path.Combine(Paths.ConfigPath,"cachedirprobe.txt");if(!File.Exists(f))return;dir=File.ReadAllText(f).Trim();Directory.CreateDirectory(dir);
  new Harmony("local.vam.cachedirprobe").Patch(typeof(CacheManager).GetMethod("GetTextureCacheDir"),null,new HarmonyMethod(typeof(CacheDirProbe).GetMethod("After")),null,null);Logger.LogInfo("texture cache dir -> "+dir);}
 public static void After(ref string __result){if(dir!=null)__result=dir;}
 public static string Dir(){return dir??"(none)";}
}
