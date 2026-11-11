#pragma once
// Opt-in diagnostic only. Timestamp PT and NGX separately on the shared D3D12
// queue; delayed readback uses the command ring's existing completion fence.
// No extra submits, waits, fences or resources during normal play.
class RrPathStageTimer {
    static constexpr unsigned Count=32,Stamps=3;
    struct Slot { UINT64 done=0;unsigned eye=0; } slots[Count];
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    UINT64* mapped=nullptr;
    UINT64 frequency=0;
    unsigned next=0,active=Count;
    double path[2]{},rr[2]{};
    unsigned samples[2]{};
    bool disabled=false;
    bool Enabled() {
        static const bool enabled=GetEnvironmentVariableW(L"VAM_PT_TIMING",nullptr,0)>0;
        return enabled&&!disabled;
    }
    bool Create(ID3D12Device* device,ID3D12CommandQueue* queue) {
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=Count*Stamps;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=Count*Stamps*sizeof(UINT64);d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(queue->GetTimestampFrequency(&frequency))||!frequency||
           FAILED(device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)))||
           FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))||
           FAILED(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped)))) {
            disabled=true;OOVR_LOG("PT/RR timestamp diagnostic unavailable");return false;
        }
        return true;
    }
public:
    void Begin(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12GraphicsCommandList* list,
               UINT64 completed,unsigned eye,UINT64 serial) {
        active=Count;
        if(!Enabled())return;
        if(!queries&&!Create(device,queue))return;
        for(unsigned i=0;i<Count;i++) {
            auto& s=slots[i];if(!s.done||s.done>completed)continue;
            const UINT64* t=mapped+i*Stamps;
            if(t[2]>=t[1]&&t[1]>=t[0]) {
                path[s.eye]+=double(t[1]-t[0])*1000/frequency;
                rr[s.eye]+=double(t[2]-t[1])*1000/frequency;
                if(++samples[s.eye]>=16) {
                    OOVR_LOGF("VAM PT split eye=%u samples=%u path_ms=%.3f rr_ms=%.3f",s.eye,samples[s.eye],path[s.eye]/samples[s.eye],rr[s.eye]/samples[s.eye]);
                    path[s.eye]=rr[s.eye]=0;samples[s.eye]=0;
                }
            }
            s.done=0;
        }
        if((serial&7)||eye>1||slots[next].done)return;
        active=next;slots[active].eye=eye;
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,active*Stamps);
    }
    void PathEnd(ID3D12GraphicsCommandList* list) {
        if(active<Count)list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,active*Stamps+1);
    }
    void End(ID3D12GraphicsCommandList* list) {
        if(active>=Count)return;
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,active*Stamps+2);
        list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,active*Stamps,Stamps,readback.Get(),active*Stamps*sizeof(UINT64));
    }
    void Submitted(UINT64 value) {
        if(active>=Count)return;
        slots[active].done=value;next=(active+1)%Count;active=Count;
    }
};
