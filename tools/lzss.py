"""Okumura-style LZSS decoder used by KCEJYUGI archives (N=4096, F=18, threshold 2, window pre-filled with 0x00)."""
import sys
def lzss(src, N=4096, F=18, T=2, fill=0x00):
    win=bytearray([fill]*N); r=N-F; out=bytearray(); i=0; flags=0
    while i<len(src):
        flags>>=1
        if not flags&0x100: flags=src[i]|0xff00; i+=1
        if i>=len(src): break
        if flags&1:
            c=src[i]; i+=1; out.append(c); win[r]=c; r=(r+1)&(N-1)
        else:
            if i+1>=len(src): break
            a,b=src[i],src[i+1]; i+=2
            p=a|((b&0xf0)<<4); l=(b&0x0f)+T+1
            for k in range(l):
                c=win[(p+k)&(N-1)]; out.append(c); win[r]=c; r=(r+1)&(N-1)
    return bytes(out)
if __name__ == "__main__":
    d=lzss(open(sys.argv[1],"rb").read()); print(len(d)); sys.stdout.write(d.decode("cp932","replace")[:900])
