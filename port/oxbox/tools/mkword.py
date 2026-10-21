"""mkword.py: main menu words made of the letters of ui.map's CAMPAIGN, MULTIPLAYER, SETTINGS and GAME DEMOS (normal and
highlighted, 256 x 64, as the originals): OPTIONS and PROFILES. F is SETTINGS' E without its lower bar."""
from PIL import Image, ImageFilter
import numpy as np
cores={'set':[(60,78),(80,96),(97,113),(115,131),(133,137),(141,158),(161,179),(181,199)],
       'camp':[(53,69),(70,89),(91,113),(117,134),(135,155),(157,161),(164,181),(185,202)],
       'demo':[(126,143),(146,161),(165,186),(189,209),(210,228)],
       'mp':[(34,55),(60,75),(80,92),(94,109),(112,115),(121,136),(210,226)]}
WORDS={'options':[('demo',3,None),('camp',3,None),('set',2,None),('set',4,None),('demo',3,None),('set',5,None),('set',7,None)],
       'profiles':[('mp',5,None),('mp',6,None),('demo',3,None),('set',1,'F'),('set',4,None),('mp',2,None),('set',1,None),('set',7,None)]}
GAP=3
# slices whose neighbours in their word aren't all in cores (MULTIPLAYER's L A Y run together)
BOUNDS={('mp',2):(78,93),('mp',5):(118,138),('mp',6):(208,229)}
def load(k,s): return np.asarray(Image.open(f'mm_{k}{s}.png').convert('RGBA')).astype(np.float32)
def make_f(sl, x0):
    # the stem goes straight down from row 20; nothing of the lower bar
    sl=sl.copy(); stem=sl[20].copy()
    for y in range(21,27): sl[y]=stem
    sl[26]=0; return sl
def compose(word):
    src={k:load(k,'') for k in cores}
    pick=WORDS[word]
    slices=[]
    for w,i,t in pick:
        c=cores[w]; x0,x1=c[i]
        left=(c[i-1][1]+x0)//2+1 if i>0 else x0-8
        right=(x1+c[i+1][0])//2 if i+1<len(c) else x1+8
        if (w,i) in BOUNDS: left,right=BOUNDS[(w,i)]
        sl=src[w][:,max(left,0):right+1]
        if t=='F': sl=make_f(sl,x0)
        slices.append((sl,x0-max(left,0),x1-x0+1))
    total=sum(w for _,_,w in slices)+GAP*(len(slices)-1)
    out=np.zeros((64,256,4),np.float32); x=(256-total)//2
    for sl,off,w in slices:
        for cx in range(sl.shape[1]):
            tx=x-off+cx
            if 0<=tx<256:
                a=sl[:,cx,3:4]; o=out[:,tx]
                rgb=np.where(a>o[:,3:4],sl[:,cx,:3],o[:,:3]); out[:,tx,:3]=rgb; out[:,tx,3:4]=np.maximum(o[:,3:4],a)
        x+=w+GAP
    return out.clip(0,255).astype(np.uint8)
def compose_h(word, blur=2.0, gain=2.4):
    hs={k:load(k,'_h') for k in cores}
    pick=WORDS[word]
    cov=np.zeros((64,256),np.float32)
    total=sum(cores[w][i][1]-cores[w][i][0]+1 for w,i,_ in pick)+GAP*(len(pick)-1)
    x=(256-total)//2
    for w,i,t in pick:
        x0,x1=cores[w][i]
        sl=hs[w][:,x0:x1+1]
        if t=='F': sl=make_f(sl,x0)
        c=np.clip((sl[:,:,0]-41)/(200-41),0,1)
        for cx in range(sl.shape[1]):
            tx=x+cx
            if 0<=tx<256: cov[:,tx]=np.maximum(cov[:,tx],c[:,cx])
        x+=x1-x0+1+GAP
    g=np.asarray(Image.fromarray((cov*255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))).astype(np.float32)/255
    ga=np.clip(g*gain,0,153/255)
    out=np.zeros((64,256,4),np.float32)
    blue=np.array([38,149,255],np.float32); white=np.array([255,255,255],np.float32)
    out[:,:,:3]=blue*(1-cov[:,:,None])+white*cov[:,:,None]
    out[:,:,3]=np.maximum(ga,cov)*255; out[:,:,3]=np.where(out[:,:,3]<8,0,out[:,:,3])
    return out.clip(0,255).astype(np.uint8)
if __name__=='__main__':
    sheet=Image.new('RGBA',(256,256),(40,40,40,255))
    for k,wd in enumerate(['options','profiles']):
        Image.fromarray(compose(wd),'RGBA').save(f'{wd}.png'); Image.fromarray(compose_h(wd),'RGBA').save(f'{wd}_h.png')
        for j,f in enumerate([f'{wd}.png',f'{wd}_h.png']):
            bg=Image.new('RGBA',(256,64),(40,40,40,255)); bg.alpha_composite(Image.open(f)); sheet.paste(bg,(0,(2*k+j)*64))
    sheet.resize((768,768),Image.NEAREST).save('words_sheet.png')
