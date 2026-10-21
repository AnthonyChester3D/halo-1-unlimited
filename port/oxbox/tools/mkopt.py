from PIL import Image
import numpy as np
# word -> letter cores (x0,x1 inclusive)
cores={'set':[(60,78),(80,96),(97,113),(115,131),(133,137),(141,158),(161,179),(181,199)],
       'camp':[(53,69),(70,89),(91,113),(117,134),(135,155),(157,161),(164,181),(185,202)],
       'demo':[(126,143),(146,161),(165,186),(189,209),(210,228)]}
pick=[('demo',3),('camp',3),('set',2),('set',4),('demo',3),('set',5),('set',7)]   # O P T I O N S
GAP=3
def compose(suffix):
    src={k:np.asarray(Image.open(f'mm_{k}{suffix}.png').convert('RGBA')).astype(np.float32) for k in cores}
    slices=[]
    for word,i in pick:
        c=cores[word]; x0,x1=c[i]
        left=(c[i-1][1]+x0)//2+1 if i>0 else x0-8
        right=(x1+c[i+1][0])//2 if i+1<len(c) else x1+8
        slices.append((src[word][:,max(left,0):right+1], x0-max(left,0), x1-x0+1))
    total=sum(w for _,_,w in slices)+GAP*(len(slices)-1)
    out=np.zeros((64,256,4),np.float32)
    x=(256-total)//2
    for sl,off,w in slices:
        dx=x-off
        for cx in range(sl.shape[1]):
            tx=dx+cx
            if 0<=tx<256:
                a=sl[:,cx,3:4]; o=out[:,tx]
                # premultiplied 'over' with max alpha
                na=np.maximum(o[:,3:4],a)
                rgb=np.where(a>o[:,3:4], sl[:,cx,:3], o[:,:3])
                out[:,tx,:3]=rgb[:,:]; out[:,tx,3:4]=na
        x+=w+GAP
    return out.clip(0,255).astype(np.uint8)
for s in ['','_h']:
    Image.fromarray(compose(s),'RGBA').save(f'opt{s}.png')
sheet=Image.new('RGBA',(256,128),(40,40,40,255))
for k,s in enumerate(['','_h']):
    bg=Image.new('RGBA',(256,64),(40,40,40,255)); bg.alpha_composite(Image.open(f'opt{s}.png')); sheet.paste(bg,(0,64*k))
sheet.resize((768,384),Image.NEAREST).save('opt_sheet.png')

# highlighted: white letters from the highlighted slices, the glow made again around them (a blur, as the originals')
from PIL import ImageFilter
def compose_h(blur, gain):
    src=np.asarray(Image.open('mm_set_h.png').convert('RGBA'))
    hs={k:np.asarray(Image.open(f'mm_{k}_h.png').convert('RGBA')).astype(np.float32) for k in cores}
    cov=np.zeros((64,256),np.float32)
    total=sum(cores[w][i][1]-cores[w][i][0]+1 for w,i in pick)+GAP*(len(pick)-1)
    x=(256-total)//2
    for word,i in pick:
        x0,x1=cores[word][i]
        sl=hs[word][:,x0:x1+1]
        c=np.clip((sl[:,:,0]-41)/(255-41),0,1)       # whiteness = the letter's coverage
        for cx in range(sl.shape[1]):
            tx=x+cx
            if 0<=tx<256: cov[:,tx]=np.maximum(cov[:,tx],c[:,cx])
        x+=x1-x0+1+GAP
    g=np.asarray(Image.fromarray((cov*255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))).astype(np.float32)/255
    ga=np.clip(g*gain,0,153/255)
    out=np.zeros((64,256,4),np.float32)
    blue=np.array([38,149,255],np.float32); white=np.array([255,255,255],np.float32)
    out[:,:,:3]=blue*(1-cov[:,:,None])+white*cov[:,:,None]
    out[:,:,3]=np.maximum(ga,cov)*255
    out[:,:,3]=np.where(out[:,:,3]<8,0,out[:,:,3])
    return out.clip(0,255).astype(np.uint8)
Image.fromarray(compose_h(2.0,2.4),'RGBA').save('opt_h.png')
sheet=Image.new('RGBA',(256,192),(40,40,40,255))
for k,f in enumerate(['opt.png','opt_h.png','mm_set_h.png']):
    bg=Image.new('RGBA',(256,64),(40,40,40,255)); bg.alpha_composite(Image.open(f)); sheet.paste(bg,(0,64*k))
sheet.resize((768,576),Image.NEAREST).save('opt_sheet.png')
