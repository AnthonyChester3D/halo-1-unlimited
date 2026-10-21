import struct
def c565(c): return (((c>>11)&31)*255//31, ((c>>5)&63)*255//63, (c&31)*255//31)
def decode(data,w,h):
    out=bytearray(w*h*4)
    i=0
    for by in range(0,h,4):
        for bx in range(0,w,4):
            a=struct.unpack_from('<Q',data,i)[0]; c0,c1,bits=struct.unpack_from('<HHI',data,i+8); i+=16
            p0,p1=c565(c0),c565(c1)
            pal=[p0,p1,tuple((2*x+y)//3 for x,y in zip(p0,p1)),tuple((x+2*y)//3 for x,y in zip(p0,p1))]
            for py in range(4):
                for px in range(4):
                    k=py*4+px; al=((a>>(4*k))&15)*17; col=pal[(bits>>(2*k))&3]
                    o=((by+py)*w+bx+px)*4; out[o:o+4]=bytes(col)+bytes([al])
    return bytes(out)
