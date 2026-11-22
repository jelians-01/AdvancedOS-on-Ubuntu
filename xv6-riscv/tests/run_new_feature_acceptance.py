#!/usr/bin/env python3
"""Run the reviewed B joint/MBR matrices on either compiled architecture.

Requires a NEW output directory. Reuses the existing strict guest runner and
the candidate's fixture builders/host checks; never edits their source files.
"""
import argparse
import importlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def execute(options):
    require(__debug__, "Do not use python -O; teammate checks use assertions")
    source, out = options.source.resolve(), options.out.resolve()
    require(not out.exists(), "output directory must be new: " + str(out))
    require(not any(c.isspace() or c == ',' for c in str(out)),
            "output path cannot contain whitespace or commas")
    portable = source.parent / "tools/bin"
    if portable.is_dir():
        os.environ["PATH"] = str(portable) + os.pathsep + os.environ.get("PATH", "")
    qemu = options.qemu or (
        (str(source.parent / "tools/bin/qemu-system-loongarch64") if (source.parent / "tools/bin/qemu-system-loongarch64").is_file() else "qemu-system-loongarch64")
        if options.arch == "loongarch" else "qemu-system-riscv64")
    for tool in (qemu, "gcc", "make", "mke2fs", "debugfs", "e2fsck", "mkfs.fat", "fsck.fat"):
        require(shutil.which(tool) is not None, "missing tool: " + tool)
    artifacts = source / "build/loongarch" if options.arch == "loongarch" else source
    kernel = artifacts / ("kernel.elf" if options.arch == "loongarch" else "kernel/kernel")
    root_image = artifacts / "fs.img"
    programs = ("testrun", "sync", "vfsdir", "ext2mkdir", "ext2unlink", "ext2link",
                "fat32vwrite", "fat32shared", "fat32mbr", "fsjoint")
    for path in (kernel, root_image, *(artifacts / "user" / ("_" + p) for p in programs)):
        require(path.is_file(), "build required: " + str(path))
    with kernel.open("rb") as stream:
        header = stream.read(20)
    machine = 258 if options.arch == "loongarch" else 243
    require(header[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", header, 18)[0] == machine,
            "kernel ELF architecture does not match --arch")

    # Importing reviewed helpers does not invoke their __main__ entry points.
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("strict_acceptance", options.runner.resolve())
    strict = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(strict)
    sys.path.insert(0, str(source / "tests"))
    ext2 = importlib.import_module("run_ext2_tests")
    fat32 = importlib.import_module("run_fat32_vfs_tests")
    joint = importlib.import_module("run_fs_joint_tests")
    mbr = importlib.import_module("run_fat32_mbr_tests")
    require(ext2.SOURCE == source and fat32.XV6 == source, "wrong test modules imported")

    # Keep SOURCE/XV6 on the actual candidate for source hashes, host-test
    # includes, and git provenance. Redirect only the two build-artifact reads.
    redirects = {(source / "kernel/kernel").resolve(): kernel,
                 (source / "fs.img").resolve(): root_image}

    class ArtifactCopies:
        def __getattr__(self, name):
            return getattr(shutil, name)

        def copyfile(self, src, dst, *args, **kwargs):
            require(Path(dst).resolve().is_relative_to(out), "copy target outside test output")
            return shutil.copyfile(redirects.get(Path(src).resolve(), src), dst, *args, **kwargs)

    ext2.shutil = fat32.shutil = ArtifactCopies()
    out.mkdir(parents=True, exist_ok=False)
    runner = strict.Runner(source, out, qemu, options.arch)
    try:
        runner.results.update({
            "scope": "B joint operations and FAT32 MBR feature integration",
            "source_commit": subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
            "initial_kernel_sha256": strict.digest(kernel),
            "initial_root_image_sha256": strict.digest(root_image),
            "qemu_version": subprocess.check_output([qemu, "--version"], text=True).splitlines()[0],
            "runner_sha256": strict.digest(Path(__file__)),
            "strict_runner_sha256": strict.digest(options.runner),
            "source_file_sha256": {
                str(p.relative_to(source)): strict.digest(p)
                for folder in ("kernel", "user", "tests", "mkfs")
                for p in sorted((source / folder).rglob("*"))
                if p.is_file() and p.suffix in (".c", ".h", ".S", ".py", ".pl", ".ld")},
        })
        for name in ("Makefile", "Makefile.loongarch"):
            runner.results["source_file_sha256"][name] = strict.digest(source / name)
        for name in ("runner-used.py", "strict-runner-used.py"):
            shutil.copyfile(Path(__file__) if name == "runner-used.py" else options.runner, out / name)
        runner.save()

        def ext_guest(directory, name, commands, readonly=False, image="ext2.img", fat_image=None):
            images = {0: directory / "fs.img", 1: directory / image}
            boot = ["mount xv6 dev=1 /: OK", "mount ext2 dev=2 /mnt: OK"]
            if fat_image is not None:
                images[2] = directory / fat_image
                boot.append("mount fat32 dev=3 /fat: OK")
            runner.guest(directory, name, commands, images,
                         readonly=(1,) if readonly else (), boot=boot)

        def fat_guest(directory, commands, *, fat_image="fat32.img", log_name="qemu.log"):
            root = directory / ("root-" + Path(log_name).stem + ".img")
            require(not root.exists(), "root copy already exists")
            shutil.copyfile(directory / "fs.img", root)
            images = {0: root}
            boot = ["mount xv6 dev=1 /: OK"]
            if fat_image is not None:
                images[2] = directory / fat_image
                boot.append("mount fat32 dev=3 /fat: OK")
            runner.guest(directory, Path(log_name).stem, commands, images, boot=boot)

        ext2.guest, fat32.guest = ext_guest, fat_guest
        suites = (("joint", joint, "joint.json"), ("mbr", mbr, "mbr.json"))
        for name, module, manifest_name in suites:
            if options.suite not in ("all", name):
                continue
            directory = out / name
            module.prepare(directory)
            manifest_path = directory / manifest_name
            data = json.loads(manifest_path.read_text())
            data.update({"arch": options.arch, "cpu_count": runner.results["cpu_count"],
                         "memory": runner.results["memory"],
                         "transport": "virtio-pci" if options.arch == "loongarch" else "virtio-mmio",
                         "scope": name + " feature acceptance on " + options.arch,
                         "qemu_version": runner.results["qemu_version"]})
            manifest_path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            module.run(directory)
            module.check(directory)
            require(json.loads(manifest_path.read_text()).get("status") == "PASS",
                    "host validation did not mark PASS: " + name)
            runner.record(name + " host bytes/fsck/allocation", status="PASS", manifest=str(manifest_path))
        runner.results["status"] = "PASS"
        print(options.arch + " NEW FEATURE ACCEPTANCE PASSED", flush=True)
    except KeyboardInterrupt:
        runner.results["status"] = "INTERRUPTED"
        raise
    except Exception as error:
        runner.results.update(status="FAIL", error=str(error))
        raise
    finally:
        runner.results["log_sha256"] = {str(p.relative_to(out)): strict.digest(p)
                                        for p in sorted(out.rglob("*.log"))}
        runner.save()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="candidate xv6-riscv directory")
    parser.add_argument("--out", type=Path, required=True, help="new disposable evidence directory")
    parser.add_argument("--arch", choices=("riscv", "loongarch"), required=True)
    parser.add_argument("--qemu")
    parser.add_argument("--suite", choices=("all", "joint", "mbr"), default="all")
    parser.add_argument("--runner", type=Path,
                        default=Path(__file__).with_name("run_acceptance.py"))
    execute(parser.parse_args())
