import sys
# Track layout detected from the raw image (sector = 2352 bytes).
# Data track 1: LBA 0..45141 (ISO volume), 45142..45291 = mode-1 encoded pregap of track 2.
# Audio pregap runs (150 sectors of digital silence) precede each subsequent track.
PREGAPS = [55953,63371,70707,78327,85363,92698,100290,107889,115480,122560,129588,
           136715,143892,151167,158539,165911,173505,181306,182268,183293,183895,
           184479,185384,186751,194155,201491,209128,210849]
def msf(l): return f"{l//4500:02d}:{(l//75)%60:02d}:{l%75:02d}"
binname = sys.argv[1] if len(sys.argv) > 1 else "CYCLONES.bin"
out = [f'FILE "{binname}" BINARY', '  TRACK 01 MODE1/2352', '    INDEX 01 00:00:00',
       '  TRACK 02 AUDIO', f'    INDEX 00 {msf(45142)}', f'    INDEX 01 {msf(45292)}']
for i,p in enumerate(PREGAPS):
    out += [f'  TRACK {i+3:02d} AUDIO', f'    INDEX 00 {msf(p)}', f'    INDEX 01 {msf(p+150)}']
print("\r\n".join(out), end="\r\n")
