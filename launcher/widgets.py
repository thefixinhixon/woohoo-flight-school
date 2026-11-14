"""Widget kit and theme for the launcher.

Pure Tk (the build kits ship Python with Tk only). Controls are drawn on canvases so they look the same on Linux,
Windows and macOS. Tk does not anti-alias canvas shapes, so rounded corners and circles are small PNG images
rendered here with 4x4 supersampling (cached); straight edges are ordinary canvas rectangles and lines.
"""
import base64, struct, zlib
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

# colour tokens; launcher/design/make_mockup.py draws the FigCraft mockup from the same table
THEME = dict(
    bg='#0E2A47', rail='#0A223B', surface='#163A5F', surface2='#1D4873', line='#2C5A86',
    text='#F4F8FC', muted='#A9C1D9', accent='#4FC3F7', on_accent='#06213A',
    play='#FF8A3D', play_hover='#FF9F5E', play_press='#E8742A', on_play='#2A1200',
    ok='#3DDC97', error='#FF6B6B', warn='#FFC857',
    glass='#0B2A5C', glass_line='#3B6FB0', console='#081A2E', console_text='#CFE3F5',
)
T = THEME
S = 1.0          # UI scale (pixels per 1/96 inch), set by init()
F = {}           # named fonts, set by init()


def px(v): return int(round(v * S))


def init(root):
    """scale, fonts and ttk styles; call once after creating the Tk root"""
    global S
    try: S = max(1.0, round(root.winfo_fpixels('1i') / 96.0 * 4) / 4)
    except tk.TclError: S = 1.0
    # Tk without Xft (e.g. python-build-standalone's) lists only X core fonts, in lower case
    fams = {n.lower(): n for n in tkfont.families(root)}
    pick = lambda names, default: next((fams[n.lower()] for n in names if n.lower() in fams), default)
    base = pick(['Lato', 'Inter', 'Segoe UI', 'Cantarell', 'Noto Sans', 'Ubuntu', 'DejaVu Sans', 'Nimbus Sans L', 'Nimbus Sans', 'Helvetica'],
                tkfont.nametofont('TkDefaultFont', root).actual('family'))
    heavy = pick(['Lato Black', 'Inter Black', 'Segoe UI Black', 'Noto Sans Black'], None)
    mono = pick(['JetBrains Mono', 'Cascadia Mono', 'DejaVu Sans Mono', 'Consolas', 'Menlo', 'Courier New', 'Nimbus Mono L'],
                tkfont.nametofont('TkFixedFont', root).actual('family'))

    def mk(name, fam, size, bold=False):
        F[name] = tkfont.Font(root, family=fam, size=-px(size), weight='bold' if bold else 'normal')
    mk('h1', heavy or base, 24, not heavy); mk('play', heavy or base, 20, not heavy); mk('caps', heavy or base, 11, not heavy)
    mk('key', heavy or base, 12, not heavy); mk('label', base, 14, True); mk('body', base, 13); mk('bodyb', base, 13, True)
    mk('small', base, 12); mk('tiny', heavy or base, 9, not heavy); mk('nav', base, 14); mk('navb', base, 14, True)
    mk('btn', base, 14, True); mk('btns', base, 13, True); mk('glyph', base, 15, True); mk('mono', mono, 12)
    mk('sym', base, 12); mk('syml', base, 15)   # symbols (check marks, arrows): core fonts have them only in the regular weight
    root.option_add('*Font', F['body'])
    st = ttk.Style(root)
    try: st.theme_use('clam')
    except tk.TclError: pass
    st.configure('Pwr.Vertical.TScrollbar', background=T['surface2'], troughcolor=T['console'], bordercolor=T['console'],
                 arrowcolor=T['muted'], lightcolor=T['surface2'], darkcolor=T['surface2'], gripcount=0, arrowsize=px(12))
    st.map('Pwr.Vertical.TScrollbar', background=[('active', T['line'])])


# ---------------------------------------------------------------- anti-aliased shapes
def _rgb(c): return int(c[1:3], 16), int(c[3:5], 16), int(c[5:7], 16)


def _png(w, h, pix):
    raw = b''.join(b'\0' + bytes(pix[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return base64.b64encode(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


_cache = {}


def _disc(master, w, h, cx, cy, r, fill, outline, outside=None):
    """w x h image of the part of a disc (centre cx, cy, radius r; 1-px outline ring) inside it, or with outside=colour
    only what lies outside the disc in that colour (a mask that rounds the corners of a picture)"""
    key = (w, h, cx, cy, r, fill, outline, outside)
    img = _cache.get(key)
    if img: return img
    N = 4; rin = r - 1 if outline else r; pix = bytearray(w * h * 4)
    f = _rgb(fill) if fill else None; o = _rgb(outline) if outline else None; out = _rgb(outside) if outside else None
    r2, rin2 = r * r, rin * rin
    for y in range(h):
        for x in range(w):
            a_in = a_ring = 0
            for j in range(N):
                dy = y + (j + .5) / N - cy; dy2 = dy * dy
                for i in range(N):
                    dx = x + (i + .5) / N - cx; d2 = dx * dx + dy2
                    if d2 <= rin2: a_in += 1
                    elif d2 <= r2: a_ring += 1
            k = (y * w + x) * 4
            if out:
                a = N * N - a_in - a_ring
                pix[k:k + 4] = bytes((*out, a * 255 // (N * N)))
                continue
            if f:
                ring = o or f; tot = a_in + a_ring
                if not tot: continue
                c = [(a_in * f[n] + a_ring * ring[n]) // tot for n in range(3)]
            elif o:
                tot = a_ring; c = list(o)
                if not tot: continue
            else: continue
            pix[k:k + 4] = bytes((*c, tot * 255 // (N * N)))
    img = _cache[key] = tk.PhotoImage(master=master, data=_png(w, h, pix))
    return img


def rounded(cv, x0, y0, x1, y1, r, fill, outline=None, tags=()):
    """rounded rectangle [x0, x1) x [y0, y1) on canvas cv; returns the item ids"""
    x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
    r = int(max(0, min(r, (x1 - x0) // 2, (y1 - y0) // 2)))
    ids = []
    if r == 0:
        return [cv.create_rectangle(x0, y0, x1 - 1, y1 - 1, fill=fill or '', outline=outline or '', tags=tags)]
    for (x, y, cx, cy) in ((x0, y0, r, r), (x1 - r, y0, 0, r), (x0, y1 - r, r, 0), (x1 - r, y1 - r, 0, 0)):
        ids.append(cv.create_image(x, y, anchor='nw', image=_disc(cv, r, r, cx, cy, r, fill, outline), tags=tags))
    if fill:
        if x1 - x0 > 2 * r: ids.append(cv.create_rectangle(x0 + r, y0, x1 - r, y1, fill=fill, width=0, tags=tags))
        if y1 - y0 > 2 * r: ids.append(cv.create_rectangle(x0, y0 + r, x1, y1 - r, fill=fill, width=0, tags=tags))
    if outline:
        if x1 - x0 > 2 * r:
            ids.append(cv.create_line(x0 + r, y0, x1 - r, y0, fill=outline, tags=tags))
            ids.append(cv.create_line(x0 + r, y1 - 1, x1 - r, y1 - 1, fill=outline, tags=tags))
        if y1 - y0 > 2 * r:
            ids.append(cv.create_line(x0, y0 + r, x0, y1 - r, fill=outline, tags=tags))
            ids.append(cv.create_line(x1 - 1, y0 + r, x1 - 1, y1 - r, fill=outline, tags=tags))
    return ids


def round_corners(cv, x0, y0, x1, y1, r, outside, tags=()):
    """masks that round the corners of a picture drawn at that box, in the colour around it"""
    for (x, y, cx, cy) in ((x0, y0, r, r), (x1 - r, y0, 0, r), (x0, y1 - r, r, 0), (x1 - r, y1 - r, 0, 0)):
        cv.create_image(x, y, anchor='nw', image=_disc(cv, r, r, cx, cy, r, None, None, outside), tags=tags)


def circle(cv, cx, cy, d, fill, tags=()):
    d = int(d)
    return cv.create_image(int(cx - d / 2), int(cy - d / 2), anchor='nw', image=_disc(cv, d, d, d / 2, d / 2, d / 2, fill, None), tags=tags)


# ---------------------------------------------------------------- buttons
KINDS = dict(   # fill, hover, press, text, outline
    primary=(T['play'], T['play_hover'], T['play_press'], T['on_play'], None),
    secondary=(T['surface2'], '#245887', '#1A4269', T['text'], None),
    ghost=(T['surface'], T['surface2'], T['bg'], T['text'], T['line']),
    glass=(T['glass'], '#123870', '#09234D', T['text'], T['glass_line']),
    danger=(T['error'], '#FF8585', '#E05555', '#2A0505', None),
    disabled=('#1A3E63', '#1A3E63', '#1A3E63', '#6F8BA6', None),
)


class Pill:
    """a rounded button drawn on any canvas (also over pictures); call place(x0, y0, x1, y1) to (re)draw it"""
    _n = 0

    def __init__(self, cv, text, command=None, kind='secondary', font=None, icon='', radius=None):
        Pill._n += 1; self.tag = f'pill{Pill._n}'
        self.cv, self.text, self.command, self.kind, self.font, self.icon, self.radius = cv, text, command, kind, font or F['btn'], icon, radius
        self.enabled = True; self.hover = self.down = False; self.box = None
        # hit-tested against the box, not bound to the items: drawing a new state replaces the items under the pointer
        cv.bind('<Motion>', self._motion, add='+')
        cv.bind('<Leave>', lambda e: self.hover and self._set(hover=False, down=False), add='+')
        cv.bind('<ButtonPress-1>', lambda e: self._inside(e) and self._set(hover=True, down=True), add='+')
        cv.bind('<ButtonRelease-1>', self._release, add='+')

    def _inside(self, e):
        b = self.box
        return bool(b) and b[0] <= e.x < b[2] and b[1] <= e.y < b[3]

    def _motion(self, e):
        h = self._inside(e)
        if h != self.hover: self._set(hover=h)

    def width_for(self, pad=36):
        return self.font.measure((self.icon + '  ' if self.icon else '') + self.text) + px(pad)

    def _set(self, **kw):
        for k, v in kw.items(): setattr(self, k, v)
        if 'hover' in kw: self.cv.configure(cursor='hand2' if self.hover and self.enabled else '')
        self.draw()

    def _release(self, e):
        if not self.down: return
        fire = self.down and self._inside(e) and self.enabled
        self._set(down=False)
        if fire and self.command: self.command()

    def configure(self, **kw):
        for k in ('text', 'kind', 'icon', 'command'):
            if k in kw: setattr(self, k, kw[k])
        if 'enabled' in kw: self.enabled = kw['enabled']
        self.draw()

    def place(self, x0, y0, x1, y1):
        self.box = (x0, y0, x1, y1); self.draw()

    def draw(self):
        if not self.box: return
        cv = self.cv; cv.delete(self.tag)
        x0, y0, x1, y1 = self.box
        fill, hov, prs, fg, ol = KINDS[self.kind if self.enabled else 'disabled']
        c = prs if self.down and self.hover else (hov if self.hover else fill)
        r = self.radius if self.radius is not None else (y1 - y0) // 2
        rounded(cv, x0, y0, x1, y1, r, c, ol, tags=(self.tag,))
        label = (self.icon + '  ' if self.icon else '') + self.text
        cv.create_text((x0 + x1) // 2, (y0 + y1) // 2 + (1 if self.down and self.hover else 0), text=label, fill=fg, font=self.font, tags=(self.tag,))


class PillButton(tk.Canvas):
    def __init__(self, master, text, command=None, kind='secondary', bg=None, width=None, height=36, font=None, icon=''):
        bg = bg or master.cget('bg')
        super().__init__(master, bg=bg, highlightthickness=0, bd=0, height=px(height))
        self.pill = Pill(self, text, command, kind, font or (F['btns'] if height < 40 else F['btn']), icon)
        self.fixed = width
        self.configure(width=px(width) if width else self.pill.width_for())
        self.bind('<Configure>', lambda e: self.pill.place(0, 0, e.width, e.height))

    def set(self, **kw):
        self.pill.configure(**kw)
        if not self.fixed and 'text' in kw: self.configure(width=self.pill.width_for())


# ---------------------------------------------------------------- inputs
class Segmented(tk.Canvas):
    """one-of-n choice bound to a StringVar; labels may differ from the stored values"""

    def __init__(self, master, var, values, labels=None, bg=None, command=None):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, height=px(36))
        self.var, self.values, self.labels = var, list(values), list(labels or values)
        self.enabled = True; self.hover = None; self.command = command
        self.widths = [max(px(56), F['body'].measure(l) + px(26)) for l in self.labels]
        self.configure(width=sum(self.widths) + px(8))
        self.bind('<Motion>', lambda e: self._hover(self._at(e.x)))
        self.bind('<Leave>', lambda e: self._hover(None))
        self.bind('<Button-1>', self._click)
        var.trace_add('write', lambda *a: self.draw())
        self.bind('<Configure>', lambda e: self.draw())

    def _at(self, x):
        cx = px(4)
        for i, w in enumerate(self.widths):
            if cx <= x < cx + w: return i
            cx += w
        return None

    def _hover(self, i):
        if i != self.hover: self.hover = i; self.configure(cursor='hand2' if i is not None and self.enabled else ''); self.draw()

    def _click(self, e):
        i = self._at(e.x)
        if i is not None and self.enabled:
            self.var.set(self.values[i])
            if self.command: self.command()

    def set_enabled(self, on): self.enabled = on; self.draw()

    def draw(self):
        self.delete('all')
        w, h = int(self.cget('width')), int(self.cget('height'))
        rounded(self, 0, 0, w, h, h // 2, T['bg'], T['line'])
        cx = px(4); cur = self.var.get()
        for i, (v, l, bw) in enumerate(zip(self.values, self.labels, self.widths)):
            on = v == cur
            if on: rounded(self, cx, px(4), cx + bw, h - px(4), (h - px(8)) // 2, T['accent'] if self.enabled else T['line'])
            elif i == self.hover and self.enabled: rounded(self, cx, px(4), cx + bw, h - px(4), (h - px(8)) // 2, T['surface2'])
            fg = (T['on_accent'] if self.enabled else T['text']) if on else (T['muted'] if self.enabled else '#6F8BA6')
            self.create_text(cx + bw // 2, h // 2, text=l, fill=fg, font=F['bodyb'] if on else F['body'])
            cx += bw


class Toggle(tk.Canvas):
    def __init__(self, master, var, bg=None, command=None):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, width=px(46), height=px(26), cursor='hand2')
        self.var, self.command = var, command
        self.pos = 1.0 if var.get() else 0.0
        self.bind('<Button-1>', self._click)
        var.trace_add('write', lambda *a: self._animate())
        self.draw()

    def _click(self, e):
        self.var.set(not self.var.get())
        if self.command: self.command()

    def _animate(self):
        goal = 1.0 if self.var.get() else 0.0
        if abs(self.pos - goal) < 0.01: self.pos = goal; self.draw(); return
        self.pos += max(-0.25, min(0.25, goal - self.pos)); self.draw()
        self.after(16, self._animate)

    def draw(self):
        self.delete('all')
        w, h = px(46), px(26)
        rounded(self, 0, 0, w, h, h // 2, T['accent'] if self.pos > 0.5 else T['line'])
        d = px(20); x = px(3) + (w - d - px(6)) * self.pos
        circle(self, x + d / 2, h / 2, d, T['text'])


class Stepper(tk.Canvas):
    def __init__(self, master, var, lo, hi, bg=None, fmt=str):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, width=px(120), height=px(36))
        self.var, self.lo, self.hi, self.fmt = var, lo, hi, fmt
        self.hover = None
        self.bind('<Button-1>', self._click)
        self.bind('<Motion>', lambda e: self._hov(-1 if e.x < px(40) else (1 if e.x > px(80) else None)))
        self.bind('<Leave>', lambda e: self._hov(None))
        var.trace_add('write', lambda *a: self.draw())
        self.draw()

    def value(self):
        try: return max(self.lo, min(self.hi, int(float(self.var.get()))))
        except ValueError: return self.lo

    def _hov(self, d):
        if d != self.hover: self.hover = d; self.configure(cursor='hand2' if d else ''); self.draw()

    def _click(self, e):
        d = -1 if e.x < px(40) else (1 if e.x > px(80) else 0)
        if d: self.var.set(str(max(self.lo, min(self.hi, self.value() + d))))

    def draw(self):
        self.delete('all')
        w, h = px(120), px(36)
        rounded(self, 0, 0, w, h, h // 2, T['bg'], T['line'])
        v = self.value()
        for d, x, ch in ((-1, px(20), '–'), (1, w - px(20), '+')):
            can = self.lo < v if d < 0 else v < self.hi
            if self.hover == d and can: circle(self, x, h / 2, px(28), T['surface2'])
            self.create_text(x, h // 2, text=ch, fill=T['text'] if can else '#55708C', font=F['label'])
        self.create_text(w // 2, h // 2, text=self.fmt(v), fill=T['text'], font=F['bodyb'])


class Slider(tk.Canvas):
    def __init__(self, master, var, lo, hi, step, bg=None, width=260, fmt='{:.2f}'):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, width=px(width), height=px(36))
        self.var, self.lo, self.hi, self.step, self.fmt = var, lo, hi, step, fmt
        self.enabled = True
        for ev in ('<Button-1>', '<B1-Motion>'): self.bind(ev, self._drag)
        var.trace_add('write', lambda *a: self.draw())
        self.draw()

    def set_enabled(self, on): self.enabled = on; self.configure(cursor='hand2' if on else ''); self.draw()

    def value(self):
        try: return max(self.lo, min(self.hi, float(self.var.get())))
        except ValueError: return self.lo

    def _track(self): return px(10), int(self.cget('width')) - px(60)

    def _drag(self, e):
        if not self.enabled: return
        a, b = self._track()
        t = max(0.0, min(1.0, (e.x - a) / (b - a)))
        v = round((self.lo + t * (self.hi - self.lo)) / self.step) * self.step
        self.var.set(self.fmt.format(v))

    def draw(self):
        self.delete('all')
        a, b = self._track(); h = px(36); y = h // 2
        t = (self.value() - self.lo) / (self.hi - self.lo); x = a + (b - a) * t
        on = self.enabled
        rounded(self, a, y - px(2), b, y + px(2), px(2), T['line'])
        if x - a >= px(4): rounded(self, a, y - px(2), int(x), y + px(2), px(2), T['accent'] if on else '#55708C')
        circle(self, x, y, px(18), T['text'] if on else '#6F8BA6')
        self.create_text(int(self.cget('width')) - px(4), y, anchor='e', text=self.fmt.format(self.value()),
                         fill=T['text'] if on else T['muted'], font=F['bodyb'])


class Entry(tk.Canvas):
    """rounded text field"""

    def __init__(self, master, var, bg=None, width=300, placeholder=''):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, width=px(width), height=px(36))
        self.e = tk.Entry(self, textvariable=var, bd=0, relief='flat', highlightthickness=0, bg=T['bg'], fg=T['text'],
                          insertbackground=T['text'], selectbackground=T['accent'], selectforeground=T['on_accent'], font=F['body'],
                          disabledbackground=T['bg'], disabledforeground=T['muted'])
        self.win = self.create_window(px(12), px(18), anchor='w', window=self.e)
        self.focus = False
        self.e.bind('<FocusIn>', lambda e: self._f(True)); self.e.bind('<FocusOut>', lambda e: self._f(False))
        self.bind('<Configure>', lambda e: self.draw())
        self.bind('<Button-1>', lambda e: self.e.focus_set())

    def _f(self, on): self.focus = on; self.draw()

    def draw(self):
        self.delete('bgshape')
        w, h = self.winfo_width(), self.winfo_height()
        if w < 4: w, h = int(self.cget('width')), int(self.cget('height'))
        for i in rounded(self, 0, 0, w, h, px(8), T['bg'], T['accent'] if self.focus else T['line'], tags=('bgshape',)): self.tag_lower(i)
        self.itemconfigure(self.win, width=w - px(24))


# ---------------------------------------------------------------- containers
class Card(tk.Canvas):
    """rounded surface; put children in .body (a Frame); the card follows the body's height"""
    INSET = 6

    def __init__(self, master, title=None, bg=None, width=None):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, height=px(40))
        if width: self.configure(width=px(width))
        self.body = tk.Frame(self, bg=T['surface'])
        if title:
            tk.Label(self.body, text=title.upper(), bg=T['surface'], fg=T['muted'], font=F['caps']).pack(anchor='w', padx=px(14), pady=(px(10), 0))
        i = px(self.INSET)
        self.win = self.create_window(i, i, anchor='nw', window=self.body)
        self.bind('<Configure>', self._layout)
        self.body.bind('<Configure>', self._fit)

    def _fit(self, e=None):
        h = self.body.winfo_reqheight() + 2 * px(self.INSET)
        if int(self.cget('height')) != h: self.configure(height=h)

    def _layout(self, e):
        self.delete('bgshape')
        rounded(self, 0, 0, e.width, e.height, px(12), T['surface'], T['line'], tags=('bgshape',))
        self.tag_lower('bgshape')
        self.itemconfigure(self.win, width=e.width - 2 * px(self.INSET))


def setting_row(body, label, desc, make_control, last=False):
    """label + description on the left, a control on the right, a divider below; returns the control"""
    row = tk.Frame(body, bg=T['surface']); row.pack(fill='x', padx=px(14))
    left = tk.Frame(row, bg=T['surface']); left.pack(side='left', fill='x', expand=True, pady=px(10))
    tk.Label(left, text=label, bg=T['surface'], fg=T['text'], font=F['label']).pack(anchor='w')
    if desc: tk.Label(left, text=desc, bg=T['surface'], fg=T['muted'], font=F['small'], justify='left').pack(anchor='w')
    ctl = make_control(row); ctl.pack(side='right', padx=(px(12), 0))
    if not last: tk.Frame(body, bg=T['line'], height=1).pack(fill='x', padx=px(14))
    return ctl


class StageStepper(tk.Canvas):
    """build stages as dots on a line: pending, active (pulsing), done, failed"""

    def __init__(self, master, stages, bg=None):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, height=px(62))
        self.stages = stages; self.state = ['pending'] * len(stages); self.phase = 0
        self.bind('<Configure>', lambda e: self.draw())
        self._pulse()

    def set(self, states): self.state = list(states); self.draw()

    def _pulse(self):
        self.phase ^= 1
        if 'active' in self.state: self.draw()
        self.after(550, self._pulse)

    def draw(self):
        self.delete('all')
        w = self.winfo_width()
        if w < 50: return
        n = len(self.stages); a, b = px(46), w - px(46); step = (b - a) / (n - 1); y = px(18)
        rounded(self, a, y - 1, b, y + 2, 1, T['line'])
        last_done = max([i for i, s in enumerate(self.state) if s in ('done', 'active', 'failed')], default=-1)
        if last_done > 0: rounded(self, a, y - 1, int(a + step * last_done), y + 2, 1, T['accent'])
        for i, (name, st) in enumerate(zip(self.stages, self.state)):
            x = a + step * i
            col = dict(done=T['ok'], failed=T['error'], active=T['accent'] if self.phase else '#8AD8FA').get(st, T['surface2'])
            if st == 'active': circle(self, x, y, px(30), '#24577F')
            circle(self, x, y, px(20), col)
            if st in ('done', 'failed'):
                self.create_text(x, y, text='✓' if st == 'done' else '!', fill=T['bg'], font=F['sym'] if st == 'done' else F['key'])
            on = st != 'pending'
            self.create_text(x, y + px(30), text=name, fill=T['text'] if on else T['muted'], font=F['bodyb'] if on else F['body'])


class Dot(tk.Canvas):
    def __init__(self, master, color, d=10, bg=None):
        super().__init__(master, bg=bg or master.cget('bg'), highlightthickness=0, bd=0, width=px(d), height=px(d))
        self.d = d; self.set(color)

    def set(self, color):
        self.delete('all'); circle(self, px(self.d) / 2, px(self.d) / 2, px(self.d), color)


class Toast:
    """short message in the bottom-right corner"""

    def __init__(self, root): self.root = root; self.cv = None; self.job = None

    def show(self, text, color=None, ms=1800):
        if self.cv: self.cv.destroy()
        if self.job: self.root.after_cancel(self.job)
        w = F['bodyb'].measure(text) + px(52); h = px(40)
        cv = self.cv = tk.Canvas(self.root, bg=T['bg'], highlightthickness=0, bd=0, width=w, height=h)
        rounded(cv, 0, 0, w, h, h // 2, T['glass'], T['glass_line'])
        circle(cv, px(20), h / 2, px(10), color or T['ok'])
        cv.create_text(px(34), h // 2, anchor='w', text=text, fill=T['text'], font=F['bodyb'])
        cv.place(relx=1, rely=1, x=-px(18), y=-px(40), anchor='se')
        self.job = self.root.after(ms, self.hide)

    def hide(self):
        if self.cv: self.cv.destroy(); self.cv = None
        self.job = None
