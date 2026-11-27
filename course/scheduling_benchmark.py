"""Paired scheduling trials against afserver and four real afrender processes."""
import hashlib
import json
import os
import statistics

import cv2
import numpy as np

from control import read_events
from image_job import export_png, submit_image


def check_schedulers(runtime, start, shutdown, until, done):
    side = 64
    inputs = {}
    for label, size in (("normal", 194), ("high", 126)):
        y, x = np.indices((size, size))
        image = np.stack(((13*x+7*y)%256, (x*x+19*y)%256, (x+3*y*y)%256), axis=-1).astype(np.uint8)
        path = runtime / f"scheduler-input-{label}.png"
        assert cv2.imwrite(str(path), image)
        inputs[label] = path, 255-image

    def records(directory, state):
        return [e for e in read_events(directory.name)[1] if e["state"] == state]

    output = runtime / "scheduler-results.json"
    samples = []
    reference = {}
    for round_number in range(1, 4):
        # Alternate which policy runs first to reduce order effects.
        policies = ("priority", "fifo") if round_number % 2 else ("fifo", "priority")
        for policy in policies:
            shutdown()
            os.environ["IMAGE_LAB_SCHEDULER"] = policy
            start()
            normal, normal_dir = submit_image(inputs["normal"][0], "invert", "normal", side)
            until("first four normal dispatches", lambda: len(records(normal_dir, "dispatched")) >= 4)
            high, high_dir = submit_image(inputs["high"][0], "invert", "high", side)
            until("high job finished", lambda: done(high))
            until("normal job finished", lambda: done(normal))

            dispatches = {label: records(directory, "dispatched")
                          for label, directory in (("normal", normal_dir), ("high", high_dir))}
            order = sorted((e["dispatch_sequence"], label, e["tile"]) for label, items in dispatches.items() for e in items)
            labels = [label for _, label, _ in order]
            expected = (["normal"]*4 + ["high"]*4 + ["normal"]*12 if policy=="priority"
                        else ["normal"]*16 + ["high"]*4)
            assert labels == expected, (policy, order)
            assert [tile for _, label, tile in order if label=="normal"] == list(range(16))
            assert [tile for _, label, tile in order if label=="high"] == list(range(4))

            normal_arrival = records(normal_dir, "queued")[0]["time_ms"]
            high_arrival = records(high_dir, "queued")[0]["time_ms"]
            assert dispatches["normal"][3]["time_ms"] <= high_arrival <= dispatches["normal"][4]["time_ms"]
            wait = dispatches["high"][0]["time_ms"] - high_arrival
            elapsed = records(normal_dir, "committed")[-1]["time_ms"] - normal_arrival
            assert wait >= 0 and elapsed >= 0
            sample = dict(round=round_number, policy=policy, high_first_dispatch_ms=wait,
                          normal_completion_ms=elapsed, arrival_gap_ms=high_arrival-normal_arrival,
                          dispatch_order=[[label,tile] for _,label,tile in order], images={})
            for label, directory in (("normal", normal_dir), ("high", high_dir)):
                png = export_png(directory, runtime / f"scheduler-{round_number}-{policy}-{label}.png")
                actual = cv2.imread(str(png), cv2.IMREAD_UNCHANGED)
                assert actual is not None and np.array_equal(actual, inputs[label][1]), png
                if label in reference:
                    assert np.array_equal(actual, reference[label]), png
                else:
                    reference[label] = actual
                sample["images"][label] = dict(different_pixels=0, sha256=hashlib.sha256(png.read_bytes()).hexdigest())
            samples.append(sample)
            print(f"PASS scheduler round {round_number} {policy}: high wait {wait} ms, "
                  f"normal completion {elapsed} ms, 2 PNGs pixel-identical")

    summary = {}
    for policy in ("priority", "fifo"):
        summary[policy] = {}
        for metric in ("high_first_dispatch_ms", "normal_completion_ms"):
            values = [s[metric] for s in samples if s["policy"] == policy]
            summary[policy][metric] = dict(mean=statistics.mean(values), median=statistics.median(values))
    output.write_text(json.dumps(dict(workers=4, block_side=side, delay_ms=0,
                                      normal_size=[194,194], high_size=[126,126],
                                      samples=samples, summary=summary), indent=2)+"\n")
