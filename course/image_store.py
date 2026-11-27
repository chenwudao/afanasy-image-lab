"""Python callers share the same locked C++ inode implementation as afserver."""
from pathlib import Path
import subprocess


def fs(*args, data=None):
    return subprocess.run(["af-image-fs", *map(str, args)], input=data,
                          stdout=subprocess.PIPE, check=True).stdout


def entry(directory, name):
    return "/" + Path(directory).name + "/" + name


def read(directory, name):
    return fs("read", entry(directory, name))


def names(directory):
    return fs("list", "/" + Path(directory).name).decode().splitlines()
