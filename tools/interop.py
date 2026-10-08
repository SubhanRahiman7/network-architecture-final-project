#!/usr/bin/env python3
"""Interop test: the C++ programs against independent Python implementations, both directions.   (make interop)"""
import os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = sys.executable
fails = 0


def check(cond, label):
    global fails
    print(("ok   " if cond else "FAIL ") + label)
    fails += 0 if cond else 1


with tempfile.TemporaryDirectory() as doc:
    open(os.path.join(doc, "hello.txt"), "wb").write(b"Hello, packed binary world!\n")
    blob = bytes(range(256)) * 4096                       # 1 MiB of every byte value, including NUL
    open(os.path.join(doc, "blob.bin"), "wb").write(blob)
    secret = os.path.join(os.path.dirname(doc), "secret.txt")
    open(secret, "w").write("secret\n")

    print("== Python client (written from the spec) -> C++ server pbfd")
    srv = subprocess.Popen([os.path.join(ROOT, "pbfd"), doc, "9301"], stderr=subprocess.PIPE)
    time.sleep(0.6)
    r = subprocess.run([PY, os.path.join(HERE, "stranger_client.py"), "127.0.0.1", "9301"], capture_output=True, text=True)
    print(r.stdout, end="")
    check(r.returncode == 0, "every conformance check passed against pbfd")
    srv.terminate()
    log = srv.communicate()[1].decode()
    check(log.count("accepted") == 1, "the whole run used exactly one TCP connection")

    print("== C++ client pbf -> Python server (written from the spec)")
    py = subprocess.Popen([PY, os.path.join(HERE, "stranger_server.py"), doc, "9302"], stdout=subprocess.PIPE)
    py.stdout.readline()
    c = subprocess.run([os.path.join(ROOT, "pbf"), "127.0.0.1:9302/hello.txt", "/blob.bin", "/hello.txt"], capture_output=True)
    check(c.returncode == 0 and c.stdout == b"Hello, packed binary world!\n" + blob + b"Hello, packed binary world!\n",
          "three files over one connection, binary-safe (exit 0)")
    c = subprocess.run([os.path.join(ROOT, "pbf"), "127.0.0.1:9302/missing"], capture_output=True)
    check(c.returncode == 1, "404 -> exit status 1")
    c = subprocess.run([os.path.join(ROOT, "pbf"), "-v", "-H", "X-Trace: demo", "127.0.0.1:9302/hello.txt"], capture_output=True)
    check(c.returncode == 0 and b"X-Trace" in c.stderr, "-v and a literal -H header work against it")
    py.terminate()

print("\nINTEROP OK" if not fails else f"\n{fails} CHECK(S) FAILED")
sys.exit(1 if fails else 0)
