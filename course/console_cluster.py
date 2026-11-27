"""Start a private, persistent single-machine Afanasy image console."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time


def configure_pool(request):
    action={"type":"pools","mask":"^/$","user_name":"image-console","host_name":"localhost"}
    request({"action":dict(action,params={"max_tasks_host":1,"capacity_host":1,"heartbeat_sec":1,
                                          "new_paused":False,"new_nimby":False})})
    request({"action":dict(action,operation={"type":"farm","mode":"service_add","name":"image_tile"})})


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port",type=int,default=8080)
    parser.add_argument("--native-port",type=int,default=51000)
    args=parser.parse_args()
    if args.port==args.native_port:
        parser.error("web and native ports must differ")
    root=Path(__file__).resolve().parents[1]
    store=Path(os.environ["IMAGE_LAB_ROOT"]).resolve()
    store.mkdir(parents=True,exist_ok=True,mode=0o700)
    config=store/"console-config.json"
    paths=[str(root/"afanasy/python"),str(root/"lib/python")]
    # Refuse to attach to or reconfigure an existing farm.
    for port in (args.native_port,args.port):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1",port))
    config.write_text(json.dumps({"cgru_config":{"af_servername":"127.0.0.1","af_serverport":args.native_port,
                                  "af_store_folder":str(store/"server")}}))
    os.environ.update(CGRU_LOCATION=str(root),AF_ROOT=str(root/"afanasy"),IMAGE_LAB_ROOT=str(store),
                      CGRU_CUSTOM_CONFIG=str(config),PYTHONPATH=os.pathsep.join(paths))
    sys.path[:0]=paths
    import afnetwork
    from image_store import fs
    def request(data):
        ok,value=afnetwork.sendServer(json.dumps(data))
        return value if ok else {}
    processes=[]
    def launch(command,name):
        with (store/(name+".log")).open("a") as log:
            proc=subprocess.Popen(command,env=dict(os.environ,AF_HOSTNAME=name),stdout=log,
                                  stderr=subprocess.STDOUT,start_new_session=True)
        processes.append(proc)
    def interrupted(*unused):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM,interrupted)
    try:
        fs("init","16384")
        launch(["afserver"],"image-manager")
        for _ in range(100):
            if processes[0].poll() is not None:
                raise RuntimeError("afserver stopped; inspect image-manager.log")
            if request({"get":{"type":"pools"}}).get("pools"):
                break
            time.sleep(.2)
        else:
            raise RuntimeError("afserver startup timed out")
        configure_pool(request)
        for n in range(4):
            launch(["afrender"],f"image-render-{n}")
        launch([sys.executable,str(root/"course/web.py"),"serve","--port",str(args.port)],"image-web")
        print(f"Afanasy Image Lab: http://127.0.0.1:{args.port}",flush=True)
        while all(p.poll() is None for p in processes):
            time.sleep(1)
        raise RuntimeError("service exited; inspect logs in IMAGE_LAB_ROOT")
    except KeyboardInterrupt:
        pass
    finally:
        for proc in reversed(processes):
            if proc.poll() is None:
                os.killpg(proc.pid,signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid,signal.SIGKILL)
                proc.wait()


if __name__=="__main__":
    main()
