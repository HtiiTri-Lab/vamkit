// Hosts VaM's own Mono (Mono\EmbedRuntime\mono.dll, as UnityPlayer does) and runs one managed exe in it.
//     mono_host.exe <VaM folder> <exe> [args...]
#include <windows.h>
#include <stdio.h>

typedef void (*SetDirsFn)(const char*, const char*);
typedef void (*ConfigParseFn)(const char*);
typedef void (*AssembliesPathFn)(const char*);
typedef void* (*JitInitFn)(const char*);
typedef void* (*AssemblyOpenFn)(void*, const char*);
typedef int (*JitExecFn)(void*, void*, int, char**);

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "mono_host.exe <VaM folder> <exe> [args...]\n"); return 2; }
    char dll[MAX_PATH], managed[MAX_PATH], etc[MAX_PATH];
    snprintf(dll, MAX_PATH, "%s\\Mono\\EmbedRuntime\\mono.dll", argv[1]);
    snprintf(managed, MAX_PATH, "%s\\VaM_Data\\Managed", argv[1]);
    snprintf(etc, MAX_PATH, "%s\\Mono\\etc", argv[1]);
    HMODULE m = LoadLibraryA(dll);
    if (!m) { fprintf(stderr, "cannot load %s (%lu)\n", dll, GetLastError()); return 2; }
    SetDirsFn setDirs = (SetDirsFn)GetProcAddress(m, "mono_set_dirs");
    ConfigParseFn configParse = (ConfigParseFn)GetProcAddress(m, "mono_config_parse");
    AssembliesPathFn assembliesPath = (AssembliesPathFn)GetProcAddress(m, "mono_set_assemblies_path");
    JitInitFn jitInit = (JitInitFn)GetProcAddress(m, "mono_jit_init");
    AssemblyOpenFn open = (AssemblyOpenFn)GetProcAddress(m, "mono_domain_assembly_open");
    JitExecFn exec = (JitExecFn)GetProcAddress(m, "mono_jit_exec");
    if (!setDirs || !configParse || !assembliesPath || !jitInit || !open || !exec) { fprintf(stderr, "mono.dll lacks the embedding API\n"); return 2; }
    setDirs(managed, etc);
    assembliesPath(managed);   // mscorlib & co. from VaM_Data\Managed, as VaM loads them
    configParse(NULL);
    void* domain = jitInit(argv[2]);
    if (!domain) { fprintf(stderr, "mono_jit_init failed\n"); return 2; }
    void* assembly = open(domain, argv[2]);
    if (!assembly) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    int r = exec(domain, assembly, argc - 2, argv + 2);
    fflush(stdout);
    return r;
}
