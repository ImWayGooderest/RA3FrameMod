"""Tiny x86 RE helper for RA3 binaries.
usage:
  re.py <bin> dis <va> [count]        disassemble count instructions from va
  re.py <bin> fn <va>                 disassemble until ret/int3 padding
  re.py <bin> refs <va>               find instructions referencing absolute va (imm/disp)
  re.py <bin> calls <va>              find E8/E9 rel32 calls/jumps to va
  re.py <bin> rd <va> [n]             hex dump
  re.py <bin> fstart <va>             guess function start (walk back to CC/C3 padding)
"""
import sys, struct, capstone

path, cmd = sys.argv[1], sys.argv[2]
data = open(path, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
ns = struct.unpack_from('<H', data, pe + 6)[0]
base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
so = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0]
secs = []
for i in range(ns):
    name = data[so + i*40: so + i*40 + 8].rstrip(b'\0').decode()
    vs, va, rs, ra = struct.unpack_from('<IIII', data, so + i*40 + 8)
    secs.append((name, base + va, vs, ra, rs))
text = next(s for s in secs if s[0] == '.text')

def off(va):
    for n, v, vs, ra, rs in secs:
        if v <= va < v + rs:
            return ra + va - v
    raise ValueError(hex(va))

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = False

def dis(va, count=40, stop_ret=False):
    o = off(va)
    n = 0
    for ins in md.disasm(data[o:o+0x4000], va):
        print(f'{ins.address:08x}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}')
        n += 1
        if stop_ret and ins.mnemonic in ('ret', 'int3'):
            break
        if not stop_ret and n >= count:
            break

a = lambda i: int(sys.argv[i], 16)
if cmd == 'dis':
    dis(a(3), int(sys.argv[4]) if len(sys.argv) > 4 else 40)
elif cmd == 'fn':
    dis(a(3), 100000, True)
elif cmd == 'rd':
    o = off(a(3)); n = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    print(data[o:o+n].hex(' '))
elif cmd in ('refs', 'calls'):
    tgt = a(3)
    _, tv, tvs, tra, trs = text
    seg = data[tra:tra+trs]
    hits = set()
    if cmd == 'refs':
        pat = struct.pack('<I', tgt)
        i = seg.find(pat)
        while i != -1:
            # try to decode an instruction that contains this dword, starting up to 8 bytes before
            cands = []
            for back in range(1, 9):
                st = i - back
                ins = next(md.disasm(seg[st:st+16], tv + st), None)
                if ins and st + ins.size >= i + 4 and st + ins.size <= i + 4 + 4 and hex(tgt) in ins.op_str:
                    score = (('[' in ins.op_str) or ins.mnemonic in ('push', 'mov')) * 100 + back
                    cands.append((score, ins))
            if cands:
                ins = max(cands, key=lambda c: c[0])[1]
                hits.add((ins.address, ins.mnemonic + ' ' + ins.op_str))
            i = seg.find(pat, i + 1)
    else:
        for i in range(len(seg) - 5):
            if seg[i] in (0xE8, 0xE9):
                rel = struct.unpack_from('<i', seg, i + 1)[0]
                if tv + i + 5 + rel == tgt:
                    hits.add((tv + i, ('call' if seg[i] == 0xE8 else 'jmp') + f' {tgt:#x}'))
    for h in sorted(hits):
        print(f'{h[0]:08x}  {h[1]}')
elif cmd == 'fstart':
    va = a(3); o = off(va)
    while o > 0 and not (data[o-1] == 0xCC and data[o-2] == 0xCC):
        o -= 1
    print(hex(va - (off(va) - o)))
