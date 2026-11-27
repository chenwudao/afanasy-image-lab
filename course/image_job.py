"""Submit PNG/JPEG images to Afanasy and export committed tiles as a PNG."""
import argparse
from pathlib import Path
import os
import tempfile
import time

import cv2
import numpy as np

from submit import submit


def submit_image(path, operation, priority="normal", side=64):
    encoded = Path(path).read_bytes()
    if encoded.startswith(b"\x89PNG\r\n\x1a\n"):
        suffix = "png"
    elif encoded.startswith(b"\xff\xd8\xff"):
        suffix = "jpg"
    else:
        raise ValueError("expected a PNG or JPEG image")
    # The wire contract is RGB8 in stored orientation, without alpha.
    bgr = cv2.imdecode(np.frombuffer(encoded, np.uint8),
                       cv2.IMREAD_COLOR | cv2.IMREAD_IGNORE_ORIENTATION)
    if bgr is None:
        raise ValueError("could not decode input image")
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    height, width = rgb.shape[:2]
    return submit(rgb.tobytes(), width, height, operation, priority, side,
                  original=(suffix, encoded))


def export_png(directory, output):
    directory, output = Path(directory), Path(output)
    width, height, side, _ = (directory / "manifest").read_text().split()
    width, height, side = map(int, (width, height, side))
    columns = (width + side - 1) // side
    rows = (height + side - 1) // side
    canvas = np.empty((height, width, 3), dtype=np.uint8)
    for tile in range(rows * columns):
        x, y = tile % columns * side, tile // columns * side
        w, h = min(side, width - x), min(side, height - y)
        data = (directory / f"tile-{tile}.rgb").read_bytes()
        if len(data) != w * h * 3:
            raise ValueError(f"incorrect byte count for tile {tile}")
        canvas[y:y+h, x:x+w] = np.frombuffer(data, np.uint8).reshape(h, w, 3)
    ok, encoded = cv2.imencode(".png", cv2.cvtColor(canvas, cv2.COLOR_RGB2BGR))
    if not ok:
        raise OSError("PNG encoding failed")
    temp = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as stream:
            temp = stream.name
            stream.write(encoded.tobytes())
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, output)
    finally:
        if temp:
            Path(temp).unlink(missing_ok=True)
    return output


def main():
    import af
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--operation", choices=["gray", "invert"], default="gray")
    parser.add_argument("--priority", choices=["normal", "high"], default="normal")
    parser.add_argument("--side", type=int, default=64)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("choose a separate output file")
    job, directory = submit_image(args.input, args.operation, args.priority, args.side)
    print(f"Submitted job {job}, key {directory.name}", flush=True)
    while True:
        jobs = af.Cmd().getJobList(ids=[job])
        if not jobs:
            raise RuntimeError(f"cannot look up job {job}")
        state = jobs[0].get("state", "")
        if "ERR" in state:
            raise RuntimeError(f"job {job} failed: {state}")
        if "DON" in state:
            break
        time.sleep(1)
    print(export_png(directory, args.output))


if __name__ == "__main__":
    main()
