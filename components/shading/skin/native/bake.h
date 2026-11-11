// Thickness bake for skin transmission: how much skin lies behind each point, from the character's bind-pose mesh.
//
// Per vertex, rays go into the body (a cone of directions around the inverted normal) until they leave it again;
// their lengths are combined into one effective thickness, weighted the way light gets through (short paths count
// most): d = -D0 * ln(mean(exp(-L / D0))), D0 = 2 mm. A ray that meets the outside of a surface first started outside
// the body (a crease) and is dropped; one that leaves nowhere within 50 mm lets no light through. Whether light reaches
// the far side at all is not known here (pose and lights change): the light's shadow map decides that at draw time
// (shaders\transmission.hlsli). Light that gets through spreads sideways inside the skin by millimetres, while the
// vertices are millimetres apart (ears ~3-4 mm) and the raw per-vertex values jump between neighbours: the light that
// gets through (red, exp(-d / 3.67 mm)) is averaged over the surface with a Gaussian of smoothMM, between vertices that
// face the same way only (never through to the far side of a thin part). The per-vertex values are then drawn, in UV
// space, into one texture per receiving material (half floats, millimetres, a few texels of border grown around every
// UV island so that bilinear sampling never reads the empty background).
//
// Teeth (0.8.0): for the teeth and tongue submeshes no rays, two numbers per vertex from the same bind-pose mesh, drawn
// into one two-channel texture per submesh: how far back in the mouth (0 at the front of the front teeth, 1 at the back
// of the last molars, along the horizontal direction from the teeth to the lips) and, on teeth, how far from the tooth's
// biting edge toward the gum (0 at the edge, 1 at the gum end of that tooth; teeth are told apart by connected
// triangles, upper from lower by a line fitted through their centres along the arch). The lower teeth move with the jaw
// and the numbers move with them.
//
// Eyes (0.9.0): no texture, the rig the draws need to find each eye and its eyelids while they move. Per eye the
// eyeball (a sphere fitted to the sclera, the gaze from its centre to the iris's), four vertices of the sclera's front
// ring (where the iris shows) that carry the eye at draw time, and the eyelid contact: polylines where the eyelid skin
// crosses the eyeball's sphere, each point on a mesh edge (two vertices and a fraction, so it moves with the lids when
// they blink or follow the gaze), with how much of it is upper lid (1) or lower lid (0).
//
// No D3D here: tests\bake_test.cpp runs it on VaM's exported Genesis 2 OBJ.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// BAKE_SCLERA / BAKE_IRIS: the eyeball's geometry; those and BAKE_PUPIL: the draws that get the eye constants
enum { BAKE_RECEIVER = 1, BAKE_OCCLUDER = 2, BAKE_TEETH = 4, BAKE_TONGUE = 8, BAKE_LIPS = 16, BAKE_SCLERA = 32, BAKE_IRIS = 64, BAKE_PUPIL = 128, BAKE_NOSTRILS = 256, BAKE_MOUTH = 1024, BAKE_FACE = 512, BAKE_SURFACE_ONLY = 2048, BAKE_MOUTH_AO = 4096,
       // rt.h: drawn by VaM's own program (nails); bits 16..18: the submesh's part + 1 (body, face, hand, mouth, eye;
       // 0 = no PT geometry metadata for it)
       BAKE_RT_ONLY = 32768, BAKE_RT_PART_SHIFT = 16, BAKE_RT_PART_MASK = 7 << 16 };
enum { TEX_THICKNESS = 0, TEX_TEETH = 1, TEX_TONGUE = 2, TEX_MOUTH = 4, TEX_EYE = 3, TEX_REGIONAL = 5 };  // TEX_EYE: the module's eye draws (no texture)

struct BakeSub {
    int indexStart, indexCount;  // into BakeInput::indices
    int flags;                   // BAKE_*
};

static const int kEyePoints = 64;  // eyelid contact points per eye at most

struct EyePoint {
    int a = 0, b = 0;    // the mesh edge the point is on (vertex indices) ...
    float t = 0;         // ... at a + t (b - a)
    float upper = 0;     // 1 upper lid .. 0 lower lid
    bool start = false;  // begins a polyline (no segment from the point before)
};

struct EyeRig {
    int eyes = 0;  // 2 when both were found, else 0 (and why in error)
    std::string error;
    // per eye (0 = +x side), bind pose, metres
    float centre[2][3] = {}, radius[2] = {}, gaze[2][3] = {};
    float limbus[2] = {};  // radius of the sclera's front ring around the gaze axis
    // four vertices of that ring a quarter turn apart: the frame o = their mean, x = r0 - r2 normalised, z = (r1 - r3) x
    // (r0 - r2) normalised (toward the front), y = z x x; the centre and the gaze in that frame, and |r0 - r2|
    int ring[2][4] = {};
    float local[2][6] = {}, span[2] = {};
    std::vector<EyePoint> points[2];
    int chains[2] = {};
    float widthMM[2] = {}, heightMM[2] = {};  // of the eyelid contact, along and across the eye
};

struct BakeInput {
    int vertexCount = 0;
    const float *pos = nullptr;  // xyz per vertex, metres
    const float *nrm = nullptr;  // xyz per vertex
    const float *regional = nullptr; // optional verified female UV response, one float per vertex
    const float *uv = nullptr;   // uv per vertex
    int indexCount = 0;
    const int *indices = nullptr;  // triangle lists of all submeshes
    int subCount = 0;
    const BakeSub *subs = nullptr;
    float density = 1024.0f;  // texels per UV unit
    int maxSize = 2048;       // largest texture side
    int rays = 32;
    float coneDegrees = 70.0f;
    float smoothMM = 3.0f;  // Gaussian sigma of the sideways spread; 0: raw per-vertex values
    int threads = 0;        // 0: half the logical processors
};

struct BakeTexture {
    int sub = -1;
    int kind = TEX_THICKNESS;  // TEX_THICKNESS: one channel, mm; TEX_TEETH / TEX_TONGUE: two, (back, gum) 0..1
    int channels = 1;  // Nostrils thickness: two (mm, visibility); other thickness: one
    int width = 0, height = 0;
    float uvMin[2] = {0, 0}, uvMax[2] = {1, 1};  // the UV rectangle the texture covers, edge to edge
    std::vector<uint16_t> texels;                // half floats, channels interleaved, row 0 = uvMin[1]
};

struct TeethReport {
    int teeth = 0, upper = 0, lower = 0;  // connected parts of the teeth submeshes
    float frontMM = 0, depthMM = 0;       // the front of the front teeth along the forward axis, front to back
    float forward[3] = {0, 0, 1};         // horizontal, from the teeth's centre toward the lips' (or +z without lips)
};

struct BakeOutput {
    std::vector<float> vertexThickness;  // mm (kNoThickness where nothing gets through)
    std::vector<float> vertexNoseVisibility;  // 1 outside the nasal cavity; cosine hemisphere visibility, faded at its rim
    std::vector<BakeTexture> textures;   // one per receiving submesh, then one per teeth / tongue submesh
    std::vector<float> vertexBack, vertexGum;  // teeth and tongue vertices (0 elsewhere)
    TeethReport teeth;
    EyeRig eyes;
    double msBvh = 0, msRays = 0, msRaster = 0;
    int threads = 0, occluderTriangles = 0, droppedRays = 0, windingSign = 0;
};

static const float kNoThickness = 1000.0f;

bool Bake(const BakeInput &in, BakeOutput &out, std::string &error);
void EyeRigOf(const BakeInput &in, EyeRig &rig);  // part of Bake
uint16_t FloatToHalf(float f);
float HalfToFloat(uint16_t h);
