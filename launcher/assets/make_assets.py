#!/usr/bin/env python3
"""Launcher artwork: header.png, scene.png, icon.png (+ @2x) from the project's own cover illustration.

usage: python3 launcher/assets/make_assets.py [path/to/boxart]

Reuses the drawing code of the fan cover (boxart/make_art.py, original vector art; not part of the repository) and
renders only illustration: no console branding, rating badge or cover text. Needs rsvg-convert, Pillow (make_art.py
measures text with it) and the Lato font. The PNGs are committed; the launcher itself needs only Tk.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOXART = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'boxart')
sys.path.insert(0, os.path.abspath(BOXART))
import make_art as A  # noqa: E402

SVG = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vb}">{body}</svg>'


def front_without(title=True):
    """the cover's illustration with the console branding, rating, cover text (and optionally the title) left out"""
    saved = A.n3ds, A.rating_badge, A.T, A.title_block
    A.n3ds = A.rating_badge = A.T = lambda *a, **k: ''
    if not title: A.title_block = lambda *a, **k: ''
    try: svg = A.front()
    finally: A.n3ds, A.rating_badge, A.T, A.title_block = saved
    for chrome in ('<rect x="-3" y="-3" width="131" height="12.5" fill="#e60012"/>',
                   '<rect x="-3" y="9.5" width="131" height="0.35" fill="#8f0b12"/>',
                   '<rect x="-3" y="-3" width="131" height="2.2" fill="#ffffff" opacity=".12"/>'):
        assert chrome in svg, chrome
        svg = svg.replace(chrome, '')
    inner = svg[svg.index('<g clip-path="url(#trimclip)">') + len('<g clip-path="url(#trimclip)">'):svg.rindex('</g></svg>')]
    return inner


def group(body, start_marker):
    """the <g ...> ... </g> that starts with start_marker (groups in the cover are not nested past one level)"""
    i = body.index(start_marker)
    depth, j = 0, i
    for m in re.finditer(r'<g\b|</g>', body[i:]):
        depth += 1 if m.group() != '</g>' else -1
        if depth == 0: j = i + m.end(); break
    return body[i:j]


def render(svg, name, w, h):
    path = os.path.join(HERE, name + '.svg')
    open(path, 'w').write(svg)
    for scale, suffix in ((1, ''), (2, '@2x')):
        out = os.path.join(HERE, f'{name}{suffix}.png')
        subprocess.run(['rsvg-convert', '-w', str(w * scale), '-h', str(h * scale), path, '-o', out], check=True)
    os.remove(path)
    print('wrote', name, f'{w}x{h} (+@2x)')


def main():
    full = front_without(title=False)
    defs = A.front_defs()
    sky = A.lg('h_sky', 0, 0, 0, 35, [(0, '#082a9c', 1), (0.7, '#1a63dd', 1), (1, '#3f8fea', 1)])
    glider = group(full, '<g transform="translate(100 40)')
    belt = group(full, '<g transform="translate(42 48)')

    # header: 1000x140 -> viewBox 250x35. Title on the left, pilots in the middle, open sky on the right for the controls
    header = (defs.replace('</defs>', sky + '</defs>') +
              '<rect width="250" height="35" fill="url(#h_sky)"/>' +
              f'<circle cx="236" cy="2" r="40" fill="url(#f_sun)" opacity=".55" transform="translate(-30 0)"/>' +
              A.cloud(150, 27, 0.35, 0.55) + A.cloud(232, 30, 0.45, 0.35) + A.cloud(196, 9, 0.25, 0.3) +
              f'<g transform="translate(-6.5 -8.2)">{A.title_block(64.5, 25.2, 15.9)}</g>' +
              f'<g transform="translate(71 -39)">{belt}</g>' +
              f'<g transform="translate(37 -32) scale(0.95)">{glider}</g>')
    render(SVG.format(vb='0 0 250 35', body=header), 'header', 1000, 140)

    # scene (Play page art card): island, rings, pilots, biplane; 16:9-ish crop below the title
    render(SVG.format(vb='0 30.5 128 78', body=full), 'scene', 456, 278)

    # icon: the biplane on a round sky badge
    plane = group(full, '<g transform="translate(50 68)')
    icon_sky = A.lg('i_sky', 0, 58, 0, 112, [(0, '#0b3bb4', 1), (1, '#4fb6f2', 1)])
    icon = (defs.replace('</defs>', icon_sky + '</defs>') + '<clipPath id="ic"><rect x="45" y="58" width="68" height="68" rx="15"/></clipPath>'
            '<g clip-path="url(#ic)"><rect x="45" y="58" width="68" height="68" fill="url(#i_sky)"/>'
            '<rect x="45" y="108" width="68" height="20" fill="#0aa3e0"/>' + plane + '</g>')
    render(SVG.format(vb='45 58 68 68', body=icon), 'icon', 128, 128)


if __name__ == '__main__':
    main()
