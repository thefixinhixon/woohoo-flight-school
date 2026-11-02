"""Pilotwings Resort UI resources: string tables (.lng), font glyph tables (.gtx), sprite rect tables (.tab).

usage: python3 ui.py lng <lang.lng> <font.gtx> [out.json]   decode strings to Unicode (JSON {key: text})
       python3 ui.py gtx <font.gtx>                         list glyphs
       python3 ui.py tab <file.tab>                         list rects

LANG (.lng): u32 count, count x {u32 hash, u32 key off, u32 value off} sorted by hash, then NUL-terminated strings.
  hash = h37(key) & 0x7FFFFFFF (same h*37 + (c|0x20) hash as !ZAB keys). Values start with 0x01; '|' is a line break.
  Text is UTF-8 over *glyph indices*, not Unicode: MGI8ToUTF8 (VA 0x28523c) decodes each code point c and
  replaces it with glyph[c].code from the font, which is the real Unicode character.
GTEX (.gtx): 0x28-byte header (+0x18 f32 99.0?, +0x20 u32 glyph count, +0x24 u32 glyph off), then 0x2C-byte glyphs:
  u32 unicode, f32 metrics[5] (unidentified), f32 u0, v0, u1, v1, f32 page (texture <name>.<page>.tex).
TRct (.tab): u32 0, u32 mode (0 = normalized UVs, 1 = pixels), f32 sx, sy, u32 tex w, tex h (0 in mode 0), u32 count,
  then count x 6 floats. Mode 0: w, h, u0, v0, u1, v1. Mode 1: ?, ?, x0, y0, x1, y1 in texels (inside the texture
  in 77 of 81 files; the first two are usually 0).
"""
import json,struct,sys,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from zab import h37

def gtx(d):
    n,off=struct.unpack_from('<II',d,0x20)
    return [dict(code=g[0],metrics=g[1:6],uv=g[6:10],page=int(g[10])) for g in (struct.unpack_from('<I10f',d,off+i*0x2c) for i in range(n))]

def _utf8_codepoints(b):
    i=0
    while i<len(b):
        c=b[i]
        if c&0xf0==0xe0: yield (c&15)<<12|(b[i+1]&63)<<6|(b[i+2]&63); i+=3
        elif c&0xe0==0xc0: yield (c&31)<<6|(b[i+1]&63); i+=2
        else: yield c&0x7f; i+=1

def decode_text(b,glyphs):
    """Mirror of MGI8ToUTF8: code points above 0x20 are glyph indices (out of range -> '?')."""
    out=[]
    for c in _utf8_codepoints(b):
        if c>32: c=glyphs[c]['code'] if c<len(glyphs) else 63
        out.append(chr(c))
    return ''.join(out)

def lng(d,glyphs):
    n=struct.unpack_from('<I',d)[0]; out={}
    cs=lambda o:d[o:d.index(b'\0',o)]
    for i in range(n):
        h,ko,vo=struct.unpack_from('<III',d,4+i*12); k=cs(ko)
        if h37(k)&0x7fffffff!=h: raise ValueError(f'hash mismatch for {k!r}')
        v=cs(vo)
        out[k.decode('latin1')]=decode_text(v[1:] if v[:1]==b'\x01' else v,glyphs)
    return out

def tab(d):
    _,mode,sx,sy,tw,th,n=struct.unpack_from('<IIffIII',d)
    return dict(mode=mode,scale=(sx,sy),tex_size=(tw,th),rects=[struct.unpack_from('<6f',d,0x1c+i*24) for i in range(n)])

if __name__=='__main__':
    cmd=sys.argv[1]; rd=lambda p:open(p,'rb').read()
    if cmd=='lng':
        s=lng(rd(sys.argv[2]),gtx(rd(sys.argv[3])))
        if len(sys.argv)>4: json.dump(s,open(sys.argv[4],'w',encoding='utf-8'),ensure_ascii=False,indent=1)
        else:
            for k,v in s.items(): print(f'{k}\t{v}')
    elif cmd=='gtx':
        for i,g in enumerate(gtx(rd(sys.argv[2]))): print(i,hex(g['code']),repr(chr(g['code'])),g['page'],[round(x,4) for x in g['uv']],[round(x,2) for x in g['metrics']])
    elif cmd=='tab':
        t=tab(rd(sys.argv[2])); print('mode',t['mode'],'scale',t['scale'],'tex',t['tex_size'])
        for r in t['rects']: print(' ',[round(x,4) for x in r])
