#!/usr/bin/env python3
"""Run reviewed B matrices plus strict three-filesystem final acceptance.

Build the candidate first, including testrun/threefstest/vfstest1/vfstest2.
Every run needs a NEW output directory. No caller-owned image is modified.
Only the QEMU child created for a case is terminated. No package installation.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import shutil
import struct
import subprocess
import sys
import time


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for data in iter(lambda: handle.read(1024 * 1024), b""):
            result.update(data)
    return result.hexdigest()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def native_names(source):
    code = (source / "user/usertests.c").read_text()
    names = []
    for group in ("quicktests", "slowtests"):
        block = re.search(r"\b" + group + r"\[\] = \{(.*?)\n\};", code, re.S)
        require(block is not None, "usertests array missing: " + group)
        names += re.findall(r'\{\s*\w+,\s*"([^"]+)"\s*\}', block.group(1))
    require(len(names) == 69 and len(set(names)) == 69,
            "Expected rev5's 69 unique native tests; inspect source changes")
    return names


def check_output(text, program, expected, tests=None):
    text = text.replace("\r", "")
    require("panic:" not in text, "kernel panic")
    require(not re.search(r"\b(?:FAIL|FAILED)\b|NO TESTS EXECUTED|testrun: exec", text),
            "explicit failure in guest output")
    statuses = re.findall(r"^TESTRUN (\S+) status=(-?\d+)$", text, re.M)
    require(statuses == [(program, "0")],
            "missing, duplicate, mismatched, or nonzero guest exit status: " + repr(statuses))
    if isinstance(expected, str):
        expected = [expected]
    for line in expected:
        # B's exhaustion result appends '(N files)' to the expected prefix.
        matches = re.findall(r"^" + re.escape(line) + r"(?: \(\d+ files\))?$", text, re.M)
        require(len(matches) == 1, "expected exactly one guest result: " + line)
    if tests is not None:
        starts = list(re.finditer(r"(?:^|\n)test ([A-Za-z0-9_]+): ", text))
        require([m.group(1) for m in starts] == tests,
                "native test names/count/order differ from selected source cases")
        for i, start in enumerate(starts):
            stop = starts[i + 1].start() if i + 1 < len(starts) else len(text)
            body = text[start.end():stop]
            require(re.search(r"(?:^|[ \n])OK(?:\n|$)", body) is not None,
                    "no successful completion for native case " + start.group(1))
    return text


class Runner:
    def __init__(self, source, out, qemu, arch):
        self.source, self.out, self.qemu, self.arch = source, out, qemu, arch
        self.names = native_names(source)
        self.results = {"status": "RUNNING", "source": str(source),
                        "qemu": qemu, "arch": arch,
                        "cpu_count": 1 if arch == "loongarch" else 3,
                        "memory": "1G" if arch == "loongarch" else "128M",
                        "cases": [], "unverified": [
                            "arbitrary power-loss/crash recovery is not claimed"]}
        if arch == "loongarch":
            self.results["unverified"].append("LoongArch SMP is not tested; CPU count is 1")
        self.save()

    def save(self):
        target = self.out / "results.json"
        temp = target.with_suffix(".json.tmp")
        temp.write_text(json.dumps(self.results, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
        temp.replace(target)

    def record(self, case, **data):
        self.results["cases"].append({"case": case, **data})
        self.save()
        print(case + ": " + data.get("status", "RECORDED"), flush=True)

    def guest(self, directory, name, commands, images, readonly=(), boot=None):
        """images maps stable VirtIO slots 0/1/2 to disposable image paths."""
        directory = Path(directory)
        log_path = directory / (name + ".log")
        if self.arch == "loongarch":
            args = [self.qemu, "-machine", "virt", "-kernel", str(directory / "kernel"),
                    "-m", "1G", "-smp", "1", "-nographic", "-monitor", "none", "-nic", "none"]
        else:
            args = [self.qemu, "-machine", "virt", "-bios", "none", "-kernel",
                    str(directory / "kernel"), "-m", "128M", "-smp", "3",
                    "-display", "none", "-serial", "stdio", "-monitor", "none",
                    "-global", "virtio-mmio.force-legacy=false"]
        for slot, image in sorted(images.items()):
            drive = f"file={image},if=none,format=raw,id=d{slot},cache=writeback"
            if slot in readonly:
                drive += ",readonly=on"
            device = (f"virtio-blk-pci,drive=d{slot},disable-legacy=on,addr={slot + 1}"
                      if self.arch == "loongarch" else
                      f"virtio-blk-device,drive=d{slot},bus=virtio-mmio-bus.{slot}")
            args += ["-drive", drive, "-device", device]
        require(not log_path.exists(), "refusing to overwrite log: " + str(log_path))
        (directory / (name + ".command.json")).write_text(
            json.dumps(args, indent=2) + "\n", encoding="utf-8")
        process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        pending = bytearray()
        with log_path.open("xb") as log:
            def until_prompt(timeout):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    match = re.search(rb"(?:^|[\r\n])\$ ", pending)
                    if match is not None:
                        output = bytes(pending[:match.end()]).decode("utf-8", errors="replace")
                        del pending[:match.end()]
                        require("panic:" not in output, "kernel panic: " + str(log_path))
                        return output.replace("\r", "")
                    require(b"panic:" not in pending, "kernel panic: " + str(log_path))
                    for key, _ in selector.select(0.2):
                        chunk = os.read(key.fileobj.fileno(), 65536)
                        require(bool(chunk), "QEMU exited before prompt: " + str(log_path))
                        pending.extend(chunk)
                        log.write(chunk)
                        log.flush()
                raise TimeoutError("guest prompt timed out: " + str(log_path))

            try:
                startup = until_prompt(90)
                wanted = boot if boot is not None else ["mount xv6 dev=1 /: OK"]
                for line in wanted:
                    require(startup.splitlines().count(line) == 1,
                            "missing/duplicate mount result: " + line)
                for cmd, expected in commands:
                    tokens = shlex.split(cmd)
                    # One legacy B case intentionally fails before exec because
                    # sh cannot open its readonly redirection target. Its matrix
                    # additionally checks unchanged disk bytes and readback.
                    shell_rejection = cmd == "echo forbidden > /mnt/b-copy.bin"
                    program = "/" + tokens[0].lstrip("/")
                    actual = cmd if shell_rejection else "/testrun " + program + " " + " ".join(tokens[1:])
                    process.stdin.write(actual.rstrip().encode() + b"\n")
                    process.stdin.flush()
                    output = until_prompt(1500 if tokens[0] == "usertests" else 600)
                    if shell_rejection:
                        require(output.splitlines().count(expected) == 1,
                                "readonly shell rejection missing")
                        require("TESTRUN" not in output and "panic:" not in output,
                                "unexpected readonly command path")
                    else:
                        tests = None
                        if tokens[0].lstrip("/") == "usertests":
                            tests = self.names if len(tokens) == 1 else [tokens[1]]
                        check_output(output, program, expected, tests)
                    self.record(name + ": " + cmd, status="PASS", log=str(log_path),
                                guest_exit=0 if not shell_rejection else "shell open rejected")
            except Exception as error:
                self.record(name, status="FAIL", reason=str(error), log=str(log_path))
                raise
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

    def ext_guest(self, out, name, commands, readonly=False, image="ext2.img"):
        self.guest(out, name, commands, {0: out / "fs.img", 1: out / image},
                   readonly=(1,) if readonly else (), boot=[
                       "mount xv6 dev=1 /: OK", "mount ext2 dev=2 /mnt: OK"])

    def fat_guest(self, out, commands, fat_image="fat32.img", log_name="qemu.log"):
        root = out / ("root-" + Path(log_name).stem + ".img")
        require(not root.exists(), "refusing to overwrite root test image")
        shutil.copyfile(out / "fs.img", root)
        images = {0: root}
        if fat_image is not None:
            images[2] = out / fat_image
        boot = ["mount xv6 dev=1 /: OK"]
        if fat_image == "fat32.img":
            boot.append("mount fat32 dev=3 /fat: OK")
        self.guest(out, Path(log_name).stem, commands, images, boot=boot)


def threefs_lines(persistent=False):
    lines = []
    for fs in ("native", "ext2", "fat32"):
        for operation in ("source", "zero", "truncate", "overwrite"):
            lines.append(f"threefstest: verify {fs} {operation} OK")
    for source, target in (("native", "ext2"), ("ext2", "native"),
                           ("native", "fat32"), ("fat32", "native"),
                           ("ext2", "fat32"), ("fat32", "ext2")):
        lines.append(f"threefstest: copy {source} to {target} OK")
    lines.append("threefstest: " + ("PERSISTENCE PASSED" if persistent else "ALL TESTS PASSED"))
    if not persistent:
        lines.append("threefstest: SYNC OK")
    return lines


def host_command(args, log):
    result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=120)
    with log.open("ab") as target:
        target.write((json.dumps([str(a) for a in args]) + "\n").encode())
        target.write(result.stdout)
    require(result.returncode == 0, "host command failed; see " + str(log))
    return result.stdout


def data_pattern(size, seed):
    return bytes((i * 37 + (i >> 8) + seed * 29) % 251 for i in range(size))


def native_free_blocks(image):
    # rev5 on-disk superblock/bitmap, independent of the guest's allocator.
    block_size = 1024
    with Path(image).open("rb") as handle:
        handle.seek(block_size)
        fields = struct.unpack("<8I", handle.read(32))
        magic, size, _, _, _, _, _, bitmap_start = fields
        require(magic == 0x10203040, "unexpected native filesystem superblock")
        require(size > 0 and Path(image).stat().st_size == size * block_size,
                "native image size mismatch")
        bitmap_bytes = (size + 7) // 8
        require(bitmap_start * block_size + bitmap_bytes <= size * block_size,
                "native allocation bitmap out of range")
        handle.seek(bitmap_start * block_size)
        bitmap = handle.read(bitmap_bytes)
    require(len(bitmap) == bitmap_bytes, "short native bitmap read")
    allocated = sum((bitmap[i // 8] >> (i % 8)) & 1 for i in range(size))
    return size - allocated


def check_threefs_host(source, out):
    log = out / "host-check.log"
    host_command(["e2fsck", "-fn", out / "ext2.img"], log)
    host_command(["fsck.fat", "-n", out / "fat32.img"], log)
    for fs in (1, 2):
        expected = {"tsrc.bin": data_pattern(30001, fs + 1), "tzero.bin": b"",
                    "tshort.bin": data_pattern(17, 40 + fs),
                    "tover.bin": data_pattern(17, 90 + fs) + data_pattern(30001, 70 + fs)[17:]}
        for origin, target in ((0, 1), (1, 0), (0, 2), (2, 0), (1, 2), (2, 1)):
            if target == fs:
                expected[f"c{origin}{target}.bin"] = data_pattern(30001, origin + 1)
        for name, data in expected.items():
            if fs == 1:
                target = out / ("export-" + name)
                require(not target.exists(), "host export unexpectedly exists")
                host_command(["debugfs", "-R", f"dump /{name} {target}", out / "ext2.img"], log)
                actual = target.read_bytes()
            else:
                actual = host_command([source.parent / "fat32-lab/build/fat32_reader",
                                       out / "fat32.img", "cat", "/" + name.upper()], log)
            require(actual == data, "independent host byte mismatch: " + name)


def execute(options):
    require(__debug__, "Do not run with -O: reviewed teammate matrices use assertions")
    source, out = options.source.resolve(), options.out.resolve()
    require(not out.exists(), "output directory must be new")
    portable = source.parent / "tools/bin"
    if portable.is_dir():
        os.environ["PATH"] = str(portable) + os.pathsep + os.environ.get("PATH", "")
    if options.qemu is None:
        options.qemu = (str(source.parent / "tools/bin/qemu-system-loongarch64") if (source.parent / "tools/bin/qemu-system-loongarch64").is_file() else "qemu-system-loongarch64") if options.arch == "loongarch" else "qemu-system-riscv64"
    for tool in (options.qemu, "mke2fs", "debugfs", "e2fsck", "mkfs.fat", "fsck.fat", "make"):
        require(shutil.which(tool) is not None, "missing tool (not installed automatically): " + tool)
    os.environ["XV6_QEMU"] = options.qemu
    artifacts = source / "build/loongarch" if options.arch == "loongarch" else source
    kernel = artifacts / ("kernel.elf" if options.arch == "loongarch" else "kernel/kernel")
    root_image = artifacts / "fs.img"
    for path in (kernel, root_image, *(artifacts / "user" / ("_" + name) for name in (
            "testrun", "threefstest", "vfstest1", "vfstest2", "ext2test", "ext2write",
            "ext2single", "ext2dirgrow", "fat32test", "fat32persist", "fat32missing",
            "archtest", "sync"))):
        require(path.is_file(), "build required: " + str(path))
    out.mkdir(parents=True)
    runner = Runner(source, out, options.qemu, options.arch)
    try:
        runner.results["initial_kernel_sha256"] = digest(kernel)
        runner.results["initial_root_image_sha256"] = digest(root_image)
        runner.results["runner_sha256"] = digest(Path(__file__))
        shutil.copyfile(Path(__file__), out / "runner-used.py")
        runner.results["source_file_sha256"] = {
            str(path.relative_to(source)): digest(path)
            for folder in ("kernel", "user", "tests", "mkfs") for path in sorted((source / folder).rglob("*"))
            if path.is_file() and path.suffix in (".c", ".h", ".S", ".py", ".pl", ".ld")}
        runner.results["source_file_sha256"]["Makefile"] = digest(source / "Makefile")
        if (source / "Makefile.loongarch").is_file():
            runner.results["source_file_sha256"]["Makefile.loongarch"] = digest(source / "Makefile.loongarch")
        runner.results["qemu_version"] = subprocess.check_output([options.qemu, "--version"], text=True).splitlines()[0]
        provenance = source.parent / "snapshot-provenance.json"
        if provenance.is_file():
            runner.results["source_snapshot"] = json.loads(provenance.read_text())
            runner.results["source_commit"] = runner.results["source_snapshot"]["commit"]
        else:
            runner.results["source_commit"] = subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        runner.save()
        ext = load_module("reviewed_ext2", source / "tests/run_ext2_tests.py")
        fat = load_module("reviewed_fat32", source / "tests/run_fat32_vfs_tests.py")
        # Existing fixture builders use RISC-V-shaped build paths only to copy
        # already-built kernel/root files. Supply the chosen architecture's
        # artifacts under those names; keep FATLAB on the real source tree.
        inputs = out / "inputs"
        (inputs / "kernel").mkdir(parents=True)
        shutil.copyfile(kernel, inputs / "kernel/kernel")
        shutil.copyfile(root_image, inputs / "fs.img")
        ext.SOURCE, fat.XV6 = inputs, inputs
        ext.guest, fat.guest = runner.ext_guest, runner.fat_guest
        ext_out, fat_out = out / "ext2", out / "fat32"
        ext.prepare(ext_out)
        fat.prepare(fat_out)
        ext_manifest = json.loads((ext_out / "manifest.json").read_text())
        ext_manifest["architecture"] = options.arch
        ext_manifest["cpu_count"] = runner.results["cpu_count"]
        ext_manifest["memory"] = runner.results["memory"]
        ext_manifest["transport"] = "virtio-pci" if options.arch == "loongarch" else "virtio-mmio"
        (ext_out / "manifest.json").write_text(json.dumps(ext_manifest, indent=2) + "\n")
        # Save untouched fixtures for the simultaneous three-disk matrix.
        triple = out / "threefs"
        triple.mkdir()
        for src, name in ((kernel, "kernel"), (root_image, "fs.img"),
                          (ext_out / "ext2.img", "ext2.img"), (fat_out / "fat32.img", "fat32.img")):
            shutil.copyfile(src, triple / name)
        ext.run(ext_out)
        ext.check(ext_out)
        runner.record("ext2 host consistency/bytes/allocation", status="PASS")
        fat.run(fat_out)
        fat.check(fat_out)
        runner.record("fat32 host consistency/bytes", status="PASS")
        images = {i: triple / name for i, name in enumerate(("fs.img", "ext2.img", "fat32.img"))}
        boot = ["mount xv6 dev=1 /: OK", "mount ext2 dev=2 /mnt: OK", "mount fat32 dev=3 /fat: OK"]
        runner.guest(triple, "write", [("ext2test", "ext2test: ALL TESTS PASSED"),
                     ("fat32test", "fat32test: ALL TESTS PASSED"),
                     ("threefstest", threefs_lines())], images, boot=boot)
        written = {name: digest(triple / name) for name in ("fs.img", "ext2.img", "fat32.img")}
        runner.guest(triple, "reboot", [("threefstest verify", threefs_lines(True)),
                     ("fat32persist", "fat32persist: PASS")], images, boot=boot)
        reread = {name: digest(triple / name) for name in written}
        require(reread == written, "read-only persistence verification changed an image")
        runner.results["threefs_persistence_hashes"] = written
        check_threefs_host(source, triple)
        runner.record("threefs fsck/independent host byte comparison", status="PASS")
        official = ["threefstest: official " + p + " OK"
                    for p in ("/mnt/hello.c", "/hello.c", "/tests/hello.c")]
        official.append("threefstest: OFFICIAL VERIFY PASSED")
        test1 = [f"test_copy_file({a},{b}): succeed" for a, b in (
            ("/mnt/hello.c", "/hello.c"), ("/mnt/hello.c", "/tests/hello.c"),
            ("/hello.c", "/mnt/hello.c"), ("/tests/hello.c", "/mnt/hello.c"))]
        official_commands = [
            ("threefstest official-prepare", "threefstest: OFFICIAL FIXTURES READY"),
            ("vfstest1", test1), ("threefstest official-verify", official),
            ("vfstest2", "test_cross_fs_copy(): succeed"),
            ("threefstest official-verify", official),
            ("vfsdir", "vfsdir: ALL TESTS PASSED"),
            ("vfspath", "vfspath: ALL TESTS PASSED"),
            ("vfsexec", "vfsexec: ALL TESTS PASSED")]
        official_commands.append(("archtest", "archtest: read-only text, NX heap, kernel isolation PASS"))
        runner.guest(triple, "official-and-native", official_commands, images, boot=boot)
        # The original native suite needs room for MAXFILE. Keep persistence
        # samples intact and give the native suite its own fresh root image.
        native_root = triple / "native-regression.img"
        shutil.copyfile(root_image, native_root)
        runner.guest(triple, "native-full69", [("usertests", "ALL TESTS PASSED")],
                     {**images, 0: native_root}, boot=boot)
        free_before = native_free_blocks(native_root)
        runner.guest(triple, "native-badwrite-accounting", [
                     ("usertests badwrite", "ALL TESTS PASSED"), ("sync", [])],
                     {**images, 0: native_root}, boot=boot)
        free_after = native_free_blocks(native_root)
        require(free_before == free_after, "invalid-pointer writes leaked native disk blocks")
        runner.record("native badwrite exact free-block accounting", status="PASS",
                      free_blocks_before=free_before, free_blocks_after=free_after)
        host_command(["e2fsck", "-fn", triple / "ext2.img"], triple / "final-fsck.log")
        host_command(["fsck.fat", "-n", triple / "fat32.img"], triple / "final-fsck.log")
        runner.results["status"] = "PASS"
    except KeyboardInterrupt:
        runner.results["status"] = "INTERRUPTED"
        raise
    except Exception as error:
        runner.results["status"] = "FAIL"
        runner.results["error"] = str(error)
        raise
    finally:
        runner.results["log_sha256"] = {str(p.relative_to(out)): digest(p) for p in sorted(out.rglob("*.log"))}
        runner.save()


def self_test():
    good = "test alpha: kill... wait... OK\ntest beta: usertrap(): deliberate fault\nOK\nALL TESTS PASSED\nTESTRUN /usertests status=0\n"
    check_output(good, "/usertests", "ALL TESTS PASSED", ["alpha", "beta"])
    for bad in (good.replace("status=0", "status=1"), good.replace("test beta:", "test gamma:"),
                good.replace("TESTRUN /usertests status=0\n", ""),
                good + "TESTRUN /usertests status=0\n", good.replace("OK", "FAILED", 1),
                good.replace("test beta:", "panic: test beta:")):
        try:
            check_output(bad, "/usertests", "ALL TESTS PASSED", ["alpha", "beta"])
        except RuntimeError:
            pass
        else:
            raise AssertionError("negative parser self-test accepted invalid evidence")
    print("parser self-test: valid result accepted; six false-positive cases rejected")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--arch", choices=("riscv", "loongarch"), default="riscv")
    parser.add_argument("--qemu")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        require(args.source is not None and args.out is not None, "--source and --out are required")
        execute(args)
