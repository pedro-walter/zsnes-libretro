import sys,random
n,s=int(sys.argv[1]),int(sys.argv[2]); r=random.Random(s); out=[]; f=60
while f<n:
    k=r.random(); m=(1<<3) if k<0.25 else (1<<0) if k<0.5 else (1<<8) if k<0.6 else r.getrandbits(12)&~(1<<2)
    out.append("%d:%d"%(f,m)); f+=r.choice([3,5,10,20,40]); out.append("%d:0"%f); f+=r.choice([5,10,30])
print(",".join(out))
