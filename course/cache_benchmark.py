"""Replay cold LRU traces against a private disk image; verify actual syscalls."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zlib


def benchmark(root):
    sequence = "AABACABACABDDDCA"
    env = dict(os.environ, IMAGE_LAB_ROOT=str(root))
    def cli(*args, data=None):
        subprocess.run(["af-image-fs", *args], env=env, input=data, check=True)
    cli("init", "32")
    for symbol in sorted(set(sequence)):
        cli("put", "/" + symbol, data=symbol.encode())
    results = []
    for capacity, expected_hits in [(0, 0), (1, 3), (2, 7), (4, 12)]:
        trace = root / f"pread-{capacity}.log"
        args = ["strace", "-qq", "-e", "trace=pread64", "-P", str(root/"disk.img"),
                "-o", str(trace), "af-image-fs", "trace", *["/"+c for c in sequence]]
        run = subprocess.run(args, env=dict(env, IMAGE_LAB_CACHE_BLOCKS=str(capacity)),
                             stdout=subprocess.PIPE, check=True)
        row = json.loads(run.stdout)
        observed = sum(line.startswith("pread64(") for line in trace.read_text().splitlines())
        assert row["capacity"] == capacity and row["accesses"] == len(sequence)
        assert row["hits"] == expected_hits and row["data_preads"] == len(sequence)-expected_hits, row
        assert row["crc32"] == zlib.crc32(sequence.encode())
        assert observed == row["metadata_preads"] + row["data_preads"], (observed, row)
        row["image_preads_strace"] = observed
        row["hit_percent"] = 100 * row["hits"] / row["accesses"]
        results.append(row)
    return {"sequence": sequence, "block_bytes": 4096, "runs": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        report = json.dumps(benchmark(Path(tmp)), indent=2)
    print(report)
    if args.output:
        args.output.parent.mkdir(exist_ok=True, parents=True)
        args.output.write_text(report+"\n")
