"""One RISC-V joint acceptance round: shared disks, one write boot and one readback boot.

Build kernel/kernel and fs.img first, then run prepare, run and check with
the same NEW output directory. prepare/check require e2fsprogs/dosfstools;
run requires QEMU. All disks are disposable copies.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import run_ext2_tests as ext2
import run_fat32_vfs_tests as fat32


def manifest(out):
    return json.loads((out / "joint.json").read_text())


def save(out, data):
    (out / "joint.json").write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def layout(image):
    with image.open("rb") as stream:
        boot = stream.read(512)
    bps = struct.unpack_from("<H", boot, 11)[0]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fat_bytes = struct.unpack_from("<I", boot, 36)[0] * bps
    total = struct.unpack_from("<I", boot, 32)[0]
    count = (total - reserved - boot[16] * (fat_bytes // bps)) // boot[13]
    info, backup = struct.unpack_from("<HH", boot, 48)
    assert 0 < info < reserved and 0 < backup + info < reserved
    return bps, reserved * bps, fat_bytes, count, info, backup


def backup_hash(image):
    bps, _, _, _, info, backup = layout(image)
    with image.open("rb") as stream:
        stream.seek((backup + info) * bps)
        return hashlib.sha256(stream.read(bps)).hexdigest()


def prepare(out):
    ext2.unlink_prepare(out)
    fat32.vwrite_volume(out)
    paths = ("kernel/ext2.c", "kernel/fat32.c", "user/fsjoint.c",
             "user/ext2mkdir.c", "user/ext2unlink.c", "user/ext2link.c",
             "user/fat32vwrite.c", "user/fat32shared.c",
             "tests/run_fs_joint_tests.py", "tests/run_ext2_tests.py",
             "tests/run_fat32_vfs_tests.py")
    save(out, {
        "scope": "RISC-V virt, 3 CPUs, native dev1 + ext2 dev2 + FAT32 dev3",
        "baseline_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ext2.SOURCE, text=True).strip(),
        "source_sha256": {path: fat32.digest(ext2.SOURCE / path) for path in paths},
        "kernel_sha256": fat32.digest(out / "kernel"),
        "root_image_sha256_initial": fat32.digest(out / "fs.img"),
        "ext2_initial_free": ext2.super_free(out / "ext2.img"),
        "fat32_backup_fsinfo_sha256": backup_hash(out / "fat32-vwrite.img"),
        "results": [],
    })
    print(f"Prepared three-device joint fixtures: {out}", flush=True)


def run(out):
    data = manifest(out)
    fat = "fat32-vwrite.img"
    ext2.guest(out, "joint-write", [
        ("ext2unlink", "ext2unlink: PASS"),
        ("ext2mkdir", "ext2mkdir: PASS"),
        ("ext2link", "ext2link: PASS"),
        ("fat32vwrite", "fat32vwrite: PASS"),
        ("fat32shared", "fat32shared: PASS"),
        ("vfsdir", "vfsdir: ALL TESTS PASSED"),
        ("fsjoint", "fsjoint: PASS"),
    ], fat_image=fat)
    hashes = {image: fat32.digest(out / image) for image in ("ext2.img", fat)}
    data["results"].append("three-device write, directory, unlink/link and shared-file tests: PASS")
    ext2.guest(out, "joint-reboot", [("fsjoint verify", "fsjoint: REBOOT PASS")],
               fat_image=fat)
    for image, expected in hashes.items():
        if fat32.digest(out / image) != expected:
            raise AssertionError(f"readback boot modified {image}")
    data["results"].append("same three images rebooted; data disks unchanged during readback: PASS")
    data["data_image_sha256"] = hashes
    save(out, data)


def check(out):
    data = manifest(out)
    if len(data["results"]) != 2:
        raise AssertionError("complete the guest run phase before host verification")
    image = out / "ext2.img"
    log = out / "joint-check.log"
    ext2.command(["e2fsck", "-fn", image], log)
    ext2.command(["fsck.fat", "-n", out / "fat32-vwrite.img"], log)
    pattern = b"XYZ" + bytes(i % 251 for i in range(3, 13000))
    expected = {
        "/tree/inner/data.bin": pattern, "/tree/short": b"ok\n",
        "/seed/keep.txt": b"seed unchanged\n",
        "/seed/new.txt": b"new in imported directory\n",
        "/keep-a": pattern, "/seed/keep-b": pattern, "/roundtrip.bin": pattern,
    }
    for i, (path, contents) in enumerate(expected.items()):
        target = out / f"export-joint-{i}"
        ext2.command(["debugfs", "-R", f"dump {path} {target}", image], log)
        if target.read_bytes() != contents:
            raise AssertionError(f"ext2 host comparison failed: {path}")
    inodes = []
    for path in ("/keep-a", "/seed/keep-b"):
        info = ext2.command(["debugfs", "-R", f"stat {path}", image], log).decode()
        inodes.append(re.search(r"Inode:\s+(\d+)", info).group(1))
        if not re.search(r"Links:\s+2\b", info):
            raise AssertionError("persistent hard-link count incorrect")
    if inodes[0] != inodes[1]:
        raise AssertionError("persistent hard-link inode identity incorrect")
    before, after = data["ext2_initial_free"], ext2.super_free(image)
    if [before[i] - after[i] for i in range(2)] != [47, 21]:
        raise AssertionError(f"joint ext2 allocation counts wrong: {before} -> {after}")
    fat = out / "fat32-vwrite.img"
    for path, contents in {
            "/REPORT~1.TXT": b"kept", "/EXACTL~1.TXT": b"exact",
            "/SHARED~1.TXT": b"new", "/XCOPY.BIN": pattern}.items():
        result = subprocess.run([str(fat32.FATLAB / "build/fat32_reader"),
                                 str(fat), "cat", path], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        if result.returncode or result.stdout != contents:
            raise AssertionError(f"FAT32 host comparison failed: {path}")
    bps, offset, length, count, sector, _ = layout(fat)
    with fat.open("rb") as stream:
        stream.seek(offset)
        first = stream.read(length)
        if first != stream.read(length):
            raise AssertionError("FAT mirrors disagree")
        actual_free = sum((struct.unpack_from("<I", first, i * 4)[0] & 0x0FFFFFFF) == 0
                          for i in range(2, count + 2))
        stream.seek(sector * bps + 488)
        free, hint = struct.unpack("<II", stream.read(8))
        if free != actual_free or not 2 <= hint <= count + 1:
            raise AssertionError("FSInfo disagrees with FAT allocation state")
    if backup_hash(fat) != data["fat32_backup_fsinfo_sha256"]:
        raise AssertionError("backup FSInfo unexpectedly changed")
    data["results"] += [
        "host data comparisons, hard-link identity and allocation counters: PASS",
        "FAT mirrors, known FSInfo count and backup preservation: PASS",
        "e2fsck -fn and fsck.fat -n: PASS",
    ]
    data["status"] = "PASS"
    save(out, data)
    print("JOINT EXT2/FAT32 ACCEPTANCE PASSED", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "run", "check"))
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    globals()[args.phase](args.out.resolve())
