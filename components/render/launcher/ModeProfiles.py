"""Display-mode presets and byte-exact test cleanup; shared by the panel and launch.py."""
from __future__ import annotations
import base64
import configparser
import json
import os
from pathlib import Path
import re
import uuid

ROOT = Path(__file__).resolve().parent
VAM = ROOT.parent.parent
PROFILES = ROOT / 'profiles'
MODES = ('VR', 'Desktop')
LAYER = Path(os.environ.get('LOCALAPPDATA', '')) / 'OFXR Bridge/RuntimeLayer/v7443/ofxr_bridge.ini'


def atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        tmp.write_bytes(data)
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def write_json(path, value):
    atomic(path, (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except FileNotFoundError:
        return default


def directory(mode):
    if mode not in MODES:
        raise ValueError('Unknown display mode: ' + str(mode))
    return PROFILES / mode


def selected_mode():
    mode = read_json(PROFILES / 'display-mode.json', {}).get('display_mode', 'VR')
    return mode if mode in MODES else 'VR'


def select(mode):
    directory(mode)
    write_json(PROFILES / 'display-mode.json', {'display_mode': mode})


def manifest():
    return read_json(ROOT / 'profile-manifest.json')


def ini_values(path, keys):
    p = configparser.ConfigParser(interpolation=None, strict=False)
    if Path(path).is_file():
        p.read_string(Path(path).read_bytes().decode('utf-8-sig'))
    sections = {s.lower(): p[s] for s in p.sections()}
    result = {}
    for section, key in keys:
        values = sections.get(section.lower(), {})
        result.setdefault(section, {})[key] = values.get(key.lower())
    return result


def patch_ini(path, values, missing_only=False):
    """Change only named keys; preserve comments, unknown keys, BOM and newlines."""
    path = Path(path)
    raw = path.read_bytes() if path.exists() else b''
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    newline = '\r\n' if '\r\n' in text else '\n'
    lines = text.splitlines(keepends=True)
    current = ''
    wanted = {(s.lower(), k.lower()): (s, k, v) for s, keys in values.items() for k, v in keys.items()}
    seen = set()
    out = []
    for line in lines:
        section = re.match(r'^\s*\[([^]]+)\]', line)
        if section:
            current = section[1].lower()
        item = re.match(r'^(\s*([^=;#\s][^=]*?)\s*=\s*)([^\r\n]*)(\r?\n)?$', line)
        pair = (current, item[2].strip().lower()) if item else None
        if pair in wanted:
            seen.add(pair)
            value = wanted[pair][2]
            if missing_only:
                out.append(line)
            elif value is not None:
                out.append(item[1] + str(value) + (item[4] or ''))
        else:
            out.append(line)
    text = ''.join(out)
    for pair, (section, key, value) in wanted.items():
        if pair in seen or value is None:
            continue
        # Insert into the existing section, or append a new one.
        pattern = re.compile(r'(?im)^\s*\[' + re.escape(section) + r'\][^\r\n]*(?:\r?\n|$)')
        match = pattern.search(text)
        entry = key + ' = ' + str(value) + newline
        if match:
            pos = match.end()
            separator = '' if text[:pos].endswith('\n') else newline
            text = text[:pos] + separator + entry + text[pos:]
        else:
            text = text.rstrip('\r\n') + newline + '[' + section + ']' + newline + entry
    data = (b'\xef\xbb\xbf' if bom else b'') + text.encode('utf-8')
    if data != raw:
        atomic(path, data)


def file_state(path):
    if not path.exists():
        return None
    st = path.stat()
    return dict(data=base64.b64encode(path.read_bytes()).decode('ascii'), mtime_ns=st.st_mtime_ns)


def restore_file(path, state):
    if state is None:
        path.unlink(missing_ok=True)
    else:
        atomic(path, base64.b64decode(state['data']))
        os.utime(path, ns=(state['mtime_ns'], state['mtime_ns']))


def capture(mode, run=None):
    spec = manifest()
    result = dict(version=1, display_mode=mode,
                  config={p: ini_values(VAM / p, keys) for p, keys in spec['config'].items()},
                  whole_files={p: file_state(VAM / p) for p in spec['whole_files']})
    layer = Path(run) / 'desktop-presenter.ini' if mode == 'Desktop' and run else LAYER
    result['layer'] = ini_values(layer, spec['layer'])
    if run:
        result['performance'] = ini_values(Path(run) / 'performance.ini', spec['performance'])
    return result


def initialize():
    """The first launch's VR preset: the plugin settings as they are now."""
    vr = directory('VR')
    if (vr / 'plugin-settings.json').exists():
        return
    vr.mkdir(parents=True, exist_ok=True)
    # Commit marker last: an interrupted first launch repeats this.
    write_json(vr / 'plugin-settings.json', capture('VR'))


def ensure(mode):
    initialize()
    target = directory(mode)
    if mode == 'Desktop' and not (target / 'plugin-settings.json').exists():
        vr = directory('VR')
        s = read_json(vr / 'LaunchPanel.json', {})
        s.update(display_mode='Desktop', seeded_from='VR')
        write_json(target / 'LaunchPanel.json', s)
        snapshot = read_json(vr / 'plugin-settings.json')
        snapshot.update(display_mode='Desktop', seeded_from='VR')
        write_json(target / 'plugin-settings.json', snapshot)
    return target


def apply_layer(mode, run):
    s = read_json(ensure(mode) / 'plugin-settings.json')
    path = Path(run) / 'desktop-presenter.ini' if mode == 'Desktop' else LAYER
    values = s['layer']
    selection = read_json(Path(run) / 'selection.json', {})
    if 'frame_multiplier' in selection:
        values.setdefault('ofxr', {})['frame_multiplier'] = str(selection['frame_multiplier'])
    patch_ini(path, values)


def begin(mode, run, transaction, test=False):
    profile = ensure(mode)
    snapshot = read_json(profile / 'plugin-settings.json')
    spec = manifest()
    paths = list(spec['config']) + spec['whole_files']
    before = {str(VAM / p): file_state(VAM / p) for p in paths}
    if mode == 'VR':
        before[str(LAYER)] = file_state(LAYER)
    write_json(transaction, dict(version=1, mode=mode, run=str(run), test=test, before=before))
    # Journal precedes every write, including partial application failures.
    for path, values in snapshot['config'].items():
        patch_ini(VAM / path, values)
    for path, value in snapshot['whole_files'].items():
        restore_file(VAM / path, value)
    # Future menu keys absent from the panel survive without another hand list.
    patch_ini(Path(run) / 'performance.ini', snapshot.get('performance', {}), missing_only=True)
    apply_layer(mode, run)


def finish(transaction, launched=False):
    state = read_json(transaction)
    if state is None or state.get('finished'):
        return
    if state['test'] or not launched:
        for path, value in state['before'].items():
            restore_file(Path(path), value)
    else:
        profile = directory(state['mode'])
        previous = read_json(profile / 'plugin-settings.json', {})
        snapshot = capture(state['mode'], state['run'])
        if 'seeded_from' in previous:
            snapshot['seeded_from'] = previous['seeded_from']
        write_json(profile / 'plugin-settings.json', snapshot)
        # Carry changes even when the panel was closed before VaM exited.
        import LaunchPanel as panel
        s = panel.load_settings(state['mode'])
        s.update(panel.read_live((Path(state['run']) / 'performance.ini').read_text(encoding='utf-8-sig')))
        value = snapshot['layer'].get('ofxr', {}).get('frame_multiplier')
        if value in ('2', '3', '4'):
            s['frame_multiplier'] = int(value)
        panel.save_settings(s, remember=False)
    state['finished'] = True
    write_json(transaction, state)


