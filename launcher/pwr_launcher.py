"""Launcher: pick your own game dump, verify it, recompile + build the native game, manage profiles, launch.

usage: python3 launcher/pwr_launcher.py [--home DIR] [--rom FILE]

A Tk front end for tools/pipeline.py (the same stages run from the command line). Nothing here touches the ROM
beyond reading it; the build and everything extracted from the ROM stay in the install folder.
Remembers the last ROM, install folder, profile and page in ~/.config/pwr-port/launcher.json (%APPDATA%\\pwr-port on
Windows). Settings are saved to the profile as they are changed. Design: launcher/design/ (FigCraft mockup), widgets
and colours: launcher/widgets.py, artwork: launcher/assets/.
"""
import json, os, queue, sys, threading, time, tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'tools'))
sys.path.insert(0, HERE)
import pipeline as P  # noqa: E402
import widgets as ui  # noqa: E402
from widgets import T, F, px  # noqa: E402

CHOICES = dict(layout=['stacked', 'side', 'large', 'top', 'bottom', 'auto'], filter=['nearest', 'linear', 'sharp', 'fsr', 'fxaa'],
               aspect=['keep', 'integer', 'stretch'], fullscreen=['off', 'borderless', 'exclusive'], auto_gameplay=['large', 'top'],
               resolution=['1', '2', '3', '4'], aniso=['1', '2', '4', '8', '16'], draw_distance=['1', '2', '3', '4'])
ROM_TYPES = [('3DS game dumps', '*.cia *.3ds *.cci *.cxi'), ('All files', '*')]
PAGES = [('play', 'Play', '▶'), ('display', 'Display', '▣'), ('graphics', 'Graphics', '◆'), ('gameplay', 'Gameplay & audio', '♪'),
         ('setup', 'Setup', '⚙'), ('log', 'Log', '≡')]
STAGES = ['Verify', 'Extract', 'Recompile', 'Compile', 'Install']
STAGE_TEXT = ['Checking the dump…', 'Extracting the game files…', "Recompiling the game's ARM code to C…", 'Compiling the native game…',
              'Installing…']
HEAD_H, RAIL_W, STATUS_H = 140, 200, 28
x_ = lambda v: f'{v}×'


def config_path():
    base = os.environ.get('APPDATA') if os.name == 'nt' else os.environ.get('XDG_CONFIG_HOME', os.path.expanduser('~/.config'))
    return os.path.join(base or os.path.expanduser('~'), 'pwr-port', 'launcher.json')


def elide(text, font, width):
    """shorten text in the middle (paths) to fit width pixels"""
    if font.measure(text) <= width: return text
    a, b = text[:len(text) // 2], text[len(text) // 2:]
    while (a or b) and font.measure(a + '…' + b) > width:
        if len(a) >= len(b): a = a[:-1]
        else: b = b[1:]
    return a + '…' + b


def short(path):
    home = os.path.expanduser('~')
    return '~' + path[len(home):] if path.startswith(home + os.sep) else path


class Launcher(tk.Tk):
    def __init__(self, home=None, rom=None):
        super().__init__(className='WoohooFlightSchool')
        ui.init(self)
        self.title('Woohoo! Flight School')
        self.configure(bg=T['bg'])
        self.geometry(f'{px(1000)}x{px(680)}'); self.minsize(px(960), px(660))
        self.art = {n: self.load_art(n) for n in ('header', 'scene', 'icon')}
        if self.art['icon']: self.iconphoto(True, self.art['icon'])
        self.cfg = self.load_cfg()
        self.home = tk.StringVar(value=home or self.cfg.get('home') or P.default_home())
        self.rom = tk.StringVar(value=rom or self.cfg.get('rom', ''))
        self.profile = tk.StringVar(value=self.cfg.get('profile', 'default'))
        self.loaded_profile = None
        self.set = {k: tk.StringVar() for k in ('layout', 'filter', 'aspect', 'fullscreen', 'auto_gameplay', 'scale', 'args', 'resolution',
                                                 'aniso', 'fsr_sharpness', 'draw_distance')}
        self.flags = {k: tk.BooleanVar() for k in ('mute', 'show_fps', 'select_layout', 'free_look')}
        self.logq = queue.Queue(); self.calls = queue.Queue()
        self.worker = None; self.game = None; self.tail = None; self.game_t0 = 0
        self.manifest = None; self.loading = False; self.save_job = None
        self.build_t0 = None; self.stage = -1; self.verified = None
        self.toast = ui.Toast(self)

        self.build_ui()
        for v in list(self.set.values()) + list(self.flags.values()): v.trace_add('write', lambda *a: self.changed())
        self.refresh_install(); self.load_profile()
        self.show(self.cfg.get('page') if self.manifest and self.cfg.get('page') in dict((p[0], 1) for p in PAGES) else
                  ('play' if self.manifest else 'setup'))
        self.after(150, self.check_tools)
        self.after(100, self.poll)
        self.bind('<Control-Return>', lambda e: self.play())
        self.protocol('WM_DELETE_WINDOW', self.quit_app)

    # ---------------- persistence
    def load_art(self, name):
        path = os.path.join(HERE, 'assets', f'{name}@2x.png' if ui.S >= 1.5 else f'{name}.png')
        try: return tk.PhotoImage(master=self, file=path)
        except tk.TclError: return None

    def load_cfg(self):
        try: return json.load(open(config_path()))
        except (OSError, ValueError): return {}

    def save_cfg(self):
        try:
            os.makedirs(os.path.dirname(config_path()), exist_ok=True)
            json.dump(dict(home=os.path.abspath(self.home.get()), rom=os.path.abspath(self.rom.get()) if self.rom.get() else '',
                           profile=self.profile.get(), page=getattr(self, 'page', 'play')), open(config_path(), 'w'), indent=2)
        except OSError as e: self.log(f'cannot save {config_path()}: {e}')

    # ---------------- layout
    def build_ui(self):
        self.columnconfigure(1, weight=1); self.rowconfigure(1, weight=1)
        self.head = tk.Canvas(self, height=px(HEAD_H), bg=T['glass'], highlightthickness=0, bd=0)
        self.head.grid(row=0, column=0, columnspan=2, sticky='ew')
        self.play_pill = ui.Pill(self.head, 'PLAY', self.play, 'primary', F['play'], '▶')
        self.head.tag_bind('picker', '<Button-1>', self.profile_menu)
        self.head.tag_bind('picker', '<Enter>', lambda e: self.head.configure(cursor='hand2'))
        self.head.tag_bind('picker', '<Leave>', lambda e: self.head.configure(cursor=''))
        self.head.bind('<Configure>', lambda e: self.draw_header())

        self.rail = tk.Canvas(self, width=px(RAIL_W), bg=T['rail'], highlightthickness=0, bd=0)
        self.rail.grid(row=1, column=0, sticky='ns')
        self.rail.bind('<Configure>', lambda e: self.draw_rail())
        self.rail.bind('<Motion>', self.rail_hover); self.rail.bind('<Leave>', lambda e: self.rail_hover(None))
        self.rail.bind('<Button-1>', self.rail_click)
        self.rail_hot = None

        self.content = tk.Frame(self, bg=T['bg']); self.content.grid(row=1, column=1, sticky='nsew')
        self.content.rowconfigure(0, weight=1); self.content.columnconfigure(0, weight=1)
        self.pages = {}
        for key, _, _ in PAGES:
            f = tk.Frame(self.content, bg=T['bg']); f.grid(row=0, column=0, sticky='nsew'); self.pages[key] = f
            getattr(self, 'page_' + key)(f)

        sb = tk.Frame(self, bg=T['rail'], height=px(STATUS_H)); sb.grid(row=2, column=0, columnspan=2, sticky='ew'); sb.pack_propagate(False)
        tk.Frame(sb, bg=T['line'], height=1).pack(side='top', fill='x')
        self.status_dot = ui.Dot(sb, T['play'], 8, T['rail']); self.status_dot.pack(side='left', padx=(px(16), px(8)))
        self.status_l = tk.Label(sb, bg=T['rail'], fg=T['muted'], font=F['small']); self.status_l.pack(side='left')
        self.status_r = tk.Label(sb, bg=T['rail'], fg=T['muted'], font=F['small']); self.status_r.pack(side='right', padx=px(16))

    def heading(self, f, title, sub):
        tk.Label(f, text=title, bg=T['bg'], fg=T['text'], font=F['h1']).pack(anchor='w', padx=px(28), pady=(px(18), 0))
        lbl = tk.Label(f, text=sub, bg=T['bg'], fg=T['muted'], font=F['body'], justify='left')
        lbl.pack(anchor='w', padx=px(28), pady=(0, px(12)))
        return lbl

    # ---- Play
    def page_play(self, f):
        self.play_sub = self.heading(f, 'Ready for take-off', '')
        self.play_title = f.winfo_children()[0]
        row = tk.Frame(f, bg=T['bg']); row.pack(fill='x', padx=px(28))
        sw, sh = px(456), px(278)
        self.scene = tk.Canvas(row, width=sw, height=sh, bg=T['bg'], highlightthickness=0, bd=0)
        self.scene.pack(side='left', anchor='n')
        if self.art['scene']: self.scene.create_image(0, 0, anchor='nw', image=self.art['scene'])
        else: ui.rounded(self.scene, 0, 0, sw, sh, px(14), T['surface'])
        ui.round_corners(self.scene, 0, 0, sw, sh, px(14), T['bg'])
        self.banner_pill = ui.Pill(self.scene, 'Stop', self.stop, 'danger', F['btns'])
        self.go_setup_pill = ui.Pill(self.scene, 'Go to Setup', lambda: self.show('setup'), 'primary', F['btns'])

        card = ui.Card(row, 'This profile', width=244); card.pack(side='left', anchor='n', padx=(px(16), 0))
        self.summary = {}
        grid = tk.Frame(card.body, bg=T['surface']); grid.pack(fill='x', padx=px(14), pady=(px(6), px(4)))
        for i, (k, label) in enumerate([('layout', 'Layout'), ('resolution', '3D resolution'), ('filter', 'Filter'),
                                        ('draw_distance', 'Draw distance'), ('free_look', 'Free look'), ('mute', 'Sound')]):
            tk.Label(grid, text=label, bg=T['surface'], fg=T['muted'], font=F['body']).grid(row=i, column=0, sticky='w', pady=px(4))
            self.summary[k] = tk.Label(grid, bg=T['surface'], fg=T['text'], font=F['bodyb'])
            self.summary[k].grid(row=i, column=1, sticky='e', pady=px(4))
        grid.columnconfigure(1, weight=1)
        b = tk.Frame(card.body, bg=T['surface']); b.pack(fill='x', padx=px(14), pady=(px(6), px(12)))
        ui.PillButton(b, 'New…', self.new_profile, 'secondary', width=100, height=32).pack(side='left')
        ui.PillButton(b, 'Delete', self.delete_profile, 'ghost', width=100, height=32).pack(side='right')

        keys = ui.Card(f, 'In-game keys'); keys.pack(fill='x', padx=px(28), pady=(px(16), 0))
        kr = tk.Frame(keys.body, bg=T['surface']); kr.pack(fill='x', padx=px(14), pady=(px(8), px(14)))
        for key, what in [('F1', 'settings overlay'), ('F9 / Back', 'next layout'), ('F11', 'fullscreen'), ('Alt+Enter', 'fullscreen')]:
            kc = tk.Canvas(kr, bg=T['surface'], highlightthickness=0, bd=0, height=px(28), width=max(px(34), F['key'].measure(key) + px(18)))
            ui.rounded(kc, 0, 0, int(kc.cget('width')), px(28), px(6), T['bg'], T['line'])
            kc.create_text(int(kc.cget('width')) // 2, px(14), text=key, fill=T['text'], font=F['key'])
            kc.pack(side='left'); tk.Label(kr, text=what, bg=T['surface'], fg=T['muted'], font=F['small']).pack(side='left', padx=(px(8), px(26)))

    def draw_scene_banner(self):
        cv = self.scene; cv.delete('banner')
        sw, sh = px(456), px(278)
        if self.game or not self.manifest:
            x0, y0, x1, y1 = px(16), sh - px(56), sw - px(16), sh - px(16)
            ui.rounded(cv, x0, y0, x1, y1, (y1 - y0) // 2, T['glass'], T['glass_line'], tags=('banner',))
            ui.circle(cv, x0 + px(20), (y0 + y1) / 2, px(10), T['ok'] if self.game else T['play'], tags=('banner',))
            if self.game:
                m, s = divmod(int(time.time() - self.game_t0), 60)
                msg = f'Game running · {self.loaded_profile} · {m}:{s:02d}'
            else: msg = 'Not built yet'
            cv.create_text(x0 + px(36), (y0 + y1) // 2, anchor='w', text=msg, fill=T['text'], font=F['bodyb'], tags=('banner',))
            pill = self.banner_pill if self.game else self.go_setup_pill
            w = pill.width_for(28)
            pill.place(x1 - px(4) - w, y0 + px(4), x1 - px(4), y1 - px(4))
            for p in (self.banner_pill, self.go_setup_pill):
                if p is not pill: cv.delete(p.tag); p.box = None
            cv.addtag_withtag('banner', pill.tag)
        else:
            for p in (self.banner_pill, self.go_setup_pill): cv.delete(p.tag); p.box = None

    # ---- Display / Graphics / Gameplay
    def page_display(self, f):
        self.heading(f, 'Display', 'How the two screens are arranged in the window. Saved to the profile as you change them.')
        c = ui.Card(f); c.pack(fill='x', padx=px(28)); b = c.body
        s = self.set
        ui.setting_row(b, 'Screen layout', "F9 or the pad's Back cycles it in game", lambda p: ui.Segmented(p, s['layout'], CHOICES['layout']))
        self.auto_seg = ui.setting_row(b, 'Auto layout in flight', 'Used by "auto" after a moment of steering',
                                       lambda p: ui.Segmented(p, s['auto_gameplay'], CHOICES['auto_gameplay']))
        ui.setting_row(b, 'Aspect ratio', 'integer = whole multiples of the original size', lambda p: ui.Segmented(p, s['aspect'], CHOICES['aspect']))
        ui.setting_row(b, 'Fullscreen', 'F11 or Alt+Enter toggles it in game', lambda p: ui.Segmented(p, s['fullscreen'], CHOICES['fullscreen']))
        ui.setting_row(b, 'Window scale', 'Window size in multiples of the screens (400×480)', lambda p: ui.Stepper(p, s['scale'], 1, 6, fmt=x_))
        ui.setting_row(b, 'Pad Back/Select: next layout', 'Off gives Select back to the game', lambda p: ui.Toggle(p, self.flags['select_layout']), last=True)

    def page_graphics(self, f):
        self.heading(f, 'Graphics', 'Higher settings need a stronger GPU. The F1 overlay changes them while you play.')
        c = ui.Card(f); c.pack(fill='x', padx=px(28)); b = c.body
        s = self.set
        ui.setting_row(b, '3D resolution', 'Internal rendering scale', lambda p: ui.Segmented(p, s['resolution'], CHOICES['resolution'], map(x_, CHOICES['resolution'])))
        ui.setting_row(b, 'Scaling filter', 'How the screens are scaled to the window', lambda p: ui.Segmented(p, s['filter'], CHOICES['filter']))
        self.fsr_slider = ui.setting_row(b, 'FSR sharpness', '0 = sharpest (filter fsr only)', lambda p: ui.Slider(p, s['fsr_sharpness'], 0, 2, 0.05))
        ui.setting_row(b, 'Anisotropic filtering', 'Sharper textures at grazing angles', lambda p: ui.Segmented(p, s['aniso'], CHOICES['aniso'], map(x_, CHOICES['aniso'])))
        ui.setting_row(b, 'Draw distance', 'Not in the original: markers, boats and cars from further away',
                       lambda p: ui.Segmented(p, s['draw_distance'], CHOICES['draw_distance'], map(x_, CHOICES['draw_distance'])))
        ui.setting_row(b, 'Frame counter', 'Shows the game fps in the window', lambda p: ui.Toggle(p, self.flags['show_fps']), last=True)

    def page_gameplay(self, f):
        self.heading(f, 'Gameplay & audio', 'Enhancements are off by default and marked as not in the original game.')
        c = ui.Card(f); c.pack(fill='x', padx=px(28)); b = c.body
        ui.setting_row(b, 'Free look', 'Right stick orbits the chase camera (not in the original game)', lambda p: ui.Toggle(p, self.flags['free_look']))
        ui.setting_row(b, 'Mute', 'No sound output', lambda p: ui.Toggle(p, self.flags['mute']))
        ui.setting_row(b, 'Controller', 'Gamepads are picked up automatically; the mouse is the touch screen',
                       lambda p: tk.Label(p, text='automatic', bg=T['surface'], fg=T['muted'], font=F['bodyb']), last=True)
        a = ui.Card(f, 'Advanced'); a.pack(fill='x', padx=px(28), pady=(px(16), 0))
        r = tk.Frame(a.body, bg=T['surface']); r.pack(fill='x', padx=px(14), pady=(px(8), 0))
        tk.Label(r, text='Extra arguments', bg=T['surface'], fg=T['text'], font=F['label']).pack(side='left')
        ui.Entry(r, self.set['args'], width=520).pack(side='right', fill='x', expand=True, padx=(px(20), 0))
        tk.Label(a.body, text="Passed to the game as they are, after the profile's options.", bg=T['surface'], fg=T['muted'],
                 font=F['small']).pack(anchor='e', padx=px(14), pady=(px(6), px(12)))

    # ---- Setup
    def page_setup(self, f):
        self.heading(f, 'Set up the game', 'Your own decrypted dump is turned into a native game. Nothing leaves the install folder.')
        c = ui.Card(f, 'Game dump'); c.pack(fill='x', padx=px(28))
        r = tk.Frame(c.body, bg=T['surface']); r.pack(fill='x', padx=px(14), pady=(px(8), 0))
        self.verify_btn = ui.PillButton(r, 'Verify', self.verify, 'secondary', width=96)
        self.verify_btn.pack(side='right', padx=(px(8), 0))
        ui.PillButton(r, 'Browse…', self.pick_rom, 'secondary', width=104).pack(side='right', padx=(px(8), 0))
        ui.Entry(r, self.rom, width=400).pack(side='left', fill='x', expand=True)
        self.checks = tk.Frame(c.body, bg=T['surface']); self.checks.pack(fill='x', padx=px(14), pady=(px(12), px(14)))
        self.set_checks(None)

        c = ui.Card(f, 'Build'); c.pack(fill='x', padx=px(28), pady=(px(16), 0))
        r = tk.Frame(c.body, bg=T['surface']); r.pack(fill='x', padx=px(14), pady=(px(6), 0))
        tk.Label(r, text='Install folder', bg=T['surface'], fg=T['muted'], font=F['body']).pack(side='left')
        self.home_l = tk.Label(r, bg=T['surface'], fg=T['text'], font=F['bodyb']); self.home_l.pack(side='left', padx=px(12))
        ui.PillButton(r, 'Change…', self.pick_home, 'ghost', width=96, height=30).pack(side='right')
        self.stepper = ui.StageStepper(c.body, STAGES, bg=T['surface']); self.stepper.pack(fill='x', pady=(px(10), 0))
        r = tk.Frame(c.body, bg=T['surface']); r.pack(fill='x', padx=px(14), pady=(px(6), px(14)))
        self.build_btn = ui.PillButton(r, 'Build', lambda: self.build(False), 'primary', width=140, height=40)
        self.build_btn.pack(side='right')
        self.rebuild_btn = ui.PillButton(r, 'Rebuild', lambda: self.build(True), 'ghost', width=120, height=40)
        self.rebuild_btn.pack(side='right', padx=px(10))
        left = tk.Frame(r, bg=T['surface']); left.pack(side='left', fill='x', expand=True)
        self.build_msg = tk.Label(left, bg=T['surface'], fg=T['muted'], font=F['body'], anchor='w', justify='left')
        self.build_msg.pack(anchor='w', fill='x')
        self.tools_row = tk.Frame(left, bg=T['surface']); self.tools_row.pack(anchor='w', pady=(px(4), 0))

    def set_checks(self, r):
        for w in self.checks.winfo_children(): w.destroy()
        if r is None:
            rows = [(None, 'Pick your .cia, .3ds/.cci or .cxi and press Verify')]
        elif r == 'busy':
            rows = [(None, 'Checking the dump…')]
        elif isinstance(r, str):
            rows = [(False, r)]
        else:
            code_ok = bool(r.get('code_sha256')) and not any('differs' in p for p in r['problems'])
            rows = [(not r.get('encrypted'), f'{r.get("container", "?").upper()}, {"encrypted" if r.get("encrypted") else "decrypted"}'),
                    (r.get('title') is not None, f'Title {r.get("program_id")}' + ('' if r.get('title') else ' (not supported)')),
                    (code_ok, 'Code matches the supported version' if code_ok else 'Code differs from the supported version')]
        for ok, text in rows:
            col = T['muted'] if ok is None else (T['ok'] if ok else T['error'])
            cv = tk.Canvas(self.checks, bg=T['surface'], highlightthickness=0, bd=0, width=px(18), height=px(18))
            ui.circle(cv, px(9), px(9), px(18), col)
            cv.create_text(px(9), px(9), text='' if ok is None else ('✓' if ok else '✕'), fill=T['bg'], font=F['key'])
            cv.pack(side='left'); tk.Label(self.checks, text=text, bg=T['surface'], fg=T['text'] if ok is not None else T['muted'],
                                           font=F['bodyb'] if ok is not None else F['body']).pack(side='left', padx=(px(8), px(24)))

    # ---- Log
    def page_log(self, f):
        self.heading(f, 'Log', "Build output and the game's messages (pwr.log in the profile folder).")
        b = tk.Frame(f, bg=T['bg']); b.pack(side='bottom', fill='x', padx=px(28), pady=(px(12), px(18)))
        ui.PillButton(b, 'Copy', self.copy_log, 'secondary', width=100, height=34).pack(side='left')
        ui.PillButton(b, 'Clear', lambda: self.log_txt.delete('1.0', 'end'), 'ghost', width=100, height=34).pack(side='left', padx=px(8))
        ui.PillButton(b, 'Open log folder', self.open_log_folder, 'ghost', height=34).pack(side='right')
        box = tk.Frame(f, bg=T['line'], padx=1, pady=1); box.pack(fill='both', expand=True, padx=px(28))
        self.log_txt = tk.Text(box, wrap='none', font=F['mono'], bg=T['console'], fg=T['console_text'], bd=0, highlightthickness=0,
                               padx=px(14), pady=px(10), insertbackground=T['text'], selectbackground=T['surface2'])
        sb = ttk.Scrollbar(box, command=self.log_txt.yview, style='Pwr.Vertical.TScrollbar')
        self.log_txt.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y'); self.log_txt.pack(side='left', fill='both', expand=True)
        for tag, col in (('stage', T['accent']), ('ok', T['ok']), ('err', T['error']), ('cmd', T['muted']), ('game', T['console_text'])):
            self.log_txt.tag_configure(tag, foreground=col)

    # ---------------- header, rail, status
    def draw_header(self):
        cv = self.head; w = cv.winfo_width()
        cv.delete('art', 'picker')
        img = self.art['header']
        if img:
            iw = img.width()
            if w > iw:   # widen with the art's last column
                if getattr(self, '_strip_w', 0) != w - iw:
                    col = tk.PhotoImage(master=self); col.tk.call(col, 'copy', img, '-from', iw - 1, 0, iw, img.height())
                    self._strip = tk.PhotoImage(master=self); self._strip.tk.call(self._strip, 'copy', col, '-zoom', w - iw + 1, 1)
                    self._strip_w = w - iw
                cv.create_image(iw - 1, 0, anchor='nw', image=self._strip, tags=('art',))
            cv.create_image(0, 0, anchor='nw', image=img, tags=('art',))
        cv.tag_lower('art')
        x1 = w - px(20); y0, y1 = px(44), px(96)
        self.play_pill.place(x1 - px(168), y0, x1, y1)
        px1 = x1 - px(168) - px(12); px0 = px1 - px(168)
        ui.rounded(cv, px0, px(48), px1, px(92), px(22), T['glass'], T['glass_line'], tags=('picker',))
        cv.create_text(px0 + px(18), px(62), anchor='w', text='PROFILE', fill=T['muted'], font=F['tiny'], tags=('picker',))
        name = self.profile.get()
        while F['bodyb'].measure(name) > px(118) and len(name) > 2: name = name[:-2] + '…' if not name.endswith('…') else name[:-2] + '…'
        cv.create_text(px0 + px(18), px(78), anchor='w', text=name, fill=T['text'], font=F['label'], tags=('picker',))
        cv.create_text(px1 - px(22), px(70), text='▾', fill=T['muted'], font=F['label'], tags=('picker',))

    def update_play_button(self):
        if self.game: self.play_pill.configure(text='STOP', icon='■', kind='danger', enabled=True, command=self.stop)
        else: self.play_pill.configure(text='PLAY', icon='▶', kind='primary', enabled=bool(self.manifest) and not self.worker, command=self.play)

    def draw_rail(self):
        cv = self.rail; cv.delete('all'); h = cv.winfo_height()
        for i, (key, label, glyph) in enumerate(PAGES):
            y = px(20) + i * px(46); on = key == getattr(self, 'page', None)
            if on:
                ui.rounded(cv, px(12), y, px(RAIL_W - 12), y + px(40), px(10), T['surface2'])
                ui.rounded(cv, px(12), y + px(10), px(16), y + px(30), px(2), T['accent'])
            elif key == self.rail_hot:
                ui.rounded(cv, px(12), y, px(RAIL_W - 12), y + px(40), px(10), '#123254')
            cv.create_text(px(38), y + px(20), text=glyph, fill=T['accent'] if on else T['muted'], font=F['glyph'])
            cv.create_text(px(58), y + px(20), anchor='w', text=label, fill=T['text'] if on else T['muted'], font=F['navb'] if on else F['nav'])
            if key == 'setup' and not self.manifest: ui.circle(cv, px(RAIL_W - 36), y + px(20), px(9), T['play'])
            if key == 'setup' and self.worker: ui.circle(cv, px(RAIL_W - 36), y + px(20), px(9), T['accent'])
            if key == 'play' and self.game: ui.circle(cv, px(RAIL_W - 36), y + px(20), px(9), T['ok'])
        cv.create_text(px(24), h - px(24), anchor='w', text='Woohoo! Flight School', fill=T['muted'], font=F['small'])

    def _rail_index(self, y):
        i = (y - px(20)) // px(46)
        return PAGES[i][0] if 0 <= i < len(PAGES) and (y - px(20)) % px(46) < px(40) else None

    def rail_hover(self, e):
        k = self._rail_index(e.y) if e else None
        if k != self.rail_hot: self.rail_hot = k; self.rail.configure(cursor='hand2' if k else ''); self.draw_rail()

    def rail_click(self, e):
        k = self._rail_index(e.y)
        if k: self.show(k)

    def show(self, key):
        self.page = key; self.pages[key].tkraise(); self.draw_rail(); self.save_cfg()
        if key == 'log': self.log_txt.see('end')

    def update_status(self):
        if self.manifest:
            m = self.manifest
            self.status_dot.set(T['ok'])
            self.status_l.configure(text=f'Built {m.get("built", "?")} · recompiler {m.get("recompiler") or "?"} · {m.get("build_type", "Release")}')
        else:
            self.status_dot.set(T['play']); self.status_l.configure(text='Not built yet in this install folder')
        if self.worker: self.status_dot.set(T['accent'])
        self.status_r.configure(text=f'Profile: {self.profile.get()}     ' + elide(short(os.path.abspath(self.home.get())), F['small'], px(360)))

    # ---------------- helpers
    def log(self, line): self.logq.put(line)   # any thread

    def in_bg(self, fn, done):
        """run fn() on a thread, then done(result or exception) on the Tk thread"""
        def run():
            try: res = fn()
            except Exception as e: res = e   # noqa: BLE001 (shown to the user)
            self.calls.put(lambda: done(res))
        threading.Thread(target=run, daemon=True).start()

    def append_log(self, line):
        tag = 'stage' if line.startswith('[') and '/5]' in line[:6] else 'ok' if line.startswith(('BUILD OK', 'done in', '── ')) else \
            'err' if ('FAILED' in line or line.startswith(('error', '  problem'))) else 'cmd' if line.startswith('$ ') else None
        at_end = self.log_txt.yview()[1] > 0.999
        self.log_txt.insert('end', line + '\n', tag)
        if at_end: self.log_txt.see('end')

    def poll(self):
        try:
            while True: self.calls.get_nowait()()
        except queue.Empty: pass
        try:
            for _ in range(500):
                line = self.logq.get_nowait()
                self.append_log(line); self.track_build(line)
        except queue.Empty: pass
        if self.worker and not self.worker.is_alive():
            self.worker = None; self.build_done()
        if self.build_t0 and self.worker and 0 <= self.stage < 5:
            m, s = divmod(int(time.time() - self.build_t0), 60)
            self.build_msg.configure(text=f'{STAGE_TEXT[self.stage]}  {m}:{s:02d}', fg=T['muted'])
        if self.tail:
            chunk = self.tail.read()
            if chunk:
                for l in chunk.splitlines(): self.append_log(l)
        if self.game:
            if self.game.poll() is not None: self.game_exited()
            elif self.page == 'play': self.draw_scene_banner()
        self.after(100 if (self.worker or self.game) else 200, self.poll)

    def track_build(self, line):
        if line.startswith('[') and '/5]' in line[:6] and line[1].isdigit():
            self.stage = int(line[1]) - 1
            self.stepper.set(['done'] * self.stage + ['active'] + ['pending'] * (4 - self.stage))
        elif line.startswith('BUILD OK'):
            self.stepper.set(['done'] * 5)
        elif line.startswith('BUILD FAILED'):
            st = ['done'] * max(0, self.stage) + ['failed'] + ['pending'] * (4 - max(0, self.stage))
            self.stepper.set(st[:5]); self.build_msg.configure(text=(line.split(':', 1)[1].strip() if ':' in line else line), fg=T['error'], wraplength=px(420))

    def refresh_install(self):
        home = self.home.get()
        try: self.manifest = json.load(open(os.path.join(home, 'manifest.json')))
        except (OSError, ValueError): self.manifest = None
        self.home_l.configure(text=elide(short(os.path.abspath(home)), F['bodyb'], px(400)))
        if self.manifest and 'title' in self.manifest:
            self.play_title.configure(text='In flight' if self.game else 'Ready for take-off')
            self.play_sub.configure(text=f'{self.manifest["title"]} · {self.manifest.get("program_id", "")} · code verified')
        else:
            self.play_title.configure(text='Almost there')
            self.play_sub.configure(text='Build the game from your own dump first: Setup.')
        profs = P.list_profiles(home) or ['default']
        self.profiles = profs
        if self.profile.get() not in profs: self.profile.set(profs[0])
        if not self.worker and self.stage < 0:
            self.stepper.set(['done'] * 5 if self.manifest else ['pending'] * 5)
            if self.manifest: self.build_msg.configure(text='Built and installed. Rebuild extracts the files again.', fg=T['muted'])
            self.rebuild_btn.set(enabled=bool(self.manifest))
        self.update_play_button(); self.draw_rail(); self.update_status(); self.draw_scene_banner()

    def check_tools(self):
        def done(rows):
            for w in self.tools_row.winfo_children(): w.destroy()
            if isinstance(rows, Exception): return
            missing = []
            for tool, path, hint in rows:
                opt = 'optional' in tool
                name = tool.split(' ')[0].replace('python3', 'Python').replace('cmake', 'CMake')
                if tool.startswith('C compiler'): name = 'C compiler'
                if tool.startswith('SDL2 (system)'): name = 'SDL2'
                if tool.startswith('shaderc'): name = 'shaderc'
                col = T['ok'] if path else (T['warn'] if opt else T['error'])
                ui.Dot(self.tools_row, col, 8, T['surface']).pack(side='left', padx=(0, px(5)))
                tk.Label(self.tools_row, text=name, bg=T['surface'], fg=T['text'] if path else col, font=F['small']).pack(side='left', padx=(0, px(12)))
                if not path and not opt: missing.append(name); self.log(f'missing {tool}: {hint}')
            if missing and not self.manifest and not self.worker:
                self.build_msg.configure(text=f'Missing: {", ".join(missing)} (hints in Log; README lists them)', fg=T['error'], wraplength=px(420))
            elif not self.manifest and not self.worker:
                self.build_msg.configure(text='A first build takes a few minutes. Tools found:', fg=T['muted'])
        self.in_bg(lambda: P.check(log=lambda *a: None), done)

    # ---------------- settings (saved as they change)
    def load_profile(self):
        self.loading = True
        try:
            s = P.load_settings(self.home.get(), self.profile.get())
            d = P.DEFAULT_SETTINGS
            for k, v in self.set.items():
                val = s.get(k, d.get(k, ''))
                if val is None: val = d.get(k, '')
                if k in ('scale', 'resolution', 'aniso', 'draw_distance'):
                    try: val = str(int(float(val)))
                    except (TypeError, ValueError): val = str(d[k])
                elif k == 'fsr_sharpness':
                    try: val = f'{float(val):.2f}'
                    except (TypeError, ValueError): val = f'{d[k]:.2f}'
                v.set(str(val))
            if not self.set['auto_gameplay'].get(): self.set['auto_gameplay'].set('large')
            for k, v in self.flags.items(): v.set(bool(s.get(k, d.get(k))))
            self.loaded_profile = self.profile.get()
        finally: self.loading = False
        self.changed(save=False)

    def current_settings(self):
        s = {k: v.get() for k, v in self.set.items()}
        try: s['scale'] = max(1, min(6, int(s['scale'])))
        except ValueError: s['scale'] = 2
        for k, lo, hi, cast in (('resolution', 1, 4, int), ('aniso', 1, 16, int), ('fsr_sharpness', 0, 2, float), ('draw_distance', 1, 4, int)):
            try: s[k] = min(hi, max(lo, cast(float(s[k]))))
            except ValueError: s[k] = P.DEFAULT_SETTINGS[k]
        for k, v in self.flags.items(): s[k] = v.get()
        return s

    def changed(self, save=True):
        if self.loading: return
        s = self.set; fl = self.flags
        self.auto_seg.set_enabled(s['layout'].get() == 'auto')
        self.fsr_slider.set_enabled(s['filter'].get() == 'fsr')
        self.summary['layout'].configure(text=s['layout'].get() + (f' → {s["auto_gameplay"].get()}' if s['layout'].get() == 'auto' else ''))
        self.summary['resolution'].configure(text=x_(s['resolution'].get()))
        self.summary['filter'].configure(text=s['filter'].get())
        self.summary['draw_distance'].configure(text=x_(s['draw_distance'].get()))
        self.summary['free_look'].configure(text='on' if fl['free_look'].get() else 'off')
        self.summary['mute'].configure(text='muted' if fl['mute'].get() else 'on')
        if save:
            if self.save_job: self.after_cancel(self.save_job)
            self.save_job = self.after(400, self.save_profile)

    def save_profile(self, quiet=False):
        self.save_job = None
        name = self.loaded_profile or self.profile.get()
        try:
            P.create_profile(self.home.get(), name, exist_ok=True)
            P.save_settings(self.home.get(), name, self.current_settings())
        except (OSError, P.PipelineError) as e:
            self.toast.show(f'Not saved: {e}', T['error'], 4000); return
        if not quiet: self.toast.show(f'Saved to profile {name}')

    def flush_save(self):
        if self.save_job: self.after_cancel(self.save_job); self.save_profile(quiet=True)

    # ---------------- profiles
    def profile_menu(self, e=None):
        m = tk.Menu(self, tearoff=0, bg=T['surface'], fg=T['text'], activebackground=T['surface2'], activeforeground=T['text'],
                    selectcolor=T['accent'], bd=0, relief='flat', font=F['body'])
        for n in self.profiles:
            m.add_radiobutton(label=n, value=n, variable=self.profile, command=self.switch_profile)
        m.add_separator()
        m.add_command(label='New profile…', command=self.new_profile)
        m.add_command(label='Delete this profile…', command=self.delete_profile)
        m.add_command(label='Open profile folder', command=self.open_log_folder)
        x = self.head.winfo_rootx() + self.head.winfo_width() - px(200) - px(168)
        y = self.head.winfo_rooty() + px(96)
        try: m.tk_popup(x, y)
        finally: m.grab_release()

    def switch_profile(self):
        new = self.profile.get()
        if new == self.loaded_profile: return
        self.profile.set(self.loaded_profile); self.flush_save(); self.profile.set(new)
        self.load_profile(); self.draw_header(); self.update_status(); self.save_cfg()
        self.toast.show(f'Profile {new}', T['accent'])

    def new_profile(self):
        w = tk.Toplevel(self, bg=T['bg']); w.title('New profile'); w.transient(self); w.resizable(False, False)
        name = tk.StringVar()
        tk.Label(w, text='New profile', bg=T['bg'], fg=T['text'], font=F['h1']).pack(anchor='w', padx=px(24), pady=(px(18), 0))
        tk.Label(w, text='A profile has its own save data and settings.', bg=T['bg'], fg=T['muted'], font=F['body']).pack(anchor='w', padx=px(24))
        e = ui.Entry(w, name, width=320); e.pack(padx=px(24), pady=px(14), fill='x')
        err = tk.Label(w, bg=T['bg'], fg=T['error'], font=F['small']); err.pack(anchor='w', padx=px(24))
        b = tk.Frame(w, bg=T['bg']); b.pack(fill='x', padx=px(24), pady=(px(6), px(20)))

        def ok():
            n = name.get().strip()
            try: P.create_profile(self.home.get(), n)
            except (P.PipelineError, OSError) as ex: err.configure(text=str(ex)); return
            self.flush_save()
            P.save_settings(self.home.get(), n, self.current_settings())   # starts from the current settings
            w.destroy(); self.profile.set(n); self.refresh_install(); self.load_profile(); self.draw_header(); self.save_cfg()
            self.toast.show(f'Profile {n} created')
        ui.PillButton(b, 'Create', ok, 'primary', width=110).pack(side='right')
        ui.PillButton(b, 'Cancel', w.destroy, 'ghost', width=100).pack(side='right', padx=px(8))
        w.bind('<Return>', lambda ev: ok()); w.bind('<Escape>', lambda ev: w.destroy())
        w.update_idletasks()
        w.geometry(f'+{self.winfo_rootx() + (self.winfo_width() - w.winfo_width()) // 2}+{self.winfo_rooty() + px(160)}')
        e.e.focus_set(); w.grab_set()

    def delete_profile(self):
        n = self.profile.get()
        if self.game and n == self.loaded_profile:
            messagebox.showinfo('Delete profile', 'The game is running with this profile.', parent=self); return
        if not messagebox.askyesno('Delete profile', f'Delete profile "{n}" and its save data? This cannot be undone.', parent=self): return
        if self.save_job: self.after_cancel(self.save_job); self.save_job = None
        try: P.delete_profile(self.home.get(), n)
        except P.PipelineError as e: messagebox.showerror('Delete profile', str(e), parent=self); return
        self.refresh_install(); self.load_profile(); self.draw_header(); self.save_cfg()
        self.toast.show(f'Profile {n} deleted', T['warn'])

    # ---------------- ROM, build
    def pick_rom(self):
        p = filedialog.askopenfilename(parent=self, title='Your game dump', filetypes=ROM_TYPES)
        if p: self.rom.set(p); self.verify()

    def pick_home(self):
        if self.worker: return
        p = filedialog.askdirectory(parent=self, title='Install folder', mustexist=False)
        if p:
            self.flush_save(); self.home.set(p); self.stage = -1; self.refresh_install(); self.load_profile(); self.draw_header(); self.save_cfg()

    def verify(self, then=None):
        rom = self.rom.get().strip()
        if not rom: self.set_checks('Pick your game dump first'); return
        self.set_checks('busy'); self.verify_btn.set(enabled=False)

        def done(r):
            self.verify_btn.set(enabled=True)
            if isinstance(r, P.PipelineError): self.set_checks(str(r)); self.verified = None; return
            if isinstance(r, Exception): self.set_checks(f'unexpected error: {r!r}'); self.verified = None; return
            self.verified = r; self.set_checks(r); self.save_cfg()
            if r['ok'] and then: then()
            elif not r['ok']: self.toast.show('This dump cannot be used', T['error'])
        self.in_bg(lambda: P.verify(rom, log=self.log), done)

    def build(self, force):
        if self.worker or self.game: return
        rom, home = self.rom.get().strip(), self.home.get()
        if not rom: self.set_checks('Pick your game dump first'); return
        self.flush_save(); self.save_cfg()
        self.stage = 0; self.build_t0 = time.time()
        self.stepper.set(['active'] + ['pending'] * 4)
        self.log(f'── build ({"extract again" if force else "build"}) into {home}')

        def run():
            try: P.build(rom, home, force=force, log=self.log); self.log('BUILD OK')
            except P.PipelineError as e: self.log(f'BUILD FAILED: {e}')
            except Exception as e: self.log(f'BUILD FAILED (unexpected): {e!r}')   # noqa: BLE001
        self.worker = threading.Thread(target=run, daemon=True); self.worker.start()
        self.build_btn.set(text='Building…', kind='ghost', enabled=False); self.rebuild_btn.set(enabled=False)
        self.update_play_button(); self.draw_rail(); self.update_status()

    def build_done(self):
        # the queue may still hold the last lines
        try:
            while True:
                line = self.logq.get_nowait(); self.append_log(line); self.track_build(line)
        except queue.Empty: pass
        ok = all(s == 'done' for s in self.stepper.state)
        secs = int(time.time() - (self.build_t0 or time.time()))
        self.build_btn.set(text='Build', kind='primary', enabled=True); self.rebuild_btn.set(enabled=True)
        self.refresh_install()
        if ok:
            self.build_msg.configure(text=f'Built in {secs // 60}:{secs % 60:02d}. Ready to play.', fg=T['ok'])
            self.toast.show('Build finished: ready to play')
        else:
            self.toast.show('Build failed: see Log', T['error'], 4000)
        self.stage = -1 if ok else self.stage

    # ---------------- game
    def play(self):
        if self.game or self.worker: return
        if not self.manifest: self.show('setup'); self.toast.show('Build the game first', T['play']); return
        self.flush_save()
        name = self.loaded_profile or self.profile.get()
        self.log(f'── game started (profile {name}) ──')
        try: self.game = P.launch(self.home.get(), name, wait=False, log=self.log)
        except (P.PipelineError, OSError) as e:
            self.game = None; messagebox.showerror('Launch', str(e), parent=self); return
        self.game_t0 = time.time()
        try: self.tail = open(P.log_path(self.home.get(), name), errors='replace')
        except OSError: self.tail = None
        self.refresh_install()

    def stop(self):
        if not self.game: return
        g = self.game
        try: g.terminate()
        except OSError: pass
        self.after(3000, lambda: g.poll() is None and g.kill())
        self.toast.show('Stopping the game…', T['warn'])

    def game_exited(self):
        code = self.game.returncode; self.game = None
        if self.tail:
            rest = self.tail.read()
            for l in rest.splitlines(): self.append_log(l)
            self.tail.close(); self.tail = None
        self.append_log(f'── game exited with code {code} ──')
        if code not in (0, None) and code > 0: self.toast.show(f'Game exited with code {code}: see Log', T['error'], 4000)
        self.load_profile()   # the in-game F1 overlay may have changed the settings
        self.refresh_install()

    def open_log_folder(self):
        """the profile folder: pwr.log (this or the last run), pwr.log.1 (the run before), save/, settings.json"""
        d = P.profile_dir(self.home.get() or P.default_home(), self.profile.get())
        if not os.path.isdir(d): messagebox.showinfo('Log folder', f'No log yet: {d} does not exist.', parent=self); return
        import subprocess
        try:
            if os.name == 'nt': os.startfile(d)
            else: subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', d])
        except OSError as e: messagebox.showerror('Log folder', f'{d}\n{e}', parent=self)

    def copy_log(self):
        self.clipboard_clear(); self.clipboard_append(self.log_txt.get('1.0', 'end-1c')); self.toast.show('Log copied')

    def quit_app(self):
        self.flush_save(); self.save_cfg(); self.destroy()


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]); ap.add_argument('--home'); ap.add_argument('--rom')
    a = ap.parse_args()
    if os.name == 'nt':   # sharp text on scaled Windows displays
        try:
            import ctypes; ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError): pass
    Launcher(a.home, a.rom).mainloop()


if __name__ == '__main__':
    main()
