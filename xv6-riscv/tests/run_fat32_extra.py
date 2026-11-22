#!/usr/bin/env python3
"""Run reviewed 8 KiB cluster and VFAT lookup cases on disposable images."""
import argparse
import contextlib
import importlib.util
import io
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


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def execute(options):
    require(__debug__, "Do not use python -O; fixture checks use assertions")
    source, out = options.source.resolve(), options.out.resolve()
    require(not out.exists(), "output directory must be new: " + str(out))
    require(not any(c.isspace() or c == ',' for c in str(out)), "unsafe QEMU output path")
    portable = source.parent / "tools/bin"
    os.environ["PATH"] = str(portable) + os.pathsep + os.environ.get("PATH", "")
    qemu = ((str(source.parent / "tools/bin/qemu-system-loongarch64") if (source.parent / "tools/bin/qemu-system-loongarch64").is_file() else "qemu-system-loongarch64")
            if options.arch == "loongarch" else "qemu-system-riscv64")
    for tool in (qemu, "mkfs.fat", "fsck.fat"):
        require(shutil.which(tool) is not None, "missing tool: " + tool)
    artifacts = source / "build/loongarch" if options.arch == "loongarch" else source
    kernel = artifacts / ("kernel.elf" if options.arch == "loongarch" else "kernel/kernel")
    root_image = artifacts / "fs.img"
    for path in (kernel, root_image, *(artifacts / "user" / ("_" + name)
                                      for name in ("testrun", "sync", "fat32large", "fat32vfat"))):
        require(path.is_file(), "build required: " + str(path))
    with kernel.open("rb") as stream:
        header = stream.read(20)
    require(header[:6] == b"\x7fELF\x02\x01" and
            struct.unpack_from("<H", header, 18)[0] == (258 if options.arch == "loongarch" else 243),
            "wrong kernel architecture")

    sys.dont_write_bytecode = True
    strict = load("strict_fat_extra", options.runner.resolve())
    fat32 = load("fat32_extra_helpers", source / "tests/run_fat32_vfs_tests.py")
    require(fat32.XV6 == source, "wrong FAT32 module imported")
    redirects = {(source / "kernel/kernel").resolve(): kernel,
                 (source / "fs.img").resolve(): root_image}

    class ArtifactCopies:
        def __getattr__(self, name):
            return getattr(shutil, name)

        def copyfile(self, src, dst, *args, **kwargs):
            require(Path(dst).resolve().is_relative_to(out), "copy outside test output")
            return shutil.copyfile(redirects.get(Path(src).resolve(), src), dst, *args, **kwargs)

    fat32.shutil = ArtifactCopies()
    out.mkdir(parents=True, exist_ok=False)
    runner = strict.Runner(source, out, qemu, options.arch)
    try:
        runner.results.update({
            "scope": "FAT32 8 KiB cluster and VFAT cross-cluster lookup integration",
            "source_commit": subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
            "source_status": subprocess.check_output(
                ["git", "-C", str(source), "status", "--porcelain"], text=True),
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
        shutil.copyfile(__file__, out / "runner-used.py")
        shutil.copyfile(options.runner, out / "strict-runner-used.py")
        shutil.copyfile(source / "tests/run_fat32_vfs_tests.py", out / "fat32-helper-used.py")
        runner.save()

        def guest(directory, commands, *, fat_image="fat32.img", log_name="qemu.log"):
            root = directory / ("root-" + Path(log_name).stem + ".img")
            require(not root.exists(), "root copy already exists")
            shutil.copyfile(directory / "fs.img", root)
            require(strict.digest(directory / "kernel") == runner.results["initial_kernel_sha256"],
                    "kernel changed during test preparation")
            runner.guest(directory, Path(log_name).stem,
                         [*commands, ("sync", "sync: OK")],
                         {0: root, 2: directory / fat_image},
                         boot=["mount xv6 dev=1 /: OK", "mount fat32 dev=3 /fat: OK"])

        fat32.guest = guest
        guest_assertions = {
            "large": ["allocate directory cluster", "create file", "write across three clusters",
                      "reach the unaligned overwrite offset", "overwrite across a cluster boundary",
                      "size after overwrite", "verify data and neighbors"],
            "vfat": ["open root", "list cross-cluster UTF-8 name once; fall back on bad checksum",
                     "long and short paths identify the same file", "read by long name",
                     "reject corrupt long alias", "protect corrupt long slots from partial deletion"],
        }
        for name, filename in (("large", "fat32-large.img"), ("vfat", "fat32-vfat.img")):
            directory = out / name
            getattr(fat32, name + "_prepare")(directory)
            initial = strict.digest(directory / filename)
            getattr(fat32, name + "_run")(directory)
            capture = io.StringIO()
            with contextlib.redirect_stdout(capture):
                getattr(fat32, name + "_check")(directory)
            host_output = capture.getvalue()
            (directory / "host-assertions.log").write_text(host_output, encoding="utf-8")
            print(host_output, end="", flush=True)
            runner.record(name + " guest assertions and host checks", status="PASS",
                          guest_assertions=guest_assertions[name],
                          host_output=host_output.splitlines(),
                          initial_image_sha256=initial,
                          final_image_sha256=strict.digest(directory / filename))
        require(strict.digest(kernel) == runner.results["initial_kernel_sha256"], "kernel changed during run")
        require(strict.digest(root_image) == runner.results["initial_root_image_sha256"], "root image changed during run")
        runner.results["status"] = "PASS"
        print(options.arch + " FAT32 EXTRA ACCEPTANCE PASSED", flush=True)
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
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arch", choices=("riscv", "loongarch"), required=True)
    parser.add_argument("--runner", type=Path,
                        default=Path(__file__).with_name("run_acceptance.py"))
    execute(parser.parse_args())
