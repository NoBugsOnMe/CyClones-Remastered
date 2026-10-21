import sys, array
path=sys.argv[1]; start=int(sys.argv[2]); SS=2352
with open(path,'rb') as f:
    f.seek(start*SS); data=f.read()
n=len(data)//SS
peak=[]
for i in range(n):
    a=array.array('h', data[i*SS:(i+1)*SS])
    peak.append(max(abs(min(a)),abs(max(a))))
# find runs of near-silence (peak<=8)
runs=[]; cur=None
for i,p in enumerate(peak):
    q = p<=8
    if q and cur is None: cur=i
    if not q and cur is not None:
        if i-cur>=20: runs.append((cur,i-1))
        cur=None
if cur is not None: runs.append((cur,n-1))
for a,b in runs:
    print(f"silence abs {start+a:7}-{start+b:7} len {b-a+1:5} ({(b-a+1)/75:.2f}s) at t={a/75/60:.0f}:{a/75%60:05.2f}")
print('total audio', n, 'sectors', n/75, 's')
