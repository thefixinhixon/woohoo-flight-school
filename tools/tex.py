"""Decode Pilotwings Resort TEX resources (.tex/.art/.txm) to PNG.

usage: python3 tex.py <file|dir> <out dir>

Header (0x80 bytes, pixel data follows):
  +0x00 u16 width, u16 height
  +0x04 u8 mip count, u8 format (see FORMATS), u8 ?, u8 ?
  +0x08..0x0B, +0x18 = 0x18, +0x1C = 0x68: unknown (constant or near-constant)
  +0x24 u32 data size (all mips), +0x58..0x7F 0xE3 padding
Pixel data is in PICA200 layout: 8x8 tiles in Morton order, rows stored bottom-up.
"""
import os,struct,sys
from PIL import Image

# format code -> (name, bits per pixel). Engine enum, not PICA order: 0x23/0x24 are RGBA4/RGB565 (PICA has them swapped)
# and HILO8 is absent. Verified visually per format; 0x26/0x27 and 0x2b/0x2c pairings are the least certain.
FORMATS={0x20:('RGBA8',32),0x21:('RGB8',24),0x22:('RGBA5551',16),0x23:('RGBA4',16),0x24:('RGB565',16),
         0x25:('IA8',16),0x26:('I8',8),0x27:('A8',8),0x28:('ETC1',4),0x29:('ETC1A4',8),
         0x2a:('IA4',8),0x2b:('I4',4),0x2c:('A4',4)}

MORTON=[0]*64
for i in range(64):
    x=(i&1)|(i>>1&2)|(i>>2&4); y=(i>>1&1)|(i>>2&2)|(i>>3&4); MORTON[i]=(x,y)

def x5(v): return (v<<3)|(v>>2)
def x6(v): return (v<<2)|(v>>4)
def x4(v): return v*17

def texel(fmt,d,i):
    """Return RGBA tuple of texel i for the uncompressed formats."""
    if fmt=='RGBA8': a,b,g,r=d[i*4:i*4+4]; return r,g,b,a
    if fmt=='RGB8': b,g,r=d[i*3:i*3+3]; return r,g,b,255
    if fmt in('RGBA5551','RGB565','RGBA4','IA8'):
        v=d[i*2]|d[i*2+1]<<8
        if fmt=='RGBA5551': return x5(v>>11),x5(v>>6&31),x5(v>>1&31),255 if v&1 else 0
        if fmt=='RGB565': return x5(v>>11),x6(v>>5&63),x5(v&31),255
        if fmt=='RGBA4': return x4(v>>12),x4(v>>8&15),x4(v>>4&15),x4(v&15)
        return v>>8,v>>8,v>>8,v&0xff
    if fmt=='I8': return d[i],d[i],d[i],255
    if fmt=='A8': return 255,255,255,d[i]
    if fmt=='IA4': v=d[i]; return x4(v>>4),x4(v>>4),x4(v>>4),x4(v&15)
    n=d[i>>1]>>(4*(i&1))&15
    if fmt=='I4': return x4(n),x4(n),x4(n),255
    if fmt=='A4': return 255,255,255,x4(n)
    raise ValueError(fmt)

ETC_MOD=[(2,8),(5,17),(9,29),(13,42),(18,60),(24,80),(33,106),(47,183)]

def etc1_block(blk,alpha):
    """Decode one 4x4 ETC1 block (u64 little-endian) into 16 RGBA values indexed [x*4+y]... returned as dict (x,y)->rgba."""
    v=int.from_bytes(blk,'little'); hi=v>>32; lo=v&0xffffffff
    flip=hi&1; diff=hi>>1&1
    if diff:
        def c5(sh): return hi>>sh&31
        def d3(sh): t=hi>>sh&7; return t-8 if t>3 else t
        r1,g1,b1=c5(27),c5(19),c5(11); r2,g2,b2=r1+d3(24),g1+d3(16),b1+d3(8)
        base=[(x5(r1),x5(g1),x5(b1)),(x5(r2&31),x5(g2&31),x5(b2&31))]
    else:
        base=[(x4(hi>>28&15),x4(hi>>20&15),x4(hi>>12&15)),(x4(hi>>24&15),x4(hi>>16&15),x4(hi>>8&15))]
    tables=[hi>>5&7,hi>>2&7]
    out={}
    for x in range(4):
        for y in range(4):
            k=x*4+y; sub=(y>=2) if flip else (x>=2)
            idx=(lo>>k&1)|((lo>>(k+16)&1)<<1)
            m=ETC_MOD[tables[sub]]; delta=[m[0],m[1],-m[0],-m[1]][idx]
            r,g,b=(max(0,min(255,c+delta)) for c in base[sub])
            a=255 if alpha is None else x4(alpha>>(k*4)&15)
            out[x,y]=(r,g,b,a)
    return out

def decode(data,w,h,fmt):
    img=Image.new('RGBA',(w,h)); px=img.load()
    if fmt in('ETC1','ETC1A4'):
        bs=16 if fmt=='ETC1A4' else 8; pos=0
        for ty in range(0,h,8):
            for tx in range(0,w,8):
                for by,bx in ((0,0),(0,4),(4,0),(4,4)):
                    alpha=int.from_bytes(data[pos:pos+8],'little') if bs==16 else None
                    blk=etc1_block(data[pos+bs-8:pos+bs],alpha); pos+=bs
                    for (x,y),c in blk.items():
                        X,Y=tx+bx+x,ty+by+y
                        if X<w and Y<h: px[X,Y]=c
    else:
        i=0
        for ty in range(0,h,8):
            for tx in range(0,w,8):
                for x,y in MORTON:
                    if tx+x<w and ty+y<h: px[tx+x,ty+y]=texel(fmt,data,i)
                    i+=1
    return img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)

def load(path):
    d=open(path,'rb').read()
    w,h=struct.unpack_from('<HH',d); mips,code=d[4],d[5]
    if code not in FORMATS: raise ValueError(f'{path}: unknown format {code:#x}')
    fmt,bpp=FORMATS[code]
    return decode(d[0x80:0x80+w*h*bpp//8],w,h,fmt),fmt,mips

def main():
    src,out=sys.argv[1],sys.argv[2]
    files=[src] if os.path.isfile(src) else [os.path.join(r,n) for r,_,fs in os.walk(src) for n in fs if n.rsplit('.',1)[-1] in('tex','art','txm')]
    for p in sorted(files):
        img,fmt,_=load(p)
        rel=os.path.relpath(p,src) if os.path.isdir(src) else os.path.basename(p)
        o=os.path.join(out,rel+'.png'); os.makedirs(os.path.dirname(o),exist_ok=True); img.save(o)
    print(f'{len(files)} textures',file=sys.stderr)

if __name__=='__main__': main()
