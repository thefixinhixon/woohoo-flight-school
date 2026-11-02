"""Static recompiler: Pilotwings Resort ARM11 code.bin -> C.

usage: python3 arm2c.py <code.bin> <exheader.bin> <out dir>

Uses tools/funcs.py for function discovery, then translates every function to
    void f_XXXXXXXX(CPU *c)
in out/gen_NNN.c, plus out/funcs.h (prototypes) and out/table.c (sorted guest-address -> function table).
Branches inside a function become gotos; BL/BLX to known entries become direct calls; tail branches into another
function's prologue become call+return; indirect calls/jumps go through rt_call_indirect. Anything not handled
becomes rt_unimpl(c, pc, insn), and the totals are printed by encoding class so coverage can be driven to zero.
Assumes standard returns (BX LR / POP {..PC} return to the caller); POP returns are checked against the entry LR.
"""
import os,struct,sys
from collections import Counter,defaultdict
sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','tools'))
import funcs as F

# host functions run at the entry of a guest function (before its body), e.g. to print the game's debug log
HOOKS={0x210814:'rt_log_printf', 0x1b51f8:'gsp_gx_enqueue_hook',   # GX enqueue: gsp.c copies queued command lists
       0x149aa4:'freelook_pre', 0x1e34f0:'freelook_pre', 0x1ffb8c:'freelook_pre',   # chase camera updates (freelook.c)
       0x19b50c:'setting_pre', 0x19b634:'setting_pre'}                              # REAL settings by name (drawdist.c)
# host functions run after a guest function returns
POST_HOOKS={0x149aa4:'freelook_post', 0x1e34f0:'freelook_post', 0x1ffb8c:'freelook_post',
            0x19b50c:'setting_post', 0x19b634:'setting_post'}
# debug: PWR_TRACE_FUNCS=addr[,addr] at generation time prints entry registers and the return value of those functions
TRACE_FUNCS={int(x,16) for x in os.environ.get('PWR_TRACE_FUNCS','').split(',') if x.strip()}

COND=['c->z','!c->z','c->cf','!c->cf','c->n','!c->n','c->v','!c->v','(c->cf&&!c->z)','(!c->cf||c->z)',
      '(c->n==c->v)','(c->n!=c->v)','(!c->z&&c->n==c->v)','(c->z||c->n!=c->v)',None,None]

def h(v): return f'0x{v&0xffffffff:08x}u'
def rot_imm(w): imm=w&0xff; r=((w>>8)&15)*2; return ((imm>>r)|(imm<<(32-r)))&0xffffffff
def sx(v,bits): return (v^(1<<(bits-1)))-(1<<(bits-1))

class T:
    """Translation of one instruction: C code, intra-function successors, whether it falls through, size."""
    def __init__(s,code,succ=(),fall=True,size=4):
        s.code=code; s.succ=list(succ); s.fall=fall; s.size=size

class Translator:
    def __init__(s,A):
        s.A=A; s.unimpl=Counter(); s.unimpl_ex={}; s.entry_not_first=0
    def u32(s,a): return s.A.u32(a)
    def u16(s,a): return s.A.u16(a)
    def fn(s,a): return f'f_{a:08x}'
    def miss(s,cls,a,w):
        s.unimpl[cls]+=1; s.unimpl_ex.setdefault(cls,(a,w))
        return f'rt_unimpl(c, {h(a)}, {h(w)}); /* {cls} */'

    # ---------------- control-flow helpers ----------------
    def call(s,target,ret,a):
        if target in s.A.funcs: return f'c->r[14] = {h(ret)}; {s.fn(target)}(c);'
        return f'c->r[14] = {h(ret)}; rt_call_indirect(c, {h(target)}, {h(a)});'
    def tail(s,target,a):
        if target in s.A.funcs: return f'{s.fn(target)}(c); return;'
        return f'rt_call_indirect(c, {h(target)}, {h(a)}); return;'
    def ret_checked(s,expr,a):
        return f'{{ uint32_t t_ = {expr}; if ((t_ & ~1u) != (ret_lr & ~1u)) rt_ret_mismatch(c, {h(a)}, t_, ret_lr); return; }}'
    def jump_ind(s,expr,a):
        # an indirect jump to the entry lr is a return (e.g. varargs epilogues: ldr r3,[sp],#4; add sp,#16; bx r3)
        return f'{{ uint32_t t_ = {expr}; if ((t_ & ~1u) == (ret_lr & ~1u)) return; rt_call_indirect(c, t_, {h(a)}); return; }}'

    # ---------------- ARM ----------------
    def arm(s,a,entry):
        w=s.u32(a); cond=w>>28; pc=a+8
        R=lambda n: h(pc) if n==15 else f'c->r[{n}]'
        if cond==0xF: return s.arm_uncond(a,w)
        t=s.arm_body(a,w,R,pc,entry)
        if cond!=0xE and t.code:
            if not t.fall:   # conditional terminator: falls through when the condition fails
                t.fall=True
            t.code=f'if ({COND[cond]}) {{ {t.code} }}'
        return t

    def arm_uncond(s,a,w):
        if (w&0xfe000000)==0xfa000000:
            t=a+8+sx(w&0xffffff,24)*4+((w>>24)&1)*2
            return T(s.call(t|0,a+4,a) if t in s.A.funcs else s.call(t,a+4,a))
        if (w&0xfd70f000)==0xf550f000 or (w&0xff70f000)==0xf450f000: return T('/* pld */')
        if w==0xf57ff01f: return T('c->excl_valid = 0; /* clrex */')
        if (w&0xfffffff0) in (0xf57ff040,0xf57ff050,0xf57ff060): return T('__sync_synchronize(); /* dsb/dmb/isb */')
        return T(s.miss('uncond',a,w))

    def shifter(s,w,R):
        """returns (op2 expr setup lines, carry expr) for data-processing operand 2"""
        if w&(1<<25):
            v=rot_imm(w); carry='c->cf' if ((w>>8)&15)==0 else str(v>>31)
            return f'uint32_t op2 = {h(v)}; uint32_t sc = {carry};'
        rm=R(w&15); typ=(w>>5)&3
        if w&0x10:
            rs=R((w>>8)&15); fn=['lsl_rs','lsr_rs','asr_rs','ror_rs'][typ]
            return f'uint32_t sc = c->cf; uint32_t op2 = {fn}({rm}, {rs}, &sc);'
        amt=(w>>7)&31
        if typ==0:
            if amt==0: return f'uint32_t op2 = {rm}; uint32_t sc = c->cf;'
            return f'uint32_t op2 = {rm} << {amt}; uint32_t sc = ({rm} >> {32-amt}) & 1;'
        if typ==1:
            if amt==0: return f'uint32_t op2 = 0; uint32_t sc = {rm} >> 31;'
            return f'uint32_t op2 = {rm} >> {amt}; uint32_t sc = ({rm} >> {amt-1}) & 1;'
        if typ==2:
            if amt==0: return f'uint32_t op2 = (uint32_t)((int32_t){rm} >> 31); uint32_t sc = {rm} >> 31;'
            return f'uint32_t op2 = (uint32_t)((int32_t){rm} >> {amt}); uint32_t sc = ({rm} >> {amt-1}) & 1;'
        if amt==0: return f'uint32_t op2 = (c->cf << 31) | ({rm} >> 1); uint32_t sc = {rm} & 1;'
        return f'uint32_t op2 = ror32({rm}, {amt}); uint32_t sc = ({rm} >> {amt-1}) & 1;'

    def arm_body(s,a,w,R,pc,entry):
        op3=(w>>25)&7
        if op3 in (0,1):
            if op3==0 and (w&0x90)==0x90: return s.arm_mult_extra(a,w,R)
            if (w&0x01900000)==0x01000000: return s.arm_misc(a,w,R)
            return s.arm_dp(a,w,R,pc)
        if op3==2 or (op3==3 and not w&0x10): return s.arm_ldst(a,w,R,pc)
        if op3==3: return s.arm_media(a,w,R)
        if op3==4: return s.arm_ldm(a,w,R)
        if op3==5:
            t=a+8+sx(w&0xffffff,24)*4
            if w&(1<<24): return T(s.call(t,a+4,a))
            if t!=entry and t in s.A.funcs: return T(s.tail(t,a),fall=False)
            return T(f'goto L_{t:08x};',[t],fall=False)
        if op3==6: return s.arm_coproc_ls(a,w,R,pc)
        if (w>>24)&15==0xF: return T(f'rt_svc(c, {h(w&0xffffff)}, {h(a)});')
        return s.arm_coproc(a,w,R)

    def arm_dp(s,a,w,R,pc):
        op=(w>>21)&15; S=(w>>20)&1; rn=(w>>16)&15; rd=(w>>12)&15
        sh=s.shifter(w,R); A_=R(rn)
        logical=op in (0,1,8,9,12,13,14,15)
        expr={0:f'{A_} & op2',1:f'{A_} ^ op2',12:f'{A_} | op2',13:'op2',14:f'{A_} & ~op2',15:'~op2',
              8:f'{A_} & op2',9:f'{A_} ^ op2'}
        if logical:
            r=expr[op]; fl='set_nz(c, r); c->cf = sc;' if S else ''
        else:
            if S:
                r={2:f'sbc_flags(c, {A_}, op2, 1)',3:f'sbc_flags(c, op2, {A_}, 1)',4:f'adc_flags(c, {A_}, op2, 0)',
                   5:f'adc_flags(c, {A_}, op2, c->cf)',6:f'sbc_flags(c, {A_}, op2, c->cf)',7:f'sbc_flags(c, op2, {A_}, c->cf)',
                   10:f'sbc_flags(c, {A_}, op2, 1)',11:f'adc_flags(c, {A_}, op2, 0)'}[op]
            else:
                r={2:f'{A_} - op2',3:f'op2 - {A_}',4:f'{A_} + op2',5:f'{A_} + op2 + c->cf',6:f'{A_} - op2 - !c->cf',7:f'op2 - {A_} - !c->cf'}[op]
            fl=''
        if op in (8,9,10,11):
            return T(f'{{ {sh} uint32_t r = {r}; {fl} (void)r; }}')
        if rd==15:
            if S: return T(s.miss('dp-to-pc-S',a,w))
            if op==13 and not w&(1<<25) and (w&0xff0)==0 and (w&15)==14: return T('return;',fall=False)
            if op==4 and rn==15 and not w&(1<<25) and (w&0xff0)==0x100: return s.arm_switch(a,w)
            return T(f'{{ {sh} {s.jump_ind(r,a)} }}',fall=False)
        return T(f'{{ {sh} uint32_t r = {r}; {fl} c->r[{rd}] = r; }}')

    def arm_switch(s,a,w):
        """ADDcc PC,PC,Rm,LSL #2 jump table; bound from a preceding CMP Rm,#imm"""
        rm=w&15; n=None
        for b in range(a-4,a-24,-4):
            p=s.u32(b)
            if (p&0x0ff0f000)==0x03500000 and (p>>16)&15==rm: n=rot_imm(p); break
        if n is None or n>1024: return T(s.miss('switch-unbounded',a,w),fall=False)
        cases=' '.join(f'case {i}: goto L_{a+8+4*i:08x};' for i in range(n+1))
        succ=[a+8+4*i for i in range(n+1)]
        return T(f'switch (c->r[{rm}]) {{ {cases} default: {s.jump_ind(h(0),a)} }}',succ,fall=False)

    def arm_misc(s,a,w,R):
        rd=(w>>12)&15; rm=w&15; rn=(w>>16)&15; rs=(w>>8)&15
        if (w&0x0fb00000)==0x03200000:            # MSR imm / hints
            if (w&0x0fffff00)==0x0320f000 and (w&0xff)<5: return T('/* hint */')
            if (w>>16)&8: v=rot_imm(w); return T(f'c->n = {v>>31&1}; c->z = {v>>30&1}; c->cf = {v>>29&1}; c->v = {v>>28&1}; c->q = {v>>27&1};')
            return T(s.miss('msr-imm',a,w))
        if (w&0x0fbf0fff)==0x010f0000:            # MRS
            return T(f'c->r[{rd}] = (c->n << 31) | (c->z << 30) | (c->cf << 29) | (c->v << 28) | (c->q << 27) | (c->ge << 16) | 0x10u;')
        if (w&0x0fb0fff0)==0x0120f000:            # MSR reg
            code=''
            if (w>>16)&8: code+=f'{{ uint32_t t_ = {R(rm)}; c->n = t_ >> 31; c->z = (t_ >> 30) & 1; c->cf = (t_ >> 29) & 1; c->v = (t_ >> 28) & 1; c->q = (t_ >> 27) & 1; }}'
            if (w>>16)&4: code+=f'c->ge = ({R(rm)} >> 16) & 15;'
            return T(code or '/* msr (control fields ignored) */')
        if (w&0x0ffffff0)==0x012fff10:
            if rm==14: return T('return;',fall=False)
            return T(s.jump_ind(R(rm),a),fall=False)
        if (w&0x0ffffff0)==0x012fff30:
            return T(f'{{ uint32_t t_ = {R(rm)}; c->r[14] = {h(a+4)}; rt_call_indirect(c, t_, {h(a)}); }}')
        if (w&0x0fff0ff0)==0x016f0f10: return T(f'c->r[{rd}] = clz32({R(rm)});')
        if (w&0x0ff000f0)==0x01200070: return T(f'rt_bkpt(c, {h(a)});')
        if (w&0x0f9000f0)==0x01000050:            # QADD/QSUB/QDADD/QDSUB
            op=(w>>21)&3; A_=f'(int64_t)(int32_t){R(rm)}'; B_=f'(int64_t)(int32_t){R(rn)}'
            if op&2: B_=f'(int64_t)sat_s(2*{B_}, 32, c)'
            return T(f'c->r[{rd}] = (uint32_t)sat_s({A_} {"-" if op&1 else "+"} {B_}, 32, c);')
        if (w&0x0f900090)==0x01000080:            # signed 16-bit multiplies
            op=(w>>21)&3; x=(w>>5)&1; y=(w>>6)&1
            hm=f'(int32_t)(int16_t)({R(rm)} >> {16 if x else 0})'; hs=f'(int32_t)(int16_t)({R(rs)} >> {16 if y else 0})'
            if op==0: return T(f'{{ int64_t p_ = (int64_t){hm} * {hs} + (int32_t){R(rd)}; if (p_ != (int32_t)p_) c->q = 1; c->r[{rn}] = (uint32_t)p_; }}')  # SMLAxy
            if op==1:
                if x: return T(f'c->r[{rn}] = (uint32_t)(((int64_t)(int32_t){R(rm)} * {hs}) >> 16);')  # SMULWy
                return T(f'{{ int64_t p_ = (((int64_t)(int32_t){R(rm)} * {hs}) >> 16) + (int32_t){R(rd)}; if (p_ != (int32_t)p_) c->q = 1; c->r[{rn}] = (uint32_t)p_; }}')  # SMLAWy
            if op==2: return T(f'{{ int64_t p_ = (int64_t)(((uint64_t){R(rn)} << 32) | {R(rd)}) + (int64_t){hm} * {hs}; c->r[{rd}] = (uint32_t)p_; c->r[{rn}] = (uint32_t)((uint64_t)p_ >> 32); }}')
            return T(f'c->r[{rn}] = (uint32_t)({hm} * {hs});')   # SMULxy
        return T(s.miss('misc',a,w))

    def arm_mult_extra(s,a,w,R):
        if (w&0x0f0000f0)==0x00000090:            # multiplies
            op=(w>>21)&7; S=(w>>20)&1; rd=(w>>16)&15; rn=(w>>12)&15; rs=(w>>8)&15; rm=w&15
            if op==0: return T(f'c->r[{rd}] = {R(rm)} * {R(rs)};'+(f' set_nz(c, c->r[{rd}]);' if S else ''))
            if op==1: return T(f'c->r[{rd}] = {R(rm)} * {R(rs)} + {R(rn)};'+(f' set_nz(c, c->r[{rd}]);' if S else ''))
            if op==2: return T(f'{{ uint64_t p_ = (uint64_t){R(rm)} * {R(rs)} + {R(rn)} + {R(rd)}; c->r[{rn}] = (uint32_t)p_; c->r[{rd}] = (uint32_t)(p_ >> 32); }}')  # UMAAL
            if op>=4:
                lo,hi=rn,rd; sgn=op&2
                m=f'(int64_t)(int32_t){R(rm)} * (int32_t){R(rs)}' if sgn else f'(uint64_t){R(rm)} * {R(rs)}'
                acc=f' + (((uint64_t)c->r[{hi}] << 32) | c->r[{lo}])' if op&1 else ''
                fl=f' c->n = (uint32_t)(p_ >> 63); c->z = p_ == 0;' if S else ''
                return T(f'{{ uint64_t p_ = (uint64_t)({m}){acc}; c->r[{lo}] = (uint32_t)p_; c->r[{hi}] = (uint32_t)(p_ >> 32);{fl} }}')
            return T(s.miss('mult',a,w))
        if (w&0x0fb00ff0)==0x01000090:            # SWP/SWPB
            rd=(w>>12)&15; rn=(w>>16)&15; rm=w&15; b=w&(1<<22)
            if b: return T(f'{{ uint32_t t_ = __atomic_exchange_n(g_mem + {R(rn)}, (uint8_t){R(rm)}, __ATOMIC_SEQ_CST); c->r[{rd}] = t_; }}')
            return T(f'{{ uint32_t t_ = __atomic_exchange_n((uint32_t *)(g_mem + {R(rn)}), {R(rm)}, __ATOMIC_SEQ_CST); c->r[{rd}] = t_; }}')
        if (w&0x0f8000f0)==0x01800090:            # LDREX/STREX family
            op=(w>>20)&7; rn=(w>>16)&15; rd=(w>>12)&15; rm=w&15
            m={1:('ldrex32',None),0:(None,'strex32'),5:('ldrexb',None),4:(None,'strexb'),7:('ldrexh',None),6:(None,'strexh')}
            if op in m:
                l,st=m[op]
                if l: return T(f'c->r[{rd}] = {l}(c, {R(rn)});')
                return T(f'c->r[{rd}] = {st}(c, {R(rn)}, {R(rm)});')
            return T(s.miss('ldrexd/strexd',a,w))
        # extra load/store: halfword, signed byte, doubleword
        P=(w>>24)&1; U=(w>>23)&1; I=(w>>22)&1; W=(w>>21)&1; L=(w>>20)&1; rn=(w>>16)&15; rd=(w>>12)&15; sh=(w>>5)&3
        off=h(((w>>4)&0xf0)|(w&15)) if I else R(w&15)
        base=R(rn); sgn='+' if U else '-'
        ea=f'{base} {sgn} {off}' if P else base
        wb=f' c->r[{rn}] = {base} {sgn} {off};' if (not P or W) and rn!=15 else ''
        if L:
            ld={1:'rd16(ad_)',2:'(uint32_t)(int32_t)(int8_t)rd8(ad_)',3:'(uint32_t)(int32_t)(int16_t)rd16(ad_)'}[sh]
            if rd==15: return T(s.miss('extra-load-pc',a,w))
            return T(f'{{ uint32_t ad_ = {ea}; uint32_t v_ = {ld};{wb} c->r[{rd}] = v_; }}')
        if sh==1: return T(f'{{ uint32_t ad_ = {ea}; wr16(ad_, {R(rd)});{wb} }}')
        if sh==2: return T(f'{{ uint32_t ad_ = {ea}; uint32_t a_ = rd32(ad_), b_ = rd32(ad_ + 4);{wb} c->r[{rd}] = a_; c->r[{rd+1}] = b_; }}')  # LDRD
        return T(f'{{ uint32_t ad_ = {ea}; wr32(ad_, {R(rd)}); wr32(ad_ + 4, {R(rd+1)});{wb} }}')  # STRD

    def arm_ldst(s,a,w,R,pc):
        I=(w>>25)&1; P=(w>>24)&1; U=(w>>23)&1; B=(w>>22)&1; W=(w>>21)&1; L=(w>>20)&1; rn=(w>>16)&15; rd=(w>>12)&15
        sgn='+' if U else '-'
        if not I:
            off=w&0xfff
            if rn==15 and P and not W:            # literal: text is immutable, fold the value
                ad=pc+(off if U else -off)
                if L and not B and s.A.in_text(ad) and rd!=15: return T(f'c->r[{rd}] = {h(s.u32(ad))};')
            offs=h(off)
        else:
            rm=R(w&15); typ=(w>>5)&3; amt=(w>>7)&31
            if typ==0: offs=f'({rm} << {amt})' if amt else rm
            elif typ==1: offs=f'({rm} >> {amt})' if amt else '0u'
            elif typ==2: offs=f'(uint32_t)((int32_t){rm} >> {amt if amt else 31})'
            else: offs=f'ror32({rm}, {amt})' if amt else f'((c->cf << 31) | ({rm} >> 1))'
        if (w&0x0ffffff0)==0x079ff100:            # LDRcc PC,[PC,rX,LSL #2] jump table of absolute addresses
            n=s.A.switch_bound(a,w&15)
            if n is None or n>1024: return T(s.miss('ldr-pc-table-unbounded',a,w),fall=False)
            tg=[s.u32(a+8+4*i) for i in range(n)]
            cases=' '.join(f'case {i}: goto L_{t:08x};' for i,t in enumerate(tg))
            return T(f'switch (c->r[{w&15}]) {{ {cases} default: {s.jump_ind(h(0),a)} }}',tg,fall=False)
        base=R(rn)
        ea=f'{base} {sgn} {offs}' if P else base
        wb=f' c->r[{rn}] = {base} {sgn} {offs};' if (not P or W) and rn!=15 else ''
        if L:
            ld='rd8(ad_)' if B else 'rd32(ad_)'
            if rd==15:
                if rn==13 and not P and U and not I:   # LDR pc,[sp],#N: return (N = frame size popped with it)
                    return T(f'{{ uint32_t ad_ = {ea}; uint32_t v_ = {ld};{wb} {s.ret_checked("v_",a)} }}',fall=False)
                return T(f'{{ uint32_t ad_ = {ea}; uint32_t v_ = {ld};{wb} {s.jump_ind("v_",a)} }}',fall=False)
            return T(f'{{ uint32_t ad_ = {ea}; uint32_t v_ = {ld};{wb} c->r[{rd}] = v_; }}')
        st='wr8' if B else 'wr32'
        return T(f'{{ uint32_t ad_ = {ea}; uint32_t v_ = {R(rd) if rd!=15 else h(pc+4)};{wb} {st}(ad_, v_); }}')

    def arm_ldm(s,a,w,R):
        P=(w>>24)&1; U=(w>>23)&1; S=(w>>22)&1; W=(w>>21)&1; L=(w>>20)&1; rn=(w>>16)&15; regs=[i for i in range(16) if w&(1<<i)]
        n=len(regs)
        if not n: return T(s.miss('ldm-empty',a,w))
        start={(0,1):'b_',(1,1):'b_ + 4',(0,0):f'b_ - {4*n-4}',(1,0):f'b_ - {4*n}'}[(P,U)]
        wbv=f'b_ + {4*n}' if U else f'b_ - {4*n}'
        code=f'uint32_t b_ = c->r[{rn}]; uint32_t ad_ = {start};'
        if L:
            if W: code+=f' c->r[{rn}] = {wbv};'
            tail=''
            for r in regs:
                if r==15: code+=' uint32_t pc_ = rd32(ad_);'; tail=s.ret_checked('pc_',a) if rn==13 else s.jump_ind('pc_',a)
                else: code+=f' c->r[{r}] = rd32(ad_); ad_ += 4;'
            return T(f'{{ {code} {tail} }}',fall=not tail)
        vals=' '.join(f'wr32(ad_, {R(r) if r!=15 else h(a+12)}); ad_ += 4;' for r in regs)
        code+=' '+vals
        if W: code+=f' c->r[{rn}] = {wbv};'
        return T(f'{{ {code} }}')

    def arm_media(s,a,w,R):
        rd=(w>>12)&15; rn=(w>>16)&15; rm=w&15; rs=(w>>8)&15
        o=(w>>20)&0xff; o2=(w>>4)&0xf
        if (o&0xf8)==0x68 and (o2&7)==7:           # extend (+add)
            rot=((w>>10)&3)*8; v=f'ror32({R(rm)}, {rot})' if rot else R(rm)
            kind={0x68:'SXTB16',0x6a:'SXTB',0x6b:'SXTH',0x6c:'UXTB16',0x6e:'UXTB',0x6f:'UXTH'}.get(o)
            if kind is None: return T(s.miss('extend',a,w))
            ext={'SXTB':f'(uint32_t)(int32_t)(int8_t){v}','SXTH':f'(uint32_t)(int32_t)(int16_t){v}','UXTB':f'({v} & 0xffu)','UXTH':f'({v} & 0xffffu)',
                 'UXTB16':f'({v} & 0x00ff00ffu)','SXTB16':f'((uint32_t)(uint16_t)(int16_t)(int8_t){v} | ((uint32_t)(uint16_t)(int16_t)(int8_t)({v} >> 16) << 16))'}[kind]
            if rn==15: return T(f'c->r[{rd}] = {ext};')
            if kind.endswith('16'):
                return T(f'{{ uint32_t e_ = {ext}, n_ = {R(rn)}; c->r[{rd}] = ((n_ + e_) & 0xffffu) | (((n_ >> 16) + (e_ >> 16)) << 16); }}')
            return T(f'c->r[{rd}] = {R(rn)} + {ext};')
        if o==0x6b and o2==3: return T(f'c->r[{rd}] = __builtin_bswap32({R(rm)});')
        if o==0x6b and o2==0xb: return T(f'{{ uint32_t v_ = {R(rm)}; c->r[{rd}] = ((v_ & 0x00ff00ffu) << 8) | ((v_ >> 8) & 0x00ff00ffu); }}')
        if o==0x6f and o2==0xb: return T(f'c->r[{rd}] = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t){R(rm)});')
        if (o&0xfa)==0x6a and (o2&3)==1:           # SSAT / USAT
            sat=(w>>16)&31; amt=(w>>7)&31; asr=(w>>6)&1
            v=(f'(int64_t)((int32_t){R(rm)} >> {amt if amt else 31})' if asr else f'(int64_t)(int32_t)({R(rm)} << {amt})')
            if o&4: return T(f'c->r[{rd}] = sat_u({v}, {sat}, c);')
            return T(f'c->r[{rd}] = (uint32_t)sat_s({v}, {sat+1}, c);')
        if o in (0x6a,0x6e) and o2==3:             # SSAT16 / USAT16
            sat=(w>>16)&15
            if o==0x6e: lane=lambda i: f'sat_u((int16_t)({R(rm)} >> {16*i}), {sat}, c)'
            else: lane=lambda i: f'(uint16_t)sat_s((int16_t)({R(rm)} >> {16*i}), {sat+1}, c)'
            return T(f'c->r[{rd}] = (({lane(0)}) & 0xffffu) | ((uint32_t)({lane(1)}) << 16);')
        if o==0x68 and (o2&3)==1:                  # PKHBT / PKHTB
            amt=(w>>7)&31
            if o2&4: sh_=f'(uint32_t)((int32_t){R(rm)} >> {amt if amt else 31})'; return T(f'c->r[{rd}] = ({R(rn)} & 0xffff0000u) | ({sh_} & 0xffffu);')
            return T(f'c->r[{rd}] = ({R(rn)} & 0xffffu) | (({R(rm)} << {amt}) & 0xffff0000u);')
        if o==0x68 and o2==0xb:                    # SEL
            return T(f'{{ uint32_t n_ = {R(rn)}, m_ = {R(rm)}, r_ = 0; for (int i = 0; i < 4; i++) r_ |= ((c->ge >> i) & 1 ? n_ : m_) & (0xffu << (8 * i)); c->r[{rd}] = r_; }}')
        if (o&0xf8)==0x60: return s.arm_parallel(a,w,R)
        if o==0x70 and o2 in (1,3,5,7):             # SMLAD/SMLSD/SMUAD/SMUSD
            x=(o2>>1)&1; sub=(o2>>2)&1
            m2=f'ror32({R(rs)}, 16)' if x else R(rs)
            prod=f'(int64_t)(int16_t){R(rm)} * (int16_t){m2} {"-" if sub else "+"} (int64_t)(int16_t)({R(rm)} >> 16) * (int16_t)({m2} >> 16)'
            acc=f' + (int32_t){R(rd)}' if rd!=15 else ''
            return T(f'{{ int64_t p_ = {prod}{acc}; if (p_ != (int32_t)p_) c->q = 1; c->r[{rn}] = (uint32_t)p_; }}')
        if o==0x75 and (o2&0xd)==1:                # SMMUL / SMMLA (+R)
            rnd=' + 0x80000000ll' if o2&2 else ''
            acc=f'((int64_t)(int32_t){R(rd)} << 32) + ' if rd!=15 else ''
            return T(f'c->r[{rn}] = (uint32_t)(({acc}(int64_t)(int32_t){R(rm)} * (int32_t){R(rs)}{rnd}) >> 32);')
        if o==0x78 and o2==1:                      # USAD8 / USADA8
            acc=f'{R(rd)} + ' if rd!=15 else ''
            return T(f'{{ uint32_t m_ = {R(rm)}, s_ = {R(rs)}, t_ = 0; for (int i = 0; i < 32; i += 8) {{ int d_ = (int)((m_ >> i) & 255) - (int)((s_ >> i) & 255); t_ += d_ < 0 ? -d_ : d_; }} c->r[{rn}] = {acc}t_; }}')
        return T(s.miss(f'media o={o:#x} o2={o2:#x}',a,w))

    def arm_parallel(s,a,w,R):
        pre=(w>>20)&7; op=(w>>5)&7; rd=(w>>12)&15; rn=(w>>16)&15; rm=w&15
        if pre not in (1,2,3,5,6,7) or op not in (0,1,2,3,4,7): return T(s.miss('parallel',a,w))
        signed=pre in (1,2,3); kind={1:'',2:'q',3:'h',5:'',6:'q',7:'h'}[pre]
        lanes=4 if op in (4,7) else 2; bits=32//lanes
        ops={0:['+','+'],1:['-','+'],2:['+','-'],3:['-','-'],4:['+']*4,7:['-']*4}[op]
        # ASX/SAX swap halfwords of Rm
        mexpr='ror32(m_, 16)' if op in (1,2) else 'm_'
        ty='int32_t' if signed else 'uint32_t'; mask=(1<<bits)-1
        lane_ex=lambda v,i: (f'(int32_t)(int{bits}_t)({v} >> {i*bits})' if signed else f'(({v} >> {i*bits}) & {mask}u)')
        code=f'uint32_t n_ = {R(rn)}, m_ = {R(rm)}, mm_ = {mexpr}, r_ = 0, ge_ = 0;'
        for i in range(lanes):
            code+=f' {{ {ty} x_ = {lane_ex("n_",i)}, y_ = {lane_ex("mm_",i)}; int64_t s_ = (int64_t)x_ {ops[i]} (int64_t)y_;'
            if kind=='q': code+=(f' s_ = s_ > {(1<<(bits-1))-1} ? {(1<<(bits-1))-1} : s_ < -{1<<(bits-1)} ? -{1<<(bits-1)} : s_;' if signed else f' s_ = s_ > {mask} ? {mask} : s_ < 0 ? 0 : s_;')
            elif kind=='h': code+=' s_ >>= 1;'
            else: code+=(f' if (s_ >= 0) ge_ |= {((1<<(4//lanes))-1)<<(i*(4//lanes))};' if signed else (f' if (s_ >= {1<<bits} || ({ops[i]=="-"} && s_ >= 0)) ge_ |= {((1<<(4//lanes))-1)<<(i*(4//lanes))};'))
            code+=f' r_ |= ((uint32_t)s_ & {mask}u) << {i*bits}; }}'
        if not kind: code+=' c->ge = ge_;'
        code+=f' c->r[{rd}] = r_; (void)ge_;'
        return T(f'{{ {code} }}')

    # ---------------- VFP / coprocessors ----------------
    def arm_coproc_ls(s,a,w,R,pc):
        cp=(w>>8)&15
        if (w&0x0fe00000)==0x0c400000:            # MCRR/MRRC
            L=(w>>20)&1; rt2=(w>>16)&15; rt=(w>>12)&15; m=(w&15)
            if cp==10:
                sm=(m<<1)|((w>>5)&1)
                if L: return T(f'c->r[{rt}] = c->s[{sm}]; c->r[{rt2}] = c->s[{sm+1}];')
                return T(f'c->s[{sm}] = {R(rt)}; c->s[{sm+1}] = {R(rt2)};')
            if cp==11:
                if L: return T(f'c->r[{rt}] = c->s[{2*m}]; c->r[{rt2}] = c->s[{2*m+1}];')
                return T(f'c->s[{2*m}] = {R(rt)}; c->s[{2*m+1}] = {R(rt2)};')
            return T(s.miss('mcrr-other',a,w))
        if cp not in (10,11): return T(s.miss('ldc-other',a,w))
        P=(w>>24)&1; U=(w>>23)&1; D=(w>>22)&1; W=(w>>21)&1; L=(w>>20)&1; rn=(w>>16)&15; vd=(w>>12)&15; imm=(w&0xff)*4
        dbl=cp==11; first=vd if dbl else (vd<<1)|D
        base=h((pc&~3)) if rn==15 else R(rn)
        if P and not W:                            # VLDR/VSTR
            ea=f'{base} {"+" if U else "-"} {h(imm)}'
            if dbl:
                if L: return T(f'{{ uint32_t ad_ = {ea}; c->s[{2*first}] = rd32(ad_); c->s[{2*first+1}] = rd32(ad_ + 4); }}')
                return T(f'{{ uint32_t ad_ = {ea}; wr32(ad_, c->s[{2*first}]); wr32(ad_ + 4, c->s[{2*first+1}]); }}')
            if L: return T(f'c->s[{first}] = rd32({ea});')
            return T(f'wr32({ea}, c->s[{first}]);')
        # VLDM/VSTM (IA, optional writeback) or DB with writeback
        words=imm//4
        if dbl: words&=~1
        sregs=[2*first+i for i in range(words)] if dbl else [first+i for i in range(words)]
        if P and U: return T(s.miss('vldm-ib',a,w))
        start='b_' if not P else f'b_ - {imm}'
        code=f'uint32_t b_ = {base}; uint32_t ad_ = {start};'
        for r in sregs: code+=(f' c->s[{r}] = rd32(ad_); ad_ += 4;' if L else f' wr32(ad_, c->s[{r}]); ad_ += 4;')
        if W: code+=f' c->r[{rn}] = b_ {"+" if U else "-"} {imm};'
        return T(f'{{ {code} }}')

    def arm_coproc(s,a,w,R):
        cp=(w>>8)&15
        if w&0x10:                                 # MCR/MRC
            L=(w>>20)&1; opc1=(w>>21)&7; crn=(w>>16)&15; rt=(w>>12)&15; opc2=(w>>5)&7; crm=w&15
            if cp==15:
                if L and opc1==0 and crn==13 and crm==0 and opc2==3: return T(f'c->r[{rt}] = c->tls;')
                if not L and opc1==0 and crn==7 and crm==10 and opc2 in (4,5): return T('__sync_synchronize();')
                return T(s.miss('cp15',a,w))
            if cp==10 and opc1==0:
                sn=(crn<<1)|((w>>7)&1)
                if L: return T(f'c->r[{rt}] = c->s[{sn}];')
                return T(f'c->s[{sn}] = {R(rt)};')
            if cp==10 and opc1==7:
                if L:
                    if crn==1 and rt==15: return T('c->n = c->fpscr >> 31; c->z = (c->fpscr >> 30) & 1; c->cf = (c->fpscr >> 29) & 1; c->v = (c->fpscr >> 28) & 1;')
                    if crn==1: return T(f'c->r[{rt}] = c->fpscr;')
                    if crn in (0,8): return T(f'c->r[{rt}] = {h(0x410120b4 if crn==0 else 0x40000000)};')   # FPSID / FPEXC
                else:
                    if crn==1: return T(f'c->fpscr = {R(rt)}; if (c->fpscr & 0x00370000u) rt_unimpl(c, {h(a)}, c->fpscr); /* vector LEN/STRIDE unsupported */')
                    if crn==8: return T('/* fpexc write ignored */')
                return T(s.miss('vmrs/vmsr',a,w))
            if cp==11 and opc1 in (0,1):
                dn=crn; idx=2*dn+opc1
                if L: return T(f'c->r[{rt}] = c->s[{idx}];')
                return T(f'c->s[{idx}] = {R(rt)};')
            return T(s.miss('mcr/mrc-other',a,w))
        if cp not in (10,11): return T(s.miss('cdp-other',a,w))
        return s.vfp_dp(a,w)

    def vfp_dp(s,a,w):
        dbl=(w>>8)&1; D=(w>>22)&1; N=(w>>7)&1; M=(w>>5)&1
        vd=(w>>12)&15; vn=(w>>16)&15; vm=w&15
        pqrs=(((w>>23)&1)<<3)|(((w>>21)&1)<<2)|(((w>>20)&1)<<1)|((w>>6)&1)
        # results are checked for NaN (rt_nan logs the first producers; cheap: one compare per op)
        if dbl: d,n,m=vd,vn,vm; G=lambda i:f'fD(c, {i})'; P=lambda i,e:f'{{ double r_ = ({e}); if (r_ != r_) rt_nan(c, {h(a)}); sD(c, {i}, r_); }}'; ty='double'
        else: d,n,m=(vd<<1)|D,(vn<<1)|N,(vm<<1)|M; G=lambda i:f'fS(c, {i})'; P=lambda i,e:f'{{ float r_ = (float)({e}); if (r_ != r_) rt_nan(c, {h(a)}); sS(c, {i}, r_); }}'; ty='float'
        if pqrs!=15:
            ops={0:f'{G(d)} + {G(n)} * {G(m)}',1:f'{G(d)} - {G(n)} * {G(m)}',2:f'-{G(d)} + {G(n)} * {G(m)}',3:f'-{G(d)} - {G(n)} * {G(m)}',
                 4:f'{G(n)} * {G(m)}',5:f'-({G(n)} * {G(m)})',6:f'{G(n)} + {G(m)}',7:f'{G(n)} - {G(m)}',8:f'{G(n)} / {G(m)}'}
            if pqrs not in ops: return T(s.miss(f'vfp pqrs={pqrs}',a,w))
            if pqrs<4:   # multiply-accumulate: product rounded first (VFP11 is not fused)
                prod=f'({ty})({G(n)} * {G(m)})'; acc=G(d)
                e={0:f'{acc} + {prod}',1:f'{acc} - {prod}',2:f'-{acc} + {prod}',3:f'-{acc} - {prod}'}[pqrs]
                return T(P(d,e))
            return T(P(d,ops[pqrs]))
        ext=(vn<<1)|N
        if ext==0: return T(P(d,G(m)))
        if ext==1: return T(P(d,f'__builtin_fabs{"" if dbl else "f"}({G(m)})'))
        if ext==2: return T(P(d,f'-{G(m)}'))
        if ext==3: return T(P(d,f'__builtin_sqrt{"" if dbl else "f"}({G(m)})'))
        if ext in (8,9): return T(f'vfp_cmp(c, {G(d)}, {G(m)});')
        if ext in (10,11): return T(f'vfp_cmp(c, {G(d)}, 0.0);')
        if ext==15:      # FCVTDS / FCVTSD
            if dbl: return T(f'sS(c, {(vd<<1)|D}, (float)fD(c, {vm}));')
            return T(f'sD(c, {vd}, (double)fS(c, {(vm<<1)|M}));')
        sm=(vm<<1)|M; sd=(vd<<1)|D
        if ext in (16,17):  # FUITO / FSITO: source is an integer in a single register
            src=f'(double)(int32_t)c->s[{sm}]' if ext==17 else f'(double)c->s[{sm}]'
            return T(P(d,src))
        if ext in (24,25,26,27):  # FTOUI(Z) / FTOSI(Z): result integer in a single register
            src=G(m); z=ext&1; sg=ext>=26
            fn=('vcvt_s32' if z else 'vcvtr_s32') if sg else ('vcvt_u32' if z else 'vcvtr_u32')
            return T(f'c->s[{sd}] = (uint32_t){fn}({src});')
        return T(s.miss(f'vfp ext={ext}',a,w))

    # ---------------- Thumb ----------------
    def thumb(s,a,entry):
        hw=s.u16(a); pc=a+4
        R=lambda n: h(pc) if n==15 else f'c->r[{n}]'
        top=hw>>11
        if top in (0,1,2):                         # shift imm
            op=hw>>11; amt=(hw>>6)&31; rm=(hw>>3)&7; rd=hw&7
            if op==0:
                if amt==0: return T(f'c->r[{rd}] = c->r[{rm}]; set_nz(c, c->r[{rd}]);',size=2)
                return T(f'{{ uint32_t v_ = c->r[{rm}]; c->cf = (v_ >> {32-amt}) & 1; c->r[{rd}] = v_ << {amt}; set_nz(c, c->r[{rd}]); }}',size=2)
            if op==1:
                a2=amt or 32
                return T(f'{{ uint32_t v_ = c->r[{rm}]; c->cf = (v_ >> {a2-1}) & 1; c->r[{rd}] = {"0" if a2==32 else f"v_ >> {a2}"}; set_nz(c, c->r[{rd}]); }}',size=2)
            a2=amt or 32
            return T(f'{{ uint32_t v_ = c->r[{rm}]; c->cf = (v_ >> {a2-1}) & 1; c->r[{rd}] = (uint32_t)((int32_t)v_ >> {min(a2,31)}); set_nz(c, c->r[{rd}]); }}',size=2)
        if top==3:                                 # add/sub reg/imm3
            I=(hw>>10)&1; sub=(hw>>9)&1; rn=(hw>>3)&7; rd=hw&7; x=(hw>>6)&7
            b=h(x) if I else f'c->r[{x}]'
            return T(f'c->r[{rd}] = {"sbc_flags" if sub else "adc_flags"}(c, c->r[{rn}], {b}, {1 if sub else 0});',size=2)
        if top in (4,5,6,7):                       # mov/cmp/add/sub imm8
            op=(hw>>11)&3; rd=(hw>>8)&7; imm=hw&0xff
            return T([f'c->r[{rd}] = {h(imm)}; set_nz(c, c->r[{rd}]);',f'sbc_flags(c, c->r[{rd}], {h(imm)}, 1);',
                      f'c->r[{rd}] = adc_flags(c, c->r[{rd}], {h(imm)}, 0);',f'c->r[{rd}] = sbc_flags(c, c->r[{rd}], {h(imm)}, 1);'][op],size=2)
        if (hw>>10)==0x10:                         # ALU
            op=(hw>>6)&15; rm=(hw>>3)&7; rd=hw&7; D=f'c->r[{rd}]'; M=f'c->r[{rm}]'
            code={0:f'{D} &= {M}; set_nz(c, {D});',1:f'{D} ^= {M}; set_nz(c, {D});',
                  2:f'{D} = lsl_rs({D}, {M}, &c->cf); set_nz(c, {D});',3:f'{D} = lsr_rs({D}, {M}, &c->cf); set_nz(c, {D});',
                  4:f'{D} = asr_rs({D}, {M}, &c->cf); set_nz(c, {D});',5:f'{D} = adc_flags(c, {D}, {M}, c->cf);',
                  6:f'{D} = sbc_flags(c, {D}, {M}, c->cf);',7:f'{D} = ror_rs({D}, {M}, &c->cf); set_nz(c, {D});',
                  8:f'set_nz(c, {D} & {M});',9:f'{D} = sbc_flags(c, 0, {M}, 1);',10:f'sbc_flags(c, {D}, {M}, 1);',
                  11:f'adc_flags(c, {D}, {M}, 0);',12:f'{D} |= {M}; set_nz(c, {D});',13:f'{D} = {D} * {M}; set_nz(c, {D});',
                  14:f'{D} &= ~{M}; set_nz(c, {D});',15:f'{D} = ~{M}; set_nz(c, {D});'}[op]
            return T(code,size=2)
        if (hw>>10)==0x11:                         # hi-reg ops / BX / BLX
            op=(hw>>8)&3; rm=(hw>>3)&15; rd=(hw&7)|((hw>>4)&8)
            if op==3:
                if hw&0x80: return T(f'{{ uint32_t t_ = {R(rm)}; c->r[14] = {h((a+2)|1)}; rt_call_indirect(c, t_, {h(a)}); }}',size=2)
                if rm==14: return T('return;',fall=False,size=2)
                return T(s.jump_ind(R(rm),a),fall=False,size=2)
            if op==0:
                if rd==15: return T(s.jump_ind(f'{R(15)} + {R(rm)}',a),fall=False,size=2)
                return T(f'c->r[{rd}] = {R(rd)} + {R(rm)};',size=2)
            if op==1: return T(f'sbc_flags(c, {R(rd)}, {R(rm)}, 1);',size=2)
            if rd==15:
                if rm==14: return T('return;',fall=False,size=2)
                return T(s.jump_ind(R(rm),a),fall=False,size=2)
            return T(f'c->r[{rd}] = {R(rm)};',size=2)
        if top==9:                                 # ldr literal
            rd=(hw>>8)&7; ad=(pc&~3)+(hw&0xff)*4
            return T(f'c->r[{rd}] = {h(s.u32(ad))};' if s.A.in_text(ad) else f'c->r[{rd}] = rd32({h(ad)});',size=2)
        if (hw>>12)==5:                            # load/store reg offset
            op=(hw>>9)&7; rm=(hw>>6)&7; rn=(hw>>3)&7; rd=hw&7; ea=f'c->r[{rn}] + c->r[{rm}]'
            code=[f'wr32({ea}, c->r[{rd}]);',f'wr16({ea}, c->r[{rd}]);',f'wr8({ea}, c->r[{rd}]);',f'c->r[{rd}] = (uint32_t)(int32_t)(int8_t)rd8({ea});',
                  f'c->r[{rd}] = rd32({ea});',f'c->r[{rd}] = rd16({ea});',f'c->r[{rd}] = rd8({ea});',f'c->r[{rd}] = (uint32_t)(int32_t)(int16_t)rd16({ea});'][op]
            return T(code,size=2)
        if (hw>>13)==3 or (hw>>12)==8:              # load/store imm offset
            if (hw>>13)==3: B=(hw>>12)&1; L=(hw>>11)&1; sz=1 if B else 4
            else: L=(hw>>11)&1; sz=2
            off=((hw>>6)&31)*sz; rn=(hw>>3)&7; rd=hw&7; ea=f'c->r[{rn}] + {off}'
            fn={1:('rd8','wr8'),2:('rd16','wr16'),4:('rd32','wr32')}[sz]
            return T(f'c->r[{rd}] = {fn[0]}({ea});' if L else f'{fn[1]}({ea}, c->r[{rd}]);',size=2)
        if (hw>>12)==9:                            # sp-relative ldr/str
            L=(hw>>11)&1; rd=(hw>>8)&7; ea=f'c->r[13] + {(hw&0xff)*4}'
            return T(f'c->r[{rd}] = rd32({ea});' if L else f'wr32({ea}, c->r[{rd}]);',size=2)
        if (hw>>12)==10:                           # add rd, pc/sp, imm
            rd=(hw>>8)&7; imm=(hw&0xff)*4
            return T(f'c->r[{rd}] = {h((pc&~3)+imm)};' if not hw&0x800 else f'c->r[{rd}] = c->r[13] + {imm};',size=2)
        if (hw>>8)==0xb0:                          # adjust sp
            imm=(hw&0x7f)*4; return T(f'c->r[13] {"-" if hw&0x80 else "+"}= {imm};',size=2)
        if (hw>>8) in (0xb2,):                     # sxth/sxtb/uxth/uxtb
            op=(hw>>6)&3; rm=(hw>>3)&7; rd=hw&7
            e=[f'(uint32_t)(int32_t)(int16_t)c->r[{rm}]',f'(uint32_t)(int32_t)(int8_t)c->r[{rm}]',f'(c->r[{rm}] & 0xffffu)',f'(c->r[{rm}] & 0xffu)'][op]
            return T(f'c->r[{rd}] = {e};',size=2)
        if (hw>>8)==0xba:                          # rev/rev16/revsh
            op=(hw>>6)&3; rm=(hw>>3)&7; rd=hw&7
            if op==0: return T(f'c->r[{rd}] = __builtin_bswap32(c->r[{rm}]);',size=2)
            if op==1: return T(f'{{ uint32_t v_ = c->r[{rm}]; c->r[{rd}] = ((v_ & 0x00ff00ffu) << 8) | ((v_ >> 8) & 0x00ff00ffu); }}',size=2)
            if op==3: return T(f'c->r[{rd}] = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)c->r[{rm}]);',size=2)
            return T(s.miss('thumb-rev',a,hw),size=2)
        if (hw>>9)==0x5a:                          # push
            regs=[i for i in range(8) if hw&(1<<i)]+([14] if hw&0x100 else [])
            n=len(regs); code=f'uint32_t ad_ = c->r[13] - {4*n};'+''.join(f' wr32(ad_ + {4*i}, c->r[{r}]);' for i,r in enumerate(regs))+' c->r[13] -= '+str(4*n)+';'
            return T(f'{{ {code} }}',size=2)
        if (hw>>9)==0x5e:                          # pop
            regs=[i for i in range(8) if hw&(1<<i)]; pcf=hw&0x100; n=len(regs)+(1 if pcf else 0)
            code='uint32_t ad_ = c->r[13];'+''.join(f' c->r[{r}] = rd32(ad_ + {4*i});' for i,r in enumerate(regs))
            if pcf: code+=f' uint32_t pc_ = rd32(ad_ + {4*len(regs)});'
            code+=f' c->r[13] += {4*n};'
            if pcf: return T(f'{{ {code} {s.ret_checked("pc_",a)} }}',fall=False,size=2)
            return T(f'{{ {code} }}',size=2)
        if (hw>>12)==12:                           # ldmia/stmia
            L=(hw>>11)&1; rn=(hw>>8)&7; regs=[i for i in range(8) if hw&(1<<i)]
            code=f'uint32_t ad_ = c->r[{rn}];'
            for r in regs: code+=(f' c->r[{r}] = rd32(ad_); ad_ += 4;' if L else f' wr32(ad_, c->r[{r}]); ad_ += 4;')
            if not (L and rn in regs): code+=f' c->r[{rn}] = ad_;'
            return T(f'{{ {code} }}',size=2)
        if (hw>>12)==13:
            cond=(hw>>8)&15
            if cond==15: return T(f'rt_svc(c, {hw&0xff}, {h(a)});',size=2)
            if cond==14: return T(s.miss('thumb-udf',a,hw),size=2)
            t=a+4+sx(hw&0xff,8)*2
            return T(f'if ({COND[cond]}) goto L_{t:08x};',[t],size=2)
        if (hw>>11)==0x1c:
            t=a+4+sx(hw&0x7ff,11)*2
            if t!=entry and t in s.A.funcs: return T(s.tail(t,a),fall=False,size=2)
            return T(f'goto L_{t:08x};',[t],fall=False,size=2)
        if (hw>>11)==0x1e:                         # BL/BLX prefix
            lo=s.u16(a+2)
            off=(sx(hw&0x7ff,11)<<12)+((lo&0x7ff)<<1)
            if lo>=0xf800: return T(s.call(a+4+off,(a+4)|1,a),size=4)
            if lo>=0xe800:
                if ((a+4+off)&~3) in s.A.switch8:          # RVCT switch8: byte table after the call, index in r3
                    ln,tg=s.A.switch8_targets(a)
                    cases=' '.join(f'case {i}: goto L_{x:08x};' for i,x in enumerate(tg[:-1]))
                    return T(f'switch (c->r[3]) {{ {cases} default: goto L_{tg[-1]:08x}; }}',tg,fall=False,size=4)
                return T(s.call((a+4+off)&~3,(a+4)|1,a),size=4)
            return T(s.miss('thumb-bl-pair',a,hw),size=2)
        if (hw>>8)==0xbe: return T(f'rt_bkpt(c, {h(a)});',size=2)
        if hw==0xbf00 or (hw>>8)==0xbf: return T('/* hint */',size=2)
        return T(s.miss(f'thumb {hw>>8:#x}',a,hw),size=2)

    # ---------------- function emission ----------------
    def function(s,entry):
        mode=s.A.funcs[entry]; tr=s.arm if mode=='A' else s.thumb
        insns={}; labels=set(); work=[entry]; fall_tail={}
        while work:
            a=work.pop()
            if a in insns or not s.A.in_text(a): continue
            t=tr(a,entry); insns[a]=t
            for x in t.succ: labels.add(x); work.append(x)
            if t.fall:
                nx=a+t.size
                # falling into another function's entry is a tail call, except into unverified entries: those are
                # usually landing points inside a function (vtable/literal targets), so keep translating inline
                if nx!=entry and nx in s.A.funcs and nx not in s.A.unverified: fall_tail[a]=nx
                elif s.A.kind.get(nx)=='D': fall_tail[a]=None
                else: work.append(nx)
        out=[f'void {s.fn(entry)}(CPU *c) {{','    const uint32_t ret_lr = c->r[14]; (void)ret_lr;']
        if entry in HOOKS: out.append(f'    {HOOKS[entry]}(c);')
        if entry in TRACE_FUNCS or entry in POST_HOOKS:   # wrap: the body runs as a separate function so the return value can be printed
            out.insert(0,f'static void {s.fn(entry)}_body(CPU *c);')
            tr0,tr1=(f'rt_trace_fn(c, {h(entry)}, 0); ',f' rt_trace_fn(c, {h(entry)}, 1);') if entry in TRACE_FUNCS else ('','')
            post=f' {POST_HOOKS[entry]}(c);' if entry in POST_HOOKS else ''
            out.append(f'    {tr0}{s.fn(entry)}_body(c);{tr1}{post} }}')
            out.append(f'static void {s.fn(entry)}_body(CPU *c) {{'); out.append('    const uint32_t ret_lr = c->r[14]; (void)ret_lr;')
        order=sorted(insns)
        if order[0]!=entry:   # code reached by branches below the entry is emitted first; start at the entry
            out.append(f'    goto L_{entry:08x};'); s.entry_not_first+=1
        for i,a in enumerate(order):
            t=insns[a]
            if a in labels or a==entry: out.append(f'L_{a:08x}:;')
            out.append(f'    {t.code}')
            if t.fall:
                nx=a+t.size
                if a in fall_tail:
                    out.append(f'    {s.tail(fall_tail[a],a)}' if fall_tail[a] else f'    rt_unimpl(c, {h(nx)}, 0); return; /* fell into data */')
                elif i+1>=len(order) or order[i+1]!=nx:
                    out.append(f'    goto L_{nx:08x};'); labels.add(nx)
        # labels referenced by gotos must exist: anything targeted but not translated (outside text) -> trap
        missing=[x for x in labels if x not in insns]
        for x in missing: out.append(f'L_{x:08x}: rt_unimpl(c, {h(x)}, 0); return;')
        out.append('}')
        return '\n'.join(out)

def main():
    code,exh,out=sys.argv[1:4]
    d,sec=F.load(code,exh); A=F.Analyzer(d,sec)
    import io,contextlib
    sys.argv=['funcs.py',code,exh]
    with contextlib.redirect_stderr(io.StringIO()): A=F.main()
    os.makedirs(out,exist_ok=True)
    tr=Translator(A); entries=sorted(A.funcs)
    hdr=['#include "cpu.h"','void rt_trace_fn(CPU *c, uint32_t fn, int ret);']+[f'void {fn}(CPU *c);' for fn in sorted(set(HOOKS.values())|set(POST_HOOKS.values()))]+[f'void f_{e:08x}(CPU *c);' for e in entries]
    open(os.path.join(out,'funcs.h'),'w').write('\n'.join(hdr)+'\n')
    per=200
    for k in range(0,len(entries),per):
        with open(os.path.join(out,f'gen_{k//per:03d}.c'),'w') as o:
            o.write('#include "funcs.h"\n\n')
            for e in entries[k:k+per]: o.write(tr.function(e)+'\n\n')
    with open(os.path.join(out,'table.c'),'w') as o:
        o.write('#include "funcs.h"\n#include "table.h"\nconst FnEntry g_fn_table[] = {\n')
        for e in entries: o.write(f'    {{ 0x{e|(1 if A.funcs[e]=="T" else 0):08x}u, f_{e:08x} }},\n')
        o.write(f'}};\nconst unsigned g_fn_count = {len(entries)};\n')
    total=sum(tr.unimpl.values())
    print(f'functions whose entry is not their lowest address (entry goto added): {tr.entry_not_first}',file=sys.stderr)
    print(f'{len(entries)} functions -> {out} ({(len(entries)+per-1)//per} files); unimplemented instruction sites: {total}',file=sys.stderr)
    for k,v in tr.unimpl.most_common(40):
        a,w=tr.unimpl_ex[k]; print(f'  {v:6}  {k:28} e.g. {a:#x}: {w:#010x}',file=sys.stderr)

if __name__=='__main__': main()
