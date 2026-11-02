"""Unpack an unencrypted 3DS title: the main NCCH -> exheader.bin, exefs/*, code.bin, romfs.bin (+ romfs/ files).

usage: python3 cia.py <game.cia|.3ds|.cci|.cxi> <out dir> [--runtime-only]

Containers: CIA (content 0), CCI/.3ds (NCSD partition 0) and a bare NCCH (.cxi). --runtime-only writes just what the
recompiled game reads (code.bin, exheader.bin, romfs.bin). Requires the NCCH to be NoCrypto (flags[7] & 4); encrypted
content is refused, not guessed at. ExeFS .code is BLZ-decompressed when the exheader says so and the footer is non-zero.
Importable: locate_ncch() / ncch_info() / extract() are used by tools/pipeline.py.
"""
import os,struct,sys

def align(x,a=64): return (x+a-1)&~(a-1)

def blz_decompress(d):
    enc,inc=struct.unpack('<II',d[-8:])
    if enc==0 and inc==0: return bytes(d)
    hdr=enc>>24; enc&=0xffffff
    out=bytearray(len(d)+inc); out[:len(d)]=d
    src=len(d)-hdr; dst=len(out); stop=len(d)-enc
    while src>stop:
        src-=1; fl=d[src]
        for _ in range(8):
            if src<=stop: break
            if fl&0x80:
                src-=2; v=d[src]|d[src+1]<<8; n=(v>>12)+3; off=(v&0xfff)+3
                for _ in range(n): dst-=1; out[dst]=out[dst+off]
            else: src-=1; dst-=1; out[dst]=d[src]
            fl<<=1
    return bytes(out)

def romfs_files(f,R):
    f.seek(R); iv=f.read(0x60)
    if iv[:4]!=b'IVFC': raise ValueError('RomFS: no IVFC header')
    mhs=struct.unpack_from('<I',iv,8)[0]; bs=1<<struct.unpack_from('<I',iv,0x4c)[0]
    l3=R+align(0x60+mhs,bs)
    f.seek(l3); h=struct.unpack('<10I',f.read(0x28))
    f.seek(l3+h[3]); dm=f.read(h[4]); f.seek(l3+h[7]); fm=f.read(h[8]); data=l3+h[9]
    out=[]
    def walk(off,path):
        _,_,cd,cf,_,nl=struct.unpack_from('<6I',dm,off)
        p=path+'/'+dm[off+24:off+24+nl].decode('utf-16le') if off else ''
        fo=cf
        while fo!=0xffffffff:
            _,sib,do,sz,_,nl2=struct.unpack_from('<IIQQII',fm,fo)
            out.append((p+'/'+fm[fo+32:fo+32+nl2].decode('utf-16le'),data+do,sz)); fo=sib
        c=cd
        while c!=0xffffffff: walk(c,p); c=struct.unpack_from('<I',dm,c+4)[0]
    walk(0,'')
    return out

def locate_ncch(f):
    """offset of the main NCCH in an open file, and the container name; ValueError when it is none of the known formats"""
    f.seek(0); d=f.read(0x200)
    if len(d)>=0x104 and d[0x100:0x104]==b'NCCH': return 0,'cxi'
    if len(d)>=0x200 and d[0x100:0x104]==b'NCSD':
        o=struct.unpack_from('<I',d,0x120)[0]*0x200   # partition 0 (executable content)
        return o,'cci'
    if len(d)>=0x20:
        hs,_,_,cs,ts,ms=struct.unpack_from('<IHHIII',d,0)
        if hs==0x2020:
            return align(hs)+align(cs)+align(ts)+align(ms),'cia'
    raise ValueError('not a CIA, CCI/.3ds or NCCH file')

def ncch_info(f,base):
    """header fields of the NCCH at base: program id, product code, encryption"""
    f.seek(base); h=f.read(0x200)
    if h[0x100:0x104]!=b'NCCH': raise ValueError('content 0 is not an NCCH')
    return dict(program_id=struct.unpack_from('<Q',h,0x118)[0], product=h[0x150:0x160].rstrip(b'\0').decode('ascii','replace'),
                nocrypto=bool(h[0x18f]&4), header=h)

def extract(src,out,runtime_only=False,log=print):
    f=open(src,'rb'); base,kind=locate_ncch(f); info=ncch_info(f,base); h=info['header']
    if not info['nocrypto']: raise ValueError('NCCH is encrypted (NoCrypto flag not set): decrypt your own dump first')
    mu=0x200; g=lambda o:struct.unpack_from('<I',h,o)[0]
    os.makedirs(out,exist_ok=True)
    f.seek(base+0x200); exh=f.read(0x800); open(os.path.join(out,'exheader.bin'),'wb').write(exh)
    E=base+g(0x1a0)*mu; f.seek(E); eh=f.read(0x200)
    if not runtime_only: os.makedirs(os.path.join(out,'exefs'),exist_ok=True)
    code=None
    for i in range(10):
        n=eh[i*16:i*16+8].rstrip(b'\0').decode(); o,s=struct.unpack_from('<II',eh,i*16+8)
        if not n: continue
        f.seek(E+0x200+o); d=f.read(s)
        if not runtime_only: open(os.path.join(out,'exefs',n.lstrip('.')+'.bin'),'wb').write(d)
        if n=='.code':
            code=blz_decompress(d) if exh[0xd]&1 else d
            open(os.path.join(out,'code.bin'),'wb').write(code)
            log(f'code.bin: {len(code):#x} bytes (text @ {struct.unpack_from("<I",exh,0x10)[0]:#x})')
    if code is None: raise ValueError('ExeFS has no .code section')
    R=base+g(0x1b0)*mu
    f.seek(R); iv=f.read(0x60)
    if iv[:4]!=b'IVFC': raise ValueError('RomFS: no IVFC header')
    l3=R+align(0x60+struct.unpack_from('<I',iv,8)[0],1<<struct.unpack_from('<I',iv,0x4c)[0])
    l3size=struct.unpack_from('<Q',iv,0x44)[0]
    with open(os.path.join(out,'romfs.bin'),'wb') as o:   # raw level-3 image, as fs reads it
        f.seek(l3); left=l3size
        while left: chunk=f.read(min(left,8<<20)); o.write(chunk); left-=len(chunk)
    log(f'romfs.bin: level 3 at {l3-R:#x}, {l3size:#x} bytes')
    if not runtime_only:
        files=romfs_files(f,R)
        for p,o,s in files:
            d=os.path.join(out,'romfs'+p); os.makedirs(os.path.dirname(d),exist_ok=True)
            f.seek(o); open(d,'wb').write(f.read(s))
        log(f'romfs: {len(files)} files')
    return info

def main():
    args=[a for a in sys.argv[1:] if not a.startswith('--')]
    extract(args[0],args[1],runtime_only='--runtime-only' in sys.argv)

if __name__=='__main__': main()
