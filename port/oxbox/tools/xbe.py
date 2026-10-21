import struct,sys
def load(p):
    d=open(p,'rb').read()
    base=struct.unpack('<I',d[0x104:0x108])[0]
    nsec=struct.unpack('<I',d[0x11C:0x120])[0]; shdr=struct.unpack('<I',d[0x120:0x124])[0]-base
    secs=[]
    for i in range(nsec):
        o=shdr+i*0x38
        flags,va,vsz,raw,rsz,name_addr=struct.unpack('<IIIIII',d[o:o+24])
        n=d[name_addr-base:name_addr-base+16].split(b'\0')[0].decode(errors='replace')
        secs.append(dict(name=n,va=va,vsz=vsz,raw=raw,rsz=rsz,flags=flags))
    return d,base,secs
def off2va(secs,off):
    for s in secs:
        if s['raw']<=off<s['raw']+s['rsz']: return s['va']+off-s['raw'],s['name']
    return None,None
def va2off(secs,va):
    for s in secs:
        if s['va']<=va<s['va']+s['rsz']: return s['raw']+va-s['va'],s['name']
    return None,None
if __name__=='__main__':
    d,base,secs=load(sys.argv[1])
    print('base',hex(base))
    for s in secs: print('%-10s va=%08x vsz=%08x raw=%08x rsz=%08x flags=%x'%(s['name'],s['va'],s['vsz'],s['raw'],s['rsz'],s['flags']))
    for o in (0xfe329,0xfe335,0x1e80e0,0x4c64f): print(hex(o),[hex(x) if isinstance(x,int) else x for x in off2va(secs,o)])
