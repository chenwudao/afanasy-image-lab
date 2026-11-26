"""Isolated Linux acceptance run against built afserver / afrender binaries."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

from submit import submit

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "course-runtime"
OUT.mkdir(exist_ok=True)
runtime = Path(tempfile.mkdtemp(prefix="integration-", dir=OUT))
processes = []


def until(description, check, timeout=100):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(0.1)
    raise RuntimeError("timeout: " + description)


def launch(binary, name):
    path = shutil.which(binary)
    if not path:
        raise RuntimeError("missing " + binary)
    with (runtime / (name+".log")).open("w") as log:
        process = subprocess.Popen([path], env=dict(os.environ, AF_HOSTNAME=name),
                                   stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    processes.append(process)


def events(directory):
    file = directory / "events.tsv"
    if not file.exists():
        return []
    return [line.split() for line in file.read_text().splitlines() if len(line.split()) == 5]


def start():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    config = runtime / "config.json"
    config.write_text(json.dumps({"cgru_config": {"af_servername": "127.0.0.1",
                      "af_serverport": port, "af_store_folder": str(runtime / "server"),
                      "af_render_nice": 0}}))
    (runtime/"home").mkdir()
    paths = [str(ROOT/"afanasy/python"), str(ROOT/"lib/python")]
    sys.path[:0] = paths
    os.environ.update(CGRU_LOCATION=str(ROOT), AF_ROOT=str(ROOT/"afanasy"),
                      HOME=str(runtime/"home"), CGRU_CUSTOM_CONFIG=str(config),
                      IMAGE_LAB_ROOT=str(runtime), PYTHONPATH=os.pathsep.join(paths))
    import afnetwork
    def request(data):
        ok, reply = afnetwork.sendServer(json.dumps(data))
        return reply if ok else {}
    launch("afserver", "image-manager")
    until("server ready", lambda: request({"get":{"type":"pools"}}).get("pools"))
    # Limit the entire render (including across jobs) to one calculation.
    action = {"type":"pools", "mask":"^/$", "user_name":"image-test","host_name":"localhost"}
    request({"action":dict(action, params={"max_tasks_host":1,"capacity_host":1,"heartbeat_sec":1,"new_paused":False,"new_nimby":False})})
    request({"action":dict(action, operation={"type":"farm","mode":"service_add","name":"image_tile"})})
    for n in range(4):
        launch("afrender", f"image-render-{n}")
    until("four registered renders", lambda: len(request({"get":{"type":"renders"}}).get("renders",[])) == 4)


def run():
    start()
    import af
    def done(job):
        state = af.Cmd().getJobList(ids=[job]) or []
        if state and "ERR" in state[0].get("state",""):
            raise RuntimeError("job error: " + str(state))
        return state and "DON" in state[0].get("state","")
    job, single = submit(bytes([20,40,80]),1,1,"gray")
    until("single tile committed", lambda: done(job))
    reference = (77*20+150*40+29*80+128)//256
    assert (single/"tile-0.rgb").read_bytes() == bytes([reference])*3
    print("PASS single tile", job, events(single))
    pixels = bytes(range(27))
    normal, low_dir = submit(pixels,9,1,"invert",side=1,delay_ms=12000)
    first = until("four simultaneous PIDs", lambda: events(low_dir) if
                  len([e for e in events(low_dir) if e[3]=="started"])>=4 else [])
    starts = [e for e in first if e[3]=="started"]
    assert len({r[4] for r in starts}) == 4
    high, hi_dir = submit(bytes(range(6)),2,1,"invert","high",side=1)
    victim = starts[0]
    # Only terminate a live calculation of this test job.
    pid = int(victim[4])
    assert Path(f"/proc/{pid}/exe").resolve().name == "af-image-worker"
    assert ("IMAGE_JOB_KEY="+low_dir.name).encode() in Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
    os.kill(pid, signal.SIGKILL)
    until("high job", lambda: done(high))
    until("normal job with retry", lambda: done(normal), 160)
    logs = events(low_dir)
    # Dispatch sequence is assigned by the server; process start order may differ.
    sequence = [(d.name,int(r[1]),int(r[2]),int(r[4])) for d in (low_dir,hi_dir)
                for r in events(d) if r[3]=="dispatched"]
    sequence.sort(key=lambda row:row[3])
    assert [(d,t) for d,t,a,s in sequence[4:6]] == [(hi_dir.name,0),(hi_dir.name,1)], sequence
    assert [t for d,t,a,s in sequence if d==low_dir.name and a==1] == list(range(9)), sequence
    assert any(r[1]==victim[1] and r[2]=="2" and r[3]=="started" and r[4]!=victim[4] for r in logs)
    assert len(list(low_dir.glob("tile-*.rgb"))) == 9
    assert b"".join((low_dir/f"tile-{t}.rgb").read_bytes() for t in range(9)) == bytes(255-p for p in pixels)
    print("PASS four PIDs:", [r[4] for r in starts], "killed:", pid, "dispatch:", sequence)


try:
    run()
finally:
    for proc in reversed(processes):
        if proc.poll() is None:
            os.killpg(proc.pid,signal.SIGTERM)
    for proc in processes:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid,signal.SIGKILL)
            proc.wait()
    for path in runtime.glob("*.log"):
        print(path.name, path.read_text(errors="replace")[-3500:])
