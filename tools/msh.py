"""Parse Pilotwings Resort PMsh collision meshes (.msh) and export them to OBJ.

usage: python3 msh.py <file.msh> [out.obj]

Header: 3 x {u32 pointer, u32 count} for groups, vertices, faces. Pointers are addresses from the exporting
tool's memory: file offset = pointer - (groups pointer - 0x18).
Group (0x1C): f32 sphere center xyz, f32 radius (contains all the group's vertices), i32 first face, i32 end face
              (exclusive; groups tile the face list), i32 tag (0, -1, 1, 103..105, 1004, 1005: meaning unknown)
Vertex (12):  f32 x, y, z
Face (76):    u16 flags (6-bit values, meaning unknown), u16 v0, v1, v2,
              f32 normal xyz (= normalize(cross(v1-v0, v2-v0))), f32 d (= normal . v0), f32[13] unidentified
"""
import struct,sys

def parse(d):
    pa,na,pb,nb,pc,nc=struct.unpack_from('<6I',d); base=pa-0x18
    groups=[dict(center=g[:3],radius=g[3],first=g[4],end=g[5],tag=g[6]) for g in (struct.unpack_from('<4f3i',d,0x18+i*0x1c) for i in range(na))]
    verts=[struct.unpack_from('<3f',d,pb-base+i*12) for i in range(nb)]
    faces=[]
    for i in range(nc):
        o=pc-base+i*76; fl,a,b,c=struct.unpack_from('<4H',d,o); f=struct.unpack_from('<17f',d,o+8)
        faces.append(dict(flags=fl,v=(a,b,c),normal=f[:3],d=f[3],extra=f[4:]))
    return dict(groups=groups,verts=verts,faces=faces)

def write_obj(m,path):
    with open(path,'w') as o:
        for v in m['verts']: o.write('v %g %g %g\n'%v)
        for gi,g in enumerate(m['groups']):
            o.write(f"g group{gi}_tag{g['tag']}\n")
            for f in m['faces'][g['first']:g['end']]: o.write('f %d %d %d\n'%tuple(i+1 for i in f['v']))

if __name__=='__main__':
    m=parse(open(sys.argv[1],'rb').read())
    if len(sys.argv)>2: write_obj(m,sys.argv[2])
    print(f"{len(m['groups'])} groups, {len(m['verts'])} verts, {len(m['faces'])} faces",file=sys.stderr)
