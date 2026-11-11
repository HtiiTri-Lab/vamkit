; x87 fsin / fcos for comparison with Mono's Math.Sin / Math.Cos (Windows x64: arg and result in xmm0).
.code
x87_sin proc
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fsin
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
x87_sin endp

x87_cos proc
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fcos
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
x87_cos endp

; x87 control word, to report the precision control in effect.
x87_cw proc
    sub rsp, 8
    fnstcw word ptr [rsp]
    movzx eax, word ptr [rsp]
    add rsp, 8
    ret
x87_cw endp
end
