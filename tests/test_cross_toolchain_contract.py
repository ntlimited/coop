#!/usr/bin/env python3
"""Calibrate the coop aarch64-to-x86 cross-toolchain input checks."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from typing import Optional


EXPECTED_MANIFEST = {
    "schema_version": 1,
    "bundle_id": "aarch64-clang-x86_64-al2023-v4",
    "exec_arch": "aarch64",
    "target_arch": "x86_64",
    "target_cpu": "x86-64-v2",
    "target_libc": "glibc-2.34",
    "cxx_standard_library": "gcc-11-libstdc++",
}


class CrossToolchainContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parser = argparse.ArgumentParser()
        parser.add_argument("--cmake", required=True)
        parser.add_argument("--generator", required=True)
        parser.add_argument("--toolchain", required=True)
        cls.args, unknown = parser.parse_known_args()
        if unknown:
            raise ValueError(f"unexpected arguments: {unknown}")
        cls.toolchain = Path(cls.args.toolchain).resolve()

    def make_bundle(self, root: Path) -> None:
        for executable in ("clang", "clang++", "ld.lld"):
            path = root / "bin" / executable
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            path.chmod(0o755)
        (root / "usr/lib/clang/18/include").mkdir(parents=True)
        (root / "sysroot/usr/lib/gcc/x86_64-amazon-linux/11").mkdir(parents=True)
        (root / "manifest.json").write_text(
            json.dumps(EXPECTED_MANIFEST), encoding="utf-8")

    def configure(self, root: Optional[Path]) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / "probe"
            build = Path(directory) / "build"
            probe.mkdir()
            (probe / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(cross_toolchain_contract NONE)\n",
                encoding="utf-8",
            )
            environment = os.environ.copy()
            if root is None:
                environment.pop("COOP_CROSS_X86_64_ROOT", None)
            else:
                environment["COOP_CROSS_X86_64_ROOT"] = str(root)
            return subprocess.run(
                [
                    self.args.cmake,
                    "-S",
                    str(probe),
                    "-B",
                    str(build),
                    "-G",
                    self.args.generator,
                    f"-DCMAKE_TOOLCHAIN_FILE={self.toolchain}",
                ],
                check=False,
                text=True,
                capture_output=True,
                env=environment,
            )

    def assert_rejected(self, root: Optional[Path], diagnostic: str) -> None:
        result = self.configure(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(diagnostic, output)

    def test_rejects_absent_root(self) -> None:
        self.assert_rejected(None, "COOP_CROSS_X86_64_ROOT_MISSING")

    def test_rejects_malformed_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "manifest.json").write_text("not JSON", encoding="utf-8")
            self.assert_rejected(root, "COOP_CROSS_X86_64_MANIFEST_MALFORMED")

    def test_rejects_non_regular_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "manifest.json").unlink()
            os.mkfifo(root / "manifest.json")
            self.assert_rejected(root, "COOP_CROSS_X86_64_MANIFEST_MALFORMED")

    def test_rejects_missing_compiler(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "bin/clang").unlink()
            self.assert_rejected(root, "COOP_CROSS_X86_64_COMPILER_MISSING")

    def test_rejects_non_executable_compiler(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "bin/clang").chmod(0o644)
            self.assert_rejected(root, "COOP_CROSS_X86_64_COMPILER_MISSING")

    def test_rejects_unusable_compiler(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "bin/clang").write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
            self.assert_rejected(root, "COOP_CROSS_X86_64_COMPILER_MISSING")

    def test_rejects_non_regular_compiler(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "bin/clang").unlink()
            os.mkfifo(root / "bin/clang")
            self.assert_rejected(root, "COOP_CROSS_X86_64_COMPILER_MISSING")

    def test_rejects_external_compiler_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            external = Path(directory) / "external-clang"
            self.make_bundle(root)
            external.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            external.chmod(0o755)
            (root / "bin/clang").unlink()
            (root / "bin/clang").symlink_to(external)
            self.assert_rejected(root, "COOP_CROSS_X86_64_COMPILER_MISSING")

    def test_rejects_compiler_resolved_outside_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            external_bin = Path(directory) / "external-bin"
            self.make_bundle(root)
            shutil.copytree(root / "bin", external_bin)
            shutil.rmtree(root / "bin")
            (root / "bin").symlink_to(external_bin, target_is_directory=True)
            self.assert_rejected(root, "COOP_CROSS_X86_64_COMPILER_MISSING")

    def test_rejects_missing_linker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "bin/ld.lld").unlink()
            self.assert_rejected(root, "COOP_CROSS_X86_64_LINKER_MISSING")

    def test_rejects_non_executable_linker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            (root / "bin/ld.lld").chmod(0o644)
            self.assert_rejected(root, "COOP_CROSS_X86_64_LINKER_MISSING")

    def test_rejects_missing_resource_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            shutil.rmtree(root / "usr/lib/clang/18")
            self.assert_rejected(root, "COOP_CROSS_X86_64_RESOURCE_DIR_MISSING")

    def test_rejects_resource_directory_outside_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            external_usr = Path(directory) / "external-usr"
            self.make_bundle(root)
            shutil.copytree(root / "usr", external_usr)
            shutil.rmtree(root / "usr")
            (root / "usr").symlink_to(external_usr, target_is_directory=True)
            self.assert_rejected(root, "COOP_CROSS_X86_64_RESOURCE_DIR_MISSING")

    def test_rejects_direct_resource_directory_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            resource_dir = root / "usr/lib/clang/18"
            internal_target = root / "resource-dir-target"
            self.make_bundle(root)
            shutil.copytree(resource_dir, internal_target)
            shutil.rmtree(resource_dir)
            resource_dir.symlink_to(internal_target, target_is_directory=True)
            self.assert_rejected(root, "COOP_CROSS_X86_64_RESOURCE_DIR_MISSING")

    def test_rejects_symlinked_sysroot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            external_sysroot = Path(directory) / "external-sysroot"
            self.make_bundle(root)
            shutil.copytree(root / "sysroot", external_sysroot)
            shutil.rmtree(root / "sysroot")
            (root / "sysroot").symlink_to(external_sysroot, target_is_directory=True)
            self.assert_rejected(root, "COOP_CROSS_X86_64_SYSROOT_MISSING")

    def test_rejects_missing_gcc_installation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            shutil.rmtree(root / "sysroot/usr/lib/gcc/x86_64-amazon-linux/11")
            self.assert_rejected(root, "COOP_CROSS_X86_64_GCC_INSTALL_MISSING")

    def test_rejects_gcc_installation_outside_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            external_gcc = Path(directory) / "external-gcc"
            self.make_bundle(root)
            gcc_parent = root / "sysroot/usr/lib/gcc/x86_64-amazon-linux"
            shutil.copytree(gcc_parent, external_gcc)
            shutil.rmtree(gcc_parent)
            gcc_parent.symlink_to(external_gcc, target_is_directory=True)
            self.assert_rejected(root, "COOP_CROSS_X86_64_GCC_INSTALL_MISSING")

    def test_rejects_direct_gcc_installation_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            gcc_install = root / "sysroot/usr/lib/gcc/x86_64-amazon-linux/11"
            internal_target = root / "gcc-install-target"
            self.make_bundle(root)
            shutil.copytree(gcc_install, internal_target)
            shutil.rmtree(gcc_install)
            gcc_install.symlink_to(internal_target, target_is_directory=True)
            self.assert_rejected(root, "COOP_CROSS_X86_64_GCC_INSTALL_MISSING")

    def test_rejects_missing_sysroot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            shutil.rmtree(root / "sysroot")
            self.assert_rejected(root, "COOP_CROSS_X86_64_SYSROOT_MISSING")

    def test_accepts_complete_synthetic_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            result = self.configure(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_accepts_complete_bundle_with_space_in_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle with spaces"
            self.make_bundle(root)
            result = self.configure(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__])
