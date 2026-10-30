#!/usr/bin/env python3
"""Boots the QNX IFS repeatedly with the CI QEMU command line and counts kernel panics."""
import collections
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

IMG = sys.argv[1]
N = int(sys.argv[2])
PAR = int(sys.argv[3])
LOGDIR = sys.argv[4]


def boot(i):
    cmd = ["qemu-system-x86_64", "-smp", "2,maxcpus=2,cores=2", "-cpu", "Cascadelake-Server-v5", "-accel", "kvm",
           "-machine", "pc", "-m", "1G", "-nographic", "-serial", "mon:stdio",
           "-object", "rng-random,filename=/dev/urandom,id=rng0", "-device", "virtio-rng-pci,rng=rng0",
           "-netdev", "user,id=net1", "-device", "virtio-net-pci,netdev=net1", "-kernel", IMG]
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
            if "Starting Networking" in line:
                verdict = "ok"
                break
    finally:
        watchdog.cancel()
        p.kill()
        p.wait()
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
