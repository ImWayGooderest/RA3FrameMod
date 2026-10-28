import sys,re,struct,bisect
asm=[l.rstrip('\n') for l in open(sys.argv[1])]
addrs=[int(l[:8],16) for l in asm]
data=open(sys.argv[2],'rb').read()
def s_at(va):
    o=va-0x400000
    if not (0x7ce000<=o<0x8de000): return None
    e=o
    # ascii
    while e<len(data) and 32<=data[e]<127: e+=1
    if e-o>=4 and data[e]==0: return data[o:e].decode()
    # utf16
    e=o
    while e+1<len(data) and 32<=data[e]<127 and data[e+1]==0: e+=2
    if (e-o)//2>=4: return 'L"'+data[o:e:2].decode()+'"'
    return None
def func_bounds(i):
    s=i
    while s>0 and not (asm[s-1].endswith('int3 ') or asm[s-1].split(' ',1)[1].startswith('int3')): s-=1
    e=i
    while e<len(asm)-1 and not asm[e].split(' ',1)[1].startswith('int3'): e+=1
    return s,e
target=sys.argv[3]
seen=set()
for i,l in enumerate(asm):
    if target in l:
        s,e=func_bounds(i)
        if s in seen: continue
        seen.add(s)
        strs=[];calls=0
        for j in range(s,e):
            for m in re.findall(r'0x([0-9a-f]{6,8})',asm[j]):
                st=s_at(int(m,16))
                if st and st not in strs: strs.append(st)
            if ' call ' in asm[j]: calls+=1
        use=' | '.join(x.split(' ',1)[1] for x in asm[i:i+6])
        print(f'fn {asm[s][:8]} len={e-s} calls={calls} :: {use[:150]}')
        if strs: print('    strs:',strs[:8])
