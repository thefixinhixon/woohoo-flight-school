"""Extract Pilotwings Resort RST0 resource sets (.car/.res/.trk) using the external TOC in tocres.res.

usage: python3 rst.py <romfs dir> <out dir> [--list]

RST0 file: 0x80-byte header ("0TSR"), payload at +0x80.
  +0x0C flags (bit1 = payload is a PMCr chunk), +0x10 size on disk incl. header, +0x14 create_time,
  +0x20 res_count, +0x24 explode_size (unpacked payload), +0x28 disk_size (stored payload), +0x34 names_size, +0x3C crc
Payload = raw[0x80:], or its PMCr decompression; it is explode_size bytes and TOC data offsets index into it.
TOC (one per set, stored in tocres.res as "<set>.toc"):
  header 0x20: u32 version(3), header_size(0x20), create_time, res_count, explode_size, disk_size, 0, names_size
  res_count entries of 0x28: u32 name_off, char[4] fourcc (byte-reversed), u32 flags, u32 size, u32 offset, u32 crc (mgi_crc of the data), 16 zero bytes
  then a NUL-terminated name table; name_off is relative to its start.
tocres.res itself is the same entry array (fourcc "ETOC") at offset 0 of its exploded payload, without the 0x20 header.
"""
import os,struct,sys
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from pmcr import pmcr_decompress,mgi_crc

ENTRY=0x28

def cstr(b,o): return b[o:b.index(b'\0',o)].decode('latin1')

def entries(buf,base,count):
    names=base+count*ENTRY
    for i in range(count):
        no,fcc,fl,sz,off,h=struct.unpack_from('<I4sIIII',buf,base+i*ENTRY)
        yield dict(name=cstr(buf,names+no),type=fcc[::-1].decode('latin1'),flags=fl,size=sz,offset=off,hash=h)

def explode(raw):
    if raw[:4]!=b'0TSR': raise ValueError('not an RST0 file')
    if struct.unpack_from('<I',raw,0xc)[0]&2: return pmcr_decompress(raw[0x80:])
    return raw[0x80:]

def load_tocs(romfs):
    t=explode(open(os.path.join(romfs,'tocres.res'),'rb').read())
    count=struct.unpack_from('<I',open(os.path.join(romfs,'tocres.res'),'rb').read(0x24),0x20)[0]
    tocs={}
    for e in entries(t,0,count):
        blob=t[e['offset']:e['offset']+e['size']]
        if mgi_crc(blob)!=e['hash']: raise ValueError(f"tocres: bad crc on {e['name']}")
        tocs[e['name']]=blob
    return tocs

def parse_toc(blob):
    ver,hs,ct,cnt,expl,disk,_,_=struct.unpack_from('<8I',blob)
    if ver!=3 or hs!=0x20: raise ValueError(f'unexpected toc version/header {ver}/{hs:#x}')
    return dict(create_time=ct,count=cnt,explode_size=expl,disk_size=disk),list(entries(blob,hs,cnt))

def main():
    romfs,out=sys.argv[1],sys.argv[2]; listing='--list' in sys.argv
    tocs=load_tocs(romfs); total=0
    for fn in sorted(os.listdir(romfs)):
        if not fn.endswith(('.res','.car','.trk')) or fn=='tocres.res': continue
        raw=open(os.path.join(romfs,fn),'rb').read()
        hdr,ents=parse_toc(tocs[fn.rsplit('.',1)[0]+'.toc'])
        ct,(cnt,expl,disk)=struct.unpack_from('<I',raw,0x14)[0],struct.unpack_from('<III',raw,0x20)
        if (ct,cnt,expl,disk)!=(hdr['create_time'],hdr['count'],hdr['explode_size'],hdr['disk_size']):
            raise ValueError(f'{fn}: TOC mismatch')
        img=explode(raw)
        if len(img)<expl: raise ValueError(f'{fn}: payload shorter than explode_size')
        for e in ents:
            data=img[e['offset']:e['offset']+e['size']]
            if mgi_crc(data)!=e['hash']: raise ValueError(f"{fn}/{e['name']}: bad crc")
            if listing:
                print(f"{fn}\t{e['name']}\t{e['type']}\t{e['flags']:#x}\t{e['size']}")
                continue
            p=os.path.join(out,fn,e['name'])
            os.makedirs(os.path.dirname(p),exist_ok=True)
            open(p,'wb').write(data)
        total+=len(ents)
    print(f'{total} resources from {len(tocs)} sets',file=sys.stderr)

if __name__=='__main__': main()
