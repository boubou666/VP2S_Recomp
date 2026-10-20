"""Map zero / non-zero sector runs in the hidden area and dump the start of each data run."""
import glob

SECTOR = 2048
ISO = glob.glob("/work/*.iso")[0]
START_LBA = 821

runs = []  # (start_lba, end_lba, nonzero)
with open(ISO, "rb") as f:
    f.seek(START_LBA * SECTOR)
    lba = START_LBA
    cur = None
    zero = bytes(SECTOR)
    while True:
        data = f.read(SECTOR * 4096)
        if not data:
            break
        for off in range(0, len(data), SECTOR):
            nz = data[off:off + SECTOR] != zero
            if cur is None or cur[2] != nz:
                if cur:
                    runs.append(tuple(cur))
                cur = [lba, lba, nz]
            cur[1] = lba
            lba += 1
    runs.append(tuple(cur))

    data_runs = [r for r in runs if r[2]]
    print(f"{len(runs)} runs, {len(data_runs)} non-zero")
    print("=== first 40 non-zero runs (lba start-end, sectors)")
    for s, e, _ in data_runs[:40]:
        print(f"  {s:>8}-{e:<8} {e - s + 1:>8} sectors")
    big_gaps = [r for r in runs if not r[2] and r[1] - r[0] > 64]
    print(f"=== zero gaps > 64 sectors: {len(big_gaps)}")
    for s, e, _ in big_gaps[:20]:
        print(f"  {s:>8}-{e:<8} {e - s + 1:>8} sectors")

    for s, e, _ in data_runs[:3]:
        f.seek(s * SECTOR)
        buf = f.read(min(256, (e - s + 1) * SECTOR))
        print(f"=== dump lba {s}")
        for i in range(0, len(buf), 16):
            c = buf[i:i + 16]
            print(f"  {s * SECTOR + i:010x}  " + " ".join(f"{b:02x}" for b in c)
                  + "  " + "".join(chr(b) if 32 <= b < 127 else "." for b in c))
