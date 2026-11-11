#include "mouth.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

void BuildMouth(const BakeInput &in, MouthData &out) {
    const int inner=BAKE_TEETH|BAKE_TONGUE|BAKE_MOUTH|BAKE_LIPS;
    out.vertexCount=in.vertexCount;
    float lo[3]={1e20f,1e20f,1e20f},hi[3]={-1e20f,-1e20f,-1e20f};
    std::vector<int> ids(in.vertexCount,-1);
    for(int s=0;s<in.subCount;s++) if(in.subs[s].flags&inner) {
        const auto &sb=in.subs[s];
        for(int k=sb.indexStart;k<sb.indexStart+sb.indexCount;k++) {
            int i=in.indices[k];
            if(ids[i]<0) { ids[i]=(int)out.receivers.size(); out.receivers.push_back({(uint32_t)i,0,0,0}); }
            for(int a=0;a<3;a++)lo[a]=std::min(lo[a],in.pos[3*i+a]),hi[a]=std::max(hi[a],in.pos[3*i+a]);
        }
    }
    if(out.receivers.empty())return;
    // Character-sized reach. The two lateral lip vertices carry uniform scale at runtime.
    int left=-1,right=-1;
    for(int s=0;s<in.subCount;s++)if(in.subs[s].flags&BAKE_LIPS) {
        const auto&sb=in.subs[s];
        for(int k=sb.indexStart;k<sb.indexStart+sb.indexCount;k++) {
            int i=in.indices[k];
            if(left<0||in.pos[3*i]<in.pos[3*left])left=i;
            if(right<0||in.pos[3*i]>in.pos[3*right])right=i;
        }
    }
    if(left>=0&&right>=0) {
        out.spanVertex[0]=(uint32_t)left;out.spanVertex[1]=(uint32_t)right;
        float d2=0;for(int a=0;a<3;a++){float d=in.pos[3*left+a]-in.pos[3*right+a];d2+=d*d;}
        out.span=sqrtf(d2);out.radius=std::max(.025f,out.span*.8f);
    }
    std::vector<std::vector<uint32_t>> adj(out.receivers.size());
    for(int s=0;s<in.subCount;s++) if(in.subs[s].flags&BAKE_OCCLUDER) {
        const auto &sb=in.subs[s];
        for(int k=sb.indexStart;k<sb.indexStart+sb.indexCount;k+=3) {
            bool near=true;
            for(int a=0;a<3;a++) {
                float mn=1e20f,mx=-1e20f;
                for(int j=0;j<3;j++)mn=std::min(mn,in.pos[3*in.indices[k+j]+a]),mx=std::max(mx,in.pos[3*in.indices[k+j]+a]);
                if(mx<lo[a]-.035f||mn>hi[a]+.035f)near=false;
            }
            if(!near)continue;
            out.triangles.push_back({(uint32_t)in.indices[k],(uint32_t)in.indices[k+1],(uint32_t)in.indices[k+2],0});
        }
    }
    // Preserve the original triangle accumulation order for receiver normals.
    // Reordering a nearly cancelling vertex normal can perturb a grazing ray.
    for(size_t i=0;i<out.triangles.size();i++)out.triangles[i][3]=(uint32_t)i;
    auto normalOrder=out.triangles;
    std::function<void(int,int)> orderNormals=[&](int begin,int end){
        if(end-begin<=6)return;
        float mn[3]={1e20f,1e20f,1e20f},mx[3]={-1e20f,-1e20f,-1e20f};
        for(int i=begin;i<end;i++)for(int a=0;a<3;a++){
            const auto&t=normalOrder[i];float c=(in.pos[3*t[0]+a]+in.pos[3*t[1]+a]+in.pos[3*t[2]+a])/3;
            mn[a]=std::min(mn[a],c);mx[a]=std::max(mx[a],c);
        }
        int axis=0;for(int a=1;a<3;a++)if(mx[a]-mn[a]>mx[axis]-mn[axis])axis=a;
        int mid=(begin+end)/2;
        std::nth_element(normalOrder.begin()+begin,normalOrder.begin()+mid,normalOrder.begin()+end,[&](const auto&a,const auto&b){return in.pos[3*a[0]+axis]+in.pos[3*a[1]+axis]+in.pos[3*a[2]+axis]<in.pos[3*b[0]+axis]+in.pos[3*b[1]+axis]+in.pos[3*b[2]+axis];});
        orderNormals(begin,mid);orderNormals(mid,end);
    };
    orderNormals(0,(int)normalOrder.size());
    std::vector<uint32_t> normalRank(normalOrder.size());
    for(size_t i=0;i<normalOrder.size();i++)normalRank[normalOrder[i][3]]=(uint32_t)i;
    std::function<int(int,int,int)> build=[&](int begin,int end,int depth) {
        int n=(int)out.nodes.size(); out.nodes.push_back({}); out.depth=std::max(out.depth,depth);
        if(end-begin<=2)out.nodes[n]={(uint32_t)begin,0x80000000u|(uint32_t)(end-begin),0,(uint32_t)depth};
        else {
            float mn[3]={1e20f,1e20f,1e20f},mx[3]={-1e20f,-1e20f,-1e20f};
            auto centre=[&](int i,int a){const auto&t=out.triangles[i];return(in.pos[3*t[0]+a]+in.pos[3*t[1]+a]+in.pos[3*t[2]+a])/3;};
            for(int i=begin;i<end;i++)for(int a=0;a<3;a++)mn[a]=std::min(mn[a],centre(i,a)),mx[a]=std::max(mx[a],centre(i,a));
            int mid=(begin+end)/2;
            // Surface-area split: compact boxes matter more than equal triangle counts
            // for the short, divergent rays inside the mouth. Connectivity stays exact.
            struct Box {float lo[3]={1e20f,1e20f,1e20f},hi[3]={-1e20f,-1e20f,-1e20f};int count=0;
                void add(const Box&b){if(!b.count)return;count+=b.count;for(int a=0;a<3;a++){lo[a]=std::min(lo[a],b.lo[a]);hi[a]=std::max(hi[a],b.hi[a]);}}
                float area()const{float x=hi[0]-lo[0],y=hi[1]-lo[1],z=hi[2]-lo[2];return count?x*y+x*z+y*z:0;}};
            float best=1e30f;int axis=-1,split=-1;
            constexpr int bins=16;
            for(int a=0;a<3;a++)if(mx[a]-mn[a]>1e-9f){
                Box bucket[bins],left[bins],right[bins];
                for(int i=begin;i<end;i++){
                    int k=std::min(bins-1,(int)((centre(i,a)-mn[a])/(mx[a]-mn[a])*bins));
                    Box b;b.count=1;
                    for(int v=0;v<3;v++)for(int c=0;c<3;c++){float p=in.pos[3*out.triangles[i][v]+c];b.lo[c]=std::min(b.lo[c],p);b.hi[c]=std::max(b.hi[c],p);}
                    bucket[k].add(b);
                }
                Box l,r;for(int k=0;k<bins;k++){l.add(bucket[k]);left[k]=l;r.add(bucket[bins-1-k]);right[bins-1-k]=r;}
                for(int k=0;k<bins-1;k++)if(left[k].count&&right[k+1].count){float cost=left[k].area()*left[k].count+right[k+1].area()*right[k+1].count;if(cost<best){best=cost;axis=a;split=k;}}
            }
            if(axis>=0&&depth<40){
                auto p=std::partition(out.triangles.begin()+begin,out.triangles.begin()+end,[&](const auto&t){float c=(in.pos[3*t[0]+axis]+in.pos[3*t[1]+axis]+in.pos[3*t[2]+axis])/3;return std::min(bins-1,(int)((c-mn[axis])/(mx[axis]-mn[axis])*bins))<=split;});
                mid=(int)(p-out.triangles.begin());
            }else{
                axis=0;for(int a=1;a<3;a++)if(mx[a]-mn[a]>mx[axis]-mn[axis])axis=a;
                std::nth_element(out.triangles.begin()+begin,out.triangles.begin()+mid,out.triangles.begin()+end,[&](const auto&a,const auto&b){return in.pos[3*a[0]+axis]+in.pos[3*a[1]+axis]+in.pos[3*a[2]+axis]<in.pos[3*b[0]+axis]+in.pos[3*b[1]+axis]+in.pos[3*b[2]+axis];});
            }
            int left=build(begin,mid,depth+1),right=build(mid,end,depth+1);
            out.nodes[n]={(uint32_t)left,(uint32_t)right,0,(uint32_t)depth};
        }
        out.nodes[n][2]=(uint32_t)out.nodes.size();return n;
    };
    build(0,(int)out.triangles.size(),0);
    // Parent links support a single-dispatch bottom-up refit. Each leaf publishes
    // its box, and the second child to arrive computes the parent; nobody spins.
    out.nodes[0][3]=0xffffffffu;
    for(size_t n=0;n<out.nodes.size();n++)if(!(out.nodes[n][1]&0x80000000u)){
        out.nodes[out.nodes[n][0]][3]=(uint32_t)n;
        out.nodes[out.nodes[n][1]][3]=(uint32_t)n;
    }
    for(size_t t=0;t<out.triangles.size();t++)for(int a=0;a<3;a++) {
        int r=ids[out.triangles[t][a]];if(r>=0)adj[r].push_back((uint32_t)t);
    }
    for(size_t r=0;r<out.receivers.size();r++) {
        std::sort(adj[r].begin(),adj[r].end(),[&](uint32_t a,uint32_t b){return normalRank[out.triangles[a][3]]<normalRank[out.triangles[b][3]];});
        out.receivers[r][1]=(uint32_t)out.adjacency.size();out.receivers[r][2]=(uint32_t)adj[r].size();
        float normal[3]={};
        for(uint32_t t:adj[r]) {
            const auto &tri=out.triangles[t];float a[3],b[3];
            for(int c=0;c<3;c++)a[c]=in.pos[3*tri[1]+c]-in.pos[3*tri[0]+c],b[c]=in.pos[3*tri[2]+c]-in.pos[3*tri[0]+c];
            normal[0]+=a[1]*b[2]-a[2]*b[1];normal[1]+=a[2]*b[0]-a[0]*b[2];normal[2]+=a[0]*b[1]-a[1]*b[0];
        }
        int i=out.receivers[r][0];float dot=0;for(int c=0;c<3;c++)dot+=normal[c]*in.nrm[3*i+c];
        out.receivers[r][3]=dot<0?1:0;
        out.adjacency.insert(out.adjacency.end(),adj[r].begin(),adj[r].end());
    }
    for(int s=0;s<in.subCount;s++) if(in.subs[s].flags&inner) {
        const auto&sb=in.subs[s]; MouthMap m;m.sub=s;
        m.lo[0]=m.lo[1]=1e20f;m.hi[0]=m.hi[1]=-1e20f;
        for(int k=sb.indexStart;k<sb.indexStart+sb.indexCount;k++)for(int a=0;a<2;a++) {
            float v=in.uv[2*in.indices[k]+a];m.lo[a]=std::min(m.lo[a],v);m.hi[a]=std::max(m.hi[a],v);
        }
        const float density=512;const int pad=3;
        int size[2];for(int a=0;a<2;a++){size[a]=std::max(8,(int)ceilf((m.hi[a]-m.lo[a])*density)+pad*2);m.lo[a]-=pad/density;m.hi[a]=m.lo[a]+size[a]/density;}
        m.width=size[0];m.height=size[1];m.texels.assign((size_t)m.width*m.height,{0xffffffffu,0,0,0});
        for(int k=sb.indexStart;k<sb.indexStart+sb.indexCount;k+=3) {
            int v[3]={in.indices[k],in.indices[k+1],in.indices[k+2]};float x[3],y[3];
            for(int a=0;a<3;a++)x[a]=(in.uv[2*v[a]]-m.lo[0])*density-.5f,y[a]=(in.uv[2*v[a]+1]-m.lo[1])*density-.5f;
            float area=(x[1]-x[0])*(y[2]-y[0])-(x[2]-x[0])*(y[1]-y[0]);if(fabsf(area)<1e-12f)continue;
            int x0=std::max(0,(int)floorf(std::min({x[0],x[1],x[2]}))),x1=std::min(m.width-1,(int)ceilf(std::max({x[0],x[1],x[2]})));
            int y0=std::max(0,(int)floorf(std::min({y[0],y[1],y[2]}))),y1=std::min(m.height-1,(int)ceilf(std::max({y[0],y[1],y[2]})));
            for(int py=y0;py<=y1;py++)for(int px=x0;px<=x1;px++) {
                float a=((x[1]-px)*(y[2]-py)-(x[2]-px)*(y[1]-py))/area,b=((x[2]-px)*(y[0]-py)-(x[0]-px)*(y[2]-py))/area;
                if(a<-.001f||b<-.001f||a+b>1.001f)continue;
                a=std::max(0.f,a);b=std::max(0.f,b);float div=std::max(1.f,a+b);a/=div;b/=div;
                m.texels[(size_t)py*m.width+px]={(uint32_t)ids[v[0]],(uint32_t)ids[v[1]],(uint32_t)ids[v[2]],(uint32_t)(a*65535.f+.5f)|((uint32_t)(b*65535.f+.5f)<<16)};
            }
        }
        for(int p=0;p<pad;p++){auto src=m.texels;for(int y=0;y<m.height;y++)for(int x=0;x<m.width;x++)if(src[(size_t)y*m.width+x][0]==0xffffffffu){
            bool found=false;for(int dy=-1;dy<=1&&!found;dy++)for(int dx=-1;dx<=1;dx++){int xx=x+dx,yy=y+dy;if(xx<0||yy<0||xx>=m.width||yy>=m.height)continue;auto q=src[(size_t)yy*m.width+xx];if(q[0]!=0xffffffffu){m.texels[(size_t)y*m.width+x]=q;found=true;break;}}
        }}
        out.maps.push_back(std::move(m));
    }
}
