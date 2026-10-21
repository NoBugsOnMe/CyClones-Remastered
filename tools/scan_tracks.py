import sys
SYNC = b'\x00' + b'\xff'*10 + b'\x00'
path = sys.argv[1]
SS = 2352
with open(path,'rb') as f:
    data = f.read()
n = len(data)//SS
# classify each sector: data (sync + mode) or audio
kinds = []
for i in range(n):
    s = data[i*SS:i*SS+16]
    if s[:12] == SYNC:
        kinds.append('M%d' % s[15])
    else:
        kinds.append('A')
# run-length
runs = []
cur = kinds[0]; start = 0
for i,k in enumerate(kinds):
    if k != cur:
        runs.append((cur,start,i-1)); cur=k; start=i
runs.append((cur,start,n-1))
for k,a,b in runs:
    print(f"{k:3} sectors {a:7}-{b:7} count {b-a+1:7}  ({(b-a+1)/75:8.2f}s)")
