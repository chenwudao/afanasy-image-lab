"""Compare exported PNG pixels to a whole-image serial reference."""
import cv2
import numpy as np

from image_job import export_png, submit_image


def check_images(runtime, until, done):
    width, height = 130, 150
    y, x = np.indices((height, width))
    pattern = np.stack(((13*x+7*y)%256, (x*x+19*y)%256, (x+3*y*y)%256), axis=-1).astype(np.uint8)
    jobs = []
    for suffix in ("png", "jpg"):
        source = runtime / f"source.{suffix}"
        assert cv2.imwrite(str(source), pattern)
        decoded = cv2.imread(str(source), cv2.IMREAD_COLOR)
        for operation in ("gray", "invert"):
            job, directory = submit_image(source, operation)
            jobs.append((job, directory, decoded, suffix, operation))
    for job, directory, decoded, suffix, operation in jobs:
        until(f"{suffix} {operation} completion", lambda: done(job), 160)
        assert len(list(directory.glob("tile-*.rgb"))) == 9
        assert (directory / "tile-8.rgb").stat().st_size == 2*22*3
        assert (directory / f"original.{suffix}").read_bytes() == (runtime / f"source.{suffix}").read_bytes()
        # Reference traverses the complete decoded input, independently of tile geometry.
        if operation == "gray":
            b, g, r = decoded.astype(np.uint32).transpose(2, 0, 1)
            gray = ((77*r + 150*g + 29*b + 128)//256).astype(np.uint8)
            expected = np.repeat(gray[:, :, None], 3, axis=2)
        else:
            expected = 255 - decoded
        output = export_png(directory, runtime / f"{suffix}-{operation}.png")
        actual = cv2.imread(str(output), cv2.IMREAD_UNCHANGED)
        assert actual.shape == (height, width, 3) and actual.dtype == np.uint8
        differences = np.count_nonzero(np.any(actual != expected, axis=2))
        print(f"{suffix} {operation}: 9 tiles, last 2x22, {differences}/{width*height} differing pixels")
        assert differences == 0
