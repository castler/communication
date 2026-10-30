#!/usr/bin/env python3
"""Boots the QNX IFS N times through the real ITF qemu plugin code (QemuProcess, QemuTarget and
the pre-test checks with the boot retry) and reports how many boots needed a restart after a
guest kernel panic and how many still failed.

usage: qnx_itf_boot_stress.py ITF_DIR IMG QEMU_CONFIG N PAR MAX_ATTEMPTS LOGDIR
"""
import collections
import json
import logging
import queue
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor

ITF_DIR, IMG, QEMU_CONFIG, N, PAR, MAX_ATTEMPTS, LOGDIR = sys.argv[1:8]
N, PAR, MAX_ATTEMPTS = int(N), int(PAR), int(MAX_ATTEMPTS)
sys.path.insert(0, ITF_DIR)

import score.itf.plugins.qemu as qemu_plugin  # noqa: E402
from score.itf.plugins.qemu.config import load_configuration  # noqa: E402
from score.itf.plugins.qemu.qemu_target import qemu_target  # noqa: E402
from score.itf.core.utils.bunch import Bunch  # noqa: E402

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s", filename=f"{LOGDIR}/itf.log")

ports = queue.Queue()
for p in range(22000, 22000 + PAR):
    ports.put(p)

lock = threading.Lock()
stats = collections.Counter()
panic_boots = []


def make_config(port):
    cfg = json.load(open(QEMU_CONFIG))
    cfg["ssh_port"] = port
    cfg["port_forwarding"] = [{"host_port": port, "guest_port": 22}]
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(cfg, f)
    f.close()
    return load_configuration(f.name)


def one_boot(i):
    port = ports.get()
    attempts = 0
    try:
        real_restart = None
        cfg = make_config(port)
        with qemu_target(
            Bunch(qemu_config=cfg, qemu_kernel=IMG, qemu_rootfs=None, qemu_disk=None)
        ) as target:
            real_restart = target.restart

            def counting_restart():
                nonlocal attempts
                attempts += 1
                real_restart()

            target.restart = counting_restart
            qemu_plugin._boot_until_ready(target, MAX_ATTEMPTS)
        result = "ok_after_restart" if attempts else "ok"
    except Exception as e:  # noqa: BLE001
        result = "FAILED"
        logging.error("boot %d failed: %r", i, e)
    finally:
        ports.put(port)
    with lock:
        stats[result] += 1
        stats["restarts"] += attempts
        if attempts or result == "FAILED":
            panic_boots.append((i, result, attempts))
        done = sum(v for k, v in stats.items() if k.startswith("ok") or k == "FAILED")
        if done % 50 == 0 or attempts or result == "FAILED":
            print(f"[{done}/{N}] {dict(stats)}", flush=True)


with ThreadPoolExecutor(PAR) as ex:
    list(ex.map(one_boot, range(N)))

print("FINAL", dict(stats), "panic_boots", panic_boots, flush=True)
