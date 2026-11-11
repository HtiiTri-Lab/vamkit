"""Runs a D3D11 vertex shader's disassembly (vs_4_0 / vs_5_0, the instructions VaM's skin vertex shaders use) on one
vertex, to find out which output registers carry what: the generator feeds known values (an object-to-world matrix,
a position, a normal, a UV and its _MainTex_ST) and looks for the results among the outputs.
"""
import re
import struct

import numpy as np

COMP = "xyzw"


def f2u(a):
    return np.asarray(a, dtype=np.float32).view(np.uint32)


def u2f(a):
    return np.asarray(a, dtype=np.uint32).view(np.float32)


class Machine:
    def __init__(self):
        self.regs = {}      # 'r0', 'v3', 'o1' -> uint32[4]
        self.cbs = {}       # slot -> uint32[n, 4]
        self.bufs = {}      # slot -> (stride bytes, bytes)

    def reg(self, name):
        if name not in self.regs:
            self.regs[name] = np.zeros(4, dtype=np.uint32)
        return self.regs[name]


_src_rx = re.compile(r"^(-)?(\|)?(.+?)(\|)?$")


def _split_operands(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def _swz(s):
    s = s or "xyzw"
    if len(s) == 1:
        s = s * 4
    return [COMP.index(c) for c in s]


def read_src(m, s):
    """uint32[4] (already swizzled) of a source operand; modifiers applied as floats."""
    neg, absv = False, False
    if s.startswith("-"):
        neg, s = True, s[1:]
    if s.startswith("|") and s.endswith("|"):
        absv, s = True, s[1:-1]
    if s.startswith("l("):
        vals = [v.strip() for v in s[2:-1].split(",")]
        out = []
        for v in vals:
            if re.match(r"^-?\d+$", v):
                out.append(np.uint32(int(v) & 0xffffffff))
            elif v.startswith("0x"):
                out.append(np.uint32(int(v, 16)))
            else:
                out.append(f2u(np.float32(float(v))))
        if len(out) == 1:
            out = out * 4
        val = np.array(out, dtype=np.uint32)
    else:
        mm = re.match(r"^(cb\d+)\[(.+?)\](?:\.([xyzw]+))?$", s)
        if mm:
            slot = int(mm.group(1)[2:])
            idx = mm.group(2)
            im = re.match(r"^(r\d+)\.([xyzw])\s*\+\s*(\d+)$", idx)
            if im:
                i = int(m.reg(im.group(1))[COMP.index(im.group(2))]) + int(im.group(3))
            else:
                i = int(idx)
            row = m.cbs[slot][i]
            val = row[_swz(mm.group(3))]
        else:
            mm = re.match(r"^([rvo]\d+)(?:\.([xyzw]+))?$", s)
            if not mm:
                raise ValueError("operand " + s)
            val = m.reg(mm.group(1))[_swz(mm.group(2))]
    val = val.copy()
    if absv:
        val = f2u(np.abs(u2f(val)))
    if neg:
        val = f2u(-u2f(val))
    return val


def dest(s):
    mm = re.match(r"^([ro]\d+)(?:\.([xyzw]+))?$", s)
    if not mm:
        raise ValueError("destination " + s)
    return mm.group(1), [COMP.index(c) for c in (mm.group(2) or "xyzw")]


def run(m, asm):
    for line in asm.splitlines():
        try:
            _step(m, line)
        except Exception as e:
            raise RuntimeError("%s: %s" % (line.strip(), e))


def _step(m, line):
    line = line.strip()
    if not line or line.startswith("//") or line.startswith("dcl_") or line.startswith("vs_") or line == "ret":
        return
    if line.startswith("ld_structured_indexable"):
        # ld_structured_indexable(structured_buffer, stride=12)(mixed,...) dst, idx, off, tN.swz
        line = "ld_structured " + re.sub(r"^ld_structured_indexable\(.*?\)\(.*?\)\s*", "", line)
    op, _, rest = line.partition(" ")
    sat = op.endswith("_sat")
    if sat:
        op = op[:-4]
    ops = _split_operands(rest)
    dname, dmask = dest(ops[0])
    D = m.reg(dname)
    if op == "ld_structured":
        idx = int(read_src(m, ops[1])[0])
        off = int(read_src(m, ops[2])[0])
        tm = re.match(r"^t(\d+)(?:\.([xyzw]+))?$", ops[3])
        stride, data = m.bufs[int(tm.group(1))]
        base = idx * stride + off
        words = np.frombuffer(data[base:base + 16].ljust(16, b"\0"), dtype=np.uint32)
        res = words[_swz(tm.group(2))]
        for c in dmask:
            D[c] = res[c]
        return
    S = [read_src(m, o) for o in ops[1:]]
    F = [u2f(x) for x in S]
    with np.errstate(all="ignore"):
        if op == "mov":
            res = S[0]
        elif op == "add":
            res = f2u(F[0] + F[1])
        elif op == "mul":
            res = f2u(F[0] * F[1])
        elif op == "mad":
            res = f2u(F[0] * F[1] + F[2])
        elif op == "div":
            res = f2u(F[0] / F[1])
        elif op == "max":
            res = f2u(np.maximum(F[0], F[1]))
        elif op == "min":
            res = f2u(np.minimum(F[0], F[1]))
        elif op in ("dp2", "dp3", "dp4"):
            n = int(op[2])
            res = f2u(np.full(4, np.float32(np.sum(F[0][:n] * F[1][:n])), dtype=np.float32))
        elif op == "rsq":
            res = f2u(np.float32(1) / np.sqrt(F[0]))
        elif op == "sqrt":
            res = f2u(np.sqrt(F[0]))
        elif op == "ge":
            res = np.where(F[0] >= F[1], 0xffffffff, 0).astype(np.uint32)
        elif op == "lt":
            res = np.where(F[0] < F[1], 0xffffffff, 0).astype(np.uint32)
        elif op == "and":
            res = S[0] & S[1]
        elif op == "or":
            res = S[0] | S[1]
        elif op == "ishl":
            res = (S[0] << (S[1] & 31)).astype(np.uint32)
        elif op == "iadd":
            res = (S[0].astype(np.uint64) + S[1]).astype(np.uint32)
        elif op == "movc":
            res = np.where(S[0] != 0, S[1], S[2]).astype(np.uint32)
        elif op == "frc":
            res = f2u(F[0] - np.floor(F[0]))
        else:
            raise NotImplementedError(op)
    if sat:
        res = f2u(np.clip(u2f(res), 0, 1))
    for c in dmask:
        D[c] = res[c]


def find(outputs, value, tol=1e-4):
    """[(register, component)] where the outputs hold value (a float)."""
    hits = []
    for r, v in outputs.items():
        f = u2f(v)
        for c in range(4):
            if abs(float(f[c]) - value) <= tol * max(1.0, abs(value)):
                hits.append((r, c))
    return hits


def pack_buffer(rows, comps):
    return b"".join(struct.pack("<%df" % comps, *r) for r in rows)
