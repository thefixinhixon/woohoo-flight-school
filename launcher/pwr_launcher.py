"""Launcher: pick your own game dump, verify it, recompile + build the native game, manage profiles, launch.

usage: python3 launcher/pwr_launcher.py [--home DIR] [--rom FILE]

A Tk front end for tools/pipeline.py (the same stages run from the command line). Nothing here touches the ROM
beyond reading it; the build and everything extracted from the ROM stay in the install folder.
Remembers the last ROM and install folder in ~/.config/pwr-port/launcher.json (%APPDATA%\\pwr-port on Windows).
"""
import json, os, queue, sys, threading, tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import pipeline as P  # noqa: E402

CHOICES = dict(layout=['stacked', 'side', 'large', 'top', 'bottom', 'auto'], filter=['nearest', 'linear', 'sharp', 'fsr', 'fxaa'],
               aspect=['keep', 'integer', 'stretch'], fullscreen=['off', 'borderless', 'exclusive'], auto_gameplay=['large', 'top'],
               resolution=['1', '2', '3', '4'], aniso=['1', '2', '4', '8', '16'])
ROM_TYPES = [('3DS game dumps', '*.cia *.3ds *.cci *.cxi'), ('All files', '*')]


def config_path():
    base = os.environ.get('APPDATA') if os.name == 'nt' else os.environ.get('XDG_CONFIG_HOME', os.path.expanduser('~/.config'))
    return os.path.join(base or os.path.expanduser('~'), 'pwr-port', 'launcher.json')


class Launcher(tk.Tk):
    def __init__(self, home=None, rom=None):
        super().__init__()
        self.title('Woohoo! Flight School')
        self.minsize(720, 640)
        self.cfg = self.load_cfg()
        self.home = tk.StringVar(value=home or self.cfg.get('home') or P.default_home())
        self.rom = tk.StringVar(value=rom or self.cfg.get('rom', ''))
        self.status = tk.StringVar(value='Pick your game dump (.cia, .3ds/.cci or .cxi) and press Verify.')
        self.built = tk.StringVar()
        self.profile = tk.StringVar(value=self.cfg.get('profile', 'default'))
        self.set = {k: tk.StringVar() for k in ('layout', 'filter', 'aspect', 'fullscreen', 'auto_gameplay', 'scale', 'args', 'resolution', 'aniso', 'fsr_sharpness', 'draw_distance')}
        self.mute = tk.BooleanVar(); self.fps = tk.BooleanVar(); self.sel = tk.BooleanVar(value=True); self.look = tk.BooleanVar()
        self.logq = queue.Queue(); self.worker = None; self.game = None
        self.build_ui()
        self.refresh_install(); self.load_profile()
        self.after(100, self.poll)

    # ---------------- persistence
    def load_cfg(self):
        try: return json.load(open(config_path()))
        except (OSError, ValueError): return {}

    def save_cfg(self):
        os.makedirs(os.path.dirname(config_path()), exist_ok=True)
        json.dump(dict(home=os.path.abspath(self.home.get()), rom=os.path.abspath(self.rom.get()) if self.rom.get() else '', profile=self.profile.get()),
                  open(config_path(), 'w'), indent=2)

    # ---------------- layout
    def build_ui(self):
        pad = dict(padx=6, pady=3)
        f = ttk.LabelFrame(self, text='1. Game ROM'); f.pack(fill='x', **pad)
        ttk.Entry(f, textvariable=self.rom).grid(row=0, column=0, sticky='ew', **pad)
        ttk.Button(f, text='Browse...', command=self.pick_rom).grid(row=0, column=1, **pad)
        ttk.Button(f, text='Verify', command=self.verify).grid(row=0, column=2, **pad)
        self.status_lbl = ttk.Label(f, textvariable=self.status, wraplength=680); self.status_lbl.grid(row=1, column=0, columnspan=3, sticky='w', **pad)
        f.columnconfigure(0, weight=1)

        f = ttk.LabelFrame(self, text='2. Build the native game'); f.pack(fill='x', **pad)
        ttk.Label(f, text='Install folder').grid(row=0, column=0, sticky='w', **pad)
        ttk.Entry(f, textvariable=self.home).grid(row=0, column=1, sticky='ew', **pad)
        ttk.Button(f, text='Browse...', command=self.pick_home).grid(row=0, column=2, **pad)
        self.build_btn = ttk.Button(f, text='Build', command=lambda: self.build(False)); self.build_btn.grid(row=1, column=0, sticky='w', **pad)
        self.rebuild_btn = ttk.Button(f, text='Rebuild (extract again)', command=lambda: self.build(True)); self.rebuild_btn.grid(row=1, column=1, sticky='w', **pad)
        self.progress = ttk.Progressbar(f, maximum=5); self.progress.grid(row=2, column=0, columnspan=3, sticky='ew', **pad)
        ttk.Label(f, textvariable=self.built).grid(row=3, column=0, columnspan=3, sticky='w', **pad)
        f.columnconfigure(1, weight=1)

        f = ttk.LabelFrame(self, text='3. Play'); f.pack(fill='x', **pad)
        ttk.Label(f, text='Profile').grid(row=0, column=0, sticky='w', **pad)
        self.prof_box = ttk.Combobox(f, textvariable=self.profile, state='readonly', width=18); self.prof_box.grid(row=0, column=1, sticky='w', **pad)
        self.prof_box.bind('<<ComboboxSelected>>', lambda e: self.load_profile())
        ttk.Button(f, text='New...', command=self.new_profile).grid(row=0, column=2, sticky='w', **pad)
        ttk.Button(f, text='Delete', command=self.delete_profile).grid(row=0, column=3, sticky='w', **pad)
        row = 1
        for i, (k, label) in enumerate([('layout', 'Screen layout'), ('auto_gameplay', 'Auto layout in flight'), ('filter', 'Scaling filter'),
                                         ('aspect', 'Aspect ratio'), ('fullscreen', 'Fullscreen')]):
            ttk.Label(f, text=label).grid(row=row + i // 2, column=(i % 2) * 2, sticky='w', **pad)
            ttk.Combobox(f, textvariable=self.set[k], values=CHOICES[k], state='readonly', width=12).grid(row=row + i // 2, column=(i % 2) * 2 + 1, sticky='w', **pad)
        row += 3
        for i, (k, label) in enumerate([('resolution', '3D resolution (x)'), ('aniso', 'Anisotropic filter (x)')]):
            ttk.Label(f, text=label).grid(row=row, column=i * 2, sticky='w', **pad)
            ttk.Combobox(f, textvariable=self.set[k], values=CHOICES[k], state='readonly', width=12).grid(row=row, column=i * 2 + 1, sticky='w', **pad)
        row += 1
        ttk.Label(f, text='FSR sharpness (0 = most)').grid(row=row, column=0, sticky='w', **pad)
        ttk.Spinbox(f, from_=0, to=2, increment=0.25, textvariable=self.set['fsr_sharpness'], width=6).grid(row=row, column=1, sticky='w', **pad)
        ttk.Checkbutton(f, text='Frame counter', variable=self.fps).grid(row=row, column=2, sticky='w', **pad)
        row += 1
        ttk.Label(f, text='Window scale').grid(row=row, column=0, sticky='w', **pad)
        ttk.Spinbox(f, from_=1, to=6, textvariable=self.set['scale'], width=5).grid(row=row, column=1, sticky='w', **pad)
        ttk.Checkbutton(f, text='Mute', variable=self.mute).grid(row=row, column=2, sticky='w', **pad)
        ttk.Checkbutton(f, text='Pad Back/Select: next layout', variable=self.sel).grid(row=row, column=3, sticky='w', **pad)
        row += 1
        ttk.Checkbutton(f, text='Free look: right stick orbits the camera (not in the original game)', variable=self.look).grid(row=row, column=0, columnspan=4, sticky='w', **pad)
        row += 1
        ttk.Label(f, text='Draw distance (x, 1 = original)').grid(row=row, column=0, sticky='w', **pad)
        ttk.Spinbox(f, from_=1, to=4, textvariable=self.set['draw_distance'], width=5).grid(row=row, column=1, sticky='w', **pad)
        ttk.Label(f, text='Extra arguments').grid(row=row + 1, column=0, sticky='w', **pad)
        ttk.Entry(f, textvariable=self.set['args']).grid(row=row + 1, column=1, columnspan=3, sticky='ew', **pad)
        b = ttk.Frame(f); b.grid(row=row + 2, column=0, columnspan=4, sticky='w', **pad)
        ttk.Button(b, text='Save settings', command=self.save_profile).pack(side='left', padx=3)
        self.launch_btn = ttk.Button(b, text='Launch', command=self.launch); self.launch_btn.pack(side='left', padx=3)
        ttk.Button(b, text='Open log folder', command=self.open_log_folder).pack(side='left', padx=3)
        ttk.Label(f, text='In game: F11 or Alt+Enter fullscreen, F9 or pad Back/Select next screen layout, F1 settings overlay.').grid(row=row + 3, column=0, columnspan=4, sticky='w', **pad)
        f.columnconfigure(3, weight=1)

        f = ttk.LabelFrame(self, text='Log'); f.pack(fill='both', expand=True, **pad)
        self.log_txt = tk.Text(f, height=12, wrap='none', font=('TkFixedFont', 9))
        sb = ttk.Scrollbar(f, command=self.log_txt.yview); self.log_txt.configure(yscrollcommand=sb.set)
        self.log_txt.pack(side='left', fill='both', expand=True); sb.pack(side='right', fill='y')

    # ---------------- helpers
    def log(self, line): self.logq.put(line)   # any thread

    def poll(self):
        try:
            while True:
                line = self.logq.get_nowait()
                if line.startswith('[') and '/5]' in line[:6]: self.progress['value'] = int(line[1]) - 0.5
                self.log_txt.insert('end', line + '\n'); self.log_txt.see('end')
        except queue.Empty: pass
        if self.worker and not self.worker.is_alive(): self.worker = None; self.set_busy(False); self.refresh_install()
        if self.game and self.game.poll() is not None:
            self.log(f'game exited with code {self.game.returncode}'); self.game = None; self.launch_btn.state(['!disabled'])
        self.after(100, self.poll)

    def set_busy(self, busy):
        for b in (self.build_btn, self.rebuild_btn, self.launch_btn): b.state(['disabled'] if busy else ['!disabled'])

    def set_status(self, text, ok):
        self.status.set(text); self.status_lbl.configure(foreground='#1a7f37' if ok else '#b42318')

    def refresh_install(self):
        home = self.home.get()
        try:
            m = json.load(open(os.path.join(home, 'manifest.json')))
            self.built.set(f'Built: {m["title"]} ({m["program_id"]}), {m["built"]}, recompiler {m.get("recompiler") or "?"}')
        except (OSError, ValueError, KeyError):
            self.built.set('Not built yet in this install folder.')
        profs = P.list_profiles(home) or ['default']
        self.prof_box['values'] = profs
        if self.profile.get() not in profs: self.profile.set(profs[0])

    # ---------------- actions
    def pick_rom(self):
        p = filedialog.askopenfilename(title='Your game dump', filetypes=ROM_TYPES)
        if p: self.rom.set(p); self.verify()

    def pick_home(self):
        p = filedialog.askdirectory(title='Install folder', mustexist=False)
        if p: self.home.set(p); self.refresh_install(); self.load_profile()

    def verify(self):
        try:
            r = P.verify(self.rom.get(), log=self.log)
        except P.PipelineError as e:
            self.set_status(str(e), False); return False
        if r['ok']: self.set_status(f'OK: {r["title"]}, {r["container"].upper()}, title {r["program_id"]}, code matches the supported version.', True)
        else: self.set_status('Not usable: ' + '; '.join(r['problems']), False)
        self.save_cfg()
        return r['ok']

    def build(self, force):
        if self.worker or not self.verify(): return
        self.save_cfg(); self.set_busy(True); self.progress['value'] = 0
        rom, home = self.rom.get(), self.home.get()

        def run():
            try: P.build(rom, home, force=force, log=self.log); self.log('BUILD OK')
            except P.PipelineError as e: self.log(f'BUILD FAILED: {e}')
            except Exception as e: self.log(f'BUILD FAILED (unexpected): {e!r}')
        self.worker = threading.Thread(target=run, daemon=True); self.worker.start()

    def load_profile(self):
        s = P.load_settings(self.home.get(), self.profile.get())
        for k, v in self.set.items(): v.set(str(s.get(k, P.DEFAULT_SETTINGS.get(k, '')) if s.get(k) is not None else ''))
        if not self.set['auto_gameplay'].get(): self.set['auto_gameplay'].set('large')
        self.mute.set(bool(s.get('mute'))); self.fps.set(bool(s.get('show_fps'))); self.sel.set(bool(s.get('select_layout', True))); self.look.set(bool(s.get('free_look')))

    def current_settings(self):
        s = {k: v.get() for k, v in self.set.items()}
        try: s['scale'] = max(1, min(6, int(s['scale'])))
        except ValueError: s['scale'] = 2
        for k, lo, hi, cast in (('resolution', 1, 4, int), ('aniso', 1, 16, int), ('fsr_sharpness', 0, 2, float), ('draw_distance', 1, 4, int)):
            try: s[k] = min(hi, max(lo, cast(float(s[k]))))
            except ValueError: s[k] = P.DEFAULT_SETTINGS[k]
        s['mute'] = self.mute.get(); s['show_fps'] = self.fps.get(); s['select_layout'] = self.sel.get(); s['free_look'] = self.look.get()
        return s

    def save_profile(self):
        P.create_profile(self.home.get(), self.profile.get(), exist_ok=True)
        P.save_settings(self.home.get(), self.profile.get(), self.current_settings()); self.save_cfg()
        self.log(f'saved profile {self.profile.get()}')

    def new_profile(self):
        w = tk.Toplevel(self); w.title('New profile'); name = tk.StringVar()
        ttk.Label(w, text='Name (its own save data and settings)').pack(padx=8, pady=4)
        e = ttk.Entry(w, textvariable=name); e.pack(padx=8, pady=4); e.focus()

        def ok():
            try: P.create_profile(self.home.get(), name.get().strip())
            except P.PipelineError as err: messagebox.showerror('New profile', str(err), parent=w); return
            P.save_settings(self.home.get(), name.get().strip(), self.current_settings())
            w.destroy(); self.refresh_install(); self.profile.set(name.get().strip()); self.load_profile()
        ttk.Button(w, text='Create', command=ok).pack(padx=8, pady=6); w.bind('<Return>', lambda e: ok())

    def delete_profile(self):
        n = self.profile.get()
        if not messagebox.askyesno('Delete profile', f'Delete profile "{n}" and its save data? This cannot be undone.'): return
        try: P.delete_profile(self.home.get(), n)
        except P.PipelineError as e: messagebox.showerror('Delete profile', str(e)); return
        self.refresh_install(); self.load_profile()

    def open_log_folder(self):
        """the profile folder: pwr.log (this or the last run), pwr.log.1 (the run before), save/, settings.json"""
        d = P.profile_dir(self.home.get() or P.default_home(), self.profile.get())
        if not os.path.isdir(d): messagebox.showinfo('Log folder', f'No log yet: {d} does not exist.'); return
        import subprocess
        try:
            if os.name == 'nt': os.startfile(d)
            else: subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', d])
        except OSError as e: messagebox.showerror('Log folder', f'{d}\n{e}')

    def launch(self):
        if self.game: return
        self.save_profile()
        try: self.game = P.launch(self.home.get(), self.profile.get(), wait=False, log=self.log)
        except P.PipelineError as e: messagebox.showerror('Launch', str(e)); return
        self.launch_btn.state(['disabled'])


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]); ap.add_argument('--home'); ap.add_argument('--rom')
    a = ap.parse_args()
    Launcher(a.home, a.rom).mainloop()


if __name__ == '__main__':
    main()
