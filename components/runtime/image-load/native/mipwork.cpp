// Native mip chain matching Unity's Texture2D.Apply(updateMipmaps) for RGB24 / RGBA32, then optional BC.
// mode selects the box-filter rounding; the in-game MipCheck compares every mode against Unity.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

using Run=void (*)(int,void (*)(void*,int,int),void*,int,int,int);
using Task=int (*)(const char*,int);
static Run mipRun=nullptr;static int mipTask=-1;
static bool mipInit(){
 if(mipRun)return true;HMODULE m=GetModuleHandleW(L"VaMCpuBoostNative.bin");if(!m)return false;
 auto api=(int(*)())GetProcAddress(m,"CB_ApiVersion");auto task=(Task)GetProcAddress(m,"CB_Task");auto r=(Run)GetProcAddress(m,"CB_Run");
 if(!api||api()<5||!task||!r)return false;mipTask=task("image mipmaps",0);mipRun=r;return true;
}
static int defaultMode=2;
extern "C" __declspec(dllexport) void ILW_SetMipMode(int mode){defaultMode=mode;}
extern "C" __declspec(dllexport) int ILW_GetMipMode(){return defaultMode;}

int64_t ILW_MipChainBytes(int w,int h,int c,int levels){int64_t n=0;for(int i=0;i<levels;i++){n+=(int64_t)w*h*c;w=std::max(1,w/2);h=std::max(1,h/2);}return n;}
// mode 2 = Unity 2018.1 CPU mipmaps (fitted from Apply(true) dumps, 0 mismatches): truncated pair average along
// the row, then truncated average of the two rows. Modes 0/1 are plain rounded/truncated 2x2 boxes (tests only).
static inline int avg4(int a,int b,int c,int d,int mode){if(mode==2)return (((a+b)>>1)+((c+d)>>1))>>1;return mode==1?(a+b+c+d)>>2:(a+b+c+d+2)>>2;}
static inline int avg2(int a,int b,int mode){return mode==0?(a+b+1)>>1:(a+b)>>1;}
struct MipRows{const uint8_t* src;uint8_t* dst;int sw,sh,dw,dh,c,mode;};
static void mipRows(void* ctx,int b,int e){
 auto& m=*(MipRows*)ctx;
 for(int y=b;y<e;y++){
  int y0=std::min(y*2,m.sh-1),y1=std::min(y*2+1,m.sh-1);bool twoY=m.sh>1;
  const uint8_t* r0=m.src+(size_t)y0*m.sw*m.c;const uint8_t* r1=m.src+(size_t)y1*m.sw*m.c;uint8_t* o=m.dst+(size_t)y*m.dw*m.c;
  for(int x=0;x<m.dw;x++){
   int x0=std::min(x*2,m.sw-1),x1=std::min(x*2+1,m.sw-1);bool twoX=m.sw>1;
   for(int k=0;k<m.c;k++){
    int v;
    if(twoX&&twoY)v=avg4(r0[x0*m.c+k],r0[x1*m.c+k],r1[x0*m.c+k],r1[x1*m.c+k],m.mode);
    else if(twoX)v=avg2(r0[x0*m.c+k],r0[x1*m.c+k],m.mode);
    else if(twoY)v=avg2(r0[x0*m.c+k],r1[x0*m.c+k],m.mode);
    else v=r0[x0*m.c+k];
    o[x*m.c+k]=(uint8_t)v;
   }
  }
 }
}
// dst receives the whole chain including a copy of the base level.
extern "C" __declspec(dllexport) int ILW_BuildMips(const uint8_t* base,int baseBytes,uint8_t* dst,int dstBytes,int w,int h,int c,int levels,int mode){
 if(!base||!dst||w<=0||h<=0||w>16384||h>16384||(c!=3&&c!=4)||levels<=0||levels>32)return 0;
 if(mode<0)mode=defaultMode;
 int64_t baseN=(int64_t)w*h*c,total=ILW_MipChainBytes(w,h,c,levels);if(baseBytes<baseN||dstBytes!=total)return 0;
 memcpy(dst,base,(size_t)baseN);bool pool=mipInit();
 size_t off=0;int sw=w,sh=h;
 for(int i=1;i<levels;i++){
  int dw=std::max(1,sw/2),dh=std::max(1,sh/2);MipRows r{dst+off,dst+off+(size_t)sw*sh*c,sw,sh,dw,dh,c,mode};
  if(pool&&(int64_t)dw*dh>=65536)mipRun(mipTask,mipRows,&r,0,dh,std::max(1,16384/std::max(1,dw)));else mipRows(&r,0,dh);
  off+=(size_t)sw*sh*c;sw=dw;sh=dh;
 }
 return 1;
}
extern "C" int ILW_DXTMips(const uint8_t*,int,uint8_t*,int,int,int,int,int);
// Base level in, BC1/BC3 mip chain out (all levels built natively).
extern "C" __declspec(dllexport) int ILW_MipDXT(const uint8_t* base,int baseBytes,uint8_t* dst,int dstBytes,int w,int h,int c,int levels){
 int64_t total=ILW_MipChainBytes(w,h,c,levels);if(total<=0||total>INT32_MAX)return 0;
 try{std::vector<uint8_t> chain((size_t)total);if(ILW_BuildMips(base,baseBytes,chain.data(),(int)total,w,h,c,levels,-1)!=1)return 0;return ILW_DXTMips(chain.data(),(int)total,dst,dstBytes,w,h,c,levels);}catch(...){return 0;}
}
