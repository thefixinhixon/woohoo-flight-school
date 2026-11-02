"""PMCr (Pilotwings Resort RST0 compression) = 16-byte header + QuickLZ 1.5.0 level-3 stream."""
import struct,sys
_T=[]
for i in range(256):
    c=i<<24
    for _ in range(8): c=((c<<1)^0x04C11DB7 if c&0x80000000 else c<<1)&0xffffffff
    _T.append(c)
def mgi_crc(b):
    """CRC at VA 0x1c4564: MSB-first poly 0x04C11DB7, register seeded with ~BE32(first 4 bytes), bytes shifted in, result inverted."""
    if len(b)<4: r=0x690CE0EE; rest=b
    else: r=~int.from_bytes(b[:4],'big')&0xffffffff; rest=b[4:]
    for x in rest: r=(((r<<8)&0xffffffff)|x)^_T[r>>24]
    return ~r&0xffffffff
def qlz_decompress(s):
    flags=s[0]; hl=9 if flags&2 else 3
    if hl==9: csize,dsize=struct.unpack_from('<II',s,1)
    else: csize,dsize=s[1],s[2]
    if not flags&1: return bytes(s[hl:hl+dsize])
    level=(flags>>2)&3
    assert level==3,f'level {level} not implemented'
    out=bytearray(dsize); src=hl; dst=0; cw=1; last=dsize-1
    while True:
        if cw==1:
            cw=struct.unpack_from('<I',s,src)[0]; src+=4
        if dst>=last-10:
            while dst<=last:
                if cw==1: src+=4; cw=1<<31
                out[dst]=s[src]; dst+=1; src+=1; cw>>=1
            return bytes(out)
        f=int.from_bytes(s[src:src+4].ljust(4,b'\0'),'little')
        if cw&1:
            cw>>=1
            if f&3==0: off=(f&0xff)>>2; n=3; src+=1
            elif f&2==0: off=(f&0xffff)>>2; n=3; src+=2
            elif f&1==0: off=(f&0xffff)>>6; n=((f>>2)&15)+3; src+=2
            elif f&127!=3: off=(f>>7)&0x1ffff; n=((f>>2)&0x1f)+2; src+=3
            else: off=f>>15; n=((f>>7)&255)+3; src+=4
            p=dst-off
            for i in range(n): out[dst+i]=out[p+i]
            dst+=n
        else:
            out[dst]=s[src]; dst+=1; src+=1; cw>>=1
def pmcr_decompress(b):
    magic,crc,csize,dsize=struct.unpack_from('<4sIII',b,0)
    assert magic==b'PMCr',magic
    payload=b[16:16+csize]
    if mgi_crc(payload)!=crc: raise ValueError('PMCr: bad crc')
    out=qlz_decompress(payload)
    if len(out)!=dsize: raise ValueError(f'PMCr: expected {dsize}b, got {len(out)}b')
    return out
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    out=pmcr_decompress(d[0x80:])
    print(sys.argv[1],'->',len(out),'bytes, crc ok')
    if len(sys.argv)>2: open(sys.argv[2],'wb').write(out)
