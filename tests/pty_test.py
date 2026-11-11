#!/usr/bin/env python3
"""PTY based integration test for the MC68VZ328 emulator.

The emulator exposes its emulated UART on a pseudo terminal with --pty-link.
This script connects to that PTY like m68k-term/kermit would and checks the
interactive behaviour of the guest program.

Usage:
    pty_test.py load  EMULATOR ABS EXPECTED
    pty_test.py uart2 EMULATOR ABS EXPECTED
    pty_test.py ipl   EMULATOR ABS EXPECTED
"""

import errno
import os
import select
import subprocess
import sys
import tempfile
import termios
import time
import tty


class PtyChannel:
    def __init__(self, fd):
        self.fd = fd
        self.data = b""

    def expect(self, needle, timeout=10.0):
        needle = needle.encode()
        deadline = time.monotonic() + timeout
        while needle not in self.data:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    "timed out waiting for %r; received %r" % (needle, self.data)
                )
            ready, _, _ = select.select([self.fd], [], [], remaining)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError as exc:
                if exc.errno == errno.EIO:
                    chunk = b""
                else:
                    raise
            if not chunk:
                raise EOFError(
                    "PTY closed while waiting for %r; received %r"
                    % (needle, self.data)
                )
            self.data += chunk
        return self.data

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.fd, data)


def wait_for_link(path, proc, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return
        if proc.poll() is not None:
            raise RuntimeError("emulator exited before creating %s" % path)
        time.sleep(0.01)
    raise TimeoutError("PTY link %s did not appear" % path)


def open_pty(path, proc):
    wait_for_link(path, proc)
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd, when=termios.TCSANOW)
    return fd


def main():
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2

    mode, emulator, abs_file, expected = sys.argv[1:]
    tmpdir = tempfile.mkdtemp(prefix="vz328-pty-")
    link = os.path.join(tmpdir, "tty")
    fd = None
    proc = None

    try:
        if mode == "load":
            args = [emulator, "--load", abs_file, "--pty-link", link]
        elif mode == "uart2":
            args = [emulator, "--load", abs_file, "--uart2-link", link]
        elif mode == "ipl":
            args = [emulator, "--ipl", "--pty-link", link]
        else:
            print("unknown mode: %s" % mode, file=sys.stderr)
            return 2

        proc = subprocess.Popen(
            args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        fd = open_pty(link, proc)
        chan = PtyChannel(fd)

        if mode == "load":
            chan.expect("Type characters (q to quit):")
            chan.send("abq")
            chan.expect(expected)
        elif mode == "ipl":
            chan.expect("waiting for S-file")
            with open(abs_file, "rb") as f:
                chan.send(f.read())
            chan.expect("hit any key")
            chan.send("x")
            chan.expect(expected)
        else:  # uart2
            chan.expect(expected)

        try:
            rc = proc.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise RuntimeError("emulator did not exit")
        stderr = proc.stderr.read().decode(errors="replace")
        if rc != 0:
            raise RuntimeError("emulator exited with %d:\n%s" % (rc, stderr))

        print(chan.data.decode(errors="replace"))
        return 0
    except Exception as exc:  # noqa: BLE001 - report any failure to the shell
        print("PTY test failed: %s" % exc, file=sys.stderr)
        if proc is not None and proc.poll() is None:
            proc.kill()
        return 1
    finally:
        if fd is not None:
            os.close(fd)
        if proc is not None and proc.poll() is None:
            proc.kill()
        try:
            os.unlink(link)
        except OSError:
            pass
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
