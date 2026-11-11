// Reads MonoFloatTest's output and checks each IL shape against two rounding rules:
//   float  = every float operation rounded to float (IEEE single, as C++ float on SSE2)
//   double = operations done in double, rounded to float only when stored
// and, for the float locals in between, whether Mono rounded them on the store.
//     float_check.exe <file>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" double x87_sin(double);
extern "C" double x87_cos(double);
extern "C" unsigned x87_cw();
static bool same(float x, float y) { uint32_t a, b; memcpy(&a, &x, 4); memcpy(&b, &y, 4); return a == b; }
static bool samed(double x, double y) { uint64_t a, b; memcpy(&a, &x, 8); memcpy(&b, &y, 8); return a == b; }

struct Count { const char* name; long f = 0, d = 0, both = 0, none = 0; };
static void tally(Count& c, float mono, float vf, float vd) {
    bool a = same(mono, vf), b = same(mono, vd);
    if (a && b) c.both++; else if (a) c.f++; else if (b) c.d++; else c.none++;
}
static void report(const Count& c) {
    printf("%-34s float-only %7ld  double-only %7ld  both %7ld  neither %7ld\n", c.name, c.f, c.d, c.both, c.none);
}

int main(int argc, char** argv) {
    FILE* fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 2; }
    int n; fread(&n, 4, 1, fp);
    float m[12]; fread(m, 4, 12, fp);
    Count c1{"P1 a*b+c into array"}, c2{"P2 t=a*b; t+c"}, c3{"P3 matrix row into field"}, c5{"P5 x+x*k (k local)"},
          c6x{"P6 rotate x (given s,c)"}, c6y{"P6 rotate y (given s,c)"}, c7{"P7 Vector3 operator+"}, c8{"P8 (a*b)*c local"},
          cs{"sin: (float)sin vs UCRT sin"};
    long sinCrtDiff = 0, cosCrtDiff = 0, sinX87Diff = 0, cosX87Diff = 0;
    Count c5b{"P5 with k rounded (local)"};
    for (int i = 0; i < n; i++) {
        float a, b, c, d, p1, p2, p3, p5, p6x, p6y, p7x, p7y, p7z, p8, sf, cf; double sd, cd;
        fread(&a, 4, 1, fp); fread(&b, 4, 1, fp); fread(&c, 4, 1, fp); fread(&d, 4, 1, fp);
        fread(&p1, 4, 1, fp); fread(&p2, 4, 1, fp); fread(&p3, 4, 1, fp); fread(&p5, 4, 1, fp);
        fread(&p6x, 4, 1, fp); fread(&p6y, 4, 1, fp); fread(&p7x, 4, 1, fp); fread(&p7y, 4, 1, fp); fread(&p7z, 4, 1, fp);
        fread(&p8, 4, 1, fp); fread(&sf, 4, 1, fp); fread(&cf, 4, 1, fp); fread(&sd, 8, 1, fp); fread(&cd, 8, 1, fp);

        volatile float va = a, vb = b, vc = c, vd = d;   // keep the float rule honest (no folding)
        float f1 = va * vb + vc;
        float d1 = (float)((double)a * b + c);
        tally(c1, p1, f1, d1);
        tally(c2, p2, f1, d1);   // P2 with the local rounded = float rule; not rounded = double rule

        float f3 = ((va * m[0] + vb * m[1]) + vc * m[2]) + m[3];
        float d3 = (float)((((double)a * m[0] + (double)b * m[1]) + (double)c * m[2]) + m[3]);
        tally(c3, p3, f3, d3);

        float k = vd * va;                      // k is a local in Mono: rounded or not?
        float f5 = va + va * k;
        float d5 = (float)((double)a + (double)a * ((double)d * a));
        tally(c5, p5, f5, d5);
        float kr = (float)((double)d * a);
        tally(c5b, p5, f5, (float)((double)a + (double)a * kr));

        volatile float vs = sf, vco = cf;
        float f6x = va * vco - vb * vs;
        float d6x = (float)((double)a * cf - (double)b * sf);
        float f6y = va * vs + vb * vco;
        float d6y = (float)((double)a * sf + (double)b * cf);
        tally(c6x, p6x, f6x, d6x);
        tally(c6y, p6y, f6y, d6y);

        tally(c7, p7x, va + vd, (float)((double)a + d));

        float f8 = (va * vb) * vc;
        float d8 = (float)(((double)a * b) * c);
        tally(c8, p8, f8, d8);

        float ang = va * vd;
        float sfC = (float)sin((double)ang), sfD = (float)sin((double)a * d);
        tally(cs, sf, sfC, sfD);
        if (!samed(sin((double)a), sd)) sinCrtDiff++;
        if (!samed(cos((double)a), cd)) cosCrtDiff++;
        if (!samed(x87_sin((double)a), sd)) sinX87Diff++;
        if (!samed(x87_cos((double)a), cd)) cosX87Diff++;
    }
    printf("x87 control word %04x; Mono Math.Sin vs x87 fsin: %ld of %d differ; cos vs fcos %ld\n", x87_cw(), sinX87Diff, n, cosX87Diff);
    fclose(fp);
    report(c1); report(c2); report(c3); report(c5); report(c5b); report(c6x); report(c6y); report(c7); report(c8); report(cs);
    printf("Mono Math.Sin vs this exe's UCRT sin (double bits): %ld of %d differ; cos %ld\n", sinCrtDiff, n, cosCrtDiff);
    return 0;
}
