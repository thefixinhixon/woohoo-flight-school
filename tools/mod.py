"""Export Pilotwings Resort MODL resources (.mod/.shd) to Wavefront OBJ.

usage: python3 mod.py <model.mod> <out.obj> [--tex-root <dir of extracted sets>] [--png-root <tex.py output dir>]

With --tex-root, writes <out>.mtl: a .tex material maps to that texture, a .mat material to its first layer's texture.
Names are matched case-insensitively, like the game. With --png-root, map_Kd is a relative path to the PNG.

Offsets are file-relative unless noted.
Header (0x48): u32 part_count, part_off, mat_count, mat_off, 0, name_off, ?, then floats (bounds), +0x44 u32 file size.
Part (16 bytes): u32 bone group (animation track node index; 0 = static), 0, material record off, ctr0 chunk off.
"TJRM" block (optional): u32 size, u32 count, count x {char name[32], f32 frame[12], i32 parent} named pivots/attach points.
Material record: u32 1, ?, ?, 0, name off (".mat" or ".tex"), 0.
ctr0 chunk (offsets relative to chunk start C):
  +0x00 "0rtc", +0x04 chunk size
  +0x20 u32 attribute mask, +0x24 descriptors {u8 attr, u8 ncomp, u16 GL type, u32 stride, u32 offset} until a non-GL type
  +0x64 u32 vertex data size, +0x6C u32 vertex data off
  +0x70 u32 index count, u16 primitive (4 = triangles), u16 index type (0x1403 = u16), u32 index size, u32 0,
  +0x80 u32 index off, +0x84 u32 PICA command list off (starts 0xDEADBEEF, ends with the vertex shader name)
Attributes (names from code.bin): 0 iPosition, 1 iColor, 2 iNormal, 3 iTex0, 5 iQuat, 6 iBone.
"""
import os,struct,sys

GL={0x1400:('b',1),0x1401:('B',1),0x1402:('h',2),0x1403:('H',2),0x1406:('f',4)}
ATTR={0:'position',1:'color',2:'normal',3:'texcoord',5:'quat',6:'bone'}

def cstr(d,o): return d[o:d.index(b'\0',o)].decode('latin1')

def parse(d):
    npart,poff,nmat,moff=struct.unpack_from('<4I',d,0)
    parts=[]
    for i in range(npart):
        group,_,mat,C=struct.unpack_from('<4I',d,poff+i*16)
        if d[C:C+4]!=b'0rtc': raise ValueError(f'part {i}: no ctr0 chunk at {C:#x}')
        descs=[];o=C+0x24
        while True:
            a,nc,ty,stride,off=struct.unpack_from('<BBHII',d,o)
            if ty not in GL: break
            descs.append((a,nc,ty,stride,off)); o+=12
        vsz=struct.unpack_from('<I',d,C+0x64)[0]; voff=struct.unpack_from('<I',d,C+0x6c)[0]
        icnt,prim,ity,_,_,ioff=struct.unpack_from('<IHHIII',d,C+0x70)
        stride=descs[0][3]; nv=vsz//stride
        verts={}
        for a,nc,ty,_,off in descs:
            f,sz=GL[ty]
            verts[ATTR.get(a,a)]=[struct.unpack_from(f'<{nc}{f}',d,C+voff+v*stride+off) for v in range(nv)]
        f,sz=GL[ity]
        idx=struct.unpack_from(f'<{icnt}{f}',d,C+ioff)
        parts.append(dict(group=group,material=cstr(d,struct.unpack_from('<I',d,mat+16)[0]),prim=prim,verts=verts,indices=idx))
    return parts

def nodes(d):
    """Named nodes from the TJRM block: [(name, frame[12], parent)]."""
    t=d.find(b'TJRM')
    if t<0: return []
    cnt=struct.unpack_from('<I',d,t+8)[0]
    return [(d[t+12+j*84:t+44+j*84].split(b'\0')[0].decode('latin1'),struct.unpack_from('<12f',d,t+44+j*84),struct.unpack_from('<i',d,t+92+j*84)[0]) for j in range(cnt)]

_index={}
def find(root,name):
    """Case-insensitive lookup of a resource file name anywhere under root (first set in sorted order wins)."""
    if root not in _index:
        idx={}
        for r,_,fs in sorted(os.walk(root)):
            for f in fs: idx.setdefault(f.lower(),os.path.join(r,f))
        _index[root]=idx
    return _index[root].get(name.lower())

def mat_texture(root,mat):
    if not mat: return None
    if mat.lower().endswith('.tex'): return mat
    p=find(root,mat)
    if not p: return None
    sys.path.insert(0,os.path.dirname(os.path.abspath(__file__))); import zab
    z=zab.Zab(open(p,'rb').read())
    for t in z.tables.values():
        if t['name']=='layer':
            for i in range(t['count']):
                k,v=struct.unpack_from('<II',z.nod,t['nodes']+i*8)
                if k>>4==zab.h37(b'texture')&0x0fffffff and k&15==4: return zab.cstr(z.str,v)

def write_mtl(parts,path,root,png_root=None):
    with open(path,'w') as o:
        for m in sorted({p['material'] for p in parts}):
            t=mat_texture(root,m); src=find(root,t) if t else None
            if src and png_root:
                t=os.path.relpath(os.path.join(png_root,os.path.relpath(src,root))+'.png',os.path.dirname(os.path.abspath(path)))
            elif t: t+='.png'
            o.write(f'newmtl {m}\nKd 1 1 1\n'+(f'map_Kd {t}\n' if t else '')+'\n')

def write_obj(parts,path,mtl=None):
    with open(path,'w') as o:
        if mtl: o.write(f'mtllib {os.path.basename(mtl)}\n')
        base=1
        for i,p in enumerate(parts):
            v=p['verts']; n=len(v['position'])
            o.write(f"o part{i}\nusemtl {p['material']}\n")
            for x in v['position']: o.write('v %g %g %g\n'%x[:3])
            # every part writes one vt and one vn per vertex so v/vt/vn indices stay aligned across parts
            # game texcoords address rows top-down (v=0 at the top of the PNGs tex.py writes); OBJ wants v=0 at the bottom
            for t in v.get('texcoord',[(0,1)]*n): o.write('vt %g %g\n'%(t[0],1-t[1]))
            for t in v.get('normal',[(0,0,1)]*n): o.write('vn %g %g %g\n'%t[:3])
            ix=p['indices']
            for k in range(0,len(ix)-2,3):
                o.write('f '+' '.join(f'{j+base}/{j+base}/{j+base}' for j in ix[k:k+3])+'\n')
            base+=n

if __name__=='__main__':
    d=open(sys.argv[1],'rb').read(); parts=parse(d); mtl=None
    if '--tex-root' in sys.argv:
        mtl=os.path.splitext(sys.argv[2])[0]+'.mtl'
        png=os.path.abspath(sys.argv[sys.argv.index('--png-root')+1]) if '--png-root' in sys.argv else None
        write_mtl(parts,mtl,os.path.abspath(sys.argv[sys.argv.index('--tex-root')+1]),png)
    write_obj(parts,sys.argv[2],mtl)
    print(f"{len(parts)} parts, {sum(len(p['verts']['position']) for p in parts)} verts, {sum(len(p['indices'])//3 for p in parts)} tris",file=sys.stderr)
