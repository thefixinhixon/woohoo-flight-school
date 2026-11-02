"""Recursive-descent function discovery for the Pilotwings Resort ARM11 executable (code.bin).

usage: python3 funcs.py <code.bin> <exheader.bin> [out.tsv]

Decodes only what control flow needs (ARMv6K ARM + Thumb-1 with BL/BLX pairs):
  calls      ARM BL / BLX imm, Thumb BL / BLX pairs  -> new function (mode from the instruction)
  branches   ARM B<cond>, Thumb B<cond> / B           -> same function, unless the target is a known entry or
             starts with a push prologue (tail call -> new function)
  ends       BX LR, POP/LDM {..PC}, LDR PC, MOV PC, Thumb BX / POP {PC}, unconditional B
  data       PC-relative LDR / VLDR literals, ADR-addressed strings
  pointers   RVCT position-independent 'LDR rd,=off ; ADD rd,PC' function pointers -> new function
  switches   RVCT ARM 'CMP rX,#n ; ADDLS PC,PC,rX,LSL #2 ; B default ; B case0..n' tables, and
             'CMP rX,#n ; LDRLO PC,[PC,rX,LSL #2] ; B default ; .word case0..n-1' tables
Extra seeds, iterated to a fixpoint: entry point; recomp/extra_seeds.txt (run-time finds, or $PWR_EXTRA_SEEDS); the RVCT .init_array (self-relative entries at the end of .text); 4-byte words in .rodata/.data and in literal pools that point into
.text at a plausible prologue (vtables, callbacks); then (heuristic, tagged 'h') a push prologue at the start of
an unclassified gap (also after up to 3 plain AL instructions, any register list). Output: one line per function: address, mode, size of reached code, callees.
"""
import os,struct,sys
from collections import defaultdict

def load(code_path,exh_path):
    d=open(code_path,'rb').read(); e=open(exh_path,'rb').read()
    text,ro,data=(struct.unpack_from('<III',e,o) for o in (0x10,0x20,0x30))
    return d,dict(text=(text[0],text[2]),ro=(ro[0],ro[2]),data=(data[0],data[2]))

class Analyzer:
    def __init__(s,d,sec):
        s.d=d; s.base=sec['text'][0]; s.tend=s.base+sec['text'][1]; s.sec=sec
        s.kind={}                      # addr -> 'A' / 'T' code, 'D' data
        s.funcs={}                     # entry -> mode
        s.calls=defaultdict(set); s.owner={}
        s.work=[]; s.heuristic=set(); s.unverified=set(); s.veneers=[]
        s.switch8=s.find_switch8()
    def u32(s,a): return struct.unpack_from('<I',s.d,a-s.base)[0]
    def u16(s,a): return struct.unpack_from('<H',s.d,a-s.base)[0]
    def in_text(s,a): return s.base<=a<s.tend
    def add_func(s,a,mode,src=None):
        if not s.in_text(a): return
        if src is not None: s.calls[src].add(a)
        if a not in s.funcs: s.funcs[a]=mode; s.work.append((a,mode,a))
    def mark_data(s,a,n):
        for x in range(a,a+n):
            if s.in_text(x) and x not in s.kind: s.kind[x]='D'
    def mark_string(s,a):
        if not s.in_text(a): return
        o=a-s.base; e=s.d.find(b'\0',o,o+512)
        if e>o and all(32<=c<127 or c in(9,10,13) for c in s.d[o:e]): s.mark_data(a,e-o+1)

    def run(s):
        while s.work:
            entry,mode,a=s.work.pop()
            (s.arm if mode=='A' else s.thumb)(entry,a)

    def arm(s,f,a):
        todo=[a]
        while todo:
            a=todo.pop(); lits={}
            while s.in_text(a) and a+4<=s.tend:
                if a in s.kind: break
                w=s.u32(a); s.kind[a]=s.kind[a+1]=s.kind[a+2]=s.kind[a+3]='A'; s.owner[a]=f
                cond=w>>28; nxt=a+4
                if cond==0xF:
                    if (w>>25)&7==0b101:          # BLX imm -> Thumb
                        off=((w&0xffffff)^0x800000)-0x800000
                        s.add_func(a+8+off*4+((w>>24)&1)*2,'T',f)
                    a=nxt; continue
                op=(w>>25)&7
                if op==0b101:                     # B / BL
                    off=((w&0xffffff)^0x800000)-0x800000; t=a+8+off*4
                    if w&(1<<24): s.add_func(t,'A',f)
                    elif s.in_text(t) and t%4==0 and (t in s.funcs or s.is_prologue(t,'A')):
                        s.add_func(t,'A',f)               # tail call into a function with its own prologue
                        if cond==0xE: break
                    else:
                        todo.append(t)
                        if cond==0xE: break
                    a=nxt; continue
                if (w&0x0ffffff0)==0x012fff10:    # BX rm
                    if cond==0xE: break
                    a=nxt; continue
                if (w&0x0f7f0000)==0x051f0000:    # LDR rd,[pc,#imm]
                    imm=w&0xfff; t=a+8+(imm if w&0x800000 else -imm); s.mark_data(t,4)
                    if (w>>12)&15==15 and cond==0xE: break
                    if s.in_text(t): lits[(w>>12)&15]=s.u32(t)
                    a=nxt; continue
                if (w&0x0fff0ff0)==0x008f0000 and (w>>12)&15==(w&15) and (w&15) in lits:   # ADD rd,PC,rd
                    s.pc_relative_target((a+8+lits.pop(w&15))&0xffffffff)
                if (w&0x0f3f0e00)==0x0d1f0a00:    # VLDR from pc
                    imm=(w&0xff)*4; t=a+8+(imm if w&0x800000 else -imm); s.mark_data(t,8 if w&0x100 else 4)
                    a=nxt; continue
                if op==0b100 and w&(1<<20) and w&0x8000:   # LDM with PC
                    if cond==0xE: break
                    a=nxt; continue
                if (w&0x0ffffff0)==0x079ff100:    # LDRcc PC,[PC,rX,LSL #2]: table of absolute addresses follows
                    n=s.switch_bound(a,w&15)
                    if n is not None and n<1024:
                        s.mark_data(a+8,4*n)
                        for i in range(n): todo.append(s.u32(a+8+4*i))
                    if cond==0xE: break
                    a=nxt; continue
                if (w&0x0c00f000)==0x0400f000 and w&(1<<20):  # LDR pc,[...]
                    if cond==0xE: break
                    a=nxt; continue
                if (w&0x0de00000)==0x00800000 and (w>>12)&15==15 and (w>>16)&15==15 and not w&(1<<25) and (w>>5)&3==0 and (w>>7)&31==2:
                    # ADDcc PC,PC,rX,LSL #2 : jump table; bound from a preceding CMP rX,#imm
                    rx=w&15; n=None
                    for b in range(a-4,a-24,-4):
                        p=s.u32(b)
                        if (p&0x0ff0f000)==0x03500000 and (p>>16)&15==rx:
                            imm=p&0xff; r=((p>>8)&15)*2; n=((imm>>r)|(imm<<(32-r)))&0xffffffff; break
                    if n is not None and n<1024:
                        for i in range(n+2): todo.append(a+8+i*4-4 if i==0 else a+4+i*4)
                        todo.append(a+4)
                    break
                if (w&0x0de0f000)==0x01a0f000 or ((w>>26)&3==0 and (w>>12)&15==15 and (w>>21)&15 in (0,1,2,3,4,5,6,7,12,13,14,15)):
                    if cond==0xE: break           # MOV pc / data-processing into pc
                    a=nxt; continue
                if (w&0x0fff0000)==0x028f0000:    # ADR (add rd,pc,#imm) -> maybe string
                    imm=w&0xff; r=((w>>8)&15)*2; t=(a+8+(((imm>>r)|(imm<<(32-r)))&0xffffffff))&0xffffffff
                    if s.in_text(a+4) and s.u32(a+4)==(0xe12fff10|(w>>12)&15) and s.in_text(t&~1):
                        s.veneers.append((t&~1,'T' if t&1 else 'A',f))   # ADR rX,label; BX rX: ARM->Thumb veneer
                    else: s.mark_string(t)
                a=nxt

    SWITCH8=(0xe55ec001,0xe153000c,0x37de3003,0x27de300c,0xe08ec083,0xe12fff1c)
    def find_switch8(s):
        """RVCT __ARM_common_switch8: Thumb 'BLX helper' followed by a byte table {n, off[0..n-1], off_default};
        target = (call+4) + 2*off. Index in r3. Located by its exact instruction sequence."""
        found=set()
        for o in range(0,s.tend-s.base-24,4):
            if all(s.u32(s.base+o+4*i)==w for i,w in enumerate(s.SWITCH8)): found.add(s.base+o)
        return found

    def switch8_targets(s,a):
        """for a Thumb call at a (4 bytes) to the switch8 helper: (table length in bytes, [case targets..., default])"""
        t=a+4; n=s.d[t-s.base]
        offs=[s.d[t+1+i-s.base] for i in range(n+1)]
        return n+2,[t+2*o for o in offs]

    def switch_bound(s,a,rx):
        """case count from the CMP rX,#imm shortly before a jump-table instruction at a"""
        for b in range(a-4,a-68,-4):           # up to 16 instructions back; RVCT schedules setup in between
            p=s.u32(b)
            if (p&0x0ff0f000)==0x03500000 and (p>>16)&15==rx:
                imm=p&0xff; r=((p>>8)&15)*2; return ((imm>>r)|(imm<<(32-r)))&0xffffffff
            op=(p>>25)&7
            if (op in (0,1) and (p>>12)&15==rx and not (p>>21)&15 in (8,9,10,11)) or (op in (2,3) and (p>>12)&15==rx and p&(1<<20)) \
               or (op==4 and p&(1<<20) and p&(1<<rx)) or (op==5):
                return None                     # rX overwritten (or control flow) before reaching a CMP
        return None

    def pc_relative_target(s,t):
        """RVCT position-independent pointer (LDR rd,=off ; ADD rd,PC): seed it if it lands on unclaimed code."""
        if not s.in_text(t&~1) or (t&~1) in s.kind: return
        if t&1: s.add_func(t&~1,'T')
        elif t%4==0 and s.is_prologue(t,'A'): s.add_func(t,'A')

    def thumb(s,f,a):
        todo=[a]
        while todo:
            a=todo.pop(); lits={}
            while s.in_text(a) and a+2<=s.tend:
                if a in s.kind: break
                h=s.u16(a); s.kind[a]=s.kind[a+1]='T'; s.owner[a]=f; nxt=a+2
                if 0xF000<=h<0xF800 and a+4<=s.tend:
                    l=s.u16(a+2)
                    if l>=0xE800:
                        s.kind[a+2]=s.kind[a+3]='T'
                        off=(((h&0x7ff)<<12)^0x400000)-0x400000+((l&0x7ff)<<1)
                        if l>=0xF800: s.add_func(a+4+off,'T',f)
                        elif ((a+4+off)&~3) in s.switch8:          # inline switch table follows the call
                            ln,tg=s.switch8_targets(a); s.mark_data(a+4,ln)
                            for x in tg: todo.append(x)
                            break
                        else: s.add_func((a+4+off)&~3,'A',f)
                        a+=4; continue
                if 0xD000<=h<0xDE00:                       # B<cond>
                    off=((h&0xff)^0x80)-0x80; todo.append(a+4+off*2); a=nxt; continue
                if 0xE000<=h<0xE800:                       # B
                    off=((h&0x7ff)^0x400)-0x400; t=a+4+off*2
                    if s.in_text(t) and (t in s.funcs or s.is_prologue(t,'T')): s.add_func(t,'T',f)
                    else: todo.append(t)
                    break
                if (h&0xff87)==0x4700: break               # BX rm
                if 0xBD00<=h<=0xBDFF: break                # POP {..,pc}
                if (h&0xff87)==0x4687: break               # MOV pc,rm
                if 0x4800<=h<0x5000:
                    lit=((a+4)&~3)+(h&0xff)*4; s.mark_data(lit,4); lits[(h>>8)&7]=s.u32(lit)
                elif (h&0xfff8)==0x4478 and (h&7) in lits:  # ADD rd,PC after LDR rd,=off: PC-relative pointer
                    s.pc_relative_target((a+4+lits.pop(h&7))&0xffffffff)
                elif (h&0xf800)==0x2000 or (h&0xf800)==0x4800: lits.pop((h>>8)&7,None)
                if 0xA000<=h<0xA800: s.mark_string(((a+4)&~3)+(h&0xff)*4)
                a=nxt

    def pointer_seeds(s,runs=False):
        """Code pointers in .rodata/.data: accepted when the target has a prologue, or when the word sits in a run of
        >= 2 consecutive code pointers (vtable shape; virtual functions may start without a prologue or even inside
        another function's code). The latter are tagged unverified."""
        n=0
        def is_ptr(p): return s.in_text(p&~1) and ((p&1) or p%4==0)
        for name in('ro','data'):
            va,size=s.sec[name]
            words=[struct.unpack_from('<I',s.d,va-s.base+o)[0] for o in range(0,size-3,4)]
            for i,p in enumerate(words):
                if not is_ptr(p): continue
                in_run=runs and any(0<=j<len(words) and j!=i and is_ptr(words[j]) for j in range(i-4,i+5))
                if runs and not in_run and not p&1 and (p&~1) not in s.kind and s.u32(p)>>28==0xE:
                    in_run=True   # isolated pointer onto unclaimed code starting with an unconditional ARM instruction
                if p&1:
                    if (p&~1) in s.funcs: continue
                    if (s.u16(p&~1)&0xff00) in (0xb500,0xb400): s.add_func(p&~1,'T'); n+=1
                    elif in_run: s.add_func(p&~1,'T'); s.unverified.add(p&~1); n+=1
                else:
                    if p in s.funcs: continue
                    w=s.u32(p)
                    if (w&0xffff0000)==0xe92d0000 or (w&0xfffff000)==0xe24dd000 or (w&0x0fff0000)==0x052d0000: s.add_func(p,'A'); n+=1
                    elif in_run: s.add_func(p,'A'); s.unverified.add(p); n+=1
        return n

    def is_prologue(s,a,mode):
        if mode=='T': return (s.u16(a)&0xff00)==0xb500
        w=s.u32(a); return (w&0xffff4000)==0xe92d4000 or (w&0x0fff0fff)==0x052de004

    def late_push(s,a):
        """RVCT often schedules up to 3 plain AL loads/ALU ops before STMFD sp!,{..} (any register list)."""
        for i in range(4):
            if a+4*i+4>s.tend or (a+4*i) in s.kind: return False
            w=s.u32(a+4*i)
            if (w&0xffff0000)==0xe92d0000: return True
            if w>>28!=0xE or (w>>26)&3 not in (0,1) or (w>>12)&15==15: return False
        return False

    def init_array_seeds(s):
        """RVCT .init_array: a run of self-relative words (target = entry address + value) at the end of .text,
        one per C++ static initializer. Found by scanning back from the end of .text."""
        def rel(a): return (a+struct.unpack_from('<i',s.d,a-s.base)[0])&0xffffffff
        a=(s.tend-4)&~3
        while a>=s.base and not (s.in_text(rel(a)) and rel(a)%4==0): a-=4     # skip trailing non-entries
        end=a+4; n=0
        while a>=s.base and s.in_text(rel(a)) and rel(a)%4==0 and rel(a)<end-0x40: a-=4
        start=a+4
        if end-start<16: return 0,None
        s.mark_data(start,end-start)
        for e in range(start,end,4): s.add_func(rel(e),'A'); n+=1
        return n,(start,end)

    def literal_seeds(s,any_target=False):
        """Function pointers loaded from literal pools. First pass: targets with a prologue only. With any_target,
        also leaf functions (no prologue) -- tagged 'p' as unverified, since a literal may point at a text table."""
        n=0
        for a in [a for a,k in s.kind.items() if k=='D' and a%4==0 and s.kind.get(a+3)=='D']:
            p=s.u32(a)
            if s.in_text(p&~1) and (p&~1) not in s.kind:
                mode='T' if p&1 else 'A'
                if mode=='A' and p%4: continue
                if s.is_prologue(p&~1,mode): s.add_func(p&~1,mode); n+=1
                elif any_target: s.add_func(p&~1,mode); s.unverified.add(p&~1); n+=1
        return n

    def veneer_seeds(s):
        """Targets of ADR/BX veneers (callbacks reached only through such a stub), added in the late passes: a Thumb
        target may end in a call that never returns and would otherwise run on into the ARM function behind it."""
        n=0
        for t,mode,f in s.veneers:
            if t not in s.funcs and t not in s.kind: s.add_func(t,mode,f); n+=1
            else: s.calls[f].add(t)
        s.veneers=[]
        return n

    def extra_seeds(s,path):
        """Seeds found at run time (rt: indirect branch to unknown target): lines '0xADDR A|T'."""
        n=0
        try: lines=open(path).read().split('\n')
        except OSError: return 0
        for l in lines:
            l=l.split('#')[0].split()
            if len(l)>=2: s.add_func(int(l[0],16)&~1,l[1]); n+=1
        return n

    def gap_seeds(s):
        n=0; a=s.base
        while a<s.tend:
            if a in s.kind: a+=1; continue
            g=a
            while g<s.tend and g not in s.kind and s.d[g-s.base]==0: g+=1   # skip zero padding
            for st,mode in ((g+(-g&3),'A'),(g+(g&1),'T')):
                if st+4<=s.tend and st not in s.kind and (s.is_prologue(st,mode) or (mode=='A' and s.late_push(st))):
                    s.add_func(st,mode); s.heuristic.add(st); n+=1; break
            while a<s.tend and a not in s.kind: a+=1
        return n

def main():
    d,sec=load(sys.argv[1],sys.argv[2]); A=Analyzer(d,sec)
    A.add_func(sec['text'][0],'A'); A.run()
    after_entry=len(A.funcs)
    ninit,span=A.init_array_seeds(); A.run()
    if span: print(f'.init_array {span[0]:#x}-{span[1]:#x}: {ninit} static initializers',file=sys.stderr)
    extra=A.extra_seeds(os.environ.get('PWR_EXTRA_SEEDS',os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','recomp','extra_seeds.txt'))); A.run()
    seeded=A.pointer_seeds(); A.run()
    lit=gap=0
    while True:
        n=A.literal_seeds(); A.run(); lit+=n
        if n: continue
        n=A.gap_seeds(); A.run(); gap+=n
        if not n: break
    # unverified passes last, so they never claim bytes that a verified seed decodes differently
    while True:
        n=A.pointer_seeds(runs=True); A.run(); seeded+=n
        m=A.literal_seeds(any_target=True); A.run(); lit+=m
        k=A.literal_seeds(); A.run(); lit+=k
        v=A.veneer_seeds(); A.run(); seeded+=v
        if not (n or m or k or v): break
    for f in [f for f in A.unverified if A.kind.get(f)!=A.funcs[f]]:   # unverified pointer landed inside other-mode code
        del A.funcs[f]; A.unverified.discard(f)
    code=sum(1 for k in A.kind.values() if k in 'AT'); data=sum(1 for k in A.kind.values() if k=='D')
    total=sec['text'][1]
    print(f'functions: {len(A.funcs)} ({after_entry} reached from entry, {extra} extra seeds, {seeded} section-pointer seeds, {lit} literal-pointer seeds ({len(A.unverified)} unverified leaf), {gap} heuristic gap seeds)',file=sys.stderr)
    print(f'text {total:#x} bytes: code {code/total:.1%}, data {data/total:.1%}, unclassified {(total-code-data)/total:.1%}',file=sys.stderr)
    print(f"modes: ARM {sum(1 for m in A.funcs.values() if m=='A')}, Thumb {sum(1 for m in A.funcs.values() if m=='T')}",file=sys.stderr)
    if len(sys.argv)>3:
        size=defaultdict(int)
        for a,f in A.owner.items(): size[f]+=4 if A.funcs.get(f)=='A' else 2
        with open(sys.argv[3],'w') as o:
            for f in sorted(A.funcs): o.write(f"{f:#010x}\t{A.funcs[f]}{'h' if f in A.heuristic else ''}{'p' if f in A.unverified else ''}\t{size[f]}\t{','.join(hex(c) for c in sorted(A.calls[f]))}\n")
    return A

if __name__=='__main__': main()
