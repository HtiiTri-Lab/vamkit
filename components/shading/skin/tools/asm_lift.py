"""DXBC pixel program -> equivalent HLSL, instruction by instruction (what VaM's skin programs use: ps_4_0 / ps_5_0,
float and integer ALU, sample / sample_l / sample_c_lz, if / loop / breakc, an immediate constant buffer).

The disassembly is read with hex literals (D3DDisassemble flag PRINT_HEX_LITERALS), so every constant keeps its exact
bits. Registers are float4 locals; integer instructions reinterpret them with asint/asuint and write the bits back
with asfloat, as the hardware's typeless registers do. Inputs are declared in the original's signature order with the
original's component counts, which gives the original's input registers (checked by the caller).

lift(dx) -> Program: .instrs (parsed instructions), .hlsl(insert=None, header="") -> source with entry "main";
insert maps an instruction index to (code before, code after) lines - how tools\\gen_spec_table.py patches in its
specular changes.
"""
import ctypes
import re
import struct

import skin_shaders as S

D3D_DISASM_PRINT_HEX_LITERALS = 0x80
COMP = "xyzw"
HLSL_TYPES = {1: "float", 3: "float2", 7: "float3", 15: "float4"}
TEX_TYPES = {"texture2d": ("Texture2D", 2), "texturecube": ("TextureCube", 3), "texture3d": ("Texture3D", 3)}


def disassemble_hex(dx):
    out = ctypes.c_void_p()
    hr = S._compiler().D3DDisassemble(dx, len(dx), D3D_DISASM_PRINT_HEX_LITERALS, None, ctypes.byref(out))
    if hr != 0:
        raise RuntimeError("D3DDisassemble 0x%08x" % (hr & 0xffffffff))
    return S._blob_bytes(out).decode("latin1").rstrip("\0")


def bits_of(tok):
    """a literal component as 32-bit pattern (hex form, or a decimal the disassembler printed anyway)"""
    tok = tok.strip()
    if tok.lower().startswith(("0x", "-0x")):
        v = int(tok, 16)
        return v & 0xffffffff
    if re.fullmatch(r"-?\d+", tok):
        return int(tok) & 0xffffffff
    return struct.unpack("<I", struct.pack("<f", float(tok)))[0]


def fstr(bits):
    """HLSL text for a float with exactly these bits"""
    f = struct.unpack("<f", struct.pack("<I", bits))[0]
    exp = (bits >> 23) & 0xff
    if exp == 0 and bits & 0x7fffff or exp == 0xff:      # denormal, inf, nan: keep the bits
        return "asfloat(0x%08xu)" % bits
    if f == 0:
        return "-0.0" if bits & 0x80000000 else "0.0"
    s = repr(f)
    # shortest decimal that gives these float32 bits
    for p in range(6, 10):
        t = "%.*g" % (p, f)
        if struct.unpack("<I", struct.pack("<f", float(t)))[0] == bits:
            s = t
            break
    if "e" not in s and "." not in s:
        s += ".0"
    return s


class Operand:
    def __init__(self, text):
        self.text = text
        t = text.strip()
        self.neg = t.startswith("-")
        if self.neg:
            t = t[1:]
        self.abs = t.startswith("|") and t.endswith("|")
        if self.abs:
            t = t[1:-1]
        self.kind = None
        self.index = None       # register number, cb slot, t/s slot
        self.row = None         # cb / icb row: (reg operand text or None, constant)
        self.swz = None         # 4-letter swizzle (sources) or mask (dests)
        self.lit = None         # literal bits, 4 entries
        m = re.fullmatch(r"l\((.*)\)", t)
        if m:
            vals = [bits_of(x) for x in m.group(1).split(",")]
            if len(vals) == 1:
                vals = vals * 4
            self.kind, self.lit = "l", vals
            return
        if t == "null":
            self.kind = "null"
            return
        m = re.fullmatch(r"(icb|cb(\d+))\[(.+?)\](?:\.([xyzw]+))?", t)
        if m:
            self.kind = "icb" if m.group(1) == "icb" else "cb"
            self.index = int(m.group(2)) if m.group(2) else None
            idx = m.group(3).strip()
            mm = re.fullmatch(r"(r\d+\.[xyzw])\s*\+\s*(\d+)", idx)
            self.row = (mm.group(1), int(mm.group(2))) if mm else (None, int(idx))
            self.swz = m.group(4) or "xyzw"
            return
        m = re.fullmatch(r"([rvots])(\d+)(?:\.([xyzw]+))?", t)
        if m:
            self.kind, self.index, self.swz = m.group(1), int(m.group(2)), m.group(3) or "xyzw"
            return
        raise ValueError("operand %r" % text)

    @property
    def sel(self):
        """source swizzle as 4 letters (a single letter is a scalar select: replicated)"""
        if len(self.swz) == 4:
            return self.swz
        if len(self.swz) == 1:
            return self.swz * 4
        raise ValueError("source swizzle %r" % self.text)

    @property
    def mask(self):
        return [COMP.index(c) for c in self.swz]


class Instr:
    def __init__(self, line, depth):
        self.line = line
        self.depth = depth
        head, _, rest = line.partition(" ")
        self.sat = head.endswith("_sat")
        if self.sat:
            head = head[:-4]
        self.resdim = None
        m = re.fullmatch(r"([a-z_0-9]+?)(_indexable)?(?:\((\w+)\))?(?:\([^)]*\))?", head)
        self.op = m.group(1)
        if m.group(3):
            self.resdim = m.group(3)
        self.ops = [Operand(x) for x in split_top(rest)] if rest.strip() else []


def split_top(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [x.strip() for x in out]


FLOAT_BIN = {"add": "({0} + {1})", "mul": "({0} * {1})", "div": "({0} / {1})", "min": "min({0}, {1})", "max": "max({0}, {1})"}
FLOAT_UN = {"mov": "{0}", "rsq": "rsqrt({0})", "sqrt": "sqrt({0})", "exp": "exp2({0})", "log": "log2({0})", "frc": "frac({0})",
            "round_ni": "floor({0})", "round_pi": "ceil({0})", "round_z": "trunc({0})", "round_ne": "round({0})"}
FCMP = {"eq": "==", "ne": "!=", "lt": "<", "ge": ">="}
ICMP = {"ieq": ("asint", "=="), "ine": ("asint", "!="), "ilt": ("asint", "<"), "ige": ("asint", ">="),
        "ult": ("asuint", "<"), "uge": ("asuint", ">=")}
IBIN = {"iadd": ("asint", "+"), "imul": ("asint", "*"), "and": ("asuint", "&"), "or": ("asuint", "|"), "xor": ("asuint", "^"),
        "ishl": ("asint", "<<"), "ishr": ("asint", ">>"), "ushr": ("asuint", ">>")}


class Program:
    def __init__(self, dx):
        self.dx = dx
        self.asm = disassemble_hex(dx)
        self.target = None
        self.temps = 0
        self.cbs = {}            # slot -> rows
        self.textures = {}       # slot -> dim name
        self.samplers = {}       # slot -> comparison?
        self.icb = None          # list of 4-bit tuples
        self.inputs = []         # dcl_input_ps lines
        self.instrs = []
        self.sig = S.signature(dx, b"ISGN")
        self._parse()

    def _parse(self):
        text = self.asm
        m = re.search(r"dcl_immediateConstantBuffer\s*\{(.*?)\}\s*\n", text, re.S)
        if m:
            rows = re.findall(r"\{([^{}]*)\}", m.group(1) + "}")
            self.icb = [[bits_of(x) for x in r.split(",")] for r in rows if r.strip()]
            text = text[:m.start()] + text[m.end():]
        depth = 0
        for raw in text.splitlines():
            t = raw.strip()
            if not t or t.startswith("//"):
                continue
            if re.fullmatch(r"ps_\d_\d", t):
                self.target = t
                continue
            if t.startswith("dcl_"):
                self._decl(t)
                continue
            ins = Instr(t, depth)
            if ins.op in ("endif", "endloop", "else"):
                ins.depth = depth - 1
            if ins.op in ("if_nz", "if_z", "loop"):
                depth += 1
            elif ins.op in ("endif", "endloop"):
                depth -= 1
            self.instrs.append(ins)

    def _decl(self, t):
        if t.startswith("dcl_temps"):
            self.temps = int(t.split()[1])
        elif t.startswith("dcl_constantbuffer"):
            m = re.match(r"dcl_constantbuffer CB(\d+)\[(\d+)\]", t)
            self.cbs[int(m.group(1))] = int(m.group(2))
        elif t.startswith("dcl_sampler"):
            m = re.match(r"dcl_sampler s(\d+), mode_(\w+)", t)
            self.samplers[int(m.group(1))] = m.group(2) == "comparison"
        elif t.startswith("dcl_resource_texture"):
            m = re.match(r"dcl_resource_(texture\w+) \(float,float,float,float\) t(\d+)", t)
            if not m:
                raise ValueError("resource %r" % t)
            self.textures[int(m.group(2))] = m.group(1)
        elif t.startswith("dcl_input_ps"):
            if not re.match(r"dcl_input_ps linear v\d+\.[xyzw]+$", t):
                raise ValueError("input %r" % t)
            self.inputs.append(t)
        elif t.startswith(("dcl_output", "dcl_globalFlags")):
            if t.startswith("dcl_output") and t != "dcl_output o0.xyzw":
                raise ValueError("output %r" % t)
        else:
            raise ValueError("declaration %r" % t)

    # ---- operands -> HLSL ----

    def row_expr(self, op):
        reg, const = op.row
        if reg is None:
            return str(const)
        return "asuint(%s) + %d" % (reg, const)

    def base(self, op):
        if op.kind == "r":
            return "r%d" % op.index
        if op.kind == "v":
            return "v%d" % op.index
        if op.kind == "cb":
            return "cb%d[%s]" % (op.index, self.row_expr(op))
        if op.kind == "icb":
            return "icb[%s]" % self.row_expr(op)
        raise ValueError(op.text)

    def src(self, op, comps, kind="f"):
        """the operand's components for dest components comps, as float (kind f), int (i) or uint (u)"""
        n = len(comps)
        if op.kind == "l":
            bits = [op.lit[c] for c in comps]
            if kind == "f":
                vals = [fstr(b) for b in bits]
                e = vals[0] if n == 1 else "float%d(%s)" % (n, ", ".join(vals))
            elif kind == "i":
                vals = ["%d" % (b - (1 << 32) if b & 0x80000000 else b) for b in bits]
                e = vals[0] if n == 1 else "int%d(%s)" % (n, ", ".join(vals))
            else:
                vals = ["0x%08xu" % b for b in bits]
                e = vals[0] if n == 1 else "uint%d(%s)" % (n, ", ".join(vals))
        else:
            e = "%s.%s" % (self.base(op), "".join(op.sel[c] for c in comps))
            if kind == "i":
                e = "asint(%s)" % e
            elif kind == "u":
                e = "asuint(%s)" % e
        if op.abs:
            e = "abs(%s)" % e
        if op.neg:
            e = "(-%s)" % e
        return e

    def dest(self, op):
        return "%s.%s" % (self.base(op) if op.kind != "o" else "o%d" % op.index, op.swz)

    # ---- instructions -> HLSL ----

    def stmt(self, ins):
        op, a = ins.op, ins.ops
        if op == "sincos":
            out = []
            src = a[2]
            sat = "saturate" if ins.sat else ""
            # sin and cos each through their own temporary: both read the source before either is written
            for d, fn, t in ((a[0], "sin", "tmp"), (a[1], "cos", "tmp2")):
                if d.kind != "null":
                    out.append("%s.%s = %s(%s(%s));" % (t, d.swz, sat, fn, self.src(src, d.mask)))
            for d, t in ((a[0], "tmp"), (a[1], "tmp2")):
                if d.kind != "null":
                    out.append("%s = %s.%s;" % (self.dest(d), t, d.swz))
            return " ".join(out)
        d = a[0]
        comps = d.mask
        n = len(comps)
        sat = (lambda e: "saturate(%s)" % e) if ins.sat else (lambda e: e)
        if op in FLOAT_UN:
            e = FLOAT_UN[op].format(self.src(a[1], comps))
        elif op in FLOAT_BIN:
            e = FLOAT_BIN[op].format(self.src(a[1], comps), self.src(a[2], comps))
        elif op == "mad":
            e = ("mad(%s, %s, %s)" if self.precise else "(%s * %s + %s)") % (self.src(a[1], comps), self.src(a[2], comps), self.src(a[3], comps))
        elif op in ("dp2", "dp3", "dp4"):
            k = int(op[2])
            e = "dot(%s, %s)" % (self.src(a[1], list(range(k))), self.src(a[2], list(range(k))))
            if n > 1:
                e = "(%s).%s" % (e, "x" * n)
        elif op == "movc":
            e = "(%s != 0 ? %s : %s)" % (self.src(a[1], comps, "u"), self.src(a[2], comps), self.src(a[3], comps))
        elif op in FCMP:
            e = "asfloat((uint%s)(%s %s %s) * 0xffffffffu)" % ("" if n == 1 else n, self.src(a[1], comps), FCMP[op], self.src(a[2], comps))
        elif op in ICMP:
            k, o = ICMP[op]
            kk = "i" if k == "asint" else "u"
            e = "asfloat((uint%s)(%s %s %s) * 0xffffffffu)" % ("" if n == 1 else n, self.src(a[1], comps, kk), o, self.src(a[2], comps, kk))
        elif op in IBIN:
            k, o = IBIN[op]
            kk = "i" if k == "asint" else "u"
            e = "asfloat(%s %s %s)" % (self.src(a[1], comps, kk), o, self.src(a[2], comps, kk))
        elif op == "itof":
            e = "(float%s)%s" % ("" if n == 1 else n, self.src(a[1], comps, "i"))
        elif op == "utof":
            e = "(float%s)%s" % ("" if n == 1 else n, self.src(a[1], comps, "u"))
        elif op == "ftoi":
            e = "asfloat((int%s)%s)" % ("" if n == 1 else n, self.src(a[1], comps))
        elif op == "ftou":
            e = "asfloat((uint%s)%s)" % ("" if n == 1 else n, self.src(a[1], comps))
        elif op in ("sample", "sample_l", "sample_c_lz"):
            t, s = a[2], a[3]
            dim = self.textures[t.index]
            k = TEX_TYPES[dim][1]
            coord = self.src(a[1], list(range(k)))
            if op == "sample":
                call = "t%d.Sample(s%d, %s)" % (t.index, s.index, coord)
            elif op == "sample_l":
                call = "t%d.SampleLevel(s%d, %s, %s)" % (t.index, s.index, coord, self.src(a[4], [0]))
            else:
                call = "t%d.SampleCmpLevelZero(s%d, %s, %s).xxxx" % (t.index, s.index, coord, self.src(a[4], [0]))
            e = "(%s).%s" % (call, "".join(t.sel[c] for c in comps))
        else:
            raise ValueError("instruction %r" % ins.line)
        e = sat(e)
        return "%s = %s;" % (self.dest(d), e)

    def hlsl(self, insert=None, header="", precise=False):
        insert = insert or {}
        self.precise = precise
        L = ["// lifted from DXBC by tools\\asm_lift.py", header]
        for slot, rows in sorted(self.cbs.items()):
            L.append("cbuffer CB%d : register(b%d) { float4 cb%d[%d]; };" % (slot, slot, slot, rows))
        for slot, dim in sorted(self.textures.items()):
            L.append("%s t%d : register(t%d);" % (TEX_TYPES[dim][0], slot, slot))
        for slot, cmp_ in sorted(self.samplers.items()):
            L.append("%s s%d : register(s%d);" % ("SamplerComparisonState" if cmp_ else "SamplerState", slot, slot))
        if self.icb:
            L.append("static const float4 icb[%d] = {" % len(self.icb))
            L.append(",\n".join("    float4(%s)" % ", ".join(fstr(b) for b in r) for r in self.icb))
            L.append("};")
        L.append("struct PSIn {")
        regs = {}
        for i, (sem, idx, reg, mask, sv, ct) in enumerate(self.sig):
            if mask not in HLSL_TYPES:
                raise ValueError("input mask %d" % mask)
            name = "SV_POSITION" if sem == "SV_POSITION" else "%s%d" % (sem, idx)
            L.append("    %s e%d : %s;" % (HLSL_TYPES[mask], i, name))
            for c in range(4):
                if mask & (1 << c):
                    regs[(reg, c)] = "i.e%d.%s" % (i, COMP[c])
        L.append("};")
        L.append("float4 main(PSIn i) : SV_Target {")
        used = sorted({r for r, c in regs})
        for r in used:
            L.append("    float4 v%d = float4(%s);" % (r, ", ".join(regs.get((r, c), "0") for c in range(4))))
        for r in range(self.temps):
            L.append("    %sfloat4 r%d = 0;" % ("precise " if precise else "", r))
        L.append("    float4 o0 = 0;")
        L.append("    float4 tmp = 0, tmp2 = 0;")
        ind = 1
        for k, ins in enumerate(self.instrs):
            before, after = insert.get(k, ("", ""))
            if before:
                L.extend("    " * ind + x for x in before.splitlines())
            if ins.op == "if":
                raise ValueError("if without _nz/_z")
            if ins.op == "if_nz" or ins.op == "if_z":
                L.append("    " * ind + "if (asuint(%s) %s 0) {" % (self.src(ins.ops[0], [0], "f"), "!=" if ins.op == "if_nz" else "=="))
                ind += 1
            elif ins.op == "else":
                L.append("    " * (ind - 1) + "} else {")
            elif ins.op in ("endif", "endloop"):
                ind -= 1
                L.append("    " * ind + "}")
            elif ins.op == "loop":
                L.append("    " * ind + "[loop] for (;;) {")
                ind += 1
            elif ins.op in ("breakc_nz", "breakc_z"):
                L.append("    " * ind + "if (asuint(%s) %s 0) break;" % (self.src(ins.ops[0], [0], "f"), "!=" if ins.op == "breakc_nz" else "=="))
            elif ins.op in ("discard_nz", "discard_z"):  # 0.22.0: VaM's clothing alpha clip
                L.append("    " * ind + "if (asuint(%s) %s 0) discard;" % (self.src(ins.ops[0], [0], "f"), "!=" if ins.op == "discard_nz" else "=="))
            elif ins.op == "break":
                L.append("    " * ind + "break;")
            elif ins.op == "ret":
                L.append("    " * ind + "return o0;")
            else:
                L.append("    " * ind + self.stmt(ins) + "  // " + ins.line)
            if after:
                L.extend("    " * ind + x for x in after.splitlines())
        if not self.instrs or self.instrs[-1].op != "ret":
            L.append("    return o0;")
        L.append("}")
        return "\n".join(L) + "\n"


def lift(dx):
    return Program(dx)
