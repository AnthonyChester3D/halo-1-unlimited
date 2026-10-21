import numpy as np, struct
def enc565(c): return ((int(c[0])*31+127)//255<<11)|((int(c[1])*63+127)//255<<5)|((int(c[2])*31+127)//255)
def dec565(v): return np.array([((v>>11)&31)*255//31, ((v>>5)&63)*255//63, (v&31)*255//31],np.float32)
def encode(img):   # img HxWx4 uint8
    h,w,_=img.shape; out=bytearray()
    for by in range(0,h,4):
        for bx in range(0,w,4):
            blk=img[by:by+4,bx:bx+4].reshape(16,4).astype(np.float32)
            a=0
            for k in range(16): a|=(int(round(blk[k,3]/17)))<<(4*k)
            rgb=blk[:,:3]; wts=blk[:,3]
            if wts.max()==0: wts=np.ones(16)
            # endpoints: weighted min/max along luminance
            lum=rgb@np.array([.3,.59,.11])
            m=wts>0
            lo=rgb[m][lum[m].argmin()]; hi=rgb[m][lum[m].argmax()]
            c0,c1=enc565(hi),enc565(lo)
            if c0<c1: c0,c1=c1,c0
            if c0==c1:
                bits=0
            else:
                p0,p1=dec565(c0),dec565(c1)
                pal=[p0,p1,(2*p0+p1)/3,(p0+2*p1)/3]
                bits=0
                for k in range(16):
                    d=[((rgb[k]-p)**2).sum() for p in pal]
                    bits|=int(np.argmin(d))<<(2*k)
            out+=struct.pack('<QHHI',a,c0,c1,bits)
    return bytes(out)
