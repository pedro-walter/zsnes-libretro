# Object loading, layout and symbol resolution for the x86->C lifter.
import os, sys
from elftools.elf.elffile import ELFFile

R_386_32 = 1
R_386_PC32 = 2
CODE_SHIFT = 21                 # code id = (objidx+1) << 21 | text offset
EXT_OBJ = 0x7F                  # pseudo object holding foreign (C) functions
SENT_OBJ = 0x7E                 # return-to-C sentinel
ARENA_BASE = 0x30000000
ARENA_SIZE = 0x10000000
DATA_BASE = ARENA_BASE + 0x10000

def align(x, a):
    a = max(a, 1)
    return (x + a - 1) // a * a

class Obj:
    pass

def load_obj(path, idx, root):
    o = Obj()
    o.path = path
    o.idx = idx
    o.name = os.path.relpath(path, root)
    o.cname = o.name.replace('/', '_').replace('.', '_')
    e = ELFFile(open(path, 'rb'))
    secs = list(e.iter_sections())
    byname = {s.name: s for s in secs}
    t = byname.get('.text'); d = byname.get('.data'); b = byname.get('.bss')
    o.text = t.data() if t is not None else b''
    o.data = bytearray(d.data()) if d is not None else bytearray()
    o.data_align = d['sh_addralign'] if d is not None else 1
    o.bss_size = b['sh_size'] if b is not None else 0
    o.bss_align = b['sh_addralign'] if b is not None else 1
    o.syms = []
    st = byname['.symtab']
    names = {i: s.name for i, s in enumerate(secs)}
    for s in st.iter_symbols():
        sh = s['st_shndx']
        if isinstance(sh, int):
            sec = names.get(sh, '?')
        else:
            sec = {'SHN_UNDEF': 'UND', 'SHN_ABS': 'ABS', 'SHN_COMMON': 'COM'}.get(sh, sh)
        info = s['st_info']
        o.syms.append(dict(name=s.name, sec=sec, value=s['st_value'], bind=info['bind'], type=info['type'], size=s['st_size']))
    o.rel_text = []
    o.rel_data = []
    for nm, lst in (('.rel.text', o.rel_text), ('.rel.data', o.rel_data)):
        rs = byname.get(nm)
        if rs is None: continue
        for r in rs.iter_relocations():
            lst.append((r['r_offset'], r['r_info_sym'], r['r_info_type']))
    return o

class Prog:
    pass

def build_prog(obj_paths, root, skip_text, cobjs, extra_foreign_funcs):
    P = Prog()
    P.objs = [load_obj(p, i, root) for i, p in enumerate(obj_paths)]
    P.skip_text = skip_text
    addr = DATA_BASE
    for o in P.objs:
        o.data_addr = align(addr, max(o.data_align, 4)); addr = o.data_addr + len(o.data)
        o.bss_addr = align(addr, max(o.bss_align, 4)); addr = o.bss_addr + o.bss_size
    P.data_end = addr
    P.gtext = {}; P.gdata = {}
    for o in P.objs:
        for s in o.syms:
            if s['bind'] != 'STB_GLOBAL' or s['sec'] in ('UND', 'ABS', 'COM'): continue
            if s['sec'] == '.text':
                if s['name'] in P.gtext: print('dup text', s['name'], file=sys.stderr)
                P.gtext[s['name']] = (o.idx, s['value'])
            elif s['sec'] == '.data':
                P.gdata[s['name']] = o.data_addr + s['value']
            elif s['sec'] == '.bss':
                P.gdata[s['name']] = o.bss_addr + s['value']
    # C objects: foreign definitions
    P.cfunc = set(extra_foreign_funcs); P.cvar = {}; P.cvar_init = {}
    for p in cobjs:
        e = ELFFile(open(p, 'rb'))
        secs = list(e.iter_sections())
        for s in e.get_section_by_name('.symtab').iter_symbols():
            sh = s['st_shndx']
            if not isinstance(sh, int) or s['st_info']['bind'] == 'STB_LOCAL': continue
            if s['st_info']['type'] == 'STT_FUNC': P.cfunc.add(s.name)
            elif s['st_info']['type'] in ('STT_OBJECT', 'STT_NOTYPE', 'STT_TLS'):
                P.cvar[s.name] = max(s['st_size'], 4)
                if secs[sh].name == '.data':
                    P.cvar_init[s.name] = bytes(secs[sh].data()[s['st_value']:s['st_value'] + s['st_size']])
    P.foreign_funcs = {}      # name -> index
    P.foreign_vars = {}       # name -> arena address
    P.missing = {}            # name -> index (text in skipped objects / unresolved)
    P.data_end = align(P.data_end, 32)
    return P

def alloc_var(P, name):
    if name in P.foreign_vars: return P.foreign_vars[name]
    sz = P.cvar.get(name, 4)
    a = align(P.data_end, 32 if sz >= 32 else 8)
    P.foreign_vars[name] = a
    P.data_end = a + sz
    return a

def code_id(objidx, off):
    return ((objidx + 1) << CODE_SHIFT) | off

def ext_id(name, P):
    if name not in P.foreign_funcs: P.foreign_funcs[name] = len(P.foreign_funcs)
    return (EXT_OBJ << CODE_SHIFT) | P.foreign_funcs[name]

def missing_id(name, P):
    if name not in P.missing: P.missing[name] = len(P.missing)
    return (0x7D << CODE_SHIFT) | P.missing[name]

def resolve(P, o, symidx):
    """returns ('d', addr) | ('t', code id) | ('x', code id of foreign fn)"""
    s = o.syms[symidx]
    sec = s['sec']
    if s['type'] == 'STT_SECTION':
        if sec == '.text': return ('t', code_id(o.idx, 0), o.idx, 0)
        if sec == '.data': return ('d', o.data_addr)
        if sec == '.bss': return ('d', o.bss_addr)
        raise Exception('section sym ' + sec)
    if sec == '.text':
        return ('t', code_id(o.idx, s['value']), o.idx, s['value'])
    if sec == '.data': return ('d', o.data_addr + s['value'])
    if sec == '.bss': return ('d', o.bss_addr + s['value'])
    if sec == 'ABS': return ('d', s['value'])
    if sec == 'UND':
        n = s['name']
        if n in P.gtext:
            oi, off = P.gtext[n]
            return ('t', code_id(oi, off), oi, off)
        if n in P.gdata: return ('d', P.gdata[n])
        if n in P.cvar and n not in P.cfunc: return ('d', alloc_var(P, n))
        if n in P.cfunc or n in ('malloc', 'free', 'calloc', 'realloc', 'memcpy', 'memset', 'strlen', 'printf', 'puts', 'putchar', 'getchar', 'exit'):
            return ('x', ext_id(n, P), n)
        return ('m', missing_id(n, P), n)
    raise Exception('bad sym %r' % s)
