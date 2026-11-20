"""Write a small, original test SoundFont (synthesized waveforms only)."""
import struct, numpy as np, sys
SR=22050
def chunk(cid,data): return cid+struct.pack('<I',len(data))+data+(b'\0' if len(data)%2 else b'')
def lst(kind,data): return chunk(b'LIST',kind+data)
def name20(s): return s.encode()[:19].ljust(20,b'\0')
samples=[]   # (name, int16 array, loopStart, loopEnd, root, corr, type, link)
def add(name,x,root,loop=None,stype=1,link=0,corr=0):
    x=np.clip(np.round(x*32767*0.8),-32768,32767).astype(np.int16)
    samples.append([name,x,loop,root,corr,stype,link]); return len(samples)-1
def cycle(kind,freq,secs):
    n=int(SR*secs); t=np.arange(n)/SR; ph=(t*freq)%1
    return {'saw':2*ph-1,'sine':np.sin(2*np.pi*ph),'square':np.where(ph<0.5,1,-1.0)}[kind]
def looped(kind,root,secs=0.5):
    f=440*2**((root-69)/12); per=SR/f; ncyc=int(0.25*SR/per); L=int(round(ncyc*per))
    # pick a frequency that makes the loop an exact number of cycles
    f=ncyc*SR/L; x=cycle(kind,f,secs); start=len(x)-L-10
    return x,(start,start+L), 69+12*np.log2(f/440)
saw={}
for r in (48,64,76):
    x,lp,true=looped('saw',r); cents=int(round((true-r)*100)); saw[r]=add('saw%d'%r,x,r,lp,corr=-cents if False else 0)
    samples[saw[r]][4]=0; samples[saw[r]][3]=r
    # store exact pitch via pitchCorrection (cents the sample is sharp/flat -> correction)
    samples[saw[r]][4]=int(round((r-true)*100))
_x,_l,_t=looped('sine',60); sine60=add('sine60',_x,60,_l); samples[sine60][4]=int(round((60-_t)*100))
_x,_l,_t=looped('square',60); L60=add('padL',_x,60,_l,stype=4); R60=add('padR',_x,60,_l,stype=2)
samples[L60][4]=samples[R60][4]=int(round((60-_t)*100))
samples[L60][6]=R60; samples[R60][6]=L60
rng=np.random.default_rng(1)
def hit(dur,decay,tone=None):
    n=int(SR*dur); t=np.arange(n)/SR; env=np.exp(-t/decay)
    return env*(np.sin(2*np.pi*tone*t) if tone else rng.uniform(-1,1,n))
kick=add('kick',hit(0.4,0.08,55),36); snare=add('snare',hit(0.3,0.06),38); chh=add('chh',hit(0.1,0.02),42); ohh=add('ohh',hit(0.8,0.3),46)
# sdta
pcm=b''; hdr=[]; pos=0
for s in samples:
    x=s[1]; st=pos; pcm+=x.tobytes()+b'\0\0'*46; end=st+len(x)
    ls,le=(st+s[2][0],st+s[2][1]) if s[2] else (st,end)
    hdr.append((s[0],st,end,ls,le,SR,s[3],s[4],s[6],s[5])); pos=end+46
# generators
G=dict(startloop=2,endloop=3,fc=8,q=9,pan=17,atk=34,hold=35,dec=36,sus=37,rel=38,inst=41,key=43,vel=44,att=48,coarse=51,fine=52,sid=53,mode=54,excl=57,root=58)
def gen(op,val):
    if op in (43,44): return struct.pack('<HBB',op,val[0],val[1])
    return struct.pack('<Hh',op,val)
tc=lambda sec: int(round(1200*np.log2(sec)))
insts=[]   # name, list of zones (list of gens), first may be global
insts.append(('Saw Multi',[[gen(G['rel'],tc(0.3)),gen(G['atk'],tc(0.01))],   # global
   [gen(G['key'],(0,56)),gen(G['mode'],1),gen(G['sid'],saw[48])],
   [gen(G['key'],(57,70)),gen(G['mode'],1),gen(G['sid'],saw[64])],
   [gen(G['key'],(71,127)),gen(G['mode'],1),gen(G['sid'],saw[76])]]))
insts.append(('Vel Layers',[[gen(G['key'],(0,127)),gen(G['vel'],(0,63)),gen(G['mode'],1),gen(G['sid'],sine60)],
   [gen(G['key'],(0,127)),gen(G['vel'],(64,127)),gen(G['mode'],1),gen(G['fc'],tc(1)*0+8000),gen(G['sid'],saw[64])]]))
insts.append(('Stereo Pad',[[gen(G['atk'],tc(0.2)),gen(G['rel'],tc(1.0)),gen(G['dec'],tc(1.0)),gen(G['sus'],60)],
   [gen(G['pan'],-500),gen(G['mode'],1),gen(G['sid'],L60)],[gen(G['pan'],500),gen(G['mode'],1),gen(G['sid'],R60)]]))
insts.append(('Kit',[[gen(G['key'],(36,36)),gen(G['sid'],kick)],[gen(G['key'],(38,38)),gen(G['sid'],snare)],
   [gen(G['key'],(42,42)),gen(G['excl'],1),gen(G['sid'],chh)],[gen(G['key'],(46,46)),gen(G['excl'],1),gen(G['sid'],ohh)]]))
presets=[('Saw Multi',0,0,[[gen(G['inst'],0)]]),
         ('Saw Up Octave',0,1,[[gen(G['coarse'],12)],[gen(G['inst'],0)]]),   # global preset zone adds +12
         ('Velocity Layers',0,2,[[gen(G['inst'],1)]]),
         ('Stereo Pad',0,3,[[gen(G['inst'],2)]]),
         ('Test Kit',128,0,[[gen(G['inst'],3)]])]
def hydra(items,linkop,count_name,bagfmt):
    pass
# build pdta
phdr=b'';pbag=b'';pgen=b'';pmod=b''; ngen=0; nbag=0
for nm,bank,prog,zones in presets:
    phdr+=name20(nm)+struct.pack('<HHHIII',prog,bank,nbag,0,0,0)
    for z in zones:
        pbag+=struct.pack('<HH',ngen,0); nbag+=1
        for g in z: pgen+=g; ngen+=1
phdr+=name20('EOP')+struct.pack('<HHHIII',0,0,nbag,0,0,0); pbag+=struct.pack('<HH',ngen,0); pgen+=struct.pack('<HH',0,0)
pmod=b'\0'*10
inst=b'';ibag=b'';igen=b''; ngen=0; nbag=0
for nm,zones in insts:
    inst+=name20(nm)+struct.pack('<H',nbag)
    for z in zones:
        ibag+=struct.pack('<HH',ngen,0); nbag+=1
        for g in z: igen+=g; ngen+=1
inst+=name20('EOI')+struct.pack('<H',nbag); ibag+=struct.pack('<HH',ngen,0); igen+=struct.pack('<HH',0,0); imod=b'\0'*10
shdr=b''
for (nm,st,end,ls,le,sr,root,corr,link,typ) in hdr: shdr+=name20(nm)+struct.pack('<IIIIIBbHH',st,end,ls,le,sr,root,corr,link,typ)
shdr+=name20('EOS')+b'\0'*26
info=lst(b'INFO',chunk(b'ifil',struct.pack('<HH',2,1))+chunk(b'isng',b'EMU8000\0')+chunk(b'INAM',b'Meltdown Test Font\0\0'))
sdta=lst(b'sdta',chunk(b'smpl',pcm))
pdta=lst(b'pdta',b''.join(chunk(c,d) for c,d in [(b'phdr',phdr),(b'pbag',pbag),(b'pmod',pmod),(b'pgen',pgen),(b'inst',inst),(b'ibag',ibag),(b'imod',imod),(b'igen',igen),(b'shdr',shdr)]))
body=b'sfbk'+info+sdta+pdta
open(sys.argv[1],'wb').write(b'RIFF'+struct.pack('<I',len(body))+body)
print('wrote',sys.argv[1],len(body)+8,'bytes,',len(samples),'samples,',len(presets),'presets')
for i,h in enumerate(hdr): print(i,h[0],'root',h[6],'corr',h[7],'loop len',h[4]-h[3])
