#!/usr/bin/env python3
"""Boots the QNX IFS repeatedly with the CI QEMU command line, counts kernel panics and
captures vCPU registers plus guest memory through the QEMU monitor when one occurs."""
import collections
import os
import socket
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

IMG = sys.argv[1]
N = int(sys.argv[2])
PAR = int(sys.argv[3])
LOGDIR = sys.argv[4]
EXTRA = sys.argv[5].split() if len(sys.argv) > 5 else []

MON_CMDS = [
    "info registers -a",
    "info cpus",
    "cpu 0", "x /400gx 0xffff800004e85000",
    "cpu 1", "x /400gx 0xffff800004e85000",
]


def monitor_dump(sock_path, i):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(3)
    s.connect(sock_path)
    out = b""

    def rd():
        nonlocal out
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
        except socket.timeout:
            pass

    rd()
    for c in MON_CMDS:
        s.sendall((c + "\n").encode())
        rd()
    raw = f"{LOGDIR}/boot_{i}_mem.raw"
    s.sendall(f'pmemsave 0 1073741824 "{raw}"\n'.encode())
    time.sleep(10)
    rd()
    with open(f"{LOGDIR}/boot_{i}_monitor.txt", "wb") as f:
        f.write(out)
    subprocess.run(["gzip", "-f", raw], check=False)


def boot(i):
    sock_path = f"/tmp/qmon_{os.getpid()}_{i}.sock"
    cmd = ["qemu-system-x86_64", "-smp", "2,maxcpus=2,cores=2", "-cpu", "Cascadelake-Server-v5", "-accel", "kvm",
           "-machine", "pc", "-m", "1G", "-nographic", "-serial", "stdio",
           "-monitor", f"unix:{sock_path},server,nowait",
           "-object", "rng-random,filename=/dev/urandom,id=rng0", "-device", "virtio-rng-pci,rng=rng0",
           "-netdev", "user,id=net1", "-device", "virtio-net-pci,netdev=net1", "-kernel", IMG] + EXTRA
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    watchdog = threading.Timer(30, p.kill)
    watchdog.start()
    verdict, lines = "timeout", []
    try:
        for line in p.stdout:
            lines.append(line)
            if "Shutdown[" in line:
                verdict = "PANIC"
                break
            if "Welcome to QNX" in line:
                verdict = "PANIC" if os.environ.get("DUMP_TEST") else "ok"
                break
        if verdict == "PANIC":
            time.sleep(1)
            try:
                monitor_dump(sock_path, i)
            except Exception as e:
                lines.append(f"monitor dump failed: {e!r}\n")
    finally:
        watchdog.cancel()
        p.kill()
        p.wait()
        if os.path.exists(sock_path):
            os.remove(sock_path)
    if verdict != "ok":
        with open(f"{LOGDIR}/boot_{i}_{verdict}.log", "w") as f:
            f.writelines(lines)
    return verdict


with ThreadPoolExecutor(PAR) as ex:
    res = []
    for k, v in enumerate(ex.map(boot, range(N))):
        res.append(v)
        if (k + 1) % 100 == 0:
            print(k + 1, dict(collections.Counter(res)), flush=True)
print("FINAL", dict(collections.Counter(res)), flush=True)
