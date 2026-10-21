"""mkicons.py: the postgame's gametype pictures: Halo's own from the gametype editor (ui.map's game_type_select
rules_options: CTF, King, Slayer, Oddball, Race), the picture alone (no background), cropped square, 64 x 64
A8R8G8B8 swizzled (B G R A bytes), 16 KB each"""
from PIL import Image
import numpy as np
def swz(x,y,w,h):
    r=0;o=0;m=1
    while m<w or m<h:
        if m<w:
            if x&m: r|=1<<o
            o+=1
        if m<h:
            if y&m: r|=1<<o
            o+=1
        m<<=1
    return r
out=b''; prev=[]
for i in range(5):
    im=Image.open(f'bd_rules_options_{i}.png').convert('RGBA')
    a=np.asarray(im)[:,:,3]; ys,xs=np.where(a>6)
    x0,x1,y0,y1=xs.min(),xs.max(),ys.min(),ys.max()
    side=max(x1-x0,y1-y0)+8; cx=(x0+x1)//2; cy=(y0+y1)//2
    box=(cx-side//2,cy-side//2,cx-side//2+side,cy-side//2+side)
    c=Image.new('RGBA',(side,side),(0,0,0,0)); c.paste(im.crop(box),(0,0))
    # (premultiplied resize, so the glow's edge doesn't go dark)
    p=np.asarray(c).astype(np.float32); p[:,:,:3]*=p[:,:,3:4]/255
    s=np.asarray(Image.fromarray(p.astype(np.uint8)).resize((64,64),Image.LANCZOS)).astype(np.float32)
    s[:,:,:3]=np.where(s[:,:,3:4]>0,s[:,:,:3]*255/np.maximum(s[:,:,3:4],1),0)
    s=s.clip(0,255).astype(np.uint8); prev.append(Image.fromarray(s,'RGBA'))
    buf=bytearray(64*64*4)
    for y in range(64):
        for x in range(64):
            o=swz(x,y,64,64)*4; r,g,b,al=s[y,x]; buf[o:o+4]=bytes((b,g,r,al))
    out+=bytes(buf)
open('gt_icons.bin','wb').write(out)
sheet=Image.new('RGBA',(5*80,80),(4,12,30,255))
for k,p in enumerate(prev): sheet.alpha_composite(p.resize((48,48),Image.LANCZOS),(k*80+16,16))
sheet.resize((800,160),Image.NEAREST).save('gt_sheet.png')
print(len(out))
