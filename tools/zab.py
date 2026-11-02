"""Dump Pilotwings Resort "!ZAB" value trees (.val/.mat/.va1) as indented text.

usage: python3 zab.py <file|dir> [--code code.bin] [--names wordlist.txt,...]

File = sequence of chunks: "!ZAB", u32 word count, payload (count*4 bytes). Offsets below are relative to
the chunk's count word. Chunk order: 0 tables, 1 nodes, 2 data (vectors/frames), 3 strings, [4 source names].
  table (24 bytes): u32 0x0042787C, u32 name (str off), u32 0, u32 parent (table off, 0 = none), u32 nodes (off), u32 count
  node (8 bytes):   u32 (hash<<4)|type, u32 value
  key hash (code VA 0x1d6b14): h = h*37 + (c|0x20) over the name's bytes, i.e. case-insensitive.
  types (table at VA 0x1d6b00): 0 REAL 1 INT 2 BOOL 3 COLOR 4 STRING 5 P3D 6 P2D 7 FRAME 8 TABLE
"""
import os,re,struct,sys

TYPES={0:'REAL',1:'INT',2:'BOOL',3:'COLOR',4:'STRING',5:'P3D',6:'P2D',7:'FRAME',8:'TABLE'}

def h37(s):
    h=0
    for c in s:
        h=(h*37+((c-256 if c>127 else c)|0x20))&0xffffffff
    return h

def chunks(d):
    out=[];p=0
    while p+8<=len(d):
        if d[p:p+4]!=b'!ZAB': raise ValueError(f'bad chunk magic at {p:#x}')
        n=struct.unpack_from('<I',d,p+4)[0]
        out.append(d[p+4:p+8+n*4]); p+=8+n*4
    return out

def cstr(b,o): return b[o:b.index(b'\0',o)].decode('latin1')

class Zab:
    def __init__(s,data,names=None):
        c=chunks(data); s.tab,s.nod=c[0],c[1]
        s.dat=c[2] if len(c)>2 else b''; s.str=c[3] if len(c)>3 else b''
        s.src=[x for x in c[4][4:].split(b'\0') if x] if len(c)>4 else []
        s.names=names or {}
        s.tables={}
        for o in range(4,len(s.tab)-23,24):
            vt,nm,_,par,no,cnt=struct.unpack_from('<6I',s.tab,o)
            s.tables[o]=dict(name=cstr(s.str,nm) if s.str else nm,parent=par,nodes=no,count=cnt)
    def key(s,k): return s.names.get(k>>4,f'#{k>>4:07x}')
    def value(s,t,v):
        if t==0: return repr(round(struct.unpack('<f',struct.pack('<I',v))[0],6))
        if t==1: return str(v-(1<<32) if v>>31 else v)
        if t==2: return 'true' if v else 'false'
        if t==3: return f'rgba({v&255},{v>>8&255},{v>>16&255},{v>>24})'
        if t==4: return repr(cstr(s.str,v))
        if t in(5,6): n=3 if t==5 else 2; return '('+', '.join(repr(round(x,6)) for x in struct.unpack_from(f'<{n}f',s.dat,v))+')'
        if t==7: f=struct.unpack_from('<12f',s.dat,v); return 'frame(basis='+str([round(x,4) for x in f[:9]])+', pos='+str([round(x,3) for x in f[9:]])+')'
        if t==8: return f'table@{v:#x}'
        return f'?{t}:{v:#x}'
    def children(s,o):
        t=s.tables[o]
        for i in range(t['count']):
            k,v=struct.unpack_from('<II',s.nod,t['nodes']+i*8)
            yield k,k&15,v
    def dump(s,o=None,ind=0,out=None,seen=None):
        out=[] if out is None else out; seen=set() if seen is None else seen
        if o is None:
            refd={v for t in s.tables for k,ty,v in s.children(t) if ty==8}
            for r in s.tables:
                if r not in refd: out.append(f"[{s.tables[r]['name']}]"); s.dump(r,1,out,seen)
            return '\n'.join(out)
        seen.add(o)
        for k,t,v in s.children(o):
            if t==8 and v in s.tables and v not in seen:
                out.append('  '*ind+f'{s.key(k)}: {{  # {s.tables[v]["name"]}'); s.dump(v,ind+1,out,seen); out.append('  '*ind+'}')
            else: out.append('  '*ind+f'{s.key(k)} {TYPES.get(t,t)} = {s.value(t,v)}')
        return out

IDENT=re.compile(rb'[A-Za-z_][A-Za-z0-9_]*')

def load_names(code=None,zab_files=(),extra=()):
    """Name dictionary (28-bit hash -> name). Sources: NUL-terminated strings in code.bin and their identifier
    tokens, string chunks of ZAB files, extra word lists; plus single letters and word0..word15 variants.
    Only clean sources are used: the hash is 28 bits, so noisy word lists produce false names."""
    words=set(bytes([c]) for c in b'abcdefghijklmnopqrstuvwxyz')
    blobs=[]
    if code:
        d=open(code,'rb').read()
        blobs+=[m.group() for m in re.finditer(rb'[\x20-\x7e]{2,}(?=\0)',d)]
    for p in zab_files:
        try: c=chunks(open(p,'rb').read())
        except Exception: continue
        if len(c)>3: blobs+=c[3][4:].split(b'\0')
    for p in extra: blobs+=open(p,'rb').read().split()
    for b in blobs:
        if IDENT.fullmatch(b): words.add(b)
        words.update(IDENT.findall(b))
    words|={w+str(i).encode() for w in list(words) if len(w)>1 for i in range(16)}
    names={}
    for w in sorted(words,key=lambda w:(len(w),w)): names.setdefault(h37(w)&0x0fffffff,w.decode())
    return names

if __name__=='__main__':
    a=sys.argv[1:]; code=None; extra=[]
    if '--code' in a: i=a.index('--code'); code=a[i+1]; del a[i:i+2]
    if '--names' in a: i=a.index('--names'); extra=a[i+1].split(','); del a[i:i+2]
    src=a[0]
    files=[src] if os.path.isfile(src) else sorted(os.path.join(r,n) for r,_,fs in os.walk(src) for n in fs if n.endswith(('.val','.mat','.va1')))
    names=load_names(code,files if os.path.isdir(src) else [src],extra)
    for p in files:
        print(f'### {p}'); print(Zab(open(p,'rb').read(),names).dump())
