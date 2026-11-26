"""D's local, server-side adapter. Call after checking session role / job ownership."""
import os
from pathlib import Path
import re
import shutil
import signal


def directory(key):
    if not isinstance(key, str) or not re.fullmatch("[0-9a-f]{32}", key):
        raise ValueError("invalid job key")
    return Path(os.environ["IMAGE_LAB_ROOT"]) / key


def read_events(key, cursor=0):
    """Return whole TSV records and the next byte offset for this job."""
    if cursor < 0:
        raise ValueError("negative event cursor")
    events = []
    try:
        stream = (directory(key) / "events.tsv").open("rb")
    except FileNotFoundError:
        return cursor, events
    with stream:
        stream.seek(cursor)
        for line in stream:
            if not line.endswith(b"\n"):
                break
            millis, tile, attempt, state, pid, sequence = line.decode("ascii").split()
            events.append(dict(time_ms=int(millis), tile=int(tile), attempt=int(attempt),
                               state=state, pid=int(pid), dispatch_sequence=int(sequence)))
            cursor += len(line)
    return cursor, events


def terminate_attempt(key, tile, attempt, pid):
    """Terminate the specified calculation after pinning and validating its identity."""
    directory(key)
    if min(tile, attempt-1, pid-1) < 0:
        raise ValueError("invalid attempt identity")
    descriptor = os.pidfd_open(pid)
    try:
        proc = Path("/proc") / str(pid)
        expected = {f"IMAGE_JOB_KEY={key}", f"AF_TASK_ID={tile}", f"IMAGE_ATTEMPT={attempt}"}
        environ = set((proc / "environ").read_bytes().split(b"\0"))
        worker = shutil.which("af-image-worker")
        if (not worker or not (proc / "exe").samefile(worker)
                or not {item.encode() for item in expected}.issubset(environ)):
            raise ValueError("PID does not belong to the requested attempt")
        signal.pidfd_send_signal(descriptor, signal.SIGTERM)
    finally:
        os.close(descriptor)
