"""Afanasy submission adapter for local RGB8 images. Import submit() from the UI."""
import argparse
import math
import os
from pathlib import Path
import uuid


def submit(pixels, width, height, operation, priority="normal", side=64, delay_ms=0):
    import af
    if not (0 < width <= 65535 and 0 < height <= 65535 and 0 < side <= 1024):
        raise ValueError("invalid RGB8 dimensions")
    if len(pixels) != width * height * 3 or operation not in {"gray", "invert"}:
        raise ValueError("invalid input or operation")
    priorities = {"normal": 50, "high": 100}
    level = priorities[priority]
    if not 0 <= delay_ms <= 60000:
        raise ValueError("demo delay must be 0..60000 ms")
    key = uuid.uuid4().hex
    directory = Path(os.environ["IMAGE_LAB_ROOT"]) / key
    directory.mkdir(mode=0o700)
    (directory / "source.rgb").write_bytes(pixels)
    (directory / "manifest").write_text(f"{width} {height} {side} {operation}\n")
    job = af.Job("Image " + key)
    job.setPriority(level)
    block = af.Block("tiles", "image_tile")
    block.setNumeric(0, math.ceil(width/side)*math.ceil(height/side)-1, 1)
    block.setEnv("IMAGE_JOB_KEY", key, False)
    block.setEnv("IMAGE_DEMO_DELAY_MS", str(delay_ms), False)
    block.setCapacity(1)
    block.setMaxRunTasksPerHost(1)
    block.setErrorsRetries(3)
    block.setCommand("exec af-image-worker", False, False)
    job.blocks.append(block)
    ok, reply = job.send()
    if not ok or reply.get("id", 0) <= 0:
        raise RuntimeError(f"Afanasy rejected {key}: {reply}")
    return reply["id"], directory


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("rgb", type=Path)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument("--operation", choices=["gray", "invert"], default="gray")
    parser.add_argument("--priority", choices=["normal", "high"], default="normal")
    args = parser.parse_args()
    print(submit(args.rgb.read_bytes(), args.width, args.height, args.operation, args.priority))
