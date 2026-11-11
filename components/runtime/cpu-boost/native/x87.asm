; x87 helpers for cpuboost_native.cpp. VaM's Mono JIT compiles Math.Sin / Math.Cos to fsin / fcos, so the native
; skinning uses the same instructions to stay bit-identical (Windows x64: argument and result in xmm0).
.code
cb_fsin proc
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fsin
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
cb_fsin endp

cb_fcos proc
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fcos
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
cb_fcos endp

; void cb_fsincos(double x, double* s, double* c)   x in xmm0, s in rdx, c in r8.
; Bit-identical to fsin + fcos for every float input in [-8, 8] on this machine (harness\sincos, all 2.18e9 inputs);
; callers use it only inside that range.
cb_fsincos proc
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fsincos
    fstp qword ptr [r8]
    fstp qword ptr [rdx]
    add rsp, 8
    ret
cb_fsincos endp

; unsigned cb_get_x87cw(void)
cb_get_x87cw proc
    sub rsp, 8
    fnstcw word ptr [rsp]
    movzx eax, word ptr [rsp]
    add rsp, 8
    ret
cb_get_x87cw endp

; void cb_set_x87cw(unsigned cw)   (cw in ecx)
cb_set_x87cw proc
    sub rsp, 8
    mov word ptr [rsp], cx
    fldcw word ptr [rsp]
    add rsp, 8
    ret
cb_set_x87cw endp
end
