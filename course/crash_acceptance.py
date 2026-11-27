"""SIGKILL the live server at distinct durable tile boundaries, leaving renders running."""
import os
import signal
from pathlib import Path

from image_job import export_png
from image_store import names, read
from submit import submit


def check_crashes(runtime, start, shutdown, launch, processes, events, until, done):
    import cv2
    import numpy as np

    width, height, side = 130, 150, 64
    tiles = set(range(9))
    rgb = np.frombuffer(bytes(i % 251 for i in range(width*height*3)), np.uint8)
    serial = (255-rgb).reshape(height,width,3)

    def committed(directory):
        return {int(n[5:-4]) for n in names(directory) if n.startswith("tile-") and n.endswith(".rgb")}

    for point in ("computing","before-confirm","before-stitch"):
        shutdown()
        marker = runtime / f"crash-{point}.ready"
        if point!="computing":
            os.environ.update(IMAGE_LAB_TEST_CRASH_POINT=point,IMAGE_LAB_TEST_CRASH_MARKER=str(marker),
                              IMAGE_LAB_TEST_CRASH_TILE="8")
        try:
            start()
            job, directory = submit(rgb.tobytes(),width,height,"invert",side=side,delay_ms=5000)
            if point=="computing":
                until("prior commits and later active attempts", lambda:
                      len(committed(directory))>=4 and any(row[3]=="started" and int(row[1])>=4 and
                          Path(f"/proc/{row[4]}").exists() for row in events(directory)))
            else:
                until("server stopped at "+point,marker.exists)
            server = processes[0]
            assert server.poll() is None
            os.kill(server.pid, signal.SIGKILL)
            assert server.wait(timeout=10) == -signal.SIGKILL
        finally:
            for option in ("POINT","MARKER","TILE"):
                os.environ.pop("IMAGE_LAB_TEST_CRASH_"+option,None)

        # The live renders may finish or reconnect, but only afserver can publish
        # a tile. This set is therefore stable until afserver is started again.
        files = set(names(directory))
        before = committed(directory)
        assert "result.png" not in files
        if point=="computing":
            assert 4<=len(before)<9
        elif point=="before-confirm":
            assert 8 not in before and len(before)>=4
            assert any(n.startswith("tile-8.attempt-") for n in files)
        else:
            assert before==tiles
        saved = {tile: read(directory,f"tile-{tile}.rgb") for tile in before}
        dispatches = {(int(row[1]),int(row[2])) for row in events(directory) if row[3]=="dispatched"}
        launch("afserver","image-manager")
        until("crashed job recovered",lambda: done(job),160)
        after = [(int(row[1]),int(row[2])) for row in events(directory) if row[3]=="dispatched"]
        resumed = [(tile,attempt) for tile,attempt in after if (tile,attempt) not in dispatches]
        assert {tile for tile,_ in resumed} == tiles-before, (point,before,resumed)
        assert len(resumed)==len(tiles-before)
        assert all(attempt > max((a for t,a in dispatches if t==tile),default=0)
                   for tile,attempt in resumed)
        assert committed(directory)==tiles
        for tile,pixels in saved.items():
            assert read(directory,f"tile-{tile}.rgb")==pixels
        completion = [int(row[1]) for row in events(directory) if row[3]=="committed"]
        assert len(completion)==len(set(completion))
        import af
        status = af.Cmd().getJobInfo(job)[0]
        assert status["blocks"][0]["p_tasks_done"]==9, status
        output = export_png(directory,runtime/f"recovered-{point}.png")
        actual = cv2.imread(str(output))[:,:,::-1]
        assert np.array_equal(actual,serial)
        print(f"PASS crash {point}: checkpoint={sorted(before)}, "
              f"recomputed={sorted(tiles-before)}, 9 counted, 0/19500 differing pixels")
