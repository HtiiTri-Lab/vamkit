"""Motion-vector check of a DLSS-G capture burst (OFXR_DLSSG_CAPTURE).

usage: python -I mv_check.py <burst dir> [<output dir for images>]

For each pair of consecutive captured frames A (earlier) and B, with B's
motion vectors (render pixels, B -> A, y down) and depth:
  * global fit: A sampled at B's pixel + s * motion, for s from -0.5 to 1.5;
    s = 1 is right, s = 0 is "nothing moved". Also with the vectors of the
    neighbouring pairs (one frame early / late) and a leftover offset.
  * per tile (6 x 6): best s and leftover offset, so near / far, centre /
    edge and object differences show.
  * camera check: motion recomputed from depth and clip_to_prev_clip (the
    camera data DLSS-G gets) against the motion texture.
  * the in-game DLSS-G output(s) against A, B and their average.
Misfits are mean absolute luminance differences (0..255) over pixels with
detail, so flat areas do not hide a misfit.
"""
import sys
from pathlib import Path

import numpy as np

MAGIC = 0x4358464F
TILES = 6


def read_image(path):
    raw = np.fromfile(path, dtype=np.uint8)
    header = raw[:32].view(np.uint32)
    if header[0] != MAGIC:
        raise ValueError(f'{path}: not a capture image')
    width, height, fmt, bpp, rows = (int(v) for v in header[2:7])
    data = raw[32:32 + width * bpp * rows]
    if fmt in (26, 27, 28, 29, 87, 91):  # RGBA8 / BGRA8 families
        img = data.reshape(rows, width, 4)
        return img[:, :, [2, 1, 0, 3]] if fmt in (87, 91) else img
    if fmt == 41:  # R32_FLOAT
        return data.view(np.float32).reshape(rows, width)
    if fmt == 54:  # R16_FLOAT
        return data.view(np.float16).reshape(rows, width).astype(np.float32)
    if fmt == 34:  # R16G16_FLOAT
        return data.view(np.float16).reshape(rows, width, 2).astype(np.float32)
    raise ValueError(f'{path}: format {fmt} not handled')


def read_text(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if parts:
            values[parts[0]] = [float(v) for v in parts[1:]]
    return values


def luminance(rgba):
    rgb = rgba[:, :, :3].astype(np.float32)
    return rgb[:, :, 0] * 0.299 + rgb[:, :, 1] * 0.587 + rgb[:, :, 2] * 0.114


def sample(img, x, y):
    """Bilinear sample of a 2-D image at float pixel coordinates; NaN outside."""
    h, w = img.shape
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = (x - x0).astype(np.float32)
    fy = (y - y0).astype(np.float32)
    x0 = x0.astype(np.int64)
    y0 = y0.astype(np.int64)
    valid = (x0 >= 0) & (y0 >= 0) & (x0 < w - 1) & (y0 < h - 1)
    x0 = np.clip(x0, 0, w - 2)
    y0 = np.clip(y0, 0, h - 2)
    top = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    bottom = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return np.where(valid, top * (1 - fy) + bottom * fy, np.nan)


class Pair:
    def __init__(self, folder, k):
        stem = folder / f'pair{k}'
        self.k = k
        self.text = read_text(f'{stem}.txt')
        self.color = read_image(f'{stem}-color.bin')
        self.luma = luminance(self.color)
        self.depth = read_image(f'{stem}-depth.bin')
        self.motion = read_image(f'{stem}-motion.bin')
        self.outputs = []
        for j in (1, 2, 3):
            path = Path(f'{stem}-out{j}.bin')
            if path.exists():
                self.outputs.append(luminance(read_image(path)))


class Samples:
    """Render-pixel sample points with B's value there and a motion field."""

    def __init__(self, j, i, x, y, b, mvx, mvy, sx, sy):
        self.j, self.i, self.x, self.y, self.b = j, i, x, y, b
        self.mvx, self.mvy, self.sx, self.sy = mvx, mvy, sx, sy

    def subset(self, sel):
        return Samples(self.j[sel], self.i[sel], self.x[sel], self.y[sel], self.b[sel],
                       self.mvx[sel], self.mvy[sel], self.sx, self.sy)

    def with_motion(self, mvx, mvy):
        return Samples(self.j, self.i, self.x, self.y, self.b, mvx, mvy, self.sx, self.sy)


def clean(mx, my):
    bad = ~np.isfinite(mx) | ~np.isfinite(my) | (np.abs(mx) > 1e4) | (np.abs(my) > 1e4)
    return np.where(bad, 0.0, mx), np.where(bad, 0.0, my), bad


def misfit(a_luma, s_, scale, ox=0.0, oy=0.0):
    """Mean |B - A(x + scale * mv + offset)| (offset in render pixels)."""
    xa = s_.x + (scale * s_.mvx + ox) * s_.sx
    ya = s_.y + (scale * s_.mvy + oy) * s_.sy
    d = np.abs(s_.b - sample(a_luma, xa, ya))
    d = d[np.isfinite(d)]
    return float(d.mean()) if d.size else float('nan')


def best_scale(a_luma, s_, lo=-0.5, hi=1.5, step=0.05):
    scales = np.arange(lo, hi + 1e-9, step)
    errors = np.array([misfit(a_luma, s_, s) for s in scales])
    if not np.isfinite(errors).any():
        return float('nan'), float('nan')
    k = int(np.nanargmin(errors))
    best = scales[k]
    if 0 < k < len(scales) - 1 and np.isfinite(errors[k - 1:k + 2]).all():
        l, c, r = errors[k - 1], errors[k], errors[k + 1]
        den = l - 2 * c + r
        if den > 1e-9:
            best += step * 0.5 * (l - r) / den
    return float(best), float(errors[k])


def best_offset(a_luma, s_, radius=5.0):
    """Leftover translation (render pixels) after the vectors: coarse, then fine."""
    cx = cy = 0.0
    best = None
    for step, reach in ((1.0, radius), (0.25, 0.75)):
        offsets = np.arange(-reach, reach + 1e-9, step)
        best = None
        for oy in offsets:
            for ox in offsets:
                e = misfit(a_luma, s_, 1.0, cx + ox, cy + oy)
                if np.isfinite(e) and (best is None or e < best[0]):
                    best = (e, cx + ox, cy + oy)
        if best is None:
            return float('nan'), float('nan'), float('nan')
        cx, cy = best[1], best[2]
    return cx, cy, best[0]


def camera_motion(text, depth, j, i, w, h):
    """Motion from depth and clip_to_prev_clip (row vectors, D3D clip, y up), as DLSS-G gets them."""
    m = np.array(text['clip_to_prev_clip'], dtype=np.float64).reshape(4, 4)
    jx, jy = text.get('jitter', [0.0, 0.0])[:2]
    # The guide shader's unjittered NDC (y up): content was shifted right / down by the jitter.
    ndc_x = (j + 0.5) / w * 2.0 - 1.0 - 2.0 * jx / w
    ndc_y = 1.0 - (i + 0.5) / h * 2.0 + 2.0 * jy / h
    d = depth[i, j].astype(np.float64)
    p = np.stack([ndc_x, ndc_y, d, np.ones_like(d)], axis=1) @ m
    with np.errstate(divide='ignore', invalid='ignore'):
        px = p[:, 0] / p[:, 3]
        py = p[:, 1] / p[:, 3]
    return (px - ndc_x) * 0.5 * w, (ndc_y - py) * 0.5 * h


def save_png(path, values, shape):
    try:
        from PIL import Image
    except ImportError:
        return
    img = np.nan_to_num(values.reshape(shape), nan=255.0)
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)


def main():
    folder = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)
    pairs = []
    while (folder / f'pair{len(pairs)}.txt').exists():
        pairs.append(Pair(folder, len(pairs)))
    if len(pairs) < 2:
        sys.exit('need at least two pairs')
    rh, rw = pairs[0].depth.shape
    oh, ow = pairs[0].color.shape[:2]
    step, border = 3, 0.04
    jj, ii = np.meshgrid(np.arange(int(rw * border), rw - int(rw * border), step),
                         np.arange(int(rh * border), rh - int(rh * border), step))
    grid_shape = jj.shape
    j_all, i_all = jj.ravel(), ii.ravel()
    sx, sy = ow / rw, oh / rh
    x_all = (j_all + 0.5) * sx - 0.5
    y_all = (i_all + 0.5) * sy - 0.5
    t0 = pairs[0].text
    print(f'{folder.name}: {len(pairs)} frames, output {ow}x{oh}, render {rw}x{rh}, '
          f'lite {int(t0.get("lite", [0])[0])}, outputs {int(t0.get("outputs", [1])[0])}')
    for p in pairs:
        t = p.text
        cm = t.get('centre_motion_px', [0, 0])
        print(f'  frame {p.k}: serial {int(t["serial"][0])}, reset {int(t["reset"][0])}, '
              f'{t.get("frame_time_delta_ms", [0])[0]:.1f} ms, jitter {t["jitter"][0]:+.3f} {t["jitter"][1]:+.3f}, '
              f'camera turn at the centre {cm[0]:+.1f} {cm[1]:+.1f} px')

    for k in range(1, len(pairs)):
        a, b = pairs[k - 1], pairs[k]
        b_all = sample(b.luma, x_all, y_all)
        # Detail: local contrast of B above its 40th percentile.
        gx = np.abs(sample(b.luma, x_all + sx, y_all) - sample(b.luma, x_all - sx, y_all))
        gy = np.abs(sample(b.luma, x_all, y_all + sy) - sample(b.luma, x_all, y_all - sy))
        g = np.nan_to_num(gx + gy)
        detail = g > max(4.0, float(np.percentile(g, 40)))
        mvx, mvy, bad = clean(b.motion[i_all, j_all, 0].astype(np.float64), b.motion[i_all, j_all, 1].astype(np.float64))
        everything = Samples(j_all, i_all, x_all, y_all, b_all, mvx, mvy, sx, sy)
        s_ = everything.subset(detail)
        mag = np.hypot(mvx, mvy)
        print(f'\npair {k - 1} -> {k}: |mv| median {np.median(mag):.1f} px, 95th {np.percentile(mag, 95):.1f} px, '
              f'{bad.mean() * 100:.1f}% invalid; {detail.mean() * 100:.0f}% of the samples have detail')
        e0, e1 = misfit(a.luma, s_, 0.0), misfit(a.luma, s_, 1.0)
        sb, eb = best_scale(a.luma, s_)
        print(f'  global: misfit without motion {e0:.2f}, with the vectors {e1:.2f}; best scale s={sb:.3f} (misfit {eb:.2f})')
        for name, other in (('one frame early', pairs[k - 1] if k >= 2 else None),
                            ('one frame late', pairs[k + 1] if k + 1 < len(pairs) else None)):
            if other is None:
                continue
            ox_, oy_, _ = clean(other.motion[i_all, j_all, 0].astype(np.float64), other.motion[i_all, j_all, 1].astype(np.float64))
            s_other = everything.with_motion(ox_, oy_).subset(detail)
            so, eso = best_scale(a.luma, s_other)
            print(f'  the vectors {name}: misfit {misfit(a.luma, s_other, 1.0):.2f}; best scale s={so:.3f} (misfit {eso:.2f})')
        dx, dy, eoff = best_offset(a.luma, s_)
        print(f'  leftover offset after the vectors: {dx:+.2f} {dy:+.2f} render px (misfit {eoff:.2f})')

        cx, cy = camera_motion(b.text, b.depth, j_all, i_all, rw, rh)
        dcam = np.hypot(cx - mvx, cy - mvy)
        fin = np.isfinite(dcam)
        print(f'  camera check: |motion texture - motion from depth + clip_to_prev_clip| median {np.median(dcam[fin]):.3f} px, '
              f'{(dcam[fin] > 0.5).mean() * 100:.1f}% over 0.5 px')
        s_cam = everything.with_motion(np.nan_to_num(cx), np.nan_to_num(cy)).subset(detail)
        sc, esc = best_scale(a.luma, s_cam)
        print(f'  camera-only motion: misfit {misfit(a.luma, s_cam, 1.0):.2f}; best scale s={sc:.3f} (misfit {esc:.2f})')

        ti = np.minimum((s_.i * TILES) // rh, TILES - 1)
        tj = np.minimum((s_.j * TILES) // rw, TILES - 1)
        depth_s = b.depth[s_.i, s_.j]
        mag_s = np.hypot(s_.mvx, s_.mvy)
        print('  per tile: best s | leftover offset (render px) | median |mv| px | median depth')
        for r in range(TILES):
            cells = []
            for c in range(TILES):
                sel = (ti == r) & (tj == c)
                if sel.sum() < 150:
                    cells.append(f'{"--":^25}')
                    continue
                tile = s_.subset(sel)
                st, _ = best_scale(a.luma, tile, lo=0.0, hi=2.0)
                ox, oy, _ = best_offset(a.luma, tile, radius=3.0)
                cells.append(f'{st:4.2f} {ox:+4.1f},{oy:+4.1f} {np.median(mag_s[sel]):4.0f} {np.median(depth_s[sel]):.4f}')
            print('   ' + ' | '.join(cells))

        if out_dir:
            warped = sample(a.luma, x_all + mvx * sx, y_all + mvy * sy)
            save_png(out_dir / f'pair{k}-misfit-vectors.png', np.abs(b_all - warped) * 4.0, grid_shape)
            still = sample(a.luma, x_all, y_all)
            save_png(out_dir / f'pair{k}-misfit-none.png', np.abs(b_all - still) * 4.0, grid_shape)

        if b.outputs:
            a_d = sample(a.luma, s_.x, s_.y)
            for n, out in enumerate(b.outputs, 1):
                o = sample(out, s_.x, s_.y)
                ea = np.nanmean(np.abs(o - a_d))
                eb_ = np.nanmean(np.abs(o - s_.b))
                eab = np.nanmean(np.abs(o - 0.5 * (a_d + s_.b)))
                print(f'  in-game output {n}: |out - A| {ea:.2f}, |out - B| {eb_:.2f}, |out - (A+B)/2| {eab:.2f}')


if __name__ == '__main__':
    main()
