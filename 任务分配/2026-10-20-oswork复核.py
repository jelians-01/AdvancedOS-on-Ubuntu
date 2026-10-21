"""Reproduce the supplied commit in a disposable Linux directory; keep evidence."""
import hashlib
import io
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tarfile
import tempfile
import time

ROOT = Path("/mnt/d/AdvancedOS")
SOURCE = ROOT / "队友进展/oswork/AdvancedOS-on-Ubuntu"
OUT = ROOT / "任务分配"
PREFIX = "2026-10-20-oswork"
COMMIT = "a9e1454a70801450f64bf8e0bb5351d47426d821"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, cwd=None):
    result = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    with (OUT / f"{PREFIX}-build.log").open("ab") as log:
        log.write(("\n$ " + " ".join(map(str, args)) + "\n").encode())
        log.write(result.stdout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {args[0]}")
    return result.stdout


def guest(work, disk, name, commands):
    root_disk = work / f"{name}-root.img"
    shutil.copy2(work / "xv6-riscv/fs.img", root_disk)
    args = ["qemu-system-riscv64", "-machine", "virt", "-bios", "none",
            "-kernel", str(work / "xv6-riscv/kernel/kernel"), "-m", "128M",
            "-smp", "3", "-nographic", "-monitor", "none",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={root_disk},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0"]
    if disk:
        args += ["-drive", f"file={disk},if=none,format=raw,id=x1,cache=writeback",
                 "-device", "virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1"]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    output = bytearray()
    results = {}
    log_path = OUT / f"{PREFIX}-{name}.log"
    with log_path.open("wb") as log:
        log.write(("$ " + " ".join(args) + "\n").encode())

        def until_prompt(timeout):
            begin = len(output)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if select.select([proc.stdout], [], [], 0.5)[0]:
                    data = os.read(proc.stdout.fileno(), 65536)
                    if not data:
                        raise RuntimeError("QEMU exited before shell prompt")
                    output.extend(data)
                    log.write(data)
                    log.flush()
                    part = bytes(output[begin:])
                    if b"panic:" in part:
                        return part.decode(errors="replace"), "panic"
                    if part.endswith(b"$ "):
                        return part.decode(errors="replace"), "prompt"
            return bytes(output[begin:]).decode(errors="replace"), "timeout"

        try:
            text, status = until_prompt(45)
            results["boot"] = {"status": status, "output": text}
            if status == "prompt":
                for command, timeout in commands:
                    proc.stdin.write((command + "\n").encode())
                    proc.stdin.flush()
                    text, status = until_prompt(timeout)
                    results[command] = {"status": status, "output": text}
                    print(f"{name}: {command}: {status}", flush=True)
                    if status != "prompt":
                        break
        finally:
            proc.terminate()
            try:
                tail, _ = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                tail, _ = proc.communicate(timeout=5)
            log.write(tail)
    return {"commands": results, "log": str(log_path), "qemu_args": args}


def main():
    # Fail rather than replace an earlier evidence set.
    if (OUT / f"{PREFIX}-manifest.json").exists():
        raise RuntimeError("Evidence already exists; preserve it and use a new prefix.")
    work = Path(tempfile.mkdtemp(prefix="advancedos-oswork-review-"))
    archive = subprocess.check_output(
        ["git", "-C", str(SOURCE), "archive", COMMIT, "xv6-riscv", "ext2-lab"])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(work, filter="data")
    (OUT / f"{PREFIX}-build.log").write_text(
        f"Source: {SOURCE}\nCommit: {COMMIT}\nWorkspace: {work}\n")
    manifest = {"source": str(SOURCE), "commit": COMMIT, "work": str(work)}
    for command in [["gcc", "--version"], ["riscv64-linux-gnu-gcc", "--version"],
                    ["qemu-system-riscv64", "--version"], ["mke2fs", "-V"]]:
        run(command)
    run(["make", "-j4", "TOOLPREFIX=riscv64-linux-gnu-", "kernel/kernel", "fs.img"],
        work / "xv6-riscv")
    run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-o",
         str(work / "ext2_reader"), str(work / "ext2-lab/ext2_reader.c")])
    image = work / "review.ext2"
    with image.open("xb") as f:
        f.truncate(32 * 1024 * 1024)
    run(["mke2fs", "-t", "ext2", "-F", "-b", "1024", "-I", "128", "-O", "none",
         str(image)])
    (work / "hello.txt").write_bytes(b"hello from ext2\n")
    (work / "multi.bin").write_bytes(bytes(i % 251 for i in range(3073)))
    for filename in ["hello.txt", "multi.bin"]:
        run(["debugfs", "-w", "-R",
             f"write {work / filename} /{filename}", str(image)])
    run(["e2fsck", "-fn", str(image)])
    for command in [["info"], ["ls", "/"]]:
        run([str(work / "ext2_reader"), str(image)] + command)
    manifest["image_before"] = sha(image)
    manifest["image_parameters"] = {
        "size": 32 * 1024 * 1024, "block_size": 1024, "inode_size": 128,
        "features": "none", "hello": "hello from ext2\\n", "hello_bytes": 16,
        "multi_bytes": 3073, "multi_pattern": "byte[i] = i % 251"}
    print(f"Built untouched commit in {work}", flush=True)
    manifest["original"] = guest(work, image, "original", [
        ("ext2test", 60), ("ls /mnt", 30), ("cat /mnt/hello.txt", 30),
        ("wc /mnt/multi.bin", 30), ("ls /mnt/..", 30),
        ("vfsdir", 90), ("vfspath", 90), ("vfsexec", 90),
        ("usertests", 900)])
    manifest["image_after"] = sha(image)
    manifest["single_root"] = guest(work, None, "single-root", [
        ("usertests copyin", 60)])
    # A corrupted root inode table pointer must fail safely, not panic.
    corrupt = work / "bad-root-table.ext2"
    shutil.copy2(image, corrupt)
    with corrupt.open("r+b") as f:
        f.seek(2 * 1024 + 8)
        f.write(b"\0" * 4)
    manifest["bad_root_table"] = guest(work, corrupt, "bad-root-table", [])
    manifest["evidence_hashes"] = {
        p.name: sha(p) for p in OUT.glob(f"{PREFIX}-*.log")}
    (OUT / f"{PREFIX}-manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print("Evidence saved.", flush=True)


if __name__ == "__main__":
    main()
