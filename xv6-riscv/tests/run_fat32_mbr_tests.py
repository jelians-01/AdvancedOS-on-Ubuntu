"""One FAT32 MBR feature round: parser guards, write boot, readback boot, host check.

Build kernel/kernel and fs.img first. Run prepare/run/check with the same NEW
output directory. Only disposable images in that directory are modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import run_fat32_vfs_tests as base
from run_fs_joint_tests import layout, backup_hash

START = 1024 * 1024
TAIL = 1024 * 1024


def save(out, data):
    (out / "mbr.json").write_text(json.dumps(data, indent=2) + "\n")


def load(out):
    return json.loads((out / "mbr.json").read_text())


def sha(data):
    return hashlib.sha256(data).hexdigest()


def prepare(out):
    base.prepare_guest(out)
    base.vwrite_volume(out)
    volume = out / "fat32-vwrite.img"
    with volume.open("r+b") as stream:
        boot = stream.read(512)
        backup = struct.unpack_from("<H", boot, 50)[0]
        for offset in (28, backup * 512 + 28):
            stream.seek(offset)
            stream.write(struct.pack("<I", START // 512))
    payload = volume.read_bytes()
    prefix = bytearray(b"\xa5" * START)
    prefix[:512] = bytes(512)

    def partition(slot, kind, start, sectors):
        offset = 446 + slot * 16
        prefix[offset + 4] = kind
        struct.pack_into("<II", prefix, offset + 8, start, sectors)

    # FAT32 is deliberately not the first partition. Other partitions contain
    # sentinels whose complete hashes must survive allocation and reclamation.
    partition(0, 0x83, 128, 512)
    partition(2, 0x0c, START // 512, len(payload) // 512)
    partition(3, 0x83, (START + len(payload)) // 512 + 512, 512)
    prefix[510:512] = b"\x55\xaa"
    suffix = b"\x5a" * TAIL
    image = out / "fat32-mbr.img"
    image.write_bytes(prefix + payload + suffix)
    base.command(["gcc", "-O1", "-g", "-Wall", "-Werror",
                  "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                  "-fsanitize=address,undefined", "-Wl,--gc-sections",
                  str(base.XV6 / "tests/fat32_mbr_test.c"), "-o", str(out / "mbr-parser")],
                 log=out / "parser.log")
    parser_output = base.command([str(out / "mbr-parser"), str(image)],
                                 log=out / "parser.log")
    assert "mount cases PASS" in parser_output
    save(out, {
        "baseline_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=base.XV6, text=True).strip(),
        "source_sha256": {name: base.digest(base.XV6 / name) for name in
                          ("kernel/fat32.c", "user/fat32mbr.c",
                           "tests/fat32_mbr_test.c", "tests/run_fat32_mbr_tests.py")},
        "kernel_sha256": base.digest(out / "kernel"),
        "offset": START, "length": len(payload),
        "prefix_sha256": sha(prefix), "suffix_sha256": sha(suffix),
        "backup_fsinfo_sha256": backup_hash(volume),
        "parser": parser_output.strip(), "guest_pass": False,
    })
    print(parser_output.strip(), flush=True)


def run(out):
    data = load(out)
    base.guest(out, [("fat32vwrite", "fat32vwrite: PASS"),
                     ("fat32shared", "fat32shared: PASS")],
               fat_image="fat32-mbr.img", log_name="qemu-mbr-write.log")
    image = out / "fat32-mbr.img"
    expected = base.digest(image)
    base.guest(out, [("fat32mbr", "fat32mbr: REBOOT PASS")],
               fat_image="fat32-mbr.img", log_name="qemu-mbr-reboot.log")
    assert base.digest(image) == expected, "readback boot modified the disk"
    data["guest_pass"] = True
    data["readback_sha256"] = expected
    save(out, data)


def check(out):
    data = load(out)
    assert data["guest_pass"], "complete the run phase first"
    disk = (out / "fat32-mbr.img").read_bytes()
    assert sha(disk[:START]) == data["prefix_sha256"], "MBR/prefix partition changed"
    assert sha(disk[START + data["length"]:]) == data["suffix_sha256"], "suffix partition changed"
    image = out / "partition-after.img"
    image.write_bytes(disk[START:START + data["length"]])
    base.command(["fsck.fat", "-n", str(image)], log=out / "check.log")
    volume = image.read_bytes()
    bps, fat, fat_bytes, count, info, _ = layout(image)
    assert volume[fat:fat + fat_bytes] == volume[fat + fat_bytes:fat + 2 * fat_bytes]
    actual_free = sum((struct.unpack_from("<I", volume, fat + i * 4)[0] & 0xfffffff) == 0
                      for i in range(2, count + 2))
    free, hint = struct.unpack_from("<II", volume, info * bps + 488)
    assert free == actual_free and 2 <= hint <= count + 1
    assert backup_hash(image) == data["backup_fsinfo_sha256"]

    # Independent FAT/root reader uses volume-relative offsets after extraction.
    boot = volume[:512]
    cluster_bytes = bps * boot[13]
    data_offset = base.large_data_offset(boot)

    def chain(first):
        seen = set()
        while first < 0xffffff8:
            assert 2 <= first <= count + 1 and first not in seen
            seen.add(first)
            pos = data_offset + (first - 2) * cluster_bytes
            yield volume[pos:pos + cluster_bytes]
            first = struct.unpack_from("<I", volume, fat + first * 4)[0] & 0xfffffff

    expected = {b"REPORT~1TXT": b"kept", b"EXACTL~1TXT": b"exact", b"SHARED~1TXT": b"new"}
    root = b"".join(chain(struct.unpack_from("<I", boot, 44)[0]))
    for pos in range(0, len(root), 32):
        entry = root[pos:pos + 32]
        if entry[0] == 0:
            break
        if entry[0] == 0xe5 or entry[11] == 0x0f:
            continue
        if entry[:11] in expected:
            first = (struct.unpack_from("<H", entry, 20)[0] << 16) | struct.unpack_from("<H", entry, 26)[0]
            size = struct.unpack_from("<I", entry, 28)[0]
            assert b"".join(chain(first))[:size] == expected.pop(entry[:11])
    assert not expected, "missing persisted files"
    data["status"] = "PASS"
    data["host_checks"] = "outside partitions unchanged; contents, FAT mirrors, FSInfo, backup, fsck.fat PASS"
    save(out, data)
    print("FAT32 MBR FEATURE ROUND PASSED", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "run", "check"))
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    globals()[args.phase](args.out.resolve())
