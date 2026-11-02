"""Decode Pilotwings Resort SFX0 resources (.sfx) to WAV.

usage: python3 sfx.py <file|dir> <out dir>

Header (0x80 bytes), mono Nintendo DSP-ADPCM (8-byte frames: 1 predictor/scale byte + 14 4-bit samples):
  +0x00 u32 data size (file = 0x80 + data)    +0x04 u32 header size (0x80)
  +0x08 u8 loop flag, u8 ?(1), +0x0C u32 0x10000 ?       +0x10 u32 sample rate   +0x14 u32 2*rate ?
  +0x20 u32 sample count                       +0x2C u32 loop flag
  +0x30 u32 loop start, +0x34 loop end (nibble addresses; 2 nibbles of frame header per 16)
  +0x38 u32 current address (0)
  +0x3C s16 coef[16], u16 gain, u16 predictor/scale, s16 hist1, s16 hist2,
        u16 loop predictor/scale, s16 loop hist1, s16 loop hist2
Looping sounds get a RIFF 'smpl' chunk with the loop points in samples.
"""
import os,struct,sys,wave

def parse(d):
    size,hs=struct.unpack_from('<II',d,0)
    rate=struct.unpack_from('<I',d,0x10)[0]; ns=struct.unpack_from('<I',d,0x20)[0]
    loop,ls,le=struct.unpack_from('<III',d,0x2c)
    coef=struct.unpack_from('<16h',d,0x3c)
    gain,ps,h1,h2=struct.unpack_from('<HHhh',d,0x5c)
    return dict(size=size,header=hs,rate=rate,samples=ns,loop=loop,loop_start=ls,loop_end=le,coef=coef,ps=ps,hist=(h1,h2))

def nibble_to_sample(n): return n//16*14+max(0,n%16-2)

def decode(d,h):
    data=d[h['header']:h['header']+h['size']]; c=h['coef']; h1,h2=h['hist']; out=[]
    for f in range(0,len(data),8):
        ps=data[f]; scale=1<<(ps&15); ci=ps>>4
        c1,c2=c[ci*2],c[ci*2+1]
        for i in range(14):
            if len(out)>=h['samples']: return out
            b=data[f+1+i//2]; n=(b>>4) if i%2==0 else (b&15)
            if n>=8: n-=16
            s=((n*scale)<<11)+1024+c1*h1+c2*h2
            s=max(-32768,min(32767,s>>11))
            out.append(s); h2,h1=h1,s
    return out

def add_smpl(path,rate,start,end):
    """Append a RIFF 'smpl' chunk with one forward loop [start,end] (inclusive, in samples)."""
    body=struct.pack('<9I',0,0,1000000000//rate,60,0,0,0,1,0)+struct.pack('<6I',0,0,start,end,0,0)
    with open(path,'r+b') as f:
        f.seek(0,2); f.write(b'smpl'+struct.pack('<I',len(body))+body)
        n=f.tell(); f.seek(4); f.write(struct.pack('<I',n-8))

def main():
    src,out=sys.argv[1],sys.argv[2]
    files=[src] if os.path.isfile(src) else sorted(os.path.join(r,n) for r,_,fs in os.walk(src) for n in fs if n.endswith('.sfx'))
    for p in files:
        d=open(p,'rb').read(); h=parse(d); pcm=decode(d,h)
        rel=os.path.relpath(p,src) if os.path.isdir(src) else os.path.basename(p)
        o=os.path.join(out,rel+'.wav'); os.makedirs(os.path.dirname(o),exist_ok=True)
        with wave.open(o,'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(h['rate']); w.writeframes(struct.pack(f'<{len(pcm)}h',*pcm))
        if h['loop']: add_smpl(o,h['rate'],nibble_to_sample(h['loop_start']),nibble_to_sample(h['loop_end']))
    print(f'{len(files)} sounds',file=sys.stderr)

if __name__=='__main__': main()
