"""Faster SimXR picture series of VaM (2026-10-18): one VaM for several scenes, the head placed from what the eye
camera sees, every capture checked as it lands, the analysis running while VaM still captures.

    from shoot import Shoot
    with Shoot('eyesteeth', out, ini={('Performance', 'FrameGeneration'): 0}) as s:
        for name, path in scenes.items():
            s.load(path)                                   # the same VaM (a new one only past the time budget)
            eyes = s.frame('eyes', 0.15, (1400, 900))      # head pose + crop, checked (FramingError if not found)
            s.ini('NR', 'Enabled', 1)
            c = s.still('%s-eyes-nr1' % name, eyes)        # 4 frames, checked, taken again once if a check fails
            s.later(analyse_one, c)                        # in the background; s.done() waits for all of it

Framing (Optics, find_face, find_mouth): a whole-eye preview at 1/4 size, OpenCV's Haar face and eye detectors; the
distance from the parallax of a 3 cm sideways step (template match of the face); then the SimXR head moves so that the
target (both eyes' midpoint, or the lips' centroid) sits on the left eye's optical axis at the wanted distance; one
more preview checks it and corrects once. Distances are from the eye camera to the face plane. The head pose found
for a view is reused in the next scene (the same person, only the lights change) and checked with one preview there.

Checks after each still capture: VaM's menus stayed closed (with them open the DLSS bridge renders native frames), the
frames were rendered at the requested pose, they have settled (NR / DLSS are temporal), the picture is not black, and
the target is still in the crop where the light lets the detector see it. Each check's result goes into the capture's
record; a failed one (but the detector) means one more try.

Left eye only: its optics are SimXR's Quest 3 (source\\SimXR\\hmd\\sim.cpp: tangents up 0.9657, down 1.4281, left 1.3764,
right 0.8391; 3072 x 3264), so its optical axis is at x 1908.5, y 1316.7 of the eye image.

Timing per step goes to <out>\\timing.json and is printed by done().
"""
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vamrun import TASK_ROOT, VamRun  # noqa: E402

VAM_ROOT = os.path.abspath(os.path.join(TASK_ROOT, '..', '..'))


class FramingError(RuntimeError):
    pass


# ---- optics ----------------------------------------------------------------------------------------------------------

class Optics:
    LEFT, RIGHT, UP, DOWN = 1.37638187, 0.839099586, 0.965688765, 1.42814791
    W, H = 3072, 3264
    FX = W / (LEFT + RIGHT)
    FY = H / (UP + DOWN)
    CX = LEFT * FX
    CY = UP * FY

    @classmethod
    def crop(cls, size):
        """(x, y, w, h) of a w x h crop centred on the optical axis"""
        w, h = size
        return [int(round(cls.CX - w / 2)), int(round(cls.CY - h / 2)), int(w), int(h)]

    @classmethod
    def offset(cls, u, v, depth):
        """head move (x, y) in metres that brings eye-image pixel (u, v) at depth onto the optical axis"""
        return (u - cls.CX) / cls.FX * depth, -(v - cls.CY) / cls.FY * depth


# ---- detection -------------------------------------------------------------------------------------------------------

_CASCADES = {}


def _cascade(name):
    if name not in _CASCADES:
        import cv2
        _CASCADES[name] = cv2.CascadeClassifier(cv2.data.haarcascades + name)
    return _CASCADES[name]


def find_face(img, scale=1.0):
    """the largest frontal face in a BGR image: {'box': [x, y, w, h], 'eyes': [(x, y, w) sorted by x] (0-2)}, in the
    image's pixels divided by scale (so in eye pixels for a preview taken at that scale); None if no face"""
    import cv2
    g = cv2.equalizeHist(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
    faces = _cascade('haarcascade_frontalface_alt2.xml').detectMultiScale(g, 1.1, 4, minSize=(32, 32))
    if len(faces) == 0:
        return None
    x, y, w, h = max(faces, key=lambda f: f[2] * f[3])
    roi = g[y:y + h * 5 // 8, x:x + w]
    eyes = _cascade('haarcascade_eye.xml').detectMultiScale(roi, 1.05, 4, minSize=(w // 8, w // 8), maxSize=(w // 3, w // 3))
    eyes = sorted(((x + ex + ew / 2) / scale, (y + ey + eh / 2) / scale, ew / scale) for ex, ey, ew, eh in eyes)
    if len(eyes) > 2:   # the two of most similar height and size
        best = min(((a, b) for i, a in enumerate(eyes) for b in eyes[i + 1:]),
                   key=lambda p: abs(p[0][1] - p[1][1]) + abs(p[0][2] - p[1][2]) + (0 if abs(p[0][0] - p[1][0]) > p[0][2] else 1e6))
        eyes = sorted(best)
    return {'box': [x / scale, y / scale, w / scale, h / scale], 'eyes': eyes}


def find_mouth(img, scale, region, margin=5.0):
    """centroid of the lips (redder than the skin around them: Lab a* above the region's 85th percentile, the largest
    blob, at least margin above the region's median: lips 8-13 on the eyes/teeth run's mid views, chin, neck and
    forehead 1-2) inside region = (x0, y0, x1, y1) in eye pixels; (x, y, area_px) in eye pixels or None"""
    import cv2
    import numpy as np
    x0, y0, x1, y1 = [int(round(v * scale)) for v in region]
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(img.shape[1], x1), min(img.shape[0], y1)
    if x1 - x0 < 8 or y1 - y0 < 8:
        return None
    lab = cv2.cvtColor(img[y0:y1, x0:x1], cv2.COLOR_BGR2LAB).astype(np.float32)
    a, light = lab[..., 1], lab[..., 0]
    lit = light > 25
    if lit.sum() < 50:
        return None
    m = ((a > np.percentile(a[lit], 85)) & lit).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    n, lab_img, stats, cent = cv2.connectedComponentsWithStats(m)
    if n < 2:
        return None
    k = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    if a[lab_img == k].mean() - np.median(a[lit]) < margin:
        return None
    return ((x0 + cent[k][0]) / scale, (y0 + cent[k][1]) / scale, float(stats[k, cv2.CC_STAT_AREA]) / scale / scale)


def match_scaled(ref, img, box, k=1.0, scales=(0.8, 0.87, 0.94, 1.0, 1.07, 1.15, 1.25)):
    """where box (x0, y0, x1, y1 in ref's pixels) of BGR image ref sits in BGR image img when everything there is
    about k times larger: (x, y) of the box centre in img's pixels (sub-pixel), the scale found, the correlation; the
    colour template at k x scales, the best one; None if the template does not fit"""
    import cv2
    x0, y0, x1, y1 = [int(round(v)) for v in box]
    x0, y0 = max(0, x0), max(0, y0)
    t0 = ref[y0:y1, x0:x1]
    if t0.shape[0] < 8 or t0.shape[1] < 8:
        return None

    def sub(c_m, c_0, c_p):
        d = c_m - 2 * c_0 + c_p
        return 0.0 if abs(d) < 1e-12 else 0.5 * (c_m - c_p) / d
    best = None
    for s in scales:
        t = cv2.resize(t0, None, fx=k * s, fy=k * s, interpolation=cv2.INTER_CUBIC) if k * s != 1.0 else t0
        if t.shape[0] >= img.shape[0] or t.shape[1] >= img.shape[1]:
            continue
        r = cv2.matchTemplate(img, t, cv2.TM_CCOEFF_NORMED)
        _, peak, _, (px, py) = cv2.minMaxLoc(r)
        if best is None or peak > best[3]:
            fx = px + (sub(r[py, px - 1], r[py, px], r[py, px + 1]) if 0 < px < r.shape[1] - 1 else 0)
            fy = py + (sub(r[py - 1, px], r[py, px], r[py + 1, px]) if 0 < py < r.shape[0] - 1 else 0)
            best = (fx + (t.shape[1] - 1) / 2, fy + (t.shape[0] - 1) / 2, k * s, float(peak))
    return best


# ---- the session -----------------------------------------------------------------------------------------------------

def vam_running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq VaM.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    return 'VaM.exe' in out


def set_ini(path, section, key, value):
    """one value in an ini file, the file replaced whole (as the VR menu does)"""
    with open(path, encoding='utf-8-sig', newline='') as f:
        raw = f.read()
    eol = '\r\n' if '\r\n' in raw else '\n'
    lines = raw.splitlines()
    cur, done = None, False
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            if cur == section and not done:
                lines.insert(i, '%s=%s' % (key, value))
                done = True
                break
            cur = s[1:-1]
        elif cur == section and '=' in s and s.split('=', 1)[0].strip() == key:
            lines[i] = '%s=%s' % (key, value)
            done = True
            break
    if not done:
        if cur != section:
            lines.append('[%s]' % section)
        lines.append('%s=%s' % (key, value))
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='') as f:
        f.write(eol.join(lines) + eol)
    for i in range(40):                 # the pipeline re-reads the file every 250 ms and may hold it open
        try:
            os.replace(tmp, path)
            break
        except PermissionError:
            if i == 39:
                raise
            time.sleep(0.05)


class Frame(dict):
    """a view: head pos / ypr, crop, distance, what framing found (dict, so it goes into JSON as it is)"""


class Shoot:
    PREVIEW_SCALE = 0.25
    SETTLE_S = 2.5          # NR and DLSS SR are temporal: frames after a pose or setting change
    MOVE_S = 0.6            # a head move reaching the rendered frames (real frames at ~30 fps, queue 1)
    PARALLAX_M = 0.03

    def __init__(self, name, out, ini=None, budget_s=500, workers=2, overrides=None):
        self.name = name
        self.out = os.path.abspath(str(out))
        os.makedirs(self.out, exist_ok=True)
        self.ini_values = dict(ini or {})
        self.budget_s = budget_s
        self.overrides = dict(overrides or {})
        self.run = None
        self.runs = []
        self.scene = None
        self.scene_s = []
        self.frames = {}
        self.timing = []
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=workers)
        self.jobs = []
        self.prefs = os.path.join(VAM_ROOT, 'prefs.json')
        self.prefs_saved = os.path.join(self.out, 'prefs.before.json')
        self.t0 = time.time()

    # -- timing
    def _t(self, what, t):
        self.timing.append({'what': what, 's': round(time.time() - t, 2), 'at': round(t - self.t0, 1)})

    def log(self, msg):
        print('  %s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)

    # -- VaM
    def __enter__(self):
        if vam_running():
            raise SystemExit('VaM is running; one VaM at a time')
        if os.path.exists(os.path.join(TASK_ROOT, 'active-test.json')):
            raise SystemExit('another test is running (active-test.json)')
        if not os.path.exists(self.prefs_saved):
            shutil.copy2(self.prefs, self.prefs_saved)
        return self

    def __exit__(self, *exc):
        try:
            self._stop()
        finally:
            with open(self.prefs, 'rb') as a, open(self.prefs_saved, 'rb') as b:
                if a.read() != b.read():
                    shutil.copy2(self.prefs_saved, self.prefs)
                    self.log('prefs.json restored')
            self.done()

    def _start(self):
        t = time.time()
        self.run = VamRun(self.name, self.overrides, max_seconds=min(600, self.budget_s + 90))   # Launch.ps1 allows 600 s
        try:
            self.run.start()
            self.run.wait_ready()
        except BaseException:
            self._stop()
            raise
        self.session_t0 = t
        for (section, key), value in self.ini_values.items():
            set_ini(self.perf_ini, section, key, value)
        self.runs.append({'run_dir': self.run.run_dir, 'log_dir': self.run.log_dir, 'scenes': []})
        self._t('VaM start', t)

    def _stop(self):
        if self.run is not None:
            t = time.time()
            run, self.run = self.run, None
            try:
                run.__exit__(None, None, None)
            finally:
                if self.runs:
                    self.runs[-1]['failure'] = run.failure
                self._t('VaM stop', t)

    @property
    def perf_ini(self):
        return os.path.join(self.run.run_dir, 'performance.ini')

    @property
    def log_dir(self):
        return self.run.log_dir

    def ini(self, section, key, value, wait_s=1.5):
        """a performance.ini value for the rest of the series (the VR menu takes outside changes over every 0.5 s)"""
        if self.ini_values.get((section, key)) == value:
            return
        self.ini_values[(section, key)] = value
        if self.run is not None:
            set_ini(self.perf_ini, section, key, value)
            time.sleep(wait_s)

    def load(self, path, name=None, freeze=True):
        """loads a scene in the running VaM; a new VaM when the next scene would not fit into the time budget"""
        need = 1.3 * max(self.scene_s) if self.scene_s else 0
        if self.run is not None and time.time() - self.session_t0 + need > self.budget_s:
            self.log('time budget: a new VaM for the next scene')
            self._stop()
        if self.run is None:
            self._start()
        self._scene_end()
        t = time.time()
        self.scene = name or os.path.splitext(os.path.basename(str(path)))[0]
        self.scene_t0 = t
        self.log('loading %s' % self.scene)
        self.run.load_scene(str(path))
        self.close_menus()
        # VaM keeps its freeze across scene loads (only the toggle, recording
        # and game mode write it): set it either way, or a frozen earlier scene
        # leaves this one frozen (Timeline idle, plugins waiting to restore).
        self.run.call_agent('freeze', on=bool(freeze))
        self.runs[-1]['scenes'].append(self.scene)
        self._t('load %s' % self.scene, t)

    def _scene_end(self):
        if self.scene is not None and getattr(self, 'scene_t0', None):
            self.scene_s.append(time.time() - self.scene_t0)
            self.scene_t0 = None

    def close_menus(self):
        """True when VaM's menus are closed (the agent's close_all, checked once more after a moment)"""
        r = self.run.call_agent('hud', close_all=True)
        if r.get('menus_open'):
            time.sleep(0.5)
            r = self.run.call_agent('hud', close_all=True)
        return not r.get('menus_open')

    def menus_open(self):
        return bool(self.run.call_agent('status').get('menus_open'))

    def head(self, pos, ypr=(0, 0, 0), wait=True):
        self.run.sim.head(pos=[round(float(v), 5) for v in pos], ypr=[float(v) for v in ypr])
        if wait:
            time.sleep(self.MOVE_S)

    # -- captures
    def _capture(self, name, dirname, **fields):
        d = os.path.join(self.run.log_dir, 'simxr', dirname)
        os.makedirs(d, exist_ok=True)
        self.run.capture(name=name, dir=d, eyes='left', timeout=180, **fields)
        entries = [json.loads(l) for l in open(os.path.join(d, 'capture.jsonl'), encoding='utf-8') if l.strip()]
        entries = sorted((e for e in entries if os.path.basename(e['file']).startswith(name + '-')), key=lambda e: e['seq'])
        return d, entries

    def preview(self, name):
        """one whole left-eye frame at 1/4 size: (BGR image, its capture record)"""
        import cv2
        d, es = self._capture(name, 'preview', frames=1, scale=self.PREVIEW_SCALE, delay_ms=150)
        e = es[-1]
        return cv2.imread(os.path.join(d, os.path.basename(e['file']))), e

    # -- framing
    def frame(self, view, distance, size, target=None, start=(0.0, 1.60, -0.6), tol_px=None):
        """head pose and crop for view ('eyes' / 'mouth' / 'face', or target=) with the target on the optical axis at
        distance (m, eye camera to the target); the crop is size (w, h) around the axis. The first scene measures the
        depth from the start pose (parallax); later scenes reuse it (the same person, only the lights change) and only
        find the target again. Raises FramingError."""
        t = time.time()
        known = self.frames.get(view)
        f = self._measure(view, distance, size, target or view, start, tol_px or 0.04 * min(size), known)
        self.frames.setdefault(view, f)
        self._t('frame %s%s' % (view, ' (depth known)' if known else ''), t)
        return f

    def _measure(self, view, distance, size, target, start, tol_px, known=None):
        S = self.PREVIEW_SCALE
        depth = known['found']['depth_m'] if known else None
        face = None
        for back in (0.0, 0.15, 0.3):   # step back until the face detector sees a face
            pos = [start[0], start[1], start[2] + back]
            self.head(pos)
            img0, e0 = self.preview('frame-%s-%s-a' % (self.scene, view))
            face = find_face(img0, S)
            if face:
                break
        if not face:
            raise FramingError('%s: no face in view from %s' % (view, pos))
        if back:
            depth, known = None, None
        u, v, box = self._target(img0, face, target)
        if known:
            # the same face box as in the first scene (same person, same pose, other lights): its target, found in
            # that scene's light, is the better one
            kx, ky, kw, kh = known['found']['face_px']
            x, y, w, h = face['box']
            if abs(x + w / 2 - kx - kw / 2) < 0.05 * kw and abs(y + h / 2 - ky - kh / 2) < 0.05 * kh and abs(w / kw - 1) < 0.08:
                (u, v), box = known['found']['target_px'], known['found']['box']
        if u is None:
            self._sheet(img0, None, '%s-%s' % (self.scene, view))
            raise FramingError('%s: %s not found in the face (see framing\\%s-%s.jpg)' % (view, target, self.scene, view))
        found = {'start': pos, 'face_px': [round(x) for x in face['box']], 'eyes_found': len(face['eyes']),
                 'target_px': [round(u), round(v)], 'box': [round(c) for c in box]}
        if depth is None:
            # the target's distance from the parallax of a sideways step: its box matched in the second preview
            self.head([pos[0] + self.PARALLAX_M] + pos[1:])
            img1, e1 = self.preview('frame-%s-%s-b' % (self.scene, view))
            m = match_scaled(img0, img1, [c * S for c in box], scales=(1.0,))
            moved = e1['render_pos'][0] - e0['render_pos'][0]
            du, dv = (m[0] / S - u, m[1] / S - v) if m else (0.0, 0.0)
            if not m or m[3] < 0.6 or abs(du) < 5 or abs(dv) > 0.2 * abs(du):
                raise FramingError('%s: parallax not measured (shift %.1f, %.1f px, match %s)' % (view, du, dv, m and round(m[3], 2)))
            depth = Optics.FX * moved / -du
            found.update(parallax_px=round(du, 1), parallax_match=round(m[3], 3))
        found['depth_m'] = round(depth, 4)
        ox, oy = Optics.offset(u, v, depth)
        f = Frame(view=view, target=target, distance=distance, size=list(size), crop=Optics.crop(size), ypr=[0, 0, 0],
                  found=found, pos=[pos[0] + ox, pos[1] + oy, pos[2] - (depth - distance)])
        # at the close pose: find the target again (eyes: their mid-distance picture, scaled, which also gives the
        # distance; mouth: the lips' colour, as the mouth seen from in front looks different from the one seen from
        # below at the start pose); correct up to twice
        for i in range(3):
            self.head(f['pos'], f['ypr'])
            img, e = self.preview('frame-%s-%s-c%d' % (self.scene, view, i))
            err, dist, score = self._close(img0, img, box, target, depth, distance)
            found['check_%d' % i] = {'error_px': err and [round(err[0]), round(err[1])], 'distance_m': round(dist, 4), 'match': score}
            if err is None:
                self._sheet(img, f, '%s-%s' % (self.scene, view))
                raise FramingError('%s: %s not found at %.2f m (see framing\\%s-%s.jpg)' % (view, target, distance, self.scene, view))
            if max(abs(err[0]), abs(err[1])) <= tol_px and abs(dist - distance) <= 0.07 * distance:
                break
            ox, oy = Optics.offset(Optics.CX + err[0], Optics.CY + err[1], dist)
            f['pos'] = [f['pos'][0] + ox, f['pos'][1] + oy, f['pos'][2] - (dist - distance)]
        else:
            self._sheet(img, f, '%s-%s' % (self.scene, view))
            raise FramingError('%s: still off after two corrections: %s' % (view, found['check_2']))
        f['pos'] = [round(p, 5) for p in f['pos']]
        self.log('framed %s: target at %.3f m -> %.3f m, head %s, %s' % (view, depth, distance, f['pos'], found['check_%d' % i]))
        self._sheet(img, f, '%s-%s' % (self.scene, view))
        return f

    @staticmethod
    def _target(img, face, target):
        """the target's pixel (u, v) in a mid-distance preview with this face, and the box (eye pixels) around it that
        is matched to find it again; (None, None, None) if not found"""
        x, y, w, h = face['box']
        eyes = face['eyes']
        # the eyes' midpoint from the Haar face box (key / side / ibl mid views: within 44 px, 4 % of the box) unless
        # both eyes were found and agree with it (the eye detector also takes lash edges for eyes)
        u, v = x + w / 2, y + h * 0.41
        sp = 0.366 * w           # eye spacing / face box on the mid views
        if len(eyes) == 2 and abs((eyes[0][0] + eyes[1][0]) / 2 - u) < 0.08 * w and abs((eyes[0][1] + eyes[1][1]) / 2 - v) < 0.06 * h \
                and abs(abs(eyes[1][0] - eyes[0][0]) / sp - 1) < 0.2:
            u, v = (eyes[0][0] + eyes[1][0]) / 2, (eyes[0][1] + eyes[1][1]) / 2
            sp = abs(eyes[1][0] - eyes[0][0])
        if target == 'face':
            u, v = x + w / 2, y + h * 0.55
            return u, v, (u - 0.3 * w, v - 0.3 * h, u + 0.3 * w, v + 0.3 * h)
        if target == 'eyes':
            return u, v, (u - 0.85 * sp, v - 0.3 * sp, u + 0.85 * sp, v + 0.3 * sp)
        if target == 'mouth':
            m = find_mouth(img, Shoot.PREVIEW_SCALE, (x + w * 0.2, y + h * 0.62, x + w * 0.8, y + h * 1.1))
            if not m:
                return None, None, None
            return m[0], m[1], (m[0] - 0.6 * sp, m[1] - 0.35 * sp, m[0] + 0.6 * sp, m[1] + 0.35 * sp)
        raise ValueError(target)

    def _close(self, ref, img, box, target, depth, distance):
        """((du, dv) eye pixels of the target from the optical axis, its distance (m), match score) in a preview at the
        close pose; (None, distance, score) if not found"""
        S = self.PREVIEW_SCALE
        if target == 'mouth':
            half = 0.05 * Optics.FX / distance       # +-5 cm around the axis
            m = find_mouth(img, S, (Optics.CX - half, Optics.CY - half * 0.6, Optics.CX + half, Optics.CY + half * 0.6))
            return ((m[0] - Optics.CX, m[1] - Optics.CY) if m else None), distance, None
        m = match_scaled(ref, img, [c * S for c in box], depth / distance)
        if not m or m[3] < 0.6:
            return None, distance, m and round(m[3], 3)
        return (m[0] / S - Optics.CX, m[1] / S - Optics.CY), depth / m[2], round(m[3], 3)

    def _sheet(self, img, f, name):
        """the preview with the crop and the target marked: <out>\\framing\\<name>.jpg"""
        import cv2
        s = self.PREVIEW_SCALE
        im = img.copy()
        if f:
            x, y, w, h = f['crop']
            cv2.rectangle(im, (int(x * s), int(y * s)), (int((x + w) * s), int((y + h) * s)), (0, 255, 255), 2)
        cv2.drawMarker(im, (int(Optics.CX * s), int(Optics.CY * s)), (0, 0, 255), cv2.MARKER_CROSS, 24, 2)
        os.makedirs(os.path.join(self.out, 'framing'), exist_ok=True)
        cv2.imwrite(os.path.join(self.out, 'framing', name + '.jpg'), im, [cv2.IMWRITE_JPEG_QUALITY, 85])

    # -- checked captures
    def still(self, tag, f, frames=4, settle_s=None, tries=2, **record):
        """frames frames of f's crop at f's pose, after the settle time; checked; returns the capture record (with
        'ok' and 'checks'), taken again (once) when a check fails"""
        settle_s = self.SETTLE_S if settle_s is None else settle_s
        for attempt in range(tries):
            t = time.time()
            closed = self.close_menus()
            self.head(f['pos'], f['ypr'], wait=False)
            time.sleep(settle_s)
            name = tag + ('-a%d' % attempt if attempt else '')
            d, es = self._capture(name, 'captures', frames=frames, every=1, scale=1.0, crop=f['crop'])
            c = dict(record, tag=name, kind='static', view=f['view'], scene=self.scene, dir=d,
                     files=[os.path.basename(e['file']) for e in es], log_dir=self.run.log_dir, pose=f['pos'])
            c['checks'] = self._check(c, es, f, closed)
            c['ok'] = all(c['checks'][k] for k in ('menus', 'pose', 'settled', 'lit'))
            self._t('still %s' % name, t)
            self.log('captured %s%s' % (name, '' if c['ok'] else ' (failed %s: again)' % ', '.join(
                k for k in ('menus', 'pose', 'settled', 'lit') if not c['checks'][k])))
            if c['ok']:
                break
        return c

    def moving(self, tag, f, frames=12, yaw_speed=10.0, lead_s=0.6, **record):
        """frames frames while the head turns at yaw_speed deg/s through f's pose (no settle: the point is motion)"""
        t = time.time()
        closed = self.close_menus()
        self.head(f['pos'], [-yaw_speed * lead_s, 0, 0], wait=False)
        time.sleep(1.5)
        self.run.sim.motion(type='turn', axis='yaw', speed=yaw_speed, duration_s=2 * lead_s + frames / 30.0, delay_ms=200)
        d, es = self._capture(tag, 'captures', frames=frames, every=1, delay_ms=200 + int(lead_s * 500), scale=1.0, crop=f['crop'])
        time.sleep(0.3)
        self.head(f['pos'], f['ypr'], wait=False)
        c = dict(record, tag=tag, kind='motion', view=f['view'], scene=self.scene, dir=d,
                 files=[os.path.basename(e['file']) for e in es], log_dir=self.run.log_dir, pose=f['pos'])
        c['checks'] = {'menus': bool(closed) and not self.menus_open()}
        c['ok'] = c['checks']['menus']
        self._t('moving %s' % tag, t)
        self.log('captured %s%s' % (tag, '' if c['ok'] else ' (menus opened)'))
        return c

    def _check(self, c, es, f, closed):
        import cv2
        import numpy as np
        ch = {'menus': bool(closed) and not self.menus_open()}
        ch['pose'] = bool(all(max(abs(a - b) for a, b in zip(e['render_pos'], self._eye_pos(f['pos']))) < 0.002 for e in es))
        small = [cv2.resize(cv2.imread(os.path.join(c['dir'], os.path.basename(e['file']))), None, fx=0.25, fy=0.25,
                            interpolation=cv2.INTER_AREA).astype(np.float32) for e in es]
        diffs = [float(np.abs(s - small[0]).mean()) for s in small[1:]]
        ch['change'] = [round(x, 3) for x in diffs]
        ch['settled'] = bool(not diffs or max(diffs) < 1.0)
        ch['level'] = round(float(small[0].mean()), 1)
        ch['lit'] = bool(ch['level'] > 6)
        return ch

    @staticmethod
    def _eye_pos(head_pos, ipd=0.0577):
        return [head_pos[0] - ipd / 2, head_pos[1], head_pos[2]]

    # -- background analysis
    def later(self, fn, *args, **kw):
        def run():
            t = time.time()
            try:
                return fn(*args, **kw)
            except Exception:
                traceback.print_exc()
                raise
            finally:
                self.timing.append({'what': 'analysis %s' % getattr(fn, '__name__', fn), 's': round(time.time() - t, 2),
                                    'at': round(t - self.t0, 1), 'background': True})
        job = self.pool.submit(run)
        self.jobs.append(job)
        return job

    def done(self):
        """waits for the background analysis; writes and prints the timing"""
        t = time.time()
        for j in self.jobs:
            try:
                j.result()
            except Exception:
                pass
        self.pool.shutdown(wait=True)
        if self.jobs:
            self._t('analysis left after VaM', t)
        self.jobs = []
        total = time.time() - self.t0
        fg = [x for x in self.timing if not x.get('background')]
        sums = {}
        for x in fg:
            k = x['what'].split(' ')[0]
            sums[k] = sums.get(k, 0) + x['s']
        with open(os.path.join(self.out, 'timing.json'), 'w', encoding='utf-8') as fh:
            json.dump({'total_s': round(total, 1), 'by_kind_s': {k: round(v, 1) for k, v in sums.items()},
                       'steps': self.timing, 'runs': self.runs, 'frames': self.frames}, fh, indent=1,
                      default=lambda x: x.item() if hasattr(x, 'item') else str(x))
        print('  time %.0f s: %s' % (total, ', '.join('%s %.0f s' % (k, v) for k, v in sorted(sums.items(), key=lambda kv: -kv[1]))), flush=True)
