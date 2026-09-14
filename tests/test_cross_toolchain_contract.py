#!/usr/bin/env python3
"""Calibrate the coop aarch64-to-x86 cross-toolchain input checks."""

import argparse
import hashlib
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
    "bundle_id": "aarch64-clang-x86_64-al2023-v5",
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

    @staticmethod
    def sha256(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def write_checksum_manifest(
        self,
        manifest: Path,
        root: Path,
        scopes: tuple[str, ...],
    ) -> None:
        paths = []
        for scope in scopes:
            base = root / scope
            if not base.is_dir():
                continue
            paths.extend(
                path for path in base.rglob("*") if path.is_file() and not path.is_symlink()
            )
        manifest.write_text(
            "".join(
                f"{self.sha256(path)}  {path.relative_to(root).as_posix()}\n"
                for path in sorted(paths)
            ),
            encoding="utf-8",
        )

    def write_sysroot_manifest(self, root: Path) -> None:
        sysroot = root / "sysroot"
        regular_files = [
            {
                "path": f"./{path.relative_to(sysroot).as_posix()}",
                "sha256": self.sha256(path),
            }
            for path in sorted(sysroot.rglob("*"))
            if path.is_file() and not path.is_symlink()
        ]
        symlinks = [
            {
                "path": f"./{path.relative_to(sysroot).as_posix()}",
                "target": path.readlink().as_posix(),
            }
            for path in sorted(sysroot.rglob("*"))
            if path.is_symlink()
        ]
        (root / "sysroot.manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "regular_files": regular_files,
                    "symlinks": symlinks,
                }
            ),
            encoding="utf-8",
        )

    def make_bundle(
        self,
        root: Path,
        clang_contents: str = "#!/bin/sh\nexit 0\n",
    ) -> None:
        for executable in ("clang", "clang++", "ld.lld"):
            path = root / "bin" / executable
            path.parent.mkdir(parents=True, exist_ok=True)
            contents = clang_contents if executable == "clang" else "#!/bin/sh\nexit 0\n"
            path.write_text(contents, encoding="utf-8")
            path.chmod(0o755)
        payload = root / "usr/lib64/llvm18/bin/clang-18"
        payload.parent.mkdir(parents=True, exist_ok=True)
        payload.write_text("fixture clang payload\n", encoding="utf-8")
        host_runtime = root / "host/lib64/libc.so.6"
        host_runtime.parent.mkdir(parents=True)
        host_runtime.write_text("fixture host runtime\n", encoding="utf-8")
        (root / "usr/lib/clang/18/include").mkdir(parents=True)
        (root / "sysroot/usr/lib/gcc/x86_64-amazon-linux/11").mkdir(parents=True)
        sysroot_header = root / "sysroot/usr/include/fixture.h"
        sysroot_header.parent.mkdir(parents=True)
        sysroot_header.write_text("fixture sysroot header\n", encoding="utf-8")
        (root / "sysroot/usr/lib64").mkdir()
        (root / "sysroot/lib64").symlink_to("usr/lib64")
        (root / "rpms").mkdir()
        (root / "rpms/fixture.rpm").write_text("fixture rpm\n", encoding="utf-8")
        self.write_checksum_manifest(
            root / "native-tools.sha256",
            root,
            ("bin", "usr", "host"),
        )
        self.write_sysroot_manifest(root)
        self.write_checksum_manifest(root / "rpms.sha256", root / "rpms", (".",))
        manifest = dict(EXPECTED_MANIFEST)
        component_paths = {
            "launcher-payload": "bin/clang",
            "clang-payload": "usr/lib64/llvm18/bin/clang-18",
            "llvm-runtime-manifest": "native-tools.sha256",
            "al2023-x86_64-sysroot-manifest": "sysroot.manifest.json",
            "al2023-x86_64-rpm-manifest": "rpms.sha256",
        }
        manifest["components"] = [
            {
                "name": name,
                "version": "fixture-1",
                "path": path,
                "sha256": self.sha256(root / path),
            }
            for name, path in component_paths.items()
        ]
        (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    def configure(
        self,
        root: Optional[Path],
        toolchain: Optional[Path] = None,
    ) -> subprocess.CompletedProcess[str]:
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
                    f"-DCMAKE_TOOLCHAIN_FILE={toolchain or self.toolchain}",
                ],
                check=False,
                text=True,
                capture_output=True,
                env=environment,
            )

    def make_pinned_toolchain(self, directory: Path, root: Path) -> Path:
        toolchain_dir = directory / "toolchain"
        toolchain_dir.mkdir()
        toolchain = toolchain_dir / self.toolchain.name
        shutil.copy2(self.toolchain, toolchain)
        shutil.copy2(
            self.toolchain.parent / "validate_aarch64_clang_x86_64_al2023_bundle.py",
            toolchain_dir / "validate_aarch64_clang_x86_64_al2023_bundle.py",
        )
        manifest = root / "manifest.json"
        digest = (
            hashlib.sha256(manifest.read_bytes()).hexdigest()
            if manifest.is_file()
            else "0" * 64
        )
        (toolchain_dir / "aarch64-clang-x86_64-al2023.manifest.sha256").write_text(
            digest + "\n", encoding="utf-8")
        return toolchain

    def assert_rejected(
        self,
        root: Optional[Path],
        diagnostic: str,
        authenticate_bundle: bool = True,
    ) -> None:
        toolchain = None
        if authenticate_bundle and root is not None:
            toolchain = self.make_pinned_toolchain(root.parent, root)
        result = self.configure(root, toolchain)
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
            self.make_bundle(root, clang_contents="#!/bin/sh\nexit 1\n")
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
            toolchain = self.make_pinned_toolchain(Path(directory), root)
            result = self.configure(root, toolchain)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_accepts_complete_bundle_with_space_in_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle with spaces"
            self.make_bundle(root)
            toolchain = self.make_pinned_toolchain(Path(directory), root)
            result = self.configure(root, toolchain)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_structurally_valid_but_unpinned_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bundle"
            self.make_bundle(root)
            marker = Path(directory) / "compiler-executed"
            compiler = root / "bin/clang"
            compiler.write_text(
                f"#!/bin/sh\n: > '{marker}'\nexit 0\n",
                encoding="utf-8",
            )
            subprocess.run([compiler, "--version"], check=True)
            self.assertTrue(marker.exists())
            marker.unlink()
            self.assert_rejected(
                root,
                "COOP_CROSS_X86_64_MANIFEST_IDENTITY_MISMATCH",
                authenticate_bundle=False,
            )
            self.assertFalse(marker.exists(), "untrusted compiler executed before pin rejection")

    def test_rejects_authenticated_manifest_with_mutated_compiler_before_execution(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            marker = directory_path / "compiler-executed"
            self.make_bundle(root)
            compiler = root / "bin/clang"
            compiler.write_text(
                f"#!/bin/sh\n: > '{marker}'\nexit 0\n",
                encoding="utf-8",
            )
            compiler.chmod(0o755)
            subprocess.run([compiler, "--version"], check=True)
            self.assertTrue(marker.exists())
            marker.unlink()

            toolchain = self.make_pinned_toolchain(directory_path, root)
            result = self.configure(root, toolchain)
            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn("COOP_CROSS_X86_64_CLOSURE_AUTHENTICATION_FAILED", output)
            self.assertFalse(marker.exists(), "mutated compiler executed before closure rejection")

    def assert_authenticated_closure_rejected(self, directory: Path, root: Path) -> None:
        toolchain = self.make_pinned_toolchain(directory, root)
        result = self.configure(root, toolchain)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("COOP_CROSS_X86_64_CLOSURE_AUTHENTICATION_FAILED", output)

    def test_rejects_authenticated_manifest_with_unlisted_native_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            self.make_bundle(root)
            (root / "usr/unlisted-native-file").write_text("unlisted\n", encoding="utf-8")
            self.assert_authenticated_closure_rejected(directory_path, root)

    def test_rejects_authenticated_manifest_with_external_host_scope(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            external = directory_path / "external-host"
            self.make_bundle(root)
            shutil.copytree(root / "host", external)
            shutil.rmtree(root / "host")
            (root / "host").symlink_to(external, target_is_directory=True)
            self.assert_authenticated_closure_rejected(directory_path, root)

    def test_rejects_authenticated_manifest_with_mutated_sysroot_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            self.make_bundle(root)
            (root / "sysroot/usr/include/fixture.h").write_text("mutated\n", encoding="utf-8")
            self.assert_authenticated_closure_rejected(directory_path, root)

    def test_rejects_authenticated_manifest_with_retargeted_sysroot_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            self.make_bundle(root)
            link = root / "sysroot/lib64"
            link.unlink()
            link.symlink_to("usr/lib")
            self.assert_authenticated_closure_rejected(directory_path, root)

    def test_rejects_authenticated_manifest_with_mutated_rpm(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            self.make_bundle(root)
            (root / "rpms/fixture.rpm").write_text("mutated\n", encoding="utf-8")
            self.assert_authenticated_closure_rejected(directory_path, root)

    def test_rejects_authenticated_manifest_with_external_rpm_scope(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            external = directory_path / "external-rpms"
            self.make_bundle(root)
            shutil.copytree(root / "rpms", external)
            shutil.rmtree(root / "rpms")
            (root / "rpms").symlink_to(external, target_is_directory=True)
            self.assert_authenticated_closure_rejected(directory_path, root)

    def test_rejects_non_regular_authenticated_closure_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            self.make_bundle(root)
            manifest = root / "native-tools.sha256"
            manifest.unlink()
            os.mkfifo(manifest)
            self.assert_authenticated_closure_rejected(directory_path, root)

    def test_rejects_multiple_manifest_pin_entries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            root = directory_path / "bundle"
            self.make_bundle(root)
            toolchain = self.make_pinned_toolchain(directory_path, root)
            pin = toolchain.parent / "aarch64-clang-x86_64-al2023.manifest.sha256"
            pin.write_text(pin.read_text(encoding="utf-8") + "0" * 64 + "\n", encoding="utf-8")
            result = self.configure(root, toolchain)
            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn("COOP_CROSS_X86_64_MANIFEST_PIN_MALFORMED", output)

    def test_cross_build_defers_the_effective_gtest_discovery_call(self) -> None:
        cmake_lists = self.toolchain.parents[2] / "CMakeLists.txt"
        source = cmake_lists.read_text(encoding="utf-8")
        self.assertIn(
            "if(CMAKE_CROSSCOMPILING)\n"
            "    # The test executable targets x86. Discover it on the target host when\n"
            "    # ctest runs, never on the aarch64 build host while the binary is linked.\n"
            "    gtest_discover_tests(coop_tests DISCOVERY_MODE PRE_TEST)\n"
            "else()\n"
            "    gtest_discover_tests(coop_tests)\n"
            "endif()",
            source,
        )
        self.assertEqual(source.count("gtest_discover_tests(coop_tests"), 2)


if __name__ == "__main__":
    unittest.main(argv=[__file__])
