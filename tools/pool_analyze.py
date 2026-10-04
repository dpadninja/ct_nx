#!/usr/bin/env python3
"""pool_analyze.py -- generate source/pool_sites.h for the reuse_draw_objects option.

Usage:  python3 tools/pool_analyze.py path/to/libchrono.so source/pool_sites.h [-v]
Needs:  llvm-nm, llvm-objdump, c++filt on PATH; pip: capstone, pyelftools

For every call to Sprite::createWithTexture(tex, rect, bool) and
RenderTexture::create(int, int) in game code, the calling function is
abstractly interpreted from the call onward (all paths, registers and stack
slots tracked) to follow the returned object. A site is accepted only if, on
every path, the object is
  - never stored outside the stack frame, passed as a non-`this` argument
    (also not via a pointer into a stack slot holding it), returned, or
    written into directly;
  - only used through cocos2d Node/Sprite/RenderTexture methods that draw,
    read, or set a property in a group the runtime can reset to the value a
    freshly created object has;
  - for render textures: first used through beginWithClear (a full clear),
    so reusing last frame's target is indistinguishable from a new one; and
    its inner sprite is only read or given resettable properties.
Everything else (UI built once and kept, objects handed to other code, ...)
is rejected. Factory PLT addresses are those of libchrono 2.1.5.
"""
import os, sys, re, json, subprocess, tempfile

if len(sys.argv) < 3:
    sys.exit(__doc__)
LIB = os.path.abspath(sys.argv[1]); OUT = os.path.abspath(sys.argv[2])
VERBOSE = '-v' in sys.argv[3:]
tmp = tempfile.mkdtemp(prefix='pool_analyze_'); os.chdir(tmp)

def run(*a, inp=None):
    return subprocess.run(a, input=inp, capture_output=True, text=True, check=True).stdout

# symbols (demangled)
nm = run('llvm-nm', '-D', '--defined-only', LIB)
open('syms.txt', 'w').write(run('c++filt', inp=nm))
# disassembly (for call-site discovery and PLT names)
open('all.s', 'w').write(run('llvm-objdump', '-d', '--no-show-raw-insn', LIB))
plt = {}
for l in open('all.s'):
    m = re.search(r'\bbl?\s+0x([0-9a-f]+) <(_Z[^>@]*)@plt>', l)
    if m: plt[int(m.group(1), 16)] = m.group(2)
names = sorted(set(plt.values()))
dm = dict(zip(names, run('c++filt', inp='\n'.join(names)).splitlines()))
json.dump({hex(k): dm[v] for k, v in plt.items()}, open('plt.json', 'w'))
# vtables of Sprite and RenderTexture (slot -> method)
from elftools.elf.elffile import ELFFile
f = ELFFile(open(LIB, 'rb')); dyn = f.get_section_by_name('.dynsym')
rel = {}
for sec in f.iter_sections():
    if sec.name == '.rela.dyn':
        for r in sec.iter_relocations():
            t = r['r_info_type']
            if t == 1027: rel[r['r_offset']] = r['r_addend']
            elif t == 257:
                sym = dyn.get_symbol(r['r_info_sym'])
                rel[r['r_offset']] = sym['st_value'] + r['r_addend'] if sym['st_shndx'] != 'SHN_UNDEF' else 'U:' + sym.name
addr2name = {}
syms = {}
for s in dyn.iter_symbols():
    syms[s.name] = s
    if s['st_value']: addr2name.setdefault(s['st_value'], s.name)
vts = {}
for vtname in ('_ZTVN7cocos2d6SpriteE', '_ZTVN7cocos2d13RenderTextureE'):
    s = syms[vtname]; base = s['st_value']; m = {}
    for off in range(0, s['st_size'] - 0x10, 8):
        v = rel.get(base + 0x10 + off)
        if v is None: continue
        m[off] = addr2name.get(v, str(v)) if isinstance(v, int) else v
    d = run('c++filt', inp='\n'.join(m.values())).splitlines()
    vts[vtname] = {hex(k): x for k, x in zip(m.keys(), d)}
json.dump(vts, open('vtables.json', 'w'))

import json, bisect, re, sys, collections
import capstone
from capstone.arm64 import *

data = open(LIB, 'rb').read()
plt = {int(k, 16): v for k, v in json.load(open('plt.json')).items()}
vt = json.load(open('vtables.json'))
VT = {'sprite': {int(k, 16): v for k, v in vt['_ZTVN7cocos2d6SpriteE'].items()},
      'rt': {int(k, 16): v for k, v in vt['_ZTVN7cocos2d13RenderTextureE'].items()}}

funcs = []
for l in open('syms.txt'):
    p = l.split(' ', 2)
    if len(p) == 3 and p[1] in 'TtWw':
        funcs.append((int(p[0], 16), p[2].strip()))
funcs.sort()
starts = sorted(set(a for a, _ in funcs))
fname = {}
for a, n in funcs: fname.setdefault(a, n)
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    s = starts[i]; e = starts[i + 1] if i + 1 < len(starts) else s + 0x10000
    return s, e, fname[s]

# .text is mapped at file offset == vaddr for this lib (checked: .text 0x550870 @ 0x550870)
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True
_cache = {}
def decode(s, e):
    if (s, e) in _cache: return _cache[(s, e)]
    ins = {i.address: i for i in md.disasm(data[s:e], s)}
    _cache[(s, e)] = ins
    return ins

FACT_SPRITE = 0xb3eff0
FACT_RT = 0xb3fb10

# --- method classification ------------------------------------------------
GROUPS = [
    ('POS',    r'::setPosition(X|Y|Z)?\(|::setPosition\('),
    ('SCALE',  r'::setScale(X|Y)?\('),
    ('ROT',    r'::setRotation(SkewX|SkewY)?\(float'),
    ('SKEW',   r'::setSkew(X|Y)\('),
    ('ANCHOR', r'::setAnchorPoint\('),
    ('VIS',    r'::setVisible\('),
    ('FLIP',   r'::setFlipped(X|Y)\('),
    ('OPAC',   r'::setOpacity\(unsigned char'),
    ('COLOR',  r'::setColor\(cocos2d::Color3B'),
    ('BLEND',  r'::setBlendFunc\(|::setOpacityModifyRGB\('),
    ('SHADER', r'::setGLProgram(State)?\('),
    ('GZ',     r'::setGlobalZOrder\('),
    ('IGNANCH',r'::setIgnoreAnchorPointForPosition\('),
    ('TEX',    r'Sprite::setTexture\(cocos2d::Texture2D\*\)|Sprite::setTextureRect\(|Sprite::setContentSize\('),
    ('HARMLESS', r'::setTag\(|::setName\(|::setLocalZOrder\(|::setZOrder\('),
]
RT_OK = re.compile(r'RenderTexture::(begin\(\)|beginWithClear\(|end\(\)|clear\(|getSprite\(|draw\(|visit\()')
SAFE_READ = re.compile(r'::(get|is)[A-Z]\w*\(|::visit\(|::draw\(|Sprite::draw\(')

def classify(name, kind):
    if not re.match(r'(CHILD:)?cocos2d::(Node|Sprite|RenderTexture)::', name):
        return None
    if kind == 'rt' and RT_OK.search(name):
        return 'RTOP'
    for g, rx in GROUPS:
        if re.search(rx, name): return g
    if SAFE_READ.search(name): return 'READ'
    return None

ARG = [ARM64_REG_X0 + i for i in range(8)]
X = lambda r: r  # capstone reg ids

def regname(r): return md.reg_name(r)
def norm(r):
    n = md.reg_name(r)
    if n and n[0] == 'w': return 'x' + n[1:]
    return n

def analyze(site, kind):
    s, e, fn = func_of(site)
    ins = decode(s, e)
    if site not in ins: return None, 'nodecode', fn
    groups = set(); reasons = []
    # state: frozenset of (loc, tag); loc = 'x19' or ('sp',off)
    start = (site + 4, frozenset({('x0', 'O')}))
    work = [start]; seen = set(); steps = 0
    while work:
        addr, st = work.pop()
        if (addr, st) in seen: continue
        seen.add((addr, st)); steps += 1
        if steps > 20000: return None, 'too-complex', fn
        st = dict(st)
        while True:
            i = ins.get(addr)
            if i is None:
                if s <= addr < e: return None, 'undecodable', fn
                break  # fell out of function (tail fallthrough)
            m = i.mnemonic; ops = i.operands
            tagged = {k for k, v in st.items() if v in ('O',)}
            def t(r): return st.get(norm(r)) if r else None
            def setr(r, v):
                k = norm(r)
                if v is None: st.pop(k, None)
                else: st[k] = v
            nxt = addr + 4
            # ---- calls ----
            if m in ('bl', 'blr', 'b', 'br') or (m == 'b' and ops):
                pass
            if m == 'bl' or m == 'blr':
                if m == 'bl':
                    tgt = ops[0].imm
                    name = plt.get(tgt)
                    if tgt in (FACT_SPRITE, FACT_RT) and (FACT_SPRITE if kind == 'sprite' else FACT_RT) == tgt and addr == site:
                        name = None
                else:
                    tv = t(ops[0].reg)
                    name = None
                    if isinstance(tv, tuple) and tv[0] == 'SLOT':
                        name = VT[kind].get(tv[1], '?slot%x' % tv[1])
                    elif isinstance(tv, tuple) and tv[0] == 'CSLOT':
                        name = VT['sprite'].get(tv[1], '?cslot%x' % tv[1]); name = 'CHILD:' + name
                x0 = st.get('x0')
                # a pointer into our own frame passed to a call: if any stack slot
                # holds the object, the callee could pick it up from there
                for a in range(0, 9):
                    pv = st.get('x%d' % a)
                    if not (isinstance(pv, tuple) and pv[0] == 'SPADDR'): continue
                    for k, v in st.items():
                        if isinstance(k, tuple) and v in ('O', 'INNER', 'CHILD'):
                            # callee may read forward from the pointer; a slot
                            # below it (same base) is out of reach
                            if k[0] != pv[1] or k[1] >= pv[2]:
                                return None, 'stack-ptr-escape@%x' % addr, fn
                for a in range(1, 8):
                    if st.get('x%d' % a) in ('O', 'INNER', 'CHILD'):
                        # passing object (or pointer into it) as an argument
                        return None, 'escape-arg %s@%x' % (name or '?', addr), fn
                # a no-argument function of an unrelated class (e.g. a static
                # getInstance()) can't receive the object, whatever x0 holds
                if name and name.endswith('()') and not re.match(
                        r'(CHILD:)?cocos2d::(Node|Sprite|RenderTexture|Ref|BlendProtocol|TextureProtocol)::', name):
                    x0 = None
                if x0 == 'O':
                    if name is None: return None, 'unknown-call-on-obj@%x' % addr, fn
                    g = classify(name, kind)
                    if g is None: return None, 'method %s@%x' % (name, addr), fn
                    if g not in ('READ', 'RTOP', 'HARMLESS'): groups.add(g)
                    if kind == 'rt' and not st.get('clr'):
                        # a reused target still holds last frame's pixels; it is
                        # only equivalent to a fresh (zeroed) one if the first
                        # thing the site does with its contents is a full clear
                        if 'beginWithClear' in name:
                            st['clr'] = True
                        elif g in ('READ', 'RTOP') and 'getSprite' not in name:
                            return None, 'used-before-clear %s@%x' % (name, addr), fn
                    if kind == 'rt' and 'getSprite' in name:
                        ret_tag = 'CHILD'
                    else:
                        ret_tag = None
                elif x0 == 'CHILD' and name is not None and False:
                    pass
                elif x0 == 'CHILD':
                    nm = name or ''
                    if kind == 'rt' and not st.get('clr') and re.search(r'::(visit|draw|getTexture)\(', nm):
                        return None, 'child-used-before-clear %s@%x' % (nm, addr), fn
                    if not re.match(r'(CHILD:)?cocos2d::(Node|Sprite)::(get|is)[A-Z]\w*\(|(CHILD:)?cocos2d::(Node|Sprite)::visit\(', nm):
                        g = classify(nm[6:] if nm.startswith('CHILD:') else nm, 'sprite')
                        if g in ('POS', 'SCALE', 'ANCHOR', 'FLIP', 'OPAC', 'COLOR', 'BLEND'):
                            groups.add('C_' + g)
                        else:
                            return None, 'child-method %s@%x' % (nm, addr), fn
                    ret_tag = None
                elif x0 == 'INNER':
                    return None, 'inner-ptr-call@%x' % addr, fn
                else:
                    ret_tag = None
                # clobber caller-saved
                for r in list(st):
                    if isinstance(r, str) and r.startswith('x'):
                        n = int(r[1:]) if r[1:].isdigit() else 99
                        if n <= 18: del st[r]
                if ret_tag: st['x0'] = ret_tag
                # re-entry of same site in a loop: new object in x0
                if m == 'bl' and addr == site:
                    st['x0'] = 'O'; st.pop('clr', None)
                addr = nxt; continue
            if m == 'ret':
                if st.get('x0') in ('O', 'INNER', 'CHILD'):
                    return None, 'returned@%x' % addr, fn
                break
            if m in ('b', 'br') and not i.cc in (0, ARM64_CC_AL, ARM64_CC_INVALID):
                pass
            if m == 'br':
                for a in range(0, 8):
                    if st.get('x%d' % a) in ('O', 'INNER', 'CHILD'):
                        return None, 'tail-indirect@%x' % addr, fn
                break
            if m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                tgt = ops[-1].imm
                cond = m != 'b'
                if not (s <= tgt < e):
                    # tail call
                    for a in range(0, 8):
                        if st.get('x%d' % a) in ('O', 'INNER', 'CHILD'):
                            return None, 'tailcall@%x' % addr, fn
                    if not cond: break
                else:
                    work.append((tgt, frozenset(st.items())))
                if not cond: break
                addr = nxt; continue
            # ---- data movement ----
            if m == 'mov' and len(ops) == 2 and ops[1].type == ARM64_OP_REG:
                setr(ops[0].reg, t(ops[1].reg)); addr = nxt; continue
            if m in ('ldr', 'ldur', 'ldp', 'ldrb', 'ldrh', 'ldrsw', 'ldrsb', 'ldrsh', 'ldnp'):
                memop = [o for o in ops if o.type == ARM64_OP_MEM][0]
                base = norm(memop.mem.base); off = memop.mem.disp
                btag = st.get(base)
                dsts = [o.reg for o in ops if o.type == ARM64_OP_REG]
                if base in ('sp', 'x29'):
                    for k, d in enumerate(dsts):
                        setr(d, st.get((base, off + 8 * k)))
                elif btag == 'O' and m in ('ldr', 'ldur') and norm(dsts[0]).startswith('x'):
                    setr(dsts[0], 'VT' if off == 0 else ('CHILD' if kind == 'rt' else 'FIELD'))
                elif btag == 'VT' and m in ('ldr', 'ldur'):
                    setr(dsts[0], ('SLOT', off))
                elif btag == 'CHILD' and off == 0 and m in ('ldr', 'ldur'):
                    setr(dsts[0], 'CVT')
                elif btag == 'CVT' and m in ('ldr', 'ldur'):
                    setr(dsts[0], ('CSLOT', off))
                else:
                    for d in dsts: setr(d, None)
                # writeback
                if memop.mem.base and i.writeback: setr(memop.mem.base, st.get(base) if base not in ('sp','x29') else None) if False else None
                addr = nxt; continue
            if m in ('str', 'stur', 'stp', 'strb', 'strh', 'stnp', 'sturb', 'sturh'):
                memop = [o for o in ops if o.type == ARM64_OP_MEM][0]
                base = norm(memop.mem.base); off = memop.mem.disp
                srcs = [o.reg for o in ops if o.type == ARM64_OP_REG]
                if base in ('sp', 'x29'):
                    for k, sr in enumerate(srcs):
                        v = st.get(norm(sr)) if norm(sr) and norm(sr).startswith('x') and m in ('str','stur','stp') else None
                        key = (base, off + 8 * k)
                        if v is None: st.pop(key, None)
                        else: st[key] = v
                else:
                    if st.get(base) in ('O', 'INNER', 'CHILD'):
                        return None, 'field-write@%x' % addr, fn
                    for sr in srcs:
                        if st.get(norm(sr)) in ('O', 'INNER', 'CHILD'):
                            return None, 'store-escape@%x' % addr, fn
                addr = nxt; continue
            # address of a stack slot
            if m in ('add', 'sub') and len(ops) == 3 and ops[1].type == ARM64_OP_REG and norm(ops[1].reg) in ('sp', 'x29') and ops[2].type == ARM64_OP_IMM:
                setr(ops[0].reg, ('SPADDR', norm(ops[1].reg), ops[2].imm if m == 'add' else -ops[2].imm)); addr = nxt; continue
            # any other instruction: derived pointers / clobbers
            regs_read, regs_write = i.regs_access()
            readtags = [st.get(norm(r)) for r in regs_read if norm(r)]
            for r in regs_write:
                k = norm(r)
                if k is None or not k.startswith('x'): continue
                if any(v in ('O', 'INNER') for v in readtags) and m in ('add', 'sub'):
                    st[k] = 'INNER'
                else:
                    st.pop(k, None)
            addr = nxt
    return groups, 'ok', fn

def sites_of(fact):
    out = []
    for a, i in []: pass
    for l in open('all.s'):
        mm = re.match(r'\s+([0-9a-f]+):\s+bl\s+0x([0-9a-f]+)', l)
        if mm and int(mm.group(2), 16) == fact:
            out.append(int(mm.group(1), 16))
    return out

res = []
for fact, kind in ((FACT_SPRITE, 'sprite'), (FACT_RT, 'rt')):
    for site in sites_of(fact):
        s, e, fn = func_of(site)
        if fn.startswith('cocos2d::'): continue
        g, why, fn = analyze(site, kind)
        res.append({'site': site, 'kind': kind, 'func': fn.split('(')[0], 'ok': why == 'ok',
                    'why': why, 'groups': sorted(g) if g is not None else None})
json.dump(res, open('sites.json', 'w'), indent=1)
c = collections.Counter((r['kind'], r['ok']) for r in res)
print(c)
if VERBOSE:
    for r in res:
        print('%-6s %s 0x%x %-45s %s %s' % (r['kind'], 'OK ' if r['ok'] else '-- ', r['site'], r['func'][:45], r['why'] if not r['ok'] else '', r['groups'] or ''))


# ---- emit pool_sites.h ----
BITS = {'POS','SCALE','ROT','SKEW','ANCHOR','VIS','FLIP','OPAC','COLOR','BLEND','SHADER','GZ','IGNANCH','TEX',
        'C_POS','C_SCALE','C_ANCHOR','C_FLIP','C_OPAC','C_COLOR','C_BLEND'}
rows = []
for x in sorted((r for r in res if r['ok']), key=lambda r: r['site']):
    assert set(x['groups']) <= BITS, x
    mask = ' | '.join('R_' + g for g in x['groups']) or '0'
    rows.append('  { 0x%06x, %s, %s }, // %s' % (x['site'], 'K_RT' if x['kind'] == 'rt' else 'K_SPRITE', mask, x['func']))
hdr = """/* pool_sites.h -- GENERATED by tools/pool_analyze.py from libchrono.so 2.1.5. Do not edit.
 *
 * Call sites of Sprite::createWithTexture(tex, rect, bool) and
 * RenderTexture::create(int, int) whose result was proven (by abstract
 * interpretation of the calling function) to be used only within the frame:
 * never stored outside the stack frame, never passed as an argument, never
 * returned, fields never written, and only drawn, read, or changed through
 * setters whose effect sprite_pool.c can undo. The mask lists the property
 * groups the site may change, which are reset to their freshly-created
 * values when a pooled object is handed out again.
 */
"""
open(OUT, 'w').write(hdr + '\n'.join(rows) + '\n')
print('%d sites written to %s' % (len(rows), OUT))
