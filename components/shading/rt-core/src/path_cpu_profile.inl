// Optional CPU wall-time diagnosis. No work, resources or thread when unset.
// Aggregate 64 successful PT records per row; never read back or wait for GPU.
struct PathCpuProfile {
    struct Output {
        FILE *file=nullptr;
        double sum[9]={};unsigned count=0;
        Output() {
            char path[2048]={};
            if(GetEnvironmentVariableA("VAM_PT_CPU_PROFILE",path,sizeof path)>0) {
                file=std::fopen(path,"wb");
                if(file)std::fprintf(file,"frame,geometries,samples,prepare_ms,constants_ms,records_ms,upload_ms,hair_prepare_ms,bindings_tess_ms,blas_ms,tlas_ms,finish_ms,motion_valid,motion_total\n");
            }
        }
        ~Output(){if(file)std::fclose(file);}
    };
    Output *output=nullptr;
    double last=0,elapsed[9]={};
    PathCpuProfile(bool path,double start) {
        static Output destination;
        if(path&&destination.file){output=&destination;last=start;}
    }
    void Mark(unsigned stage) {
        if(!output)return;
        double now=NowMs();elapsed[stage]=now-last;last=now;
    }
    void Finish(uint64_t frame,unsigned geometries) {
        if(!output)return;
        Mark(8);
        for(unsigned i=0;i<9;i++)output->sum[i]+=elapsed[i];
        if(++output->count<64)return;
        std::fprintf(output->file,"%llu,%u,%u",(unsigned long long)frame,geometries,output->count);
        for(unsigned i=0;i<9;i++){std::fprintf(output->file,",%.6f",output->sum[i]/output->count);output->sum[i]=0;}
        unsigned valid=0;for(const auto &record:g->pathMotionRecords)valid+=record.valid?1u:0u;
        std::fprintf(output->file,",%u,%zu\n",valid,g->pathMotionRecords.size());std::fflush(output->file);output->count=0;
    }
};
