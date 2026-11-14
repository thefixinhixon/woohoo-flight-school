#!/usr/bin/env python3
"""FigCraft mockup of the launcher: writes build_mockup.json, runs it into launcher.figcraft, exports png/<frame>.png.

usage (from the repository root): python3 launcher/design/make_mockup.py
Needs figcraft-cli (FigCraft 0.14). The colour tokens are the same as launcher/widgets.py's THEME.
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))
from widgets import THEME as T  # noqa: E402  (single source of the colour tokens)

FONT = 'Lato'
W, H, HEAD, RAIL, STATUS = 1000, 680, 140, 200, 28
steps = []
FRAMES = []


TOKEN = {v.lower(): k for k, v in reversed(list(T.items()))}   # colour -> token name (first name wins)


def bind(props):
    """fills and strokes that are theme colours become bindings to the file's colour variables"""
    for k in ('fill', 'stroke'):
        v = props.get(k)
        if isinstance(v, str) and v.lower() in TOKEN: props[k] = {'$var': TOKEN[v.lower()]}
    return props


def add(command, params):
    if 'props' in params: bind(params['props'])
    steps.append(dict(command=command, params=params)); return {'$step': len(steps) - 1}


def frame(name, x, y, w, h, parent=None, fill=None, radius=0, stroke=None, clip=True):
    props = dict(fill=fill or 'none', cornerRadius=radius, clip=clip)
    if stroke: props.update(stroke=stroke, strokeWeight=1, strokeAlign='inside')
    p = dict(type='frame', name=name, x=x, y=y, width=w, height=h, props=props, select=False)
    if parent: p['parent'] = parent
    return add('node.create', p)


def rect(parent, x, y, w, h, fill, radius=0, name='Rect', stroke=None):
    props = dict(fill=fill, cornerRadius=radius)
    if stroke: props.update(stroke=stroke, strokeWeight=1, strokeAlign='inside')
    return add('node.create', dict(type='rectangle', parent=parent, x=x, y=y, width=w, height=h, name=name, props=props, select=False))


def ellipse(parent, x, y, d, fill, name='Dot'):
    return add('node.create', dict(type='ellipse', parent=parent, x=x, y=y, width=d, height=d, name=name, props=dict(fill=fill), select=False))


def text(parent, x, y, s, size=14, style='Regular', fill=None, width=None, align='left', name=None):
    props = dict(fontFamily=FONT, fontStyle=style, fontSize=size, fill=fill or T['text'], textAlign=align)
    if width: props.update(width=width, textAutoResize='height')
    return add('node.create', dict(type='text', parent=parent, x=x, y=y, text=s, name=name or s[:24], props=props, select=False))


def image(parent, path, x, y, w, h, name):
    return add('image.place', dict(path=path, x=x, y=y, width=w, height=h, parent=parent, name=name, mode='fill'))


def pill(parent, x, y, w, h, label, kind='secondary', size=14, icon=''):
    fill = dict(primary=T['play'], secondary=T['surface2'], ghost=T['surface'], danger=T['error'])[kind]
    f = frame(f'Button / {label}', x, y, w, h, parent, fill, h // 2, None if kind != 'ghost' else T['line'])
    fg = T['on_play'] if kind == 'primary' else T['text']
    text(f, 0, (h - size * 1.25) / 2, (icon + '  ' if icon else '') + label, size, 'Black' if kind == 'primary' else 'Bold', fg, w, 'center')
    return f


def segmented(parent, x, y, options, active, disabled=False):
    widths = [max(56, len(o) * 8 + 26) for o in options]
    if x is None: x = CW - 20 - (sum(widths) + 8)
    f = frame('Segmented', x, y, sum(widths) + 8, 36, parent, T['bg'], 18, T['line'])
    cx = 4
    for o, w in zip(options, widths):
        on = o == active
        if on: rect(f, cx, 4, w, 28, T['accent'] if not disabled else T['line'], 14, f'Selected {o}')
        text(f, cx, 11, o, 13, 'Bold' if on else 'Regular', T['on_accent'] if on and not disabled else (T['muted'] if not on else T['text']), w, 'center')
        cx += w
    return f


def toggle(parent, x, y, on):
    f = frame('Toggle', x, y, 46, 26, parent, T['accent'] if on else T['line'], 13)
    ellipse(f, 23 if on else 3, 3, 20, T['text'])
    return f


def stepper(parent, x, y, value):
    f = frame('Stepper', x, y, 120, 36, parent, T['bg'], 18, T['line'])
    text(f, 0, 8, '–', 16, 'Bold', T['muted'], 36, 'center'); text(f, 36, 9, str(value), 14, 'Bold', T['text'], 48, 'center')
    text(f, 84, 8, '+', 16, 'Bold', T['muted'], 36, 'center')
    return f


def card(parent, x, y, w, h, title=None):
    f = frame(f'Card / {title or ""}', x, y, w, h, parent, T['surface'], 12, T['line'])
    if title: text(f, 20, 16, title.upper(), 11, 'Black', T['muted'])
    return f


def setting_row(parent, y, w, label, desc, control, last=False):
    text(parent, 20, y + 12, label, 14, 'Bold')
    if desc: text(parent, 20, y + 32, desc, 12, 'Regular', T['muted'], w - 380)
    control(parent, y)
    if not last: rect(parent, 20, y + 64, w - 40, 1, T['line'], name='Divider')


PAGES = [('Play', '▶'), ('Display', '▣'), ('Graphics', '◆'), ('Gameplay & audio', '♪'), ('Setup', '⚙'), ('Log', '≡')]


def shell(name, page, running=False, built=True):
    n = len(FRAMES); FRAMES.append(name)
    f = frame(name, (n % 4) * (W + 80), (n // 4) * (H + 80), W, H, None, T['bg'])
    image(f, 'launcher/assets/header@2x.png', 0, 0, W, HEAD, 'Header art')
    pf = frame('Profile picker', 640, 48, 160, 44, f, T['glass'], 22, T['glass_line'])
    text(pf, 18, 7, 'PROFILE', 9, 'Black', T['muted']); text(pf, 18, 20, 'default', 15, 'Bold'); text(pf, 132, 12, '▾', 14, 'Bold', T['muted'])
    if running:
        pill(f, 812, 44, 168, 52, 'STOP', 'danger', 20, '■')
    else:
        b = pill(f, 812, 44, 168, 52, 'PLAY', 'primary' if built else 'secondary', 20, '▶')
    rail = frame('Rail', 0, HEAD, RAIL, H - HEAD - STATUS, f, T['rail'])
    for i, (p, glyph) in enumerate(PAGES):
        y = 20 + i * 46
        if p == page:
            rect(rail, 12, y, RAIL - 24, 40, T['surface2'], 10, f'Active {p}'); rect(rail, 12, y + 10, 4, 20, T['accent'], 2, 'Indicator')
        text(rail, 32, y + 11, glyph, 15, 'Bold', T['accent'] if p == page else T['muted'])
        text(rail, 58, y + 11, p, 14, 'Bold' if p == page else 'Regular', T['text'] if p == page else T['muted'])
        if p == 'Setup' and not built: ellipse(rail, RAIL - 40, y + 15, 9, T['play'], 'Attention')
    text(rail, 24, H - HEAD - STATUS - 36, 'Woohoo! Flight School', 11, 'Bold', T['muted'])
    sb = frame('Status bar', 0, H - STATUS, W, STATUS, f, T['rail'])
    rect(sb, 0, 0, W, 1, T['line'], name='Top line')
    ellipse(sb, 16, 10, 8, T['ok'] if built else T['play'])
    text(sb, 32, 7, 'Built 2026-11-02 14:10 · recompiler 412b6b3' if built else 'Not built yet', 11, 'Regular', T['muted'])
    text(sb, 520, 7, 'Profile: default    ~/.local/share/pwr-port', 11, 'Regular', T['muted'], 460, 'right')
    content = frame('Content', RAIL, HEAD, W - RAIL, H - HEAD - STATUS, f, T['bg'])
    return f, content


def heading(c, title, sub):
    text(c, 28, 22, title, 24, 'Black'); text(c, 28, 54, sub, 13, 'Regular', T['muted'])


CW = W - RAIL - 56   # content card width


def page_play(running=False):
    f, c = shell('2 Play (running)' if running else '1 Play', 'Play', running)
    heading(c, 'Ready for take-off' if not running else 'In flight', 'Pilotwings Resort (USA) · 0004000000031C00 · code verified')
    art = frame('Scene card', 28, 84, 456, 278, c, T['surface'], 14, None)
    image(art, 'launcher/assets/scene@2x.png', 0, 0, 456, 278, 'Scene art')
    if running:
        g = frame('Running banner', 16, 222, 424, 40, art, T['glass'], 20, T['glass_line'])
        ellipse(g, 16, 15, 10, T['ok']); text(g, 36, 11, 'Game running · default · 0:05', 13, 'Bold', T['text'], 260)
        pill(g, 334, 4, 82, 32, 'Stop', 'danger', 13)
    k = card(c, 500, 84, 244, 278, 'This profile')
    for i, (a, b) in enumerate([('Layout', 'stacked'), ('3D resolution', '2×'), ('Filter', 'fsr'), ('Draw distance', '4×'), ('Free look', 'on'), ('Sound', 'on')]):
        text(k, 20, 46 + i * 32, a, 13, 'Regular', T['muted']); text(k, 120, 46 + i * 32, b, 13, 'Bold', T['text'], 104, 'right')
    pill(k, 20, 236, 98, 30, 'New…', 'secondary', 12); pill(k, 126, 236, 98, 30, 'Delete', 'ghost', 12)
    h = card(c, 28, 378, CW, 96, 'In-game keys')
    x = 20
    for key, what in [('F1', 'settings overlay'), ('F9 / Back', 'next layout'), ('F11', 'fullscreen'), ('Alt+Enter', 'fullscreen')]:
        kf = frame(f'Key {key}', x, 46, max(34, len(key) * 9 + 18), 28, h, T['bg'], 6, T['line'])
        text(kf, 0, 6, key, 12, 'Black', T['text'], max(34, len(key) * 9 + 18), 'center')
        text(h, x + max(34, len(key) * 9 + 18) + 8, 52, what, 12, 'Regular', T['muted'])
        x += max(34, len(key) * 9 + 18) + 8 + len(what) * 7 + 28


def page_display():
    f, c = shell('3 Display', 'Display')
    heading(c, 'Display', 'How the two screens are arranged in the window. Saved to the profile as you change them.')
    k = card(c, 28, 84, CW, 396)
    rows = [('Screen layout', 'F9 or the pad\'s Back cycles it in game', lambda p, y: segmented(p, None, y + 14, ['stacked', 'side', 'large', 'top', 'bottom', 'auto'], 'auto')),
            ('Auto layout in flight', 'Used by "auto" after a moment of steering', lambda p, y: segmented(p, None, y + 14, ['large', 'top'], 'large')),
            ('Aspect ratio', None, lambda p, y: segmented(p, None, y + 14, ['keep', 'integer', 'stretch'], 'keep')),
            ('Fullscreen', 'F11 or Alt+Enter toggles it', lambda p, y: segmented(p, None, y + 14, ['off', 'borderless', 'exclusive'], 'off')),
            ('Window scale', 'Window size in multiples of 400×480', lambda p, y: stepper(p, CW - 140, y + 14, 2)),
            ('Pad Back/Select: next layout', 'Off gives Select back to the game', lambda p, y: toggle(p, CW - 66, y + 19, True))]
    for i, (a, b, ctl) in enumerate(rows): setting_row(k, i * 65, CW, a, b, ctl, i == len(rows) - 1)


def page_graphics():
    f, c = shell('4 Graphics', 'Graphics')
    heading(c, 'Graphics', 'Higher settings need a stronger GPU. The F1 overlay changes them while you play.')
    k = card(c, 28, 84, CW, 396)
    rows = [('3D resolution', 'Internal rendering scale', lambda p, y: segmented(p, None, y + 14, ['1×', '2×', '3×', '4×'], '2×')),
            ('Scaling filter', 'How the screens are scaled to the window', lambda p, y: segmented(p, None, y + 14, ['nearest', 'linear', 'sharp', 'fsr', 'fxaa'], 'fsr')),
            ('FSR sharpness', '0 = sharpest', lambda p, y: (rect(p, CW - 260, y + 30, 200, 4, T['line'], 2), rect(p, CW - 260, y + 30, 50, 4, T['accent'], 2),
                                                            ellipse(p, CW - 218, y + 23, 18, T['text']), text(p, CW - 50, y + 22, '0.25', 13, 'Bold'))),
            ('Anisotropic filtering', 'Sharper textures at grazing angles', lambda p, y: segmented(p, None, y + 14, ['1×', '2×', '4×', '8×', '16×'], '8×')),
            ('Draw distance', 'Beyond the original: markers, boats and cars from further away', lambda p, y: segmented(p, None, y + 14, ['1×', '2×', '3×', '4×'], '4×')),
            ('Frame counter', 'Shows game fps in the window', lambda p, y: toggle(p, CW - 66, y + 19, False))]
    for i, (a, b, ctl) in enumerate(rows): setting_row(k, i * 65, CW, a, b, ctl, i == len(rows) - 1)


def page_gameplay():
    f, c = shell('5 Gameplay & audio', 'Gameplay & audio')
    heading(c, 'Gameplay & audio', 'Enhancements are off by default and marked as not in the original game.')
    k = card(c, 28, 84, CW, 196)
    rows = [('Free look', 'Right stick orbits the chase camera (not in the original game)', lambda p, y: toggle(p, CW - 66, y + 19, True)),
            ('Mute', 'No sound output', lambda p, y: toggle(p, CW - 66, y + 19, False)),
            ('Controller', 'Gamepads are picked up automatically; the mouse is the touch screen', lambda p, y: text(p, CW - 160, y + 22, 'automatic', 13, 'Bold', T['muted'], 140, 'right'))]
    for i, (a, b, ctl) in enumerate(rows): setting_row(k, i * 65, CW, a, b, ctl, i == len(rows) - 1)
    a = card(c, 28, 296, CW, 112, 'Advanced')
    text(a, 20, 44, 'Extra arguments', 14, 'Bold')
    e = frame('Entry', 180, 38, CW - 200, 36, a, T['bg'], 8, T['line']); text(e, 12, 9, '--renderer=vk', 13, 'Regular', T['muted'])
    text(a, 180, 82, 'Passed to the game as they are, after the profile\'s options.', 11, 'Regular', T['muted'])


def page_setup(building=False):
    f, c = shell('7 Setup (building)' if building else '6 Setup (first run)', 'Setup', built=False)
    heading(c, 'Set up the game', 'Your own decrypted dump is turned into a native game. Nothing leaves the install folder.')
    r = card(c, 28, 84, CW, 150, 'Game dump')
    e = frame('ROM entry', 20, 40, CW - 250, 36, r, T['bg'], 8, T['line']); text(e, 12, 9, '~/Games/Pilotwings Resort (USA).cia', 13)
    pill(r, CW - 222, 40, 96, 36, 'Browse…', 'secondary', 13); pill(r, CW - 116, 40, 96, 36, 'Verify', 'secondary', 13)
    for i, (ok, s) in enumerate([(True, 'CIA, decrypted'), (True, 'Title 0004000000031C00'), (True, 'Code matches the supported version')]):
        ellipse(r, 20 + i * 220, 98, 18, T['ok']); text(r, 20 + i * 220, 99, '✓', 12, 'Black', T['bg'], 18, 'center'); text(r, 46 + i * 220, 99, s, 13, 'Bold')
    b = card(c, 28, 250, CW, 222, 'Build')
    text(b, 20, 40, 'Install folder', 13, 'Regular', T['muted']); text(b, 130, 40, '~/.local/share/pwr-port', 13, 'Bold')
    stages = ['Verify', 'Extract', 'Recompile', 'Compile', 'Install']
    done = 2 if building else -1
    sx = 60; step = (CW - 140) / 4
    rect(b, sx + 9, 89, CW - 140, 3, T['line'], 2, 'Track')
    if building: rect(b, sx + 9, 89, step * 2, 3, T['accent'], 2, 'Track done')
    for i, s in enumerate(stages):
        x = sx + i * step
        col = T['ok'] if i < done else (T['accent'] if i == done else T['surface2'])
        ellipse(b, x, 81, 20, col, f'Stage {s}')
        if i < done: text(b, x, 83, '✓', 12, 'Black', T['bg'], 20, 'center')
        text(b, x - 40, 110, s, 12, 'Bold' if i <= done else 'Regular', T['text'] if i <= done else T['muted'], 100, 'center')
    if building:
        text(b, 20, 146, 'Recompiling the game\'s ARM code to C…  1 min 12 s', 13, 'Regular', T['muted'])
        pill(b, CW - 200, 166, 180, 40, 'Building…', 'ghost', 14)
    else:
        text(b, 20, 146, 'About 3 minutes on first build. Needs a C compiler, CMake and glslc (all found).', 13, 'Regular', T['muted'])
        pill(b, CW - 330, 166, 160, 40, 'Rebuild', 'ghost', 14); pill(b, CW - 160, 166, 140, 40, 'Build', 'primary', 15)


def page_log():
    f, c = shell('8 Log', 'Log')
    heading(c, 'Log', 'Build output and the game\'s messages (pwr.log in the profile folder).')
    k = frame('Console', 28, 84, CW, 340, c, T['console'], 12, T['line'])
    lines = ['[3/5] recompile', '$ python3 recomp/arm2c.py code.bin exheader.bin gen', '  10942 functions, 1.9 M lines', '[4/5] compile',
             '[5/5] install', 'done in 168 s: ~/.local/share/pwr-port/bin/pwr', '── game started (profile default) ──',
             'pwr: vulkan: AMD Radeon RX 6700 XT', 'pwr: controller opened: 8BitDo Pro 2', 'pwr: game-fps 60  draws 1160']
    for i, l in enumerate(lines):
        col = T['accent'] if l.startswith('[') else (T['ok'] if l.startswith(('done', '──')) else T['console_text'])
        text(k, 16, 14 + i * 22, l, 12, 'Regular', col)
    pill(c, 28, 438, 110, 34, 'Copy', 'secondary', 13); pill(c, 146, 438, 110, 34, 'Clear', 'ghost', 13)
    pill(c, CW - 172, 438, 200, 34, 'Open log folder', 'ghost', 13)


def main():
    add('page.rename', dict(name='Launcher'))
    add('variable.createCollection', dict(name='Theme'))
    for k, v in T.items(): add('variable.create', dict(collection='Theme', name=k, type='color', value=v))
    page_play(); page_play(running=True); page_display(); page_graphics(); page_gameplay(); page_setup(); page_setup(building=True); page_log()
    js = os.path.join(HERE, 'build_mockup.json')
    json.dump(steps, open(js, 'w'), indent=1, ensure_ascii=False)
    out = os.path.join(HERE, 'launcher.figcraft')
    r = subprocess.run(['figcraft-cli', 'run', js, '--out', out], capture_output=True, text=True, cwd=REPO)   # image paths are repo-relative
    if r.returncode: print(r.stdout[-3000:], r.stderr[-3000:]); sys.exit(1)
    os.makedirs(os.path.join(HERE, 'png'), exist_ok=True)
    subprocess.run(['figcraft-cli', 'export', out, '-o', os.path.join(HERE, 'png'), '--format', 'png'], check=True)
    print('wrote', out)


if __name__ == '__main__':
    main()
