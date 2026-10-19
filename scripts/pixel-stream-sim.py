import itertools
W=2048
# our frame: left half -> link0, right half -> link1, 24bpp R,G,B bytes
def line_bytes(pixels):  # pixels: list of (r,g,b) length 2048
    L0=bytes(c for p in pixels[:1024] for c in p); L1=bytes(c for p in pixels[1024:] for c in p); return L0,L1
def bits(b): return ''.join(f'{x:08b}' for x in b)
def decode(L0,L1,N,first,off,perm,bitorder):
    b0,b1=bits(L0),bits(L1)
    if bitorder=='lsb': b0=''.join(x[::-1] for x in (b0[i:i+8] for i in range(0,len(b0),8))); b1=''.join(x[::-1] for x in (b1[i:i+8] for i in range(0,len(b1),8)))
    a,b=(b0,b1) if first==0 else (b1,b0)
    s=''.join(a[i:i+N]+b[i:i+N] for i in range(0,len(a),N))
    s=s[off:]+s[:off]
    out=[]
    for i in range(0,len(s)-23,24):
        w=s[i:i+24]; ch=[int(w[0:8],2),int(w[8:16],2),int(w[16:24],2)]
        out.append(tuple(ch[perm[k]] for k in range(3)))
    return out[:W]
def mean(px): n=len(px); return tuple(sum(p[k] for p in px)/n for k in range(3))
NAMES={'black':(0,0,0),'white':(255,255,255),'red':(255,0,0),'green':(0,255,0),'blue':(0,0,255),'yellow':(255,255,0),'magenta':(255,0,255),'cyan':(0,255,255)}
def name(c):
    best=min(NAMES,key=lambda n:sum((a-b)**2 for a,b in zip(NAMES[n],c))); return best
fills={'R':(255,0,0),'G':(0,255,0),'B':(0,0,255),'W':(255,255,255)}
want={'R':'green','G':'magenta','B':'yellow','W':'white'}
bars=[(255,255,255),(255,255,0),(0,255,255),(0,255,0),(255,0,255),(255,0,0),(0,0,255),(0,0,0)]
barline=[bars[x*8//W] for x in range(W)]
res=[]
for N in (4,6,8,12,16,24):
  for first in (0,1):
    for off in range(0,24):
      for perm in itertools.permutations(range(3)):
        for bo in ('msb','lsb'):
          ok=True; got={}
          for k,c in fills.items():
            L0,L1=line_bytes([c]*W); d=decode(L0,L1,N,first,off,perm,bo); got[k]=name(mean(d))
            if got[k]!=want[k]: ok=False; break
          if not ok: continue
          L0,L1=line_bytes(barline); d=decode(L0,L1,N,first,off,perm,bo)
          bands=[name(mean(d[i*256:(i+1)*256])) for i in range(8)]
          res.append((N,first,off,perm,bo,bands))
print(len(res),'models match the fills')
for r in res[:40]: print(r)

# relaxed scoring
def dist(c,n): return sum((a-b)**2 for a,b in zip(NAMES[n],c))**0.5
scored=[]
for N in (2,3,4,6,8,10,12,16,24):
  for first in (0,1):
    for off in range(0,24):
      for perm in itertools.permutations(range(3)):
        for bo in ('msb','lsb'):
          sc=0; got={}
          for k,c in fills.items():
            L0,L1=line_bytes([c]*W); d=decode(L0,L1,N,first,off,perm,bo); m=mean(d); got[k]=tuple(int(x) for x in m); sc+=dist(m,want[k])
          scored.append((sc,N,first,off,perm,bo,got))
scored.sort(key=lambda r:r[0])
for r in scored[:12]:
    sc,N,first,off,perm,bo,got=r
    L0,L1=line_bytes(barline); d=decode(L0,L1,N,first,off,perm,bo)
    bands=[tuple(int(x) for x in mean(d[i*256:(i+1)*256])) for i in range(8)]
    print(f'score {sc:6.0f} N={N} first={first} off={off} perm={perm} {bo} fills={got}')
    print('   bars:',bands)

print('==== N=12 models, fills and bars')
seen=set()
for r in scored:
    sc,N,first,off,perm,bo,got=r
    if N!=12 or bo!='msb': continue
    key=(tuple(got.values()))
    if key in seen: continue
    seen.add(key)
    L0,L1=line_bytes(barline); d=decode(L0,L1,N,first,off,perm,bo)
    bands=[tuple(int(x) for x in mean(d[i*256:(i+1)*256])) for i in range(8)]
    print(f'score {sc:6.0f} first={first} off={off} perm={perm} fills={got}')
    print('   bars:',[name(b) for b in bands])
