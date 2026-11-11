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
import struct
import subprocess
import time


XV6 = Path(__file__).resolve().parents[1]
FATLAB = XV6.parent / "fat32-lab"
REJECTED_VOLUMES = ("bad-signature", "bad-sector-size", "truncated",
                    "root-cycle", "root-mismatch")


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


def prepare_faults(out):
    clean = out / "fat32.img"
    with clean.open("rb") as stream:
        boot = stream.read(512)
    bps = struct.unpack_from("<H", boot, 11)[0]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fat_bytes = struct.unpack_from("<I", boot, 36)[0] * bps
    fat_start = reserved * bps
    root = struct.unpack_from("<I", boot, 44)[0]
    assert boot[16] == 2, "fault fixtures require two FAT copies"

    def copy(label):
        image = out / f"fat32-{label}.img"
        shutil.copyfile(clean, image)
        return image

    def patch(image, offset, data):
        with image.open("r+b") as stream:
            stream.seek(offset)
            stream.write(data)

    def link(image, fat, cluster, value):
        patch(image, fat_start + fat * fat_bytes + cluster * 4,
              struct.pack("<I", value))

    patch(copy("bad-signature"), 510, b"\x00")
    patch(copy("bad-sector-size"), 11, struct.pack("<H", 768))
    with copy("truncated").open("r+b") as stream:
        stream.truncate(clean.stat().st_size // 2)
    root_cycle = copy("root-cycle")
    for fat in range(2):
        link(root_cycle, fat, root, root)
    link(copy("root-mismatch"), 1, root, root)

    # These cluster numbers are from fat32-lab/test_fat32.py's clean fixture.
    chains = copy("bad-chains")
    for fat in range(2):
        link(chains, fat, 40, 40)  # self loop
        link(chains, fat, 45, 0x0FFFFFFF)  # too short for file size
        link(chains, fat, 50, 0x0FFFFFF7)  # bad cluster marker
    link(chains, 1, 10, 0x0FFFFFFF)  # mirrored copies disagree
    for label in (*REJECTED_VOLUMES, "bad-chains"):
        (out / f"fat32-{label}.sha256").write_text(
            digest(out / f"fat32-{label}.img") + "\n")

    active = copy("active-fat")
    patch(active, 40, struct.pack("<H", 0x0081))
    link(active, 0, 10, 0x0FFFFFF7)  # inactive copy must be ignored
    link(active, 1, 3, 0xA0000000)  # free cluster with reserved high bits
    with active.open("rb") as stream:
        stream.seek(fat_start)
        inactive = stream.read(fat_bytes)
    (out / "inactive-fat.sha256").write_text(hashlib.sha256(inactive).hexdigest())


def prepare_guest(out):
    out.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(XV6 / "kernel/kernel", out / "kernel")
    shutil.copyfile(XV6 / "fs.img", out / "fs.img")


def prepare(out):
    prepare_guest(out)
    command(["make", "-C", str(FATLAB), "all"], log=out / "prepare.log")
    reader = FATLAB / "build/fat32_reader"
    mkfs = shutil.which("mkfs.fat")
    if mkfs is None:
        raise RuntimeError("mkfs.fat not found; install Ubuntu package dosfstools")
    command(["python3", str(FATLAB / "test_fat32.py"), str(reader), mkfs,
             "--keep-image", str(out / "fat32.img")], log=out / "prepare.log")
    (out / "fat32.sha256").write_text(digest(out / "fat32.img") + "\n")
    prepare_faults(out)
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
        args += ["-drive", f"file={out / fat_image},if=none,format=raw,id=d2,cache=writeback",
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
    guest(out, [("fat32persist", "fat32persist: PASS")],
          log_name="qemu-write-reboot.log")
    for label in REJECTED_VOLUMES:
        print(f"Checking rejected volume: {label}", flush=True)
        guest(out, [("fat32missing", "fat32missing: PASS")],
              fat_image=f"fat32-{label}.img", log_name=f"qemu-{label}.log")
    guest(out, [("fat32fault", "fat32fault: PASS")],
          fat_image="fat32-bad-chains.img", log_name="qemu-bad-chains.log")
    guest(out, [("fat32test", "fat32test: ALL TESTS PASSED")],
          fat_image="fat32-active-fat.img", log_name="qemu-active-fat.log")
    guest(out, [("fat32missing", "fat32missing: PASS")],
          fat_image=None, log_name="qemu-no-fat-disk.log")


def check(out):
    for label in (*REJECTED_VOLUMES, "bad-chains"):
        expected = (out / f"fat32-{label}.sha256").read_text().strip()
        if digest(out / f"fat32-{label}.img") != expected:
            raise AssertionError(f"damaged FAT32 fixture changed: {label}")
    with (out / "fat32-active-fat.img").open("rb") as stream:
        boot = stream.read(512)
        bps = struct.unpack_from("<H", boot, 11)[0]
        fat_bytes = struct.unpack_from("<I", boot, 36)[0] * bps
        stream.seek(struct.unpack_from("<H", boot, 14)[0] * bps)
        inactive = stream.read(fat_bytes)
        stream.seek(3 * 4, os.SEEK_CUR)
        entry = struct.unpack("<I", stream.read(4))[0]
    if hashlib.sha256(inactive).hexdigest() != (out / "inactive-fat.sha256").read_text():
        raise AssertionError("inactive FAT copy changed")
    if (entry >> 28) != 0xA:
        raise AssertionError("active FAT entry lost reserved high bits")
    command(["fsck.fat", "-n", str(out / "fat32.img")], log=out / "check.log")
    hello = subprocess.run([str(FATLAB / "build/fat32_reader"), str(out / "fat32.img"),
                            "cat", "/HELLO.TXT"], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
    if hello.returncode or hello.stdout != b"hello from FAT32\n":
        raise AssertionError("host reader HELLO.TXT readback failed")
    expected_files = {
        "/NEW.TXT": b"ok\n",
        "/NEWDIR/CHILD.TXT": b"directory write works\n",
        "/NEWDIR/INNER/DEEP.TXT": b"deeper\n",
        "/OPEN.TXT": b"new",
    }
    for path, expected in expected_files.items():
        result = subprocess.run([str(FATLAB / "build/fat32_reader"),
                                 str(out / "fat32.img"), "cat", path],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode or result.stdout != expected:
            raise AssertionError(f"host reader {path} readback failed")
    for path in ("/DELROOT", "/GONE"):
        result = subprocess.run([str(FATLAB / "build/fat32_reader"),
                                 str(out / "fat32.img"), "stat", path],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode == 0:
            raise AssertionError(f"host reader found deleted path {path}")
    print("PASS: FAT32 image writable, fsck.fat clean, host readback passed")
    print("PASS: damaged images and inactive FAT unchanged; reserved high bits kept")


def large_prepare(out):
    prepare_guest(out)
    image = out / "fat32-large.img"
    with image.open("xb") as stream:
        stream.truncate(544 * 1024 * 1024)
    command(["mkfs.fat", "--invariant", "-F", "32", "-S", "512",
             "-s", "16", str(image)], log=out / "prepare.log")
    with image.open("r+b") as stream:
        boot = stream.read(512)
        bps = struct.unpack_from("<H", boot, 11)[0]
        assert bps * boot[13] == 8192, "expected an 8 KiB cluster"
        # Seed the first free cluster to catch incomplete directory zeroing.
        assert struct.unpack_from("<I", boot, 44)[0] == 2
        stream.seek(large_data_offset(boot) + 8192)
        stream.write(b"\xa5" * 8192)


def large_data_offset(boot):
    bps = struct.unpack_from("<H", boot, 11)[0]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fat_sectors = struct.unpack_from("<I", boot, 36)[0]
    return (reserved + boot[16] * fat_sectors) * bps


def large_run(out):
    guest(out, [("fat32large", "fat32large: PASS")],
          fat_image="fat32-large.img", log_name="qemu-large.log")


def large_check(out):
    image = out / "fat32-large.img"
    command(["fsck.fat", "-n", str(image)], log=out / "check.log")
    with image.open("rb") as stream:
        root = large_data_offset(stream.read(512))
        stream.seek(root)
        entry = stream.read(32)
        first = (struct.unpack_from("<H", entry, 20)[0] << 16 |
                 struct.unpack_from("<H", entry, 26)[0])
        if entry[:11] != b"BIGDIR     " or first != 3:
            raise AssertionError("BIGDIR did not use the seeded cluster")
        stream.seek(root + 8192 + 4096)
        if stream.read(4096) != bytes(4096):
            raise AssertionError("new directory retained stale data past the first page")
    print("PASS: 8 KiB cluster fully zeroed and fsck.fat clean")


def vfat_entries(name, alias, *, cluster=0, size=0, bad_checksum=False):
    units = name.encode("utf-16le")
    if len(units) % 26:
        units += b"\0\0"
        units += b"\xff\xff" * ((-len(units) % 26) // 2)
    checksum = 0
    for byte in alias:
        checksum = ((checksum >> 1) + ((checksum & 1) << 7) + byte) & 255
    slots = []
    count = len(units) // 26
    for i in range(count, 0, -1):
        chars = units[(i - 1) * 26:i * 26]
        raw = bytearray(32)
        raw[0] = i | (0x40 if i == count else 0)
        raw[11] = 0x0F
        raw[13] = checksum ^ int(bad_checksum)
        raw[1:11], raw[14:26], raw[28:32] = chars[:10], chars[10:22], chars[22:]
        slots.append(bytes(raw))
    short = bytearray(32)
    short[:11], short[11] = alias, 0x20
    struct.pack_into("<HI", short, 26, cluster, size)
    return slots + [bytes(short)]


def vfat_prepare(out):
    prepare_guest(out)
    image = out / "fat32-vfat.img"
    with image.open("xb") as stream:
        stream.truncate(64 * 1024 * 1024)
    command(["mkfs.fat", "--invariant", "-F", "32", "-S", "512",
             "-s", "1", str(image)], log=out / "prepare.log")
    good = vfat_entries("Project 长名字 🧪 Report.txt", b"PROJEC~1TXT",
                        cluster=4, size=4)
    broken = vfat_entries("Broken long name.txt", b"BROKEN~1TXT",
                          bad_checksum=True)
    with image.open("r+b") as stream:
        boot = stream.read(512)
        bps, spc, reserved, fats = struct.unpack_from("<HBHB", boot, 11)
        fat_sectors, _, _, root = struct.unpack_from("<IHHI", boot, 36)
        assert (bps, spc, root) == (512, 1, 2) and len(good) == 3
        for fat in range(fats):
            stream.seek((reserved + fat * fat_sectors) * bps + 2 * 4)
            stream.write(struct.pack("<III", 3, 0x0FFFFFFF, 0x0FFFFFFF))
        stream.seek((reserved + fats * fat_sectors) * bps)
        # The first long-name slot is the last slot of root cluster 2.
        stream.write((b"\xe5" + bytes(31)) * 15 + good[0])
        stream.write(b"".join(good[1:] + broken).ljust(512, b"\0"))
        stream.write(b"vfat")
    (out / "fat32-vfat.sha256").write_text(digest(image) + "\n")


def vfat_run(out):
    guest(out, [("fat32vfat", "fat32vfat: PASS")],
          fat_image="fat32-vfat.img", log_name="qemu-vfat.log")


def vfat_check(out):
    if digest(out / "fat32-vfat.img") != (out / "fat32-vfat.sha256").read_text().strip():
        raise AssertionError("VFAT lookup or rejected unlink changed the image")
    print("PASS: VFAT fixture unchanged")


def vwrite_prepare(out):
    prepare_guest(out)
    vwrite_volume(out)


def vwrite_volume(out):
    image = out / "fat32-vwrite.img"
    with image.open("xb") as stream:
        stream.truncate(64 * 1024 * 1024)
    command(["make", "-C", str(FATLAB), "all"], log=out / "prepare.log")
    command(["mkfs.fat", "--invariant", "-F", "32", "-S", "512",
             "-s", "1", str(image)], log=out / "prepare.log")
    with image.open("r+b") as stream:
        boot = stream.read(512)
        assert struct.unpack_from("<I", boot, 44)[0] == 2
        assert boot[13] == 1
        stream.seek(large_data_offset(boot))
        for i in range(15):
            entry = bytearray(32)
            entry[:11], entry[11] = f"F{i:07d}   ".encode(), 0x20
            stream.write(entry)
        stream.write(bytes(32))


def vwrite_run(out):
    guest(out, [("fat32vwrite", "fat32vwrite: PASS")],
          fat_image="fat32-vwrite.img", log_name="qemu-vwrite.log")


def vwrite_check(out):
    image = out / "fat32-vwrite.img"
    command(["fsck.fat", "-n", str(image)], log=out / "check.log")
    for path, expected in (("/REPORT~1.TXT", b"kept"), ("/EXACTL~1.TXT", b"exact")):
        result = subprocess.run([str(FATLAB / "build/fat32_reader"),
                                 str(image), "cat", path],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode or result.stdout != expected:
            raise AssertionError(f"host readback failed: {path}")
    # Independently compare every remaining live long slot with the reference
    # encoder. fsck above also checks freed chains and orphaned long entries.
    expected_names = {b"REPORT~1TXT": "Report 初稿 🧪.txt",
                      b"EXACTL~1TXT": "Exactly13.txt"}
    seen, pending = set(), []
    with image.open("rb") as stream:
        boot = stream.read(512)
        bps = struct.unpack_from("<H", boot, 11)[0]
        fat = struct.unpack_from("<H", boot, 14)[0] * bps
        data = large_data_offset(boot)
        cluster = struct.unpack_from("<I", boot, 44)[0]
        visited = set()
        done = False
        while not done:
            if cluster in visited:
                raise AssertionError("root chain cycle")
            visited.add(cluster)
            stream.seek(data + (cluster - 2) * bps * boot[13])
            chunk = stream.read(bps * boot[13])
            for pos in range(0, len(chunk), 32):
                raw = chunk[pos:pos + 32]
                if raw[0] == 0:
                    done = True
                    break
                if raw[0] == 0xE5:
                    if pending:
                        raise AssertionError("orphaned long name before deleted slot")
                elif raw[11] == 0x0F:
                    pending.append(raw)
                else:
                    alias = raw[:11]
                    if alias in expected_names:
                        reference = vfat_entries(expected_names[alias], alias)[:-1]
                        if pending != reference:
                            raise AssertionError("long slots differ from reference")
                        seen.add(alias)
                    elif pending:
                        raise AssertionError("unexpected long entry group")
                    pending = []
            if not done:
                stream.seek(fat + cluster * 4)
                cluster = struct.unpack("<I", stream.read(4))[0] & 0x0FFFFFFF
                done = cluster >= 0x0FFFFFF8
    if pending or seen != set(expected_names):
        raise AssertionError("missing or dangling long entries")
    print("PASS: VFAT create/delete, host data and long slots checked; fsck.fat clean")


def shared_prepare(out):
    # Reuse the clean fixture, including a directory-cluster boundary.
    vwrite_prepare(out)


def shared_run(out):
    guest(out, [("fat32shared", "fat32shared: PASS")],
          fat_image="fat32-vwrite.img", log_name="qemu-shared.log")


def shared_check(out):
    image = out / "fat32-vwrite.img"
    command(["fsck.fat", "-n", str(image)], log=out / "check.log")
    result = subprocess.run([str(FATLAB / "build/fat32_reader"),
                             str(image), "cat", "/SHARED~1.TXT"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode or result.stdout != b"new":
        raise AssertionError("host readback of recreated shared file failed")
    print("PASS: shared-file lifetime and host readback checked; fsck.fat clean")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "run", "check"))
    parser.add_argument("out", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--large-cluster", action="store_true",
                        help="use one 8 KiB cluster write/overwrite scenario")
    mode.add_argument("--vfat", action="store_true",
                      help="use one VFAT long-name lookup scenario")
    mode.add_argument("--vfat-write", action="store_true",
                      help="use one VFAT create/delete scenario")
    mode.add_argument("--shared", action="store_true",
                      help="use one multi-handle file-state scenario")
    args = parser.parse_args()
    prefix = ("large_" if args.large_cluster else "vfat_" if args.vfat else
              "vwrite_" if args.vfat_write else "shared_" if args.shared else "")
    globals()[prefix + args.phase](args.out.resolve())


if __name__ == "__main__":
    main()
