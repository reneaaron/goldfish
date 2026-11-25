# Independent model of device_entropy.cpp (pool hashing, credit) that gives
# test_main.cpp its expected seed. Run: python3 make_vector.py
import hashlib, struct
H=lambda b: hashlib.sha256(b).digest()
x=1
def nxt():
    global x
    x^=(x<<13)&0xffffffff; x^=x>>17; x^=(x<<5)&0xffffffff; return x
pool=H(b"goldfish/device-seed/v1")
def absorb(s,data):
    global pool
    pool=H(pool+bytes([s])+struct.pack(">I",len(data))+H(data))
# chip: 4 chunks
for _ in range(4): absorb(0, bytes(nxt()&0xff for _ in range(256)))
# camera 64x64, until 256 credited: noise only (change less the frame's mean
# change, 1..12 in size, about as often up as down), nothing for 3 settle frames
W=64; G=32; last=None; bits=0; frames=0
unclipped=lambda v: 8 <= v <= 247
def trunc_div(a, b): return -((-a) // b) if a < 0 else a // b
while bits<256:
    f=bytes(100+nxt()%16 for _ in range(W*W)); absorb(1,f); frames+=1
    grid=[f[(gy*W//G+W//(2*G))*W+gx*W//G+W//(2*G)] for gy in range(G) for gx in range(G)]
    c=0
    if last is not None:
        pairs=[(v,l) for v,l in zip(grid,last) if unclipped(v) and unclipped(l)]
        mean=trunc_div(sum(v-l for v,l in pairs),len(pairs)) if pairs else 0
        rs=[v-l-mean for v,l in pairs if v-l-mean!=0 and abs(v-l-mean)<=12]
        up=sum(r>0 for r in rs); down=len(rs)-up
        c=len(rs)//16
        if frames<=3 or len(pairs)<G*G//4 or abs(up-down)*4>len(rs): c=0
        bits+=min(24,c)
    last=grid
# motion: half a bit per sample off the line through the last two, on 3+ axes
hist=[]; bits=0; n=0; jit=0
while bits<192:
    s_=[struct.unpack("<h",struct.pack("<H",nxt()&0xffff))[0] for _ in range(6)]
    absorb(2, struct.pack("<6h",*s_)); n+=1
    if len(hist)==2:
        off=sum(1 for i in range(6) if s_[i]-2*hist[1][i]+hist[0][i]!=0)
        if off>=3:
            jit+=1
            if jit%2==0: bits+=1
    hist=(hist+[s_])[-2:]
# touch
bits=0;i=0;lx=None
while bits<64:
    absorb(3, struct.pack(">III",i,2*i,10*i))
    if lx is not None: bits+=1
    lx=i; i+=1
print(frames,n,i,H(pool+b"seed")[:16].hex())
