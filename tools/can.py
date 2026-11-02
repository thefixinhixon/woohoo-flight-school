"""Parse and evaluate Pilotwings Resort ChAn animations (.can); export to JSON.

usage: python3 can.py <file.can> [out.json]

Header (0x18): u32 track_count, f32 max clip length, u32 node_count (highest node index used), u32 track_off (0x18),
               u32 clip_count, u32 clip_off
Track (100 bytes): char name[32], i32 node index, i32 parent (index into this track list, -1 = root),
                   f32 rest[12] (model-space bind frame: 3x3 basis stored column-major, then position),
                   u32 key_count, u32 key_off, u32 key_end (= key_off + 36*key_count)
Clip (36 bytes):   char name[32], f32 length
Key (36 bytes):    f32 quat x,y,z,w, f32 pos x,y,z (local to the parent track), f32 scale (1.0), f32 time
Each track's keys are its clips' keys concatenated in clip order; time restarts at 0 for each clip and each clip's
segment ends exactly at the clip length.

Binding (rigid skinning): a model part's first u32 is its bone group = a track's node index. For the plane and
glider rigs, <set>_.car/lod1_skel.val maps bone group <-> model node name <-> track name.
Pose: world(track) = world(parent) * local(key); part matrix = world(track) * inverse(rest(track)).
"""
import json,math,struct,sys

def load(d):
    nt,_,nn,to,nc,co=struct.unpack_from('<IfIIII',d)
    clips=[dict(name=d[co+j*36:co+j*36+32].split(b'\0')[0].decode('latin1'),length=struct.unpack_from('<f',d,co+j*36+32)[0]) for j in range(nc)]
    tracks=[]
    for i in range(nt):
        o=to+i*100
        idx,par=struct.unpack_from('<ii',d,o+32); kc,ko,_=struct.unpack_from('<III',d,o+88)
        segs=[[]]
        for k in range(kc):
            key=struct.unpack_from('<9f',d,ko+k*36)
            if segs[-1] and key[8]<segs[-1][-1][8]: segs.append([])
            segs[-1].append(key)
        tracks.append(dict(name=d[o:o+32].split(b'\0')[0].decode('latin1'),node=idx,parent=par,
                           rest=list(struct.unpack_from('<12f',d,o+40)),clips=segs))
    return dict(node_count=nn,clips=clips,tracks=tracks)

def _mul(a,b): return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]

def _qm(x,y,z,w,p):
    return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),p[0]],[2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),p[1]],
            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),p[2]],[0,0,0,1]]

def rest_matrix(r):
    """Rest frame as a 4x4 row-major matrix (basis stored column-major in the file)."""
    return [[r[0],r[3],r[6],r[9]],[r[1],r[4],r[7],r[10]],[r[2],r[5],r[8],r[11]],[0,0,0,1]]

def sample(seg,t):
    """Interpolated (quat, pos) at time t: nlerp rotation, lerp position, clamped to the segment."""
    if t<=seg[0][8]: k=seg[0]; return k[:4],k[4:7]
    for a,b in zip(seg,seg[1:]):
        if a[8]<=t<=b[8]:
            u=(t-a[8])/max(b[8]-a[8],1e-9); q1=b[:4]
            if sum(x*y for x,y in zip(a[:4],q1))<0: q1=[-x for x in q1]
            q=[x*(1-u)+y*u for x,y in zip(a[:4],q1)]; n=math.sqrt(sum(x*x for x in q)); q=[x/n for x in q]
            return q,[x*(1-u)+y*u for x,y in zip(a[4:7],b[4:7])]
    k=seg[-1]; return k[:4],k[4:7]

def world_matrices(anim,clip,t):
    """Model-space matrix per track for clip index `clip` at time t."""
    out={}
    def w(i):
        if i not in out:
            tr=anim['tracks'][i]; q,p=sample(tr['clips'][clip],t); m=_qm(*q,p)
            out[i]=_mul(w(tr['parent']),m) if tr['parent']>=0 else m
        return out[i]
    for i in range(len(anim['tracks'])): w(i)
    return out

if __name__=='__main__':
    a=load(open(sys.argv[1],'rb').read())
    if len(sys.argv)>2:
        json.dump(a,open(sys.argv[2],'w'),indent=1)
    else:
        print('clips:',', '.join(f"{c['name']} ({c['length']:.2f}s)" for c in a['clips']))
        for t in a['tracks']: print(f"  {t['name']:24} node {t['node']:3} parent {t['parent']:3} keys/clip {[len(s) for s in t['clips']]}")
