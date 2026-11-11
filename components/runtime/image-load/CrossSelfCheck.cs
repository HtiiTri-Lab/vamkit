using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Threading;
using MVR.FileManagement;

public sealed unsafe partial class ImageLoadPipeline {
 static bool CompareDecode(string path,byte[] encoded,ImageLoaderThreaded.QueuedImage q,out string why){
  why="";var original=new ImageLoaderThreaded.QueuedImage();original.compress=q.compress;original.isNormalMap=q.isNormalMap;original.invert=q.invert;original.createAlphaFromGrayscale=q.createAlphaFromGrayscale;original.setSize=q.setSize;original.fillBackground=q.fillBackground;original.width=q.width;original.height=q.height;
  using(var s=new MemoryStream(encoded,false)){pixelBypass=true;try{originalStream.Invoke(original,new object[]{s});}finally{pixelBypass=false;}}
  int id=sourceSubmit(path,0,0,encoded.Length,0,0,Flag(q.compress),Flag(q.isNormalMap),Flag(q.invert),Flag(q.createAlphaFromGrayscale),Flag(q.setSize),Flag(q.fillBackground),q.width,q.height);if(id<=0){why="submit";return false;}
  try{var until=DateTime.UtcNow.AddSeconds(30);int state;while((state=sourcePoll(id))==0&&DateTime.UtcNow<until)Thread.Sleep(1);if(state!=1){why="native status "+state;return false;}int w,h,c,n;if(sourceInfo(id,out w,out h,out c,out n)!=1){why="native info";return false;}var bytes=new byte[n];fixed(byte* p=bytes)if(sourceCopy(id,p,n)!=1){why="native copy";return false;}
   if(w!=original.width||h!=original.height||c!=(original.textureFormat==UnityEngine.TextureFormat.RGBA32?4:3)||original.raw==null||n!=original.raw.Length){why="metadata "+w+"x"+h+" channels="+c+" bytes="+n+" vs "+original.width+"x"+original.height+" "+original.textureFormat+" bytes="+(original.raw==null?-1:original.raw.Length);return false;}
   for(int i=0;i<n;i++)if(bytes[i]!=original.raw[i]){why="byte "+i+" native="+bytes[i]+" original="+original.raw[i];return false;}return true;
  }finally{sourceFree(id);}
 }
 public static string CrossSelfCheck(string dir){
  if(crossEnabled==null||!crossEnabled.Value)return "ERROR: cross pipeline off";try{Directory.CreateDirectory(dir);int cases=0,failed=0;string first="";
   foreach(bool alpha in new[]{false,true}){byte[] encoded;using(var bmp=new Bitmap(128,128,alpha?PixelFormat.Format32bppArgb:PixelFormat.Format24bppRgb)){for(int y=0;y<128;y++)for(int x=0;x<128;x++)bmp.SetPixel(x,y,System.Drawing.Color.FromArgb(alpha?(x*7+y*3)%256:255,(x*19+y)%256,(y*11+x)%256,(x*7+y*13)%256));using(var m=new MemoryStream()){bmp.Save(m,ImageFormat.Png);encoded=m.ToArray();}}
    string path=Path.Combine(dir,alpha?"alpha.png":"rgb.png");File.WriteAllBytes(path,encoded);
    for(int mask=0;mask<32;mask++){var q=new ImageLoaderThreaded.QueuedImage();q.compress=(mask&1)!=0;q.isNormalMap=(mask&2)!=0;q.invert=(mask&4)!=0;q.createAlphaFromGrayscale=(mask&8)!=0;q.setSize=(mask&16)!=0;q.fillBackground=q.setSize;if(q.setSize){q.width=93;q.height=71;}string reason;if(!CompareDecode(path,encoded,q,out reason)){failed++;if(first=="")first="alpha="+alpha+" mask="+mask+" "+reason;}cases++;}
   }
   foreach(string spec in (Environment.GetEnvironmentVariable("VAMKIT_IMAGE_TEST_PATHS")??"").Split(new[]{';'},StringSplitOptions.RemoveEmptyEntries)){byte[] bytes=FileManager.ReadAllBytes(spec,true);string p=Path.Combine(dir,"real"+cases+(spec.EndsWith(".jpg")?".jpg":".png"));File.WriteAllBytes(p,bytes);var q=new ImageLoaderThreaded.QueuedImage();q.compress=true;q.createAlphaFromGrayscale=spec.EndsWith(".png");string reason;if(!CompareDecode(p,bytes,q,out reason)){failed++;if(first=="")first=spec+" "+reason;}cases++;}
   return "cases="+cases+" failures="+failed+" first="+first;
  }catch(Exception e){return "ERROR "+e;}
 }
}
