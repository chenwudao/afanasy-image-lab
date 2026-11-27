"""Authenticated single-machine web console for the image_tile service."""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import subprocess
import time
from contextlib import closing

# Apply the upload pixel limit before OpenCV initializes its decoder.
os.environ.setdefault("OPENCV_IO_MAX_IMAGE_PIXELS", str(16 * 1024 * 1024))
import cv2
import numpy as np
from flask import Flask, Response, abort, g, jsonify, request, send_from_directory
from werkzeug.exceptions import NotFound
from werkzeug.security import check_password_hash, generate_password_hash

from control import directory, read_events, terminate_attempt
from image_job import submit_encoded
from image_store import fs, names, read

BROWSER = Path(__file__).resolve().parents[1] / "afanasy/browser"
STATIC = BROWSER / "image_lab"
COOKIE = "image_lab_session"


def connect():
    default = Path(os.environ["IMAGE_LAB_ROOT"]) / "web.sqlite"
    db = sqlite3.connect(os.environ.get("IMAGE_LAB_AUTH_DB", default), timeout=10)
    db.row_factory = sqlite3.Row
    return db


def initialize():
    Path(os.environ["IMAGE_LAB_ROOT"]).mkdir(parents=True,exist_ok=True,mode=0o700)
    with closing(connect()) as db, db:
        db.executescript("""
            CREATE TABLE IF NOT EXISTS users(name TEXT PRIMARY KEY, password TEXT NOT NULL,
                role TEXT NOT NULL CHECK(role IN ('user','admin')));
            CREATE TABLE IF NOT EXISTS sessions(token TEXT PRIMARY KEY, name TEXT NOT NULL, expires INTEGER NOT NULL);
        """)


def account(name, password, role):
    if not re.fullmatch(r"[\w.-]{1,64}", name) or not password or role not in ("user", "admin"):
        raise ValueError("invalid account")
    initialize()
    with closing(connect()) as db, db:
        db.execute("INSERT OR REPLACE INTO users VALUES(?,?,?)",
                   (name, generate_password_hash(password, method="pbkdf2:sha256:600000"), role))
        db.execute("DELETE FROM sessions WHERE name=?", (name,))


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


def manifest(key):
    if not re.fullmatch("[0-9a-f]{32}", key):
        abort(404)
    path = directory(key)
    try:
        lines = read(path, "manifest").decode().splitlines()
    except subprocess.CalledProcessError:
        abort(404)
    fields = lines[0].split()
    identity = json.loads(lines[1]) if len(lines) > 1 else {}
    width, height, side = map(int, fields[:3])
    return dict(key=key, owner=identity.get("owner"), filename=identity.get("filename") or key,
                width=width, height=height, side=side, operation=fields[3],
                priority=fields[4] if len(fields)>4 else "normal")


def job_for_session(key):
    job = manifest(key)
    if g.user["role"] != "admin" and job["owner"] != g.user["name"]:
        abort(404)
    return job


def encoded_image(rgb):
    ok, buf = cv2.imencode(".png", cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
    if not ok:
        abort(500)
    return Response(buf.tobytes(), mimetype="image/png")


def create_app():
    initialize()
    app = Flask(__name__, static_folder=None)
    app.config["MAX_CONTENT_LENGTH"] = 16 * 1024 * 1024

    @app.before_request
    def authorize():
        if request.host.split(":")[0] not in ("localhost","127.0.0.1"):
            abort(400)
        if request.method == "POST":
            if request.headers.get("X-Image-Lab") != "1" or request.headers.get("Origin",request.host_url.rstrip("/")) != request.host_url.rstrip("/"):
                abort(403)
        if request.path.startswith("/api/") and request.path != "/api/login":
            token = request.cookies.get(COOKIE,"")
            with closing(connect()) as db:
                g.user = db.execute("""SELECT users.name,role FROM users JOIN sessions USING(name)
                    WHERE token=? AND expires>?""",(digest(token),int(time.time()))).fetchone()
            if g.user is None:
                abort(401)

    @app.after_request
    def headers(response):
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self'; frame-ancestors 'none'; form-action 'self'"
        return response

    @app.get("/")
    def page():
        return send_from_directory(STATIC, "index.html")

    @app.get("/assets/<name>")
    def assets(name):
        if name in ("common.js","tasks.js","style.css"):
            return send_from_directory(BROWSER, name)
        if name in ("console.js","console.css"):
            return send_from_directory(STATIC, name)
        abort(404)

    @app.post("/api/login")
    def login():
        form = request.get_json()
        with closing(connect()) as db, db:
            row = db.execute("SELECT * FROM users WHERE name=?", (form.get("name"),)).fetchone()
            if row is None or not check_password_hash(row["password"], form.get("password","")):
                abort(401)
            token = secrets.token_urlsafe(32)
            db.execute("DELETE FROM sessions WHERE expires<=? OR token=?",
                       (int(time.time()),digest(request.cookies.get(COOKIE,""))))
            db.execute("INSERT INTO sessions VALUES(?,?,?)",(digest(token),row["name"],int(time.time())+28800))
        response = jsonify(name=row["name"], role=row["role"])
        response.set_cookie(COOKIE,token,max_age=28800,httponly=True,samesite="Strict")
        return response

    @app.get("/api/me")
    def me():
        return jsonify(dict(g.user))

    @app.post("/api/logout")
    def logout():
        with closing(connect()) as db, db:
            db.execute("DELETE FROM sessions WHERE token=?",(digest(request.cookies[COOKIE]),))
        response = jsonify(ok=True)
        response.delete_cookie(COOKIE)
        return response

    @app.get("/api/jobs")
    def jobs():
        return jsonify(visible_jobs())

    def visible_jobs():
        import af
        result = []
        native_jobs = af.Cmd().getJobList()
        if native_jobs is None:
            abort(503)
        for native in native_jobs:
            match = re.fullmatch(r"Image ([0-9a-f]{32})",native.get("name",""))
            if not match:
                continue
            try:
                item = manifest(match[1])
            except NotFound:
                continue
            if g.user["role"] == "admin" or item["owner"] == g.user["name"]:
                result.append(dict(item, state=native.get("state",""), id=native["id"]))
        return sorted(result,key=lambda j:j["id"],reverse=True)

    @app.get("/api/queue")
    def queue():
        if g.user["role"] != "admin":
            abort(403)
        ready = []
        for job in visible_jobs():
            if "RUN" not in job["state"] and "RDY" not in job["state"]:
                continue
            pending = {}
            for e in read_events(job["key"])[1]:
                if e["state"] in ("queued","requeued"):
                    pending[e["tile"]] = e["dispatch_sequence"]
                elif e["state"] in ("dispatched","committed"):
                    pending.pop(e["tile"],None)
            ready.extend(dict(key=job["key"], filename=job["filename"], priority=job["priority"],
                              tile=t, sequence=s) for t,s in pending.items())
        fifo = os.getenv("IMAGE_LAB_SCHEDULER") == "fifo"
        return jsonify(sorted(ready,key=lambda e:(not fifo and e["priority"]!="high",e["sequence"])))

    @app.post("/api/jobs")
    def upload():
        upload = request.files.get("image")
        if upload is None or not upload.filename:
            abort(400)
        op,priority = request.form.get("operation","gray"), request.form.get("priority","normal")
        if op not in ("gray","invert") or priority not in ("normal","high"):
            abort(400)
        try:
            job,d = submit_encoded(upload.read(),op,priority,owner=g.user["name"],
                                   filename=Path(upload.filename).name[:160],
                                   delay_ms=int(os.getenv("IMAGE_LAB_DEMO_DELAY_MS","0")))
        except (ValueError,cv2.error):
            abort(400)
        return jsonify(id=job,key=d.name),201

    @app.get("/api/jobs/<key>")
    def detail(key):
        job = job_for_session(key)
        files = names(directory(key))
        job["committed"] = sorted(int(n[5:-4]) for n in files if re.fullmatch(r"tile-\d+\.rgb",n))
        job["complete"] = "result.png" in files
        return jsonify(job)

    @app.get("/api/jobs/<key>/events")
    def events(key):
        job_for_session(key)
        cursor = request.args.get("cursor",0,type=int)
        if not 0 <= cursor < 2**63:
            abort(400)
        offset, records = read_events(key,cursor)
        return jsonify(cursor=offset,events=records)

    @app.get("/api/jobs/<key>/tiles/<int:tile>")
    def preview(key,tile):
        job = job_for_session(key)
        w,h,side = job["width"],job["height"],job["side"]
        cols=(w+side-1)//side
        x,y=(tile%cols)*side,(tile//cols)*side
        if y>=h:
            abort(404)
        try:
            payload = read(directory(key),f"tile-{tile}.rgb")
        except subprocess.CalledProcessError:
            abort(404)
        return encoded_image(np.frombuffer(payload,np.uint8).reshape(min(side,h-y),min(side,w-x),3))

    @app.get("/api/jobs/<key>/download")
    def download(key):
        job_for_session(key)
        try:
            data = read(directory(key),"result.png")
        except subprocess.CalledProcessError:
            abort(409)
        return Response(data,mimetype="image/png",
                        headers={"Content-Disposition":f'attachment; filename="{key}.png"'})

    @app.post("/api/jobs/<key>/terminate")
    def terminate(key):
        if g.user["role"] != "admin":
            abort(403)
        job_for_session(key)
        body=request.get_json()
        if not all(type(body.get(k)) is int for k in ("tile","attempt","pid")):
            abort(400)
        try:
            terminate_attempt(key,body["tile"],body["attempt"],body["pid"])
        except (ValueError,OSError,OverflowError):
            abort(409)
        return jsonify(ok=True)

    return app


if __name__ == "__main__":
    parser=argparse.ArgumentParser()
    cmds=parser.add_subparsers(dest="command",required=True)
    add=cmds.add_parser("account")
    add.add_argument("name")
    add.add_argument("--role",choices=["user","admin"],default="user")
    serve=cmds.add_parser("serve")
    serve.add_argument("--port",type=int,default=8080)
    args=parser.parse_args()
    if args.command=="account":
        account(args.name,getpass.getpass(),args.role)
    else:
        create_app().run(host="127.0.0.1",port=args.port,threaded=True)
