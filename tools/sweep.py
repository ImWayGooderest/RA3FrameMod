import sys, struct, capstone
path, out = sys.argv[1], sys.argv[2]
data = open(path,'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
base = struct.unpack_from('<I', data, pe+24+28)[0]
so = pe+24+struct.unpack_from('<H', data, pe+20)[0]
vs, va, rs, ra = struct.unpack_from('<IIII', data, so+8)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata = True
with open(out,'w') as f:
    for a, sz, m, o in md.disasm_lite(data[ra:ra+vs], base+va):
        f.write(f'{a:08x} {m} {o}\n')
