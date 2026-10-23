"""Disposable xv6 FAT32 VFS integration test.

  python3 tests/run_fat32_vfs_tests.py prepare /absolute/new/output/directory
  XV6_QEMU=/path/to/qemu-system-riscv64 python3 tests/run_fat32_vfs_tests.py run /path/to/output
  python3 tests/run_fat32_vfs_tests.py check /path/to/output

The FAT32 fixture is created by fat32-lab/test_fat32.py and is attached only
as device 3. This script never writes the caller's FAT32 image.
"""
import argparse
import hashlib
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import time


XV6 = Path(__file__).resolve().parents[1]
FATLAB = XV6.parent / "fat32-lab"


def command(args, *, log=None):
    result = subprocess.run(args, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if log:
        with log.open("a", encoding="utf-8") as stream:
            stream.write("$ " + " ".join(map(str, args)) + "\n")
            stream.write(result.stdout)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(map(str, args))}\n{result.stdout}")
    return result.stdout


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def prepare(out):
    out.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(XV6 / "kernel/kernel", out / "kernel")
    shutil.copyfile(XV6 / "fs.img", out / "fs.img")
    reader = FATLAB / "build/fat32_reader"
    command(["make", "-C", str(FATLAB), "all"], log=out / "prepare.log")
    reader = FATLAB / "build/fat32_reader"
    mkfs = shutil.which("mkfs.fat")
    if mkfs is None:
        raise RuntimeError("mkfs.fat not found; install Ubuntu package dosfstools")
    command(["python3", str(FATLAB / "test_fat32.py"), str(reader), mkfs,
             "--keep-image", str(out / "fat32.img")], log=out / "prepare.log")
    (out / "fat32.sha256").write_text(digest(out / "fat32.img") + "\n")
    bad = out / "fat32-bad-signature.img"
    shutil.copyfile(out / "fat32.img", bad)
    with bad.open("r+b") as stream:
        stream.seek(510)
        stream.write(b"\x00")
    (out / "fat32-bad-signature.sha256").write_text(digest(bad) + "\n")
    print(f"Prepared FAT32 fixture: {out / 'fat32.img'}", flush=True)


def guest(out, commands, *, fat_image="fat32.img", log_name="qemu.log"):
    qemu = os.environ.get("XV6_QEMU", "qemu-system-riscv64")
    root_image = out / f"root-{Path(log_name).stem}.img"
    shutil.copyfile(out / "fs.img", root_image)
    args = [qemu, "-machine", "virt", "-bios", "none", "-kernel",
            str(out / "kernel"), "-m", "128M", "-smp", "3", "-display",
            "none", "-serial", "stdio", "-monitor", "none", "-global",
            "virtio-mmio.force-legacy=false",
            "-drive", f"file={root_image},if=none,format=raw,id=d0",
            "-device", "virtio-blk-device,drive=d0,bus=virtio-mmio-bus.0",
            ]
    if fat_image is not None:
        args += ["-drive", f"file={out / fat_image},if=none,format=raw,id=d2,cache=writeback,readonly=on",
                 "-device", "virtio-blk-device,drive=d2,bus=virtio-mmio-bus.2"]
    process = subprocess.Popen(args, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    transcript = bytearray()
    cursor = 0
    log_path = out / log_name
    with log_path.open("wb") as log:
        def until_prompt(timeout):
            nonlocal cursor
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                end = transcript.find(b"$ ", cursor)
                if end >= 0:
                    output = bytes(transcript[cursor:end + 2])
                    cursor = end + 2
                    if b"panic:" in output:
                        raise RuntimeError(output.decode(errors="replace"))
                    return output
                for key, _ in selector.select(0.2):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        raise RuntimeError(f"QEMU exited; inspect {log_path}")
                    transcript.extend(chunk)
                    log.write(chunk)
                    log.flush()
            raise TimeoutError(f"guest shell prompt timed out; inspect {log_path}")

        try:
            until_prompt(60)
            for cmd, expected in commands:
                process.stdin.write(cmd.encode() + b"\n")
                process.stdin.flush()
                output = until_prompt(120)
                if expected.encode() not in output:
                    raise RuntimeError(f"{cmd} failed: {output.decode(errors='replace')}")
                print(f"{cmd}: PASS", flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                tail, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                tail, _ = process.communicate(timeout=10)
            log.write(tail)
            selector.close()


def run(out):
    guest(out, [("fat32test", "fat32test: ALL TESTS PASSED"),
                ("vfsdir", "vfsdir: ALL TESTS PASSED"),
                ("usertests copyin", "ALL TESTS PASSED")])
    guest(out, [("fat32missing", "fat32missing: PASS")],
          fat_image="fat32-bad-signature.img", log_name="qemu-bad-signature.log")
    guest(out, [("fat32missing", "fat32missing: PASS")],
          fat_image=None, log_name="qemu-no-fat-disk.log")


def check(out):
    expected = (out / "fat32.sha256").read_text().strip()
    actual = digest(out / "fat32.img")
    if actual != expected:
        raise AssertionError("FAT32 read-only guest changed the fixture image")
    bad_expected = (out / "fat32-bad-signature.sha256").read_text().strip()
    if digest(out / "fat32-bad-signature.img") != bad_expected:
        raise AssertionError("invalid FAT32 fixture unexpectedly changed")
    command(["fsck.fat", "-n", str(out / "fat32.img")], log=out / "check.log")
    command([str(FATLAB / "build/fat32_reader"), str(out / "fat32.img"),
             "cat", "/HELLO.TXT"], log=out / "check.log")
    print("PASS: FAT32 image hash unchanged, fsck.fat clean, host reader readback passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "run", "check"))
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    globals()[args.phase](args.out.resolve())


if __name__ == "__main__":
    main()
