#!/usr/bin/env python3
"""Build disposable mkfs.fat images and exercise the read-only host parser."""

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(args, *, check=True, binary=False):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout={result.stdout[:2000]!r}\nstderr={result.stderr[:2000]!r}"
        )
    if binary:
        return result
    return result.stdout.decode("utf-8", errors="replace"), result.stderr.decode(
        "utf-8", errors="replace"
    ), result.returncode


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
    return h.digest()


def parse_layout(image):
    with image.open("rb") as source:
        boot = source.read(512)
    bps = struct.unpack_from("<H", boot, 11)[0]
    spc = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    fat_sectors = struct.unpack_from("<I", boot, 36)[0]
    total_sectors = struct.unpack_from("<I", boot, 32)[0]
    root = struct.unpack_from("<I", boot, 44)[0] & 0x0FFFFFFF
    return {
        "bps": bps,
        "spc": spc,
        "cluster_bytes": bps * spc,
        "reserved": reserved,
        "fats": fats,
        "fat_sectors": fat_sectors,
        "fat_offset": reserved * bps,
        "data_offset": (reserved + fats * fat_sectors) * bps,
        "total_sectors": total_sectors,
        "root": root,
    }


def cluster_offset(layout, cluster):
    return layout["data_offset"] + (cluster - 2) * layout["cluster_bytes"]


def set_fat_entry(image, layout, cluster, value):
    with image.open("r+b") as target:
        for fat_no in range(layout["fats"]):
            offset = (layout["reserved"] + fat_no * layout["fat_sectors"]) * layout["bps"] + cluster * 4
            target.seek(offset)
            target.write(struct.pack("<I", value))


def encode_short_name(name):
    if name in (".", ".."):
        return name.encode("ascii").ljust(11, b" ")
    base, dot, extension = name.partition(".")
    assert len(base) <= 8 and len(extension) <= 3 and (not dot or extension)
    return base.upper().encode("ascii").ljust(8, b" ") + extension.upper().encode("ascii").ljust(3, b" ")


def directory_entry(name, attributes, cluster, size):
    entry = bytearray(32)
    entry[:11] = encode_short_name(name)
    entry[11] = attributes
    struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size)
    return entry


def write_cluster(image, layout, cluster, data):
    assert len(data) <= layout["cluster_bytes"]
    with image.open("r+b") as target:
        target.seek(cluster_offset(layout, cluster))
        target.write(data.ljust(layout["cluster_bytes"], b"\0"))


def format_fixture(image, mkfs):
    with image.open("wb") as target:
        target.truncate(64 * 1024 * 1024)
    run([mkfs, "--invariant", "-F", "32", "-S", "512", "-s", "1", "-R", "32", str(image)])
    layout = parse_layout(image)
    assert layout["bps"] == 512 and layout["spc"] == 1
    assert layout["root"] == 2

    hello = b"hello from FAT32\n"
    fragmented = bytes((i * 37 + 11) % 256 for i in range(1300))
    nested = b"nested read works\n"
    set_fat_entry(image, layout, 5, 0x0FFFFFFF)
    for cluster, next_cluster in ((10, 30), (30, 17), (17, 0x0FFFFFFF)):
        set_fat_entry(image, layout, cluster, next_cluster)
    set_fat_entry(image, layout, 40, 41)
    set_fat_entry(image, layout, 41, 0x0FFFFFFF)
    set_fat_entry(image, layout, 45, 46)
    set_fat_entry(image, layout, 46, 0x0FFFFFFF)
    set_fat_entry(image, layout, 50, 51)
    set_fat_entry(image, layout, 51, 0x0FFFFFFF)
    set_fat_entry(image, layout, 80, 0x0FFFFFFF)
    set_fat_entry(image, layout, 81, 0x0FFFFFFF)
    set_fat_entry(image, layout, 2, 90)
    set_fat_entry(image, layout, 90, 0x0FFFFFFF)
    write_cluster(image, layout, 5, hello)
    for index, cluster in enumerate((10, 30, 17)):
        start = index * layout["cluster_bytes"]
        write_cluster(image, layout, cluster, fragmented[start : start + layout["cluster_bytes"]])
    write_cluster(image, layout, 40, b"L" * layout["cluster_bytes"])
    write_cluster(image, layout, 41, b"M" * layout["cluster_bytes"])
    write_cluster(image, layout, 45, b"S" * layout["cluster_bytes"])
    write_cluster(image, layout, 46, b"T" * layout["cluster_bytes"])
    write_cluster(image, layout, 50, b"B" * layout["cluster_bytes"])
    write_cluster(image, layout, 51, b"C" * layout["cluster_bytes"])
    write_cluster(image, layout, 81, nested)

    root_entries = [
        directory_entry("HELLO.TXT", 0x20, 5, len(hello)),
        directory_entry("FRAG.BIN", 0x20, 10, len(fragmented)),
        directory_entry("EMPTY.TXT", 0x20, 0, 0),
        directory_entry("LOOP.BIN", 0x20, 40, 1024),
        directory_entry("SHORT.BIN", 0x20, 45, 700),
        directory_entry("BAD.BIN", 0x20, 50, 700),
        directory_entry("SUBDIR", 0x10, 80, 0),
    ]
    deleted = bytearray(32)
    deleted[0] = 0xE5
    root_entries += [deleted] * (layout["cluster_bytes"] // 32 - len(root_entries))
    write_cluster(image, layout, layout["root"], b"".join(root_entries))
    write_cluster(image, layout, 90, directory_entry("LATE.TXT", 0x20, 0, 0))
    subdir_entries = [
        directory_entry(".", 0x10, 80, 0),
        directory_entry("..", 0x10, 0, 0),
        directory_entry("NEST.TXT", 0x20, 81, len(nested)),
        bytearray(32),
    ]
    write_cluster(image, layout, 80, b"".join(subdir_entries))
    # Unknown free-count / next-free values are explicitly valid FSInfo values.
    with image.open("r+b") as target:
        for sector in (1, 7):
            target.seek(sector * layout["bps"] + 488)
            target.write(struct.pack("<II", 0xFFFFFFFF, 0xFFFFFFFF))
    return layout, hello, fragmented, nested


def expect_failure(reader, image, command, message_part):
    stdout, stderr, status = run([reader, image, *command], check=False)
    assert status != 0, f"expected failure for {command}"
    assert message_part.lower() in stderr.lower(), (message_part, stderr)
    assert stdout == "", f"failure leaked partial output for {command}"


def main():
    if len(sys.argv) not in (3, 5) or (len(sys.argv) == 5 and sys.argv[3] != "--keep-image"):
        raise SystemExit("usage: test_fat32.py FAT32_READER MKFS.FAT [--keep-image OUTPUT]")
    reader, mkfs = sys.argv[1:3]
    keep_image = Path(sys.argv[4]) if len(sys.argv) == 5 else None
    if keep_image and keep_image.exists():
        raise SystemExit(f"refusing to overwrite existing fixture: {keep_image}")
    with tempfile.TemporaryDirectory(prefix="fat32-host-test-") as temp:
        root = Path(temp)
        image = root / "valid.img"
        layout, hello, fragmented, nested = format_fixture(image, mkfs)

        # The real formatter and consistency checker validate our fixture layout.
        fsck_out, fsck_err, fsck_status = run(["fsck.fat", "-n", str(image)], check=False)
        assert fsck_status == 0, (fsck_out, fsck_err)

        before = digest(image)
        info, _, _ = run([reader, image, "info"])
        assert "FAT type: FAT32" in info and "Root cluster: 2" in info
        listing, _, _ = run([reader, image, "ls"])
        for name in ("HELLO.TXT", "FRAG.BIN", "EMPTY.TXT", "SUBDIR", "LATE.TXT"):
            assert name in listing
        nested_listing, _, _ = run([reader, image, "ls", "subdir"])
        assert "NEST.TXT" in nested_listing
        assert run([reader, image, "ls", "SUBDIR/."])[0] == nested_listing
        assert run([reader, image, "ls", "././"])[0] == listing
        assert run([reader, image, "ls", "."])[0] == listing
        assert run([reader, image, "ls", "./."])[0] == listing
        assert run([reader, image, "ls", "SUBDIR/"])[0] == nested_listing
        expect_failure(reader, image, ["cat", "HELLO.TXT/"], "trailing slash")
        expect_failure(reader, image, ["cat", "SUBDIR"], "directory")
        expect_failure(reader, image, ["read", "FRAG.BIN", "-1", "1"], "unsigned")
        expect_failure(reader, image, ["read", "FRAG.BIN", "1301", "1"], "end-of-file")
        expect_failure(reader, image, ["cat", "NOFILE.TXT"], "not found")
        stat, _, _ = run([reader, image, "stat", "hElLo.TxT"])
        assert f"Size: {len(hello)} bytes" in stat
        cat = run([reader, image, "cat", "hello.txt"], binary=True)
        assert cat.returncode == 0 and cat.stdout == hello
        assert run([reader, image, "cat", "FRAG.BIN"], binary=True).stdout == fragmented
        nested_cat = run([reader, image, "cat", "/SUBDIR/NEST.TXT"], binary=True)
        assert nested_cat.returncode == 0 and nested_cat.stdout == nested
        read, _, _ = run([reader, image, "read", "frag.bin", "500", "40"])
        assert read.strip() == " ".join(f"{b:02x}" for b in fragmented[500:540])
        read_later, _, _ = run([reader, image, "read", "frag.bin", "600", "40"])
        assert read_later.strip() == " ".join(f"{b:02x}" for b in fragmented[600:640])
        read_eof, _, _ = run([reader, image, "read", "FRAG.BIN", "1298", "20"])
        assert read_eof.strip() == " ".join(f"{b:02x}" for b in fragmented[1298:])
        empty = run([reader, image, "cat", "empty.txt"], binary=True)
        assert empty.returncode == 0 and empty.stdout == b""
        assert digest(image) == before, "reader modified the input image"

        # ExtFlags can disable mirroring and select a non-zero active FAT.
        active_fat = root / "active-fat.img"
        shutil.copyfile(image, active_fat)
        with active_fat.open("r+b") as target:
            target.seek(40)
            target.write(struct.pack("<H", 0x0081))
        set_fat_entry(active_fat, layout, 10, 30)  # mirror 0 becomes stale
        with active_fat.open("r+b") as target:
            target.seek(layout["fat_offset"] + 10 * 4)
            target.write(struct.pack("<I", 0x0FFFFFF7))
        active_layout = dict(layout)
        active_layout["reserved"] += active_layout["fat_sectors"]
        active_layout["fats"] = 1
        set_fat_entry(active_fat, active_layout, 10, 30)
        active_before = digest(active_fat)
        assert run([reader, active_fat, "cat", "FRAG.BIN"], binary=True).stdout == fragmented
        assert digest(active_fat) == active_before, "reader modified the active-FAT image"

        mismatch = root / "fat-mismatch.img"
        shutil.copyfile(image, mismatch)
        mismatch_layout = dict(layout)
        mismatch_layout["reserved"] += mismatch_layout["fat_sectors"]
        mismatch_layout["fats"] = 1
        set_fat_entry(mismatch, mismatch_layout, layout["root"], layout["root"])
        mismatch_before = digest(mismatch)
        expect_failure(reader, mismatch, ["ls"], "mirrored FAT entries disagree")
        assert digest(mismatch) == mismatch_before, "reader modified the mismatched-FAT image"

        # Corrupt chains in copies to ensure failures are detected, not hidden.
        loop = root / "loop.img"
        shutil.copyfile(image, loop)
        with loop.open("r+b") as target:
            target.seek(layout["data_offset"] + (layout["root"] - 2) * layout["cluster_bytes"] + 3 * 32 + 28)
            target.write(struct.pack("<I", 1536))
        set_fat_entry(loop, layout, 41, 40)
        expect_failure(reader, loop, ["cat", "LOOP.BIN"], "cycle")
        expect_failure(reader, loop, ["read", "LOOP.BIN", "0", "1"], "cycle")

        dir_loop = root / "dir-loop.img"
        shutil.copyfile(image, dir_loop)
        set_fat_entry(dir_loop, layout, 90, 2)
        expect_failure(reader, dir_loop, ["ls"], "cycle")

        truncated_chain = root / "truncated-chain.img"
        shutil.copyfile(image, truncated_chain)
        set_fat_entry(truncated_chain, layout, 45, 0x0FFFFFFF)
        expect_failure(reader, truncated_chain, ["cat", "SHORT.BIN"], "ended")

        bad_chain = root / "bad-chain.img"
        shutil.copyfile(image, bad_chain)
        set_fat_entry(bad_chain, layout, 50, 0x0FFFFFF7)
        expect_failure(reader, bad_chain, ["cat", "BAD.BIN"], "bad")

        for label, offset, data, message in (
            ("signature", 510, b"\x00\x00", "signature"),
            ("sector-size", 11, struct.pack("<H", 768), "bytes-per-sector"),
            ("root-cluster", 44, struct.pack("<I", 1), "root-directory cluster"),
            ("fat-size", 36, struct.pack("<I", 1), "FAT is too small"),
            ("hidden-sectors", 28, struct.pack("<I", 1), "hidden sectors"),
            ("version", 42, struct.pack("<H", 1), "version"),
            ("active-fat", 40, struct.pack("<H", 0x008f), "active FAT index"),
            ("reserved-flags", 40, struct.pack("<H", 0x0100), "reserved ExtFlags"),
            ("fat-overflow", 36, struct.pack("<I", 0x80000000), "no data region"),
        ):
            invalid = root / f"invalid-{label}.img"
            shutil.copyfile(image, invalid)
            with invalid.open("r+b") as target:
                target.seek(offset)
                target.write(data)
            expect_failure(reader, invalid, ["info"], message)

        short_image = root / "short-image.img"
        shutil.copyfile(image, short_image)
        with short_image.open("r+b") as target:
            target.truncate(32 * 1024 * 1024)
        expect_failure(reader, short_image, ["info"], "truncated")

        # A zero-length read and a read exactly at EOF emit only the hex newline.
        assert run([reader, image, "read", "FRAG.BIN", "1300", "4"])[0] == "\n"
        assert run([reader, image, "read", "FRAG.BIN", "100", "0"])[0] == "\n"
        assert digest(image) == before
        if keep_image:
            with image.open("rb") as source, keep_image.open("xb") as target:
                shutil.copyfileobj(source, target)

        print("PASS: mkfs.fat FAT32 image parsed; fsck.fat -n clean")
        print("PASS: info, root/nested ls, case-insensitive stat/cat, fragmented read, EOF, empty file")
        print("PASS: cycle, premature EOC, bad-cluster, malformed BPB and truncated-image rejection")
        print("PASS: active FAT selection; mirrored FAT mismatch rejection; failures emit no file bytes")
        print("PASS: SHA-256 unchanged across valid, active-FAT and mismatch reader commands")
        print(f"FIXTURE: {layout['bps']} B sectors, {layout['cluster_bytes']} B clusters, "
              f"{layout['total_sectors']} sectors, fragmented FRAG.BIN clusters 10 -> 30 -> 17")
        if keep_image:
            print(f"Kept clean fixture: {keep_image}")


if __name__ == "__main__":
    main()
