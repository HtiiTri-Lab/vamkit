"""Image checks on SimXR captures (numpy + Pillow).

shift_between(a, b): horizontal/vertical content shift from a to b by phase correlation, with
the strength of the strongest second peak relative to the first (a double image - ghosting -
shows up as a second peak at the other displacement).

During a constant yaw turn every submitted frame is rendered for its own head pose, so the
content of consecutive frames must move by exactly the pose difference. In the eye image (a flat
projection) a yaw turn does not move everything equally: content near the edges moves up to
1/cos^2 more than at the optical axis, and the Quest's eye FOV is asymmetric (54 deg outer,
40 deg inner). So both images are first resampled onto a cylinder around the head's vertical axis
(columns linear in azimuth, rows in tan(elevation)), where a yaw turn is the same shift for every
pixel: dx = d(yaw) * columns per degree. A generated frame drawn for the wrong moment moves by the
wrong amount even when its pose is right, and a blend of two moments gives a second correlation
peak (ghosting).
"""
import json
import math
import os

import numpy as np
from PIL import Image


def load_gray(path):
    image = Image.open(path).convert("L")
    return np.asarray(image, dtype=np.float32)


def _window(shape):
    wy = np.hanning(shape[0])[:, None]
    wx = np.hanning(shape[1])[None, :]
    return wy * wx


def shift_between(a, b, band=0.5):
    """(dx, dy, peak, second_peak_ratio) moving image a onto image b, from the central band
    (fraction of the height and width) of each."""
    h, w = a.shape
    y0, y1 = int(h * (1 - band) / 2), int(h * (1 + band) / 2)
    x0, x1 = int(w * (1 - band) / 2), int(w * (1 + band) / 2)
    a = a[y0:y1, x0:x1]
    b = b[y0:y1, x0:x1]
    win = _window(a.shape)
    fa = np.fft.fft2((a - a.mean()) * win)
    fb = np.fft.fft2((b - b.mean()) * win)
    cross = fb * np.conj(fa)
    cross /= np.abs(cross) + 1e-9
    corr = np.real(np.fft.ifft2(cross))
    corr = np.fft.fftshift(corr)
    cy, cx = corr.shape[0] // 2, corr.shape[1] // 2
    iy, ix = np.unravel_index(np.argmax(corr), corr.shape)
    peak = float(corr[iy, ix])
    # Sub-pixel by a parabola through the neighbours.
    def refine(c_m, c_0, c_p):
        d = c_m - 2 * c_0 + c_p
        return 0.0 if abs(d) < 1e-12 else 0.5 * (c_m - c_p) / d
    dx = ix - cx + (refine(corr[iy, ix - 1], corr[iy, ix], corr[iy, (ix + 1) % corr.shape[1]]) if 0 < ix else 0)
    dy = iy - cy + (refine(corr[iy - 1, ix], corr[iy, ix], corr[(iy + 1) % corr.shape[0], ix]) if 0 < iy else 0)
    masked = corr.copy()
    masked[max(0, iy - 4):iy + 5, max(0, ix - 4):ix + 5] = -np.inf
    second = float(np.max(masked))
    return float(dx), float(dy), peak, (second / peak if peak > 0 else 1.0)


def pixels_per_degree(width, fov_tangents):
    """In the flat eye image at its optical axis, for an eye whose FOV tangents are (left, right)."""
    return width / (fov_tangents[0] + fov_tangents[1]) * math.tan(math.radians(1.0))


# Quest 3 through Virtual Desktop as SimXR reports it (source\SimXR\hmd\sim.cpp): left eye
# tangents (left, right, up, down); the right eye swaps left and right.
LEFT_EYE_TANGENTS = (1.37638187, 0.839099586, 0.965688765, 1.42814791)


def to_cylinder(image, tangents):
    """The flat eye image (tangents left, right, up, down) resampled so that columns are linear in
    azimuth and rows in tan(elevation): a yaw turn becomes a uniform horizontal shift. Returns
    (image, columns per degree). Bilinear; rows cover the elevations seen at every azimuth."""
    left, right, up, down = tangents
    h, w = image.shape
    a0, a1 = math.atan(-left), math.atan(right)
    step = (a1 - a0) / w
    azimuth = a0 + (np.arange(w) + 0.5) * step
    cos_min = min(math.cos(a0), math.cos(a1))
    elevations = np.linspace(up * cos_min, -down * cos_min, h)
    A, E = np.meshgrid(azimuth, elevations)
    tx = np.tan(A)
    ty = E / np.cos(A)
    x = (tx + left) / (left + right) * w - 0.5
    y = (up - ty) / (up + down) * h - 0.5
    x0 = np.clip(np.floor(x).astype(np.int64), 0, w - 2)
    y0 = np.clip(np.floor(y).astype(np.int64), 0, h - 2)
    fx = np.clip(x - x0, 0.0, 1.0)
    fy = np.clip(y - y0, 0.0, 1.0)
    top = image[y0, x0] * (1 - fx) + image[y0, x0 + 1] * fx
    bottom = image[y0 + 1, x0] * (1 - fx) + image[y0 + 1, x0 + 1] * fx
    out = (top * (1 - fy) + bottom * fy).astype(np.float32)
    return out, 1.0 / math.degrees(step)


def read_capture_index(capture_dir, name=None, eye="left"):
    entries = []
    with open(os.path.join(capture_dir, "capture.jsonl")) as f:
        for line in f:
            if line.strip():
                entry = json.loads(line)
                if entry["eye"] == eye and (name is None or os.path.basename(entry["file"]).startswith(name + "-")):
                    entries.append(entry)
    entries.sort(key=lambda e: e["seq"])
    return entries


def turn_consistency(capture_dir, name, fov_tangents=LEFT_EYE_TANGENTS, eye="left"):
    """For consecutive captured frames of a yaw turn: measured content shift vs the shift the
    render poses call for, on the cylinder (see the module text). Positive yaw (SimXR) = turning
    left = content moves right (+x). Captures must cover the whole eye viewport."""
    if eye != "left":
        fov_tangents = (fov_tangents[1], fov_tangents[0], fov_tangents[2], fov_tangents[3])
    entries = read_capture_index(capture_dir, name, eye)
    rows = []
    previous = None
    for entry in entries:
        if entry.get("crop") and list(entry["crop"]) != list(entry.get("eye_viewport", entry["crop"])):
            raise ValueError("%s is cropped (%s of %s); turn_consistency needs whole eye images" % (
                entry["file"], entry["crop"], entry.get("eye_viewport")))
        image, ppd = to_cylinder(load_gray(os.path.join(capture_dir, os.path.basename(entry["file"]))), fov_tangents)
        if previous is not None:
            prev_entry, prev_image = previous
            if prev_image.shape == image.shape and entry["seq"] == prev_entry["seq"] + 1:
                dyaw = entry["render_ypr"][0] - prev_entry["render_ypr"][0]
                dx, dy, peak, second = shift_between(prev_image, image)
                rows.append({"seq": entry["seq"], "dyaw_deg": round(dyaw, 4), "expected_dx": round(dyaw * ppd, 2),
                             "measured_dx": round(dx, 2), "measured_dy": round(dy, 2),
                             "error_px": round(dx - dyaw * ppd, 2), "peak": round(peak, 4),
                             "second_peak_ratio": round(second, 3)})
        previous = (entry, image)
    return rows


def contact_sheet(capture_dir, name, out_path, eye="left", columns=4, tile_width=480):
    entries = read_capture_index(capture_dir, name, eye)
    if not entries:
        return None
    tiles = []
    for entry in entries:
        image = Image.open(os.path.join(capture_dir, os.path.basename(entry["file"]))).convert("RGB")
        scale = tile_width / image.width
        tiles.append(image.resize((tile_width, int(image.height * scale))))
    rows = (len(tiles) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * tile_width, rows * tiles[0].height), (0, 0, 0))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % columns) * tile_width, (i // columns) * tiles[0].height))
    sheet.save(out_path)
    return out_path
