"""Attach the two-browser test to a live, single-host Afanasy acceptance run."""
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import urllib.request


def check_console(runtime, until):
    from web import account
    script=Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="image-auth-") as private:
        old=os.environ.get("IMAGE_LAB_AUTH_DB")
        os.environ["IMAGE_LAB_AUTH_DB"]=str(Path(private)/"auth.sqlite")
        try:
            password=secrets.token_urlsafe(24)
            for name,role in [("alice","user"),("bob","user"),("manager","admin")]:
                account(name,password,role)
            with socket.socket() as sock:
                sock.bind(("127.0.0.1",0))
                port=sock.getsockname()[1]
            base=f"http://127.0.0.1:{port}"
            with (runtime/"web.log").open("w") as log, subprocess.Popen(
                [sys.executable,str(script/"web.py"),"serve","--port",str(port)],
                env=dict(os.environ,IMAGE_LAB_DEMO_DELAY_MS="6500"),stdout=log,stderr=subprocess.STDOUT) as web:
                try:
                    def ready():
                        if web.poll() is not None:
                            raise RuntimeError("web process stopped")
                        try:
                            with urllib.request.urlopen(base,timeout=2) as r:
                                return r.status==200
                        except OSError:
                            return False
                    until("web login",ready,20)
                    subprocess.run(["node",str(script/"browser_check.cjs"),base,str(runtime)],
                                   env=dict(os.environ,IMAGE_TEST_PASSWORD=password),check=True,timeout=240)
                finally:
                    web.terminate()
        finally:
            if old is None:
                del os.environ["IMAGE_LAB_AUTH_DB"]
            else:
                os.environ["IMAGE_LAB_AUTH_DB"]=old
