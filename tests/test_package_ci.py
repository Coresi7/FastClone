"""Unit tests for scripts/package_ci.py (task ci-multiplatform-packaging).

stdlib unittest only. Fixtures are in-memory byte blobs (PE/ELF/Mach-O headers)
written to a private tmp dir for this test session; NO fixture binary is ever
executed — every subprocess goes through the injected FakeRunner seam. This
file is NOT registered in ctest and never runs in CI (05-gate §4.3).

Run from repo root:  python -m unittest tests.test_package_ci
"""

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import package_ci  # noqa: E402


# ---------------------------------------------------------------- fixtures
def pe_bytes(machine=0x8664, e_lfanew=0x80):
    b = bytearray(0x200)
    b[0:2] = b"MZ"
    b[0x3C:0x40] = e_lfanew.to_bytes(4, "little")
    if e_lfanew + 6 <= 0x200:  # out-of-range offsets: leave gaps for parse to reject
        b[e_lfanew:e_lfanew + 4] = b"PE\x00\x00"
        b[e_lfanew + 4:e_lfanew + 6] = machine.to_bytes(2, "little")
    out = bytes(b)
    assert len(out) == 0x200, f"len={len(out)}"
    assert out[0:2] == b"MZ"
    assert out[0x3C:0x40] == e_lfanew.to_bytes(4, "little"), out[0x3C:0x40].hex()
    if e_lfanew + 6 <= 0x200:
        assert out[e_lfanew:e_lfanew + 4] == b"PE\x00\x00", out[e_lfanew:e_lfanew + 4]
        assert out[e_lfanew + 4:e_lfanew + 6] == machine.to_bytes(2, "little")
    return out


def elf_bytes(ei_class=2, ei_data=1, machine=0x3E):
    b = bytearray(0x40)
    b[0:4] = b"\x7fELF"
    b[4] = ei_class
    b[5] = ei_data
    b[6] = 1
    b[18:20] = machine.to_bytes(2, "little" if ei_data == 1 else "big")
    out = bytes(b)
    assert len(out) == 0x40
    assert out[0:4] == b"\x7fELF"
    assert out[18:20] == machine.to_bytes(2, "little" if ei_data == 1 else "big")
    return out


def macho_bytes(cputype=0x01000007, endianness="LE"):
    b = bytearray(0x40)
    b[0:4] = b"\xcf\xfa\xed\xfe" if endianness == "LE" else b"\xfe\xed\xfa\xcf"
    bo = "little" if endianness == "LE" else "big"
    b[4:8] = cputype.to_bytes(4, bo)
    b[8:12] = (0).to_bytes(4, bo)
    out = bytes(b)
    assert len(out) == 0x40
    assert out[4:8] == cputype.to_bytes(4, bo)
    return out


def fat_bytes():
    return b"\xca\xfe\xba\xbe" + b"\x00" * 0x40


def macho32_bytes():
    return b"\xce\xfa\xed\xfe" + b"\x00" * 0x40


class FakeRunner(package_ci.Runner):
    """Test seam. Records every argv. Simulates `cmake --install` by copying
    files byte-for-byte from the source build dir (never executes anything)."""

    def __init__(self):
        self.calls = []
        self.versions = {"FastClone": "9.9.9", "FastCheck": "9.9.9"}
        self.install_src = None          # dir holding the two target binaries
        self.install_rc = 0
        self.probe_out = b""
        self.probe_rc = 1
        self.probe_map = {}              # argv0 -> (rc, out)
        self.exc_at = set()              # {"install","version","probe","source"}
        self.git_out = {
            "rev-parse": (0, b"031115a274e7571f1efe218c3c347d36a6a60c50\n"),
            "status": (0, b" M FastClone/version.h\n"),
        }

    def run(self, argv, timeout):
        argv = [str(a) for a in argv]
        self.calls.append((tuple(argv), timeout))
        tag = ("install" if argv[0] == "cmake" else
               "version" if argv[-1] == "--version" else
               "source" if argv[0] == "git" else "probe")
        if tag in self.exc_at:
            if tag == "source":
                raise OSError("boom")
            raise subprocess.TimeoutExpired(argv, timeout)
        if argv[0] == "cmake":  # --install: emulate, byte copy only
            if self.install_rc != 0:
                return self.install_rc, b"", b"install failed"
            prefix = Path(argv[argv.index("--prefix") + 1])
            dst = prefix / "bin"
            dst.mkdir(parents=True, exist_ok=True)
            for p in self.install_src.iterdir():
                if p.is_file() and p.name != "CMakeCache.txt":
                    (dst / p.name).write_bytes(p.read_bytes())
            return 0, b"", b""
        if argv[-1] == "--version":
            stem = Path(argv[0]).stem
            v = self.versions.get(stem)
            if v is None:
                return 1, b"", b"no version"
            return 0, f"{stem} {v}\n".encode(), b""
        if argv[0] == "git":
            rc, out = self.git_out["rev-parse" if "rev-parse" in argv else "status"]
            return rc, out, b""
        if argv[0] in self.probe_map:
            rc, out = self.probe_map[argv[0]]
            return rc, out, b""
        if argv[0] in ("dumpbin", "readelf", "otool", "getconf", "ldd", "sw_vers"):
            return self.probe_rc, self.probe_out, b""
        raise AssertionError(f"FakeRunner: unexpected argv {argv}")


# ---------------------------------------------------------------- tree maker
def make_build_tree(tmp, platform="windows", fc=None, fchk=None,
                    header_version="9.9.9", flags="/DWIN32 /D_WINDOWS /GR /EHsc",
                    cache_extra=None, compiler_id="MSVC",
                    compiler_version="14.51.36231", gen="Ninja", bt="Release"):
    tmp = Path(tmp)
    root = tmp / "root"
    build = tmp / "build"
    (root / "FastClone").mkdir(parents=True, exist_ok=True)
    (root / "FastClone" / "version.h").write_text(
        f'constexpr const char* kFastCloneVersion = "{header_version}";\n',
        encoding="utf-8")
    cache = {
        "CMAKE_HOME_DIRECTORY": str(root),
        "CMAKE_BUILD_TYPE": bt,
        "CMAKE_GENERATOR": gen,
        "CMAKE_CXX_COMPILER": "C:/VS/bin/Hostx64/x64/cl.exe",
        "CMAKE_CXX_FLAGS": flags,
        "CMAKE_PREFIX_PATH": "C:/xxHash-install",
    }
    cache.update(cache_extra or {})
    lines = [f"{k}:{'INTERNAL' if k in ('CMAKE_HOME_DIRECTORY', 'CMAKE_GENERATOR') else 'STRING'}={v}"
             for k, v in cache.items()]
    (build / "CMakeFiles" / "x").mkdir(parents=True, exist_ok=True)
    (build / "CMakeFiles" / "x" / "CMakeCXXCompiler.cmake").write_text(
        f'set(CMAKE_CXX_COMPILER_ID "{compiler_id}")\n'
        f'set(CMAKE_CXX_COMPILER_VERSION "{compiler_version}")\n',
        encoding="utf-8")
    (build / "CMakeCache.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    fc = pe_bytes() if fc is None else fc
    fchk = pe_bytes() if fchk is None else fchk
    if platform == "windows":
        (build / "FastClone.exe").write_bytes(fc)
        (build / "FastCheck.exe").write_bytes(fchk)
    else:
        (build / "FastClone").write_bytes(fc)
        (build / "FastCheck").write_bytes(fchk)
    return root, build


def base_env(tmp, job="build-test", runner_arch="X64", runner_os="Windows"):
    return {
        "RUNNER_TEMP": str(tmp / "rt"),
        "RUNNER_OS": runner_os,
        "RUNNER_ARCH": runner_arch,
        "GITHUB_SHA": "031115a274e7571f1efe218c3c347d36a6a60c50",
        "GITHUB_RUN_ID": "41008618624",
        "GITHUB_RUN_ATTEMPT": "1",
        "GITHUB_WORKFLOW": "ci",
        "GITHUB_JOB": job,
        "GITHUB_REF": "refs/heads/master",
        "GITHUB_OUTPUT": str(tmp / "github_output.txt"),
    }


def pkg_argv(tmp, kind="native", platform="windows", tests="passed",
             expected=None, header=None, out_dir=None, stage_prefix=None,
             build_dir=None):
    a = ["package", "--build-dir", str(build_dir or (tmp / "build")),
         "--platform", platform, "--kind", kind, "--tests", tests,
         "--stage-prefix", str(stage_prefix or (tmp / "stage")),
         "--out-dir", str(out_dir if out_dir else (tmp / "dist")),
         "--github-output", str(tmp / "github_output.txt")]
    if expected:
        a += ["--expected-arch", expected]
    if header:
        a += ["--version-header", str(header)]
    return a


# ---------------------------------------------------------------- arch canon
class TestNormArch(unittest.TestCase):
    def test_aliases(self):
        for v in ("X64", "x64", "AMD64", "amd64", "x86_64", "x86-64"):
            self.assertEqual(package_ci.norm_arch(v), "x64", v)
        for v in ("ARM64", "arm64", "aarch64", "AARCH64"):
            self.assertEqual(package_ci.norm_arch(v), "arm64", v)

    def test_unknown(self):
        for v in ("", "i386", "0x1234", "riscv", None):
            self.assertIsNone(package_ci.norm_arch(v), v)


class TestHeaderMatrices(unittest.TestCase):
    def test_pe_ok(self):
        ev = package_ci.parse_pe(pe_bytes(0x8664))[0]
        self.assertEqual((ev["arch"], ev["parse_status"], ev["pemachine"]),
                         ("x64", "ok", "0x8664"))
        ev = package_ci.parse_pe(pe_bytes(0xAA64))[0]
        self.assertEqual(ev["arch"], "arm64")

    def test_pe_machine_unknown(self):
        ev, _ = package_ci.parse_pe(pe_bytes(0x1234))
        self.assertEqual((ev["parse_status"], ev["arch"]), ("unknown", None))

    def test_pe_struct_fail(self):
        self.assertEqual(package_ci.parse_pe(pe_bytes(e_lfanew=0x10)),
                         (None, "pe_bad_lfanew"))
        self.assertEqual(package_ci.parse_pe(pe_bytes(e_lfanew=0xFFFF)),
                         (None, "pe_bad_lfanew"))
        self.assertEqual(package_ci.parse_pe(b"MZ" + b"\x00" * 0x10),
                         (None, "pe_too_short"))
        data = bytearray(pe_bytes())
        data[0x80:0x84] = b"NOPE"
        self.assertEqual(package_ci.parse_pe(bytes(data)),
                         (None, "pe_bad_signature"))
        self.assertEqual(package_ci.parse_pe(b"XX" + pe_bytes()[2:]),
                         (None, "pe_bad_mz"))

    def test_elf_ok_le_be(self):
        ev = package_ci.parse_elf(elf_bytes(2, 1, 0x3E))[0]
        self.assertEqual((ev["arch"], ev["endianness"]), ("x64", "LE"))
        self.assertEqual(ev["elfmachine"], "0x003E")
        ev = package_ci.parse_elf(elf_bytes(2, 2, 0xB7))[0]
        self.assertEqual((ev["arch"], ev["endianness"]), ("arm64", "BE"))

    def test_elf_fail(self):
        self.assertEqual(package_ci.parse_elf(elf_bytes()[:2]),
                         (None, "elf_too_short"))
        self.assertEqual(package_ci.parse_elf(elf_bytes(ei_class=1)),
                         (None, "unsupported_elf32"))
        bad = bytearray(elf_bytes())
        bad[5] = 3
        self.assertEqual(package_ci.parse_elf(bytes(bad)), (None, "elf_bad_data"))
        self.assertEqual(package_ci.parse_elf(b"\x7fNOPE" + elf_bytes()[4:]),
                         (None, "elf_bad_magic"))
        ev, _ = package_ci.parse_elf(elf_bytes(2, 1, 0x9999))
        self.assertEqual((ev["parse_status"], ev["arch"]), ("unknown", None))

    def test_macho_ok(self):
        ev = package_ci.parse_macho(macho_bytes(0x01000007, "LE"))[0]
        self.assertEqual((ev["arch"], ev["machocputype"]), ("x64", "0x01000007"))
        ev = package_ci.parse_macho(macho_bytes(0x0100000C, "BE"))[0]
        self.assertEqual((ev["arch"], ev["endianness"]), ("arm64", "BE"))

    def test_macho_fat_and_32(self):
        for b in (fat_bytes(),
                  b"\xbe\xba\xfe\xca" + fat_bytes()[4:],
                  b"\xca\xfe\xba\xbf" + fat_bytes()[4:]):
            self.assertEqual(package_ci.parse_macho(b),
                             (None, "unsupported_fat_binary"))
        self.assertEqual(package_ci.parse_macho(macho32_bytes()),
                         (None, "unsupported_macho32"))
        self.assertEqual(package_ci.parse_macho(b"\x00" * 0x40),
                         (None, "macho_bad_magic"))
        ev, _ = package_ci.parse_macho(macho_bytes(0x99999999))
        self.assertEqual((ev["parse_status"], ev["arch"]), ("unknown", None))

    def test_evidence_dispatch(self):
        tmp = Path(tempfile.mkdtemp(prefix="fc-ev-"))
        for blob, want in ((pe_bytes(), "x64"), (pe_bytes(0xAA64), "arm64"),
                            (elf_bytes(2, 1, 0x3E), "x64"),
                            (elf_bytes(2, 2, 0xB7), "arm64"),
                            (macho_bytes(0x01000007, "LE"), "x64"),
                            (macho_bytes(0x0100000C, "BE"), "arm64")):
            p = tmp / f"b{abs(hash(blob))}.bin"
            p.write_bytes(blob)
            self.assertEqual(package_ci.read_binary_evidence(p)["arch"], want)
        for blob, reason in ((fat_bytes(), "unsupported_fat_binary"),
                             (macho32_bytes(), "unsupported_macho32"),
                             (b"\xde\xad\xbe\xef" + b"\x00" * 0x40, "arch_unknown")):
            p = tmp / f"x{abs(hash(blob))}.bin"
            p.write_bytes(blob)
            with self.assertRaises(package_ci.PackageError) as cm:
                package_ci.read_binary_evidence(p)
            self.assertEqual((cm.exception.code, cm.exception.reason), (4, reason))


# ---------------------------------------------------------------- flow
class PackageFlow(unittest.TestCase):
    """Positive + negative through package_ci.main() with FakeRunner."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="fc-pkg-test-"))
        # tmp trees are kept on disk (no rm -rf of anything, per task rules)

    def _tree(self, **kw):
        return make_build_tree(self.tmp, **kw)

    def _native(self, header_version="9.9.9", fc=None, fchk=None,
                runner_arch="X64", job="build-test"):
        root, build = self._tree(header_version=header_version, fc=fc, fchk=fchk)
        env = base_env(self.tmp, job=job, runner_arch=runner_arch)
        fake = FakeRunner()
        fake.install_src = build
        return fake, env, pkg_argv(self.tmp, build_dir=build)

    def test_native_success_zip(self):
        fake, env, argv = self._native()
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 0)
        out_dir = self.tmp / "dist"
        self.assertTrue((out_dir / "FastClone-9.9.9-windows-x64.zip").is_file())
        self.assertTrue((out_dir / "SHA256SUMS.txt").is_file())
        self.assertTrue((out_dir / "provenance.json").is_file())
        kv = dict(l.rstrip("\n").split("=", 1)
                  for l in (self.tmp / "github_output.txt").read_text(
                      encoding="utf-8").splitlines() if "=" in l)
        self.assertEqual(kv["artifact_name"], "FastClone-9.9.9-windows-x64")
        self.assertEqual(kv["arch"], "x64")
        self.assertEqual(kv["dist_glob"], str(out_dir).replace("\\", "/") + "/*")
        for v in kv.values():
            self.assertNotIn("\n", v)

    def test_linux_tar_gz_mode_755(self):
        if package_ci.norm_arch(platform.machine()) != "x64":
            self.skipTest("host not x64")
        root, build = self._tree(platform="linux", fc=elf_bytes(2, 1, 0x3E),
                                 fchk=elf_bytes(2, 1, 0x3E))
        env = base_env(self.tmp, runner_os="Linux")
        fake = FakeRunner()
        fake.install_src = build
        fake.probe_map = {"readelf": (0, b"Dynamic section:\n  NEEDED libxxhash.so.0\n"),
                          "getconf": (0, b"glibc 2.39\n")}
        argv = pkg_argv(self.tmp, platform="linux", build_dir=build)
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 0)
        arc = self.tmp / "dist" / "FastClone-9.9.9-linux-x64.tar.gz"
        self.assertTrue(arc.is_file())
        with tarfile.open(arc) as tf:
            modes = {ti.name: ti.mode for ti in tf}
        self.assertEqual(modes.get("bin/FastClone"), 0o755)
        self.assertEqual(modes.get("bin/FastCheck"), 0o755)
        self.assertEqual(modes.get("provenance.json"), 0o644)
        prov = json.loads((self.tmp / "dist" / "provenance.json")
                          .read_text(encoding="utf-8"))
        self.assertEqual(prov["platform"]["arch_evidence"]["elfmachine"], "0x003E")
        self.assertEqual(prov["runtime_dependencies"]["min_system"], "glibc 2.39")
        self.assertEqual(prov["runtime_requirements"]["crt"], None)
        self.assertEqual(prov["build"]["msvc_runtime_library"], None)

    def test_no_leakage_members(self):
        fake, env, argv = self._native()
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 0)
        with zipfile.ZipFile(self.tmp / "dist" /
                             "FastClone-9.9.9-windows-x64.zip") as zf:
            names = sorted(zf.namelist())
        self.assertEqual(names, ["bin/FastCheck.exe", "bin/FastClone.exe",
                                 "provenance.json"])
        joined = " ".join(names)
        for banned in ("LICENSE", "Tests", "RouteProbe", ".log", ".pem",
                       "secret", "CMakeLists", "version.h"):
            self.assertNotIn(banned, joined)

    def test_provenance_fields_and_no_user_m(self):
        fake, env, argv = self._native()
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 0)
        raw = (self.tmp / "dist" / "provenance.json").read_text(encoding="utf-8")
        prov = json.loads(raw)
        for key in ("schema", "package", "product", "source", "build", "platform",
                    "execution", "runtime_dependencies", "runtime_requirements",
                    "files", "limitations"):
            self.assertIn(key, prov)
        self.assertEqual(prov["schema"], "fastclone.provenance/1")
        self.assertEqual(prov["product"]["version_source"], "binary_version_output")
        self.assertEqual(prov["product"]["version_header_value"], "9.9.9")
        self.assertEqual(prov["platform"]["arch_evidence"]["pemachine"], "0x8664")
        self.assertEqual(prov["platform"]["target_arch"], "x64")
        self.assertEqual(prov["execution"], {"on_target": True, "tests": "passed",
                                             "note": None})
        self.assertEqual(prov["build"]["msvc_runtime_library"],
                         "MultiThreadedDLL(default /MD)")
        self.assertEqual(prov["build"]["compiler_id"], "MSVC")
        self.assertEqual(prov["build"]["generator"], "Ninja")
        self.assertEqual(prov["runtime_requirements"]["vcredist_required"], True)
        self.assertEqual(prov["runtime_requirements"]["portable_static"], False)
        self.assertEqual(prov["package"]["compat"]["changed"], False)
        self.assertEqual(prov["package"]["size_bytes"],
                         (self.tmp / "dist" / "FastClone-9.9.9-windows-x64.zip")
                         .stat().st_size)
        self.assertEqual(prov["source"]["commit"],
                         "031115a274e7571f1efe218c3c347d36a6a60c50")
        self.assertNotIn("1.0.73", raw)

    def test_cross_success_arm64(self):
        root, build = self._tree(fc=pe_bytes(0xAA64), fchk=pe_bytes(0xAA64))
        env = base_env(self.tmp, job="windows-arm64")
        fake = FakeRunner()
        fake.install_src = build
        fake.probe_map = {"dumpbin": (0, b"Image has the following dependencies:\n"
                                        b"    WS2_32.dll\n")}
        argv = pkg_argv(self.tmp, kind="cross", tests="cross-build-only",
                        expected="arm64", build_dir=build)
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 0)
        prov = json.loads((self.tmp / "dist" / "provenance.json")
                          .read_text(encoding="utf-8"))
        self.assertEqual(prov["execution"]["on_target"], False)
        self.assertEqual(prov["execution"]["note"], "未在 ARM64 上运行")
        self.assertEqual(prov["product"]["version_source"],
                         "configure_generated_version_h")
        self.assertEqual(prov["platform"]["arch_evidence"]["pemachine"], "0xAA64")
        self.assertEqual(prov["platform"]["target_arch"], "arm64")
        self.assertEqual(prov["package"]["compat"],
                         {"changed": True,
                          "previous_artifact_name": "FastClone-windows-arm64",
                          "previous_form": "两裸 exe 经 upload-artifact@v4 打成 zip 容器"})
        self.assertTrue(any("ARM64" in x for x in prov["limitations"]))
        self.assertEqual(prov["runtime_requirements"]["crt"], "static(/MT)")
        self.assertEqual(prov["runtime_requirements"]["portable_static"], True)
        self.assertEqual(prov["runtime_requirements"]["actually_static"], True)

    def test_cross_host_binary_rejected(self):
        fake, env, argv = self._native()  # x64 PE tree
        argv = pkg_argv(self.tmp, kind="cross", tests="cross-build-only",
                        expected="arm64")
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 4)

    def test_cross_expected_alias_accepted(self):
        # canonical aliases accepted on the CLI and normalized
        root, build = self._tree(fc=pe_bytes(0xAA64), fchk=pe_bytes(0xAA64))
        env = base_env(self.tmp, job="windows-arm64")
        fake = FakeRunner()
        fake.install_src = build
        for alias in ("arm64", "ARM64", "aarch64"):
            argv = pkg_argv(self.tmp, kind="cross", tests="cross-build-only",
                            expected=alias, build_dir=build,
                            out_dir=self.tmp / "dist")
            argv = [a if a != str(self.tmp / "dist") else str(self.tmp / "dist")
                    for a in argv]
            rc = package_ci.main(argv, runner=fake, env=env)
            self.assertEqual(rc, 0, alias)
            break  # first success suffices; collision test elsewhere covers rerun

    def test_usage_rejections(self):
        fake, env, argv = self._native()
        build = self.tmp / "build"
        base = ["package", "--build-dir", str(build),
                "--stage-prefix", str(self.tmp / "stage"),
                "--out-dir", str(self.tmp / "dist"),
                "--github-output", str(self.tmp / "github_output.txt")]
        self.assertEqual(package_ci.main(
            base + ["--platform", "windows", "--kind", "cross",
                    "--expected-arch", "arm64", "--tests", "passed"],
            runner=fake, env=env), 2)
        self.assertEqual(package_ci.main(
            base + ["--platform", "windows", "--kind", "native",
                    "--tests", "cross-build-only"],
            runner=fake, env=env), 2)
        self.assertEqual(package_ci.main(
            base + ["--platform", "windows", "--kind", "cross",
                    "--tests", "cross-build-only"],
            runner=fake, env=env), 2)
        self.assertEqual(package_ci.main(
            base + ["--platform", "windows", "--kind", "cross",
                    "--expected-arch", "i386", "--tests", "cross-build-only"],
            runner=fake, env=env), 4)
        # sanitizer build refused (san never packaged)
        tmp2 = self.tmp / "san"
        make_build_tree(tmp2, flags="/O2 -fsanitize=address")
        fake2 = FakeRunner()
        fake2.install_src = tmp2 / "build"
        self.assertEqual(package_ci.main(
            ["package", "--build-dir", str(tmp2 / "build"),
             "--stage-prefix", str(tmp2 / "s"), "--out-dir", str(tmp2 / "d"),
             "--platform", "windows", "--kind", "native", "--tests", "passed"],
            runner=fake2, env=env), 2)

    def test_missing_env(self):
        for gone in package_ci.REQUIRED_ENV:
            fake, env, argv = self._native()
            del env[gone]
            rc = package_ci.main(argv, runner=fake, env=env)
            self.assertEqual(rc, 2, gone)

    def test_native_arch_mismatch(self):
        fake, env, argv = self._native(runner_arch="ARM64")  # header x64
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 4)
        fake, env, argv = self._native(runner_arch="RISCV")
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 4)
        # native with contradicting --expected-arch reference
        fake, env, argv = self._native()
        argv = pkg_argv(self.tmp, expected="arm64")
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 4)

    def test_binaries_differ(self):
        fake, env, argv = self._native(fc=pe_bytes(0x8664), fchk=pe_bytes(0xAA64))
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 4)

    def test_header_structural_fail(self):
        fake, env, argv = self._native(fc=b"MZ" + b"\x00" * 0x10)
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 4)
        fake, env, argv = self._native(fc=fat_bytes())
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 4)

    def test_version_cases(self):
        fake, env, argv = self._native()
        fake.versions["FastCheck"] = "9.9.8"          # binaries disagree
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 5)
        fake, env, argv = self._native(header_version="8.8.8")  # header != binary
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 5)
        fake, env, argv = self._native()
        fake.versions["FastClone"] = None             # --version rc != 0
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 5)
        fake, env, argv = self._native()
        fake.versions["FastClone"] = "not-a-version"  # unparseable output
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 5)

    def test_whitelist_violations(self):
        for extra in ("FastCloneTests.exe", "probe.log", "key.pem"):
            fake, env, argv = self._native()
            (self.tmp / "build" / extra).write_bytes(b"x")
            self.assertEqual(package_ci.main(argv, runner=fake, env=env), 3, extra)
        fake, env, argv = self._native()
        (self.tmp / "build" / "FastCheck.exe").unlink()
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 3)

    def test_install_and_timeout_failures(self):
        for exc in (None, "install", "version", "probe", "source"):
            fake, env, argv = self._native()
            if exc is None:
                fake.install_rc = 1
            else:
                fake.exc_at.add(exc)
            self.assertEqual(package_ci.main(argv, runner=fake, env=env), 7, exc)

    def test_probe_unknown_is_not_a_claim(self):
        fake, env, argv = self._native()
        fake.probe_rc, fake.probe_out = 1, b""
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 0)
        prov = json.loads((self.tmp / "dist" / "provenance.json")
                          .read_text(encoding="utf-8"))
        self.assertEqual(prov["runtime_dependencies"]["probe_status"], "unknown")
        self.assertIsNone(prov["runtime_dependencies"]["libraries"])
        self.assertIsNone(prov["runtime_requirements"]["actually_static"])
        self.assertTrue(any("unknown" in x for x in prov["limitations"]))

    def test_probe_ok_dynamic_crt(self):
        fake, env, argv = self._native()
        fake.probe_map["dumpbin"] = (0, b"Image has the following dependencies:\n"
                                        b"    MSVCP140.dll\n    VCRUNTIME140.dll\n")
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 0)
        prov = json.loads((self.tmp / "dist" / "provenance.json")
                          .read_text(encoding="utf-8"))
        self.assertEqual(prov["runtime_dependencies"]["probe_status"], "ok")
        self.assertEqual(prov["runtime_dependencies"]["libraries"],
                         ["MSVCP140.dll", "VCRUNTIME140.dll"])
        self.assertEqual(prov["runtime_requirements"]["actually_static"], False)

    def test_output_collision_refuses_and_keeps_old_bytes(self):
        fake, env, argv = self._native()
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 0)
        dist = self.tmp / "dist"
        old = {p.name: p.read_bytes() for p in dist.iterdir()}
        fake2, env2, argv2 = self._native()  # same out-dir default tmp/dist
        self.assertEqual(package_ci.main(argv2, runner=fake2, env=env2), 7)
        self.assertEqual({p.name: p.read_bytes() for p in dist.iterdir()}, old)

    def test_github_output_rejects_newline_nul(self):
        fake, env, argv = self._native()
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 0)
        out_file = self.tmp / "github_output.txt"
        before = out_file.read_bytes()
        for bad in ({"k": "a\nb"}, {"k": "v\0w"}, {"bad key": "x"}, {"k": "a\rb"}):
            with self.assertRaises(package_ci.PackageError) as cm:
                package_ci.write_github_output(out_file, bad)
            self.assertEqual(cm.exception.code, 2)
        self.assertEqual(out_file.read_bytes(), before)  # never a half line

    def test_runner_is_the_only_exec_path(self):
        fake, env, argv = self._native()
        rc = package_ci.main(argv, runner=fake, env=env)
        self.assertEqual(rc, 0)
        argvs = [c[0] for c in fake.calls]
        self.assertTrue(any(a[0] == "cmake" for a in argvs))
        self.assertTrue(any(a[0] == "git" for a in argvs))
        self.assertTrue(any(a[0] == "dumpbin" for a in argvs))
        self.assertTrue(any(a[-1] == "--version" for a in argvs))
        for a in argvs:
            if a[-1] == "--version":
                self.assertTrue(a[0].endswith(".exe"))


# ---------------------------------------------------------------- verify
class TestVerify(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="fc-verify-test-"))

    def _dist(self, sub="t", fc=None, fchk=None):
        make_build_tree(self.tmp / sub, fc=fc, fchk=fchk)
        env = base_env(self.tmp / sub)
        fake = FakeRunner()
        fake.install_src = self.tmp / sub / "build"
        out = self.tmp / sub / "dist"
        argv = pkg_argv(self.tmp / sub, build_dir=self.tmp / sub / "build",
                        out_dir=out)
        self.assertEqual(package_ci.main(argv, runner=fake, env=env), 0)
        return out

    def test_verify_ok(self):
        out = self._dist()
        self.assertEqual(package_ci.main(["verify", "--dist-dir", str(out)]), 0)

    def test_verify_tamper_and_manifest(self):
        out = self._dist()
        arc = next(p for p in out.iterdir() if p.suffix == ".zip")
        data = bytearray(arc.read_bytes())
        data[-1] ^= 0xFF
        arc.write_bytes(bytes(data))
        self.assertEqual(package_ci.main(["verify", "--dist-dir", str(out)]), 6)
        out2 = self._dist("t2")
        p = out2 / "provenance.json"
        prov = json.loads(p.read_text(encoding="utf-8"))
        prov["package"]["size_bytes"] += 1
        p.write_text(json.dumps(prov), encoding="utf-8")
        self.assertEqual(package_ci.main(["verify", "--dist-dir", str(out2)]), 6)
        out3 = self._dist("t3")
        p = out3 / "provenance.json"
        prov = json.loads(p.read_text(encoding="utf-8"))
        prov["files"][0]["sha256"] = "0" * 64
        p.write_text(json.dumps(prov), encoding="utf-8")
        self.assertEqual(package_ci.main(["verify", "--dist-dir", str(out3)]), 6)
        out4 = self._dist("t4")
        (out4 / "SHA256SUMS.txt").unlink()
        self.assertEqual(package_ci.main(["verify", "--dist-dir", str(out4)]), 6)

    def _evil(self, names, sub):
        out = self._dist(sub)
        arc = next(p for p in out.iterdir() if p.suffix == ".zip")
        with zipfile.ZipFile(arc) as zf:
            payload = {zi.filename: zf.read(zi) for zi in zf.infolist()}
        with zipfile.ZipFile(arc, "w") as zf:
            for n in names:
                zi = zipfile.ZipInfo(n, date_time=(1980, 1, 1, 0, 0, 0))
                zi.external_attr = 0o755 << 16
                zf.writestr(zi, payload.get(n, payload.get("provenance.json", b"{}")))
        return out

    def test_traversal_and_member_contract(self):
        cases = (["../evil.exe", "bin/FastClone.exe", "bin/FastCheck.exe",
                  "provenance.json"],
                 ["/abs/path.exe", "bin/FastClone.exe", "bin/FastCheck.exe",
                  "provenance.json"],
                 [r"bin\FastClone.exe", "bin/FastCheck.exe", "provenance.json"],
                 ["bin/FastClone.exe", "bin/FastClone.exe", "provenance.json"],
                 ["bin/FastClone.exe", "bin/FastCheck.exe", "provenance.json",
                  "extra.txt"])
        for i, names in enumerate(cases):
            out = self._evil(names, f"e{i}")
            self.assertEqual(package_ci.main(["verify", "--dist-dir", str(out)]), 6)

    def test_inarchive_vs_toplevel_disagree(self):
        out = self._dist("d")
        p = out / "provenance.json"
        prov = json.loads(p.read_text(encoding="utf-8"))
        prov["product"]["version"] = "0.0.1"
        p.write_text(json.dumps(prov), encoding="utf-8")
        self.assertEqual(package_ci.main(["verify", "--dist-dir", str(out)]), 6)

    def test_missing_dist(self):
        self.assertEqual(
            package_ci.main(["verify", "--dist-dir", str(self.tmp / "nope")]), 6)


# ---------------------------------------------------------------- static CI
class TestStaticCiGating(unittest.TestCase):
    """Pure text/regex line scan of the real .github/workflows/ci.yml — no
    third-party YAML dep in product/test code (05-gate §4.3)."""

    def setUp(self):
        self.text = (REPO / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8")
        self.lines = self.text.splitlines()
        # comment-stripped view: YAML comments are prose, never keys/flags
        self.code = "\n".join(
            re.sub(r"(^|\s)#.*$", "", l).rstrip() for l in self.lines)

    def _job_block(self, job):
        start = next(i for i, l in enumerate(self.lines)
                     if l.rstrip() == f"  {job}:")
        end = len(self.lines)
        for j in range(start + 1, len(self.lines)):
            if self.lines[j] and not self.lines[j].startswith(" "):
                end = j
                break
        return self.lines[start:end]

    def test_job_inventory_unchanged(self):
        body = self.text.split("jobs:", 1)[1]
        jobs = [l.strip()[:-1] for l in body.splitlines()
                if re.match(r"^  [A-Za-z0-9_-]+:$", l)]
        self.assertEqual(sorted(jobs),
                         ["build-test", "sanitizers", "windows-arm64"])
        self.assertEqual(self.text.count("runs-on:"), 3)
        self.assertIn("ubuntu-latest, windows-latest, macos-latest", self.text)
        self.assertEqual(self.text.count("san: [address, undefined]"), 1)
        self.assertIn("address, undefined", self.text)

    def test_ctest_precedes_package(self):
        bt = self._job_block("build-test")
        ctest = next(i for i, l in enumerate(bt) if "Test (ctest)" in l)
        pkg = next(i for i, l in enumerate(bt) if "Package (install→archive)" in l)
        self.assertLess(ctest, pkg)

    def test_sanitizers_have_no_package(self):
        san = "\n".join(self._job_block("sanitizers"))
        self.assertNotIn("Package", san)
        self.assertNotIn("package_ci.py", san)

    def test_arm64_replaced_upload(self):
        wa = "\n".join(self._job_block("windows-arm64"))
        self.assertIn("Package (cross)", wa)
        self.assertNotIn("Upload ARM64 artifact", wa)
        self.assertIn("--kind cross --expected-arch arm64 --tests cross-build-only", wa)
        self.assertIn("|| exit /b 1", wa)
        lines = self._job_block("windows-arm64")
        build = next(i for i, l in enumerate(lines) if "Build (ARM64, no test)" in l)
        pkg = next(i for i, l in enumerate(lines) if "Package (cross)" in l)
        self.assertLess(build, pkg)

    def test_no_continue_on_error(self):
        self.assertNotIn("continue-on-error", self.code)

    def test_uploads_are_strict(self):
        self.assertEqual(self.code.count("if-no-files-found: error"), 2)
        self.assertEqual(self.code.count("steps.pkg.outputs.dist_glob"), 2)
        self.assertEqual(self.code.count("steps.pkg.outputs.artifact_name"), 2)
        self.assertEqual(self.code.count("uses: actions/upload-artifact@v4"), 2)
        self.assertNotIn("retention-days", self.code)

    def test_protected_sections_untouched(self):
        self.assertIn("permissions:", self.text)
        self.assertIn("contents: read", self.text)
        self.assertIn("branches: [master]", self.text)
        self.assertIn("pull_request", self.text)
        self.assertIn("arch: x64", self.text)
        self.assertIn("arch: amd64_arm64", self.text)
        self.assertIn("MultiThreaded", self.text)
        self.assertIn("BUILD_SHARED_LIBS=OFF", self.text)
        self.assertIn("actions/checkout@v4", self.text)

    def test_run_blocks_only_controlled_context(self):
        def assert_ok(text, line):
            for interp in re.findall(r"\$\{\{([^}]*)\}\}", text):
                self.assertTrue(
                    interp.strip().startswith(("runner.", "matrix.", "env.")),
                    f"untrusted ${{{{{interp}}}}} in run: {line!r}")

        i = 0
        while i < len(self.lines):
            l = self.lines[i]
            m = re.match(r"^\s*run:\s*(.*)$", l)
            if m and "|" not in l:
                assert_ok(m.group(1), l)
                i += 1
                continue
            if m and "|" in l:
                base = len(l) - len(l.lstrip())
                j = i + 1
                while j < len(self.lines) and (
                        not self.lines[j].strip()
                        or len(self.lines[j]) - len(self.lines[j].lstrip()) > base):
                    assert_ok(self.lines[j], self.lines[j])
                    j += 1
                i = j
                continue
            i += 1
        self.assertIn("runner.os", self.code)  # controlled contexts really used
        self.assertIn("${{ matrix.os }}", self.code)


# ---------------------------------------------------------------- freeze (17-design §3.5)
class FreezeVersionTests(unittest.TestCase):
    """CI version freeze tests (17-design §3.5, 18 §5 C1): exactly one method
    per V number (V-01..V-09, V-10, V-11) -> 43 existing + 11 = 54 total.

    Every configure runs the REAL cmake binary against a tempdir fixture whose
    CMakeLists.txt is a slice of the REAL CMakeLists.txt (START anchor
    ^option(FC_VERSION_FREEZE .. END anchor ^message(STATUS "FastClone version:
    ${FASTCLONE_VERSION}"), each anchor must hit exactly 1 line, else self.fail
    — never a silent skip). project(... LANGUAGES NONE): no compiler probe, no
    xxhash, no real target is configured. The two repo-path set() lines are
    repointed into the fixture, so the real repo is NEVER configured here: the
    working tree's FastClone/version.h and .fcversion are neither read as a
    base nor written (AC-12 / R-01 safety). FakeRunner is not reused — the only
    subprocess is cmake itself; no fixture binary is ever executed (05 §4.3).
    V-10's remote evidence stays REMOTE-PENDING; locally only its static
    contract is asserted.
    """

    _START = re.compile(r'^option\(FC_VERSION_FREEZE')
    _END = re.compile(r'^message\(STATUS "FastClone version: \$\{FASTCLONE_VERSION\}"\)')
    _HEADER_LINE = re.compile(
        r'^set\(FC_VERSION_HEADER "\$\{CMAKE_CURRENT_SOURCE_DIR\}/FastClone/version\.h"\)$')
    _COUNTER_LINE = re.compile(
        r'^set\(FC_VERSION_COUNTER_FILE "\$\{CMAKE_CURRENT_SOURCE_DIR\}/\.fcversion"\)$')

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="fc-freeze-test-"))
        # tmp trees are kept on disk (no rm -rf of anything, per task rules)

    # ---- the real source is the only truth (R-03: fail loud, never skip) ----
    def _slice(self):
        lines = (REPO / "CMakeLists.txt").read_text(encoding="utf-8").splitlines()
        starts = [i for i, l in enumerate(lines) if self._START.match(l)]
        ends = [i for i, l in enumerate(lines) if self._END.match(l)]
        if len(starts) != 1:
            self.fail("START anchor ^option(FC_VERSION_FREEZE must hit exactly 1 line, got %d" % len(starts))
        if len(ends) != 1:
            self.fail("END anchor must hit exactly 1 line, got %d" % len(ends))
        return lines[starts[0]:ends[0] + 1]

    def _fixture(self, name, header_value="1.0.73", fcversion=None, with_header=True):
        """LANGUAGES-NONE fixture inside self.tmp; the real repo stays untouched."""
        fx = self.tmp / name
        fx.mkdir(parents=True, exist_ok=True)
        sl = self._slice()
        joined = "\n".join(sl)
        # the slice must carry every structural element the freeze relies on
        for needle in ("option(FC_VERSION_FREEZE",
                       "set(FC_VERSION_HEADER",
                       "set(FC_VERSION_COUNTER_FILE",
                       "function(fc_split_version",
                       "if(FC_VERSION_FREEZE)",
                       "else()",
                       "endif()",
                       "set(FC_NEW_VERSION",
                       "set(FASTCLONE_VERSION",
                       'message(STATUS "FastClone version: ${FASTCLONE_VERSION}")'):
            self.assertIn(needle, joined)
        self.assertEqual(joined.count("if(FC_VERSION_FREEZE)"), 1)
        n_header = n_counter = 0
        out = []
        for line in sl:
            if self._HEADER_LINE.match(line):
                out.append('set(FC_VERSION_HEADER "%s")'
                           % (fx / "version.h").as_posix())
                n_header += 1
            elif self._COUNTER_LINE.match(line):
                out.append('set(FC_VERSION_COUNTER_FILE "%s")'
                           % (fx / ".fcversion").as_posix())
                n_counter += 1
            else:
                out.append(line)
        self.assertEqual(n_header, 1, "FC_VERSION_HEADER repointed exactly once")
        self.assertEqual(n_counter, 1, "FC_VERSION_COUNTER_FILE repointed exactly once")
        (fx / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(FreezeFixture LANGUAGES NONE)\n"
            + "\n".join(out) + "\n", encoding="utf-8")
        if with_header:
            (fx / "version.h").write_text(
                "namespace fc {\n"
                "\n"
                "constexpr const char* kFastCloneVersion = \"" + header_value + "\";\n"
                "\n"
                "}  // namespace fc\n", encoding="utf-8")
        if fcversion is not None:
            (fx / ".fcversion").write_text(fcversion, encoding="utf-8")
        return fx

    def _cmake(self):
        exe = os.environ.get("FC_CMAKE") or shutil.which("cmake")
        if not exe:
            self.fail("cmake not found: set FC_CMAKE or put cmake on PATH (17-design §3.5)")
        return exe

    def _configure(self, fx, flags, build="build"):
        return subprocess.run(
            [self._cmake(), "-S", str(fx), "-B", str(fx / build), *flags],
            capture_output=True, text=True, timeout=120)

    @staticmethod
    def _version_line(proc):
        m = re.search(r"FastClone version: (\S+)", proc.stdout or "")
        return m.group(1) if m else None

    # V-01 (FR-17 / AC-01 local): default OFF keeps the counter cadence verbatim
    # (.fcversion base=1.0.0 counter=5 -> 1.0.6, counter written back 6, header
    # rewritten by the legacy block, byte-for-byte as before this change).
    def test_v01_off_default_keeps_counter(self):
        fx = self._fixture("v01", header_value="1.0.5",
                           fcversion="base=1.0.0\ncounter=5\n")
        proc = self._configure(fx, [])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self._version_line(proc), "1.0.6")
        self.assertIn("counter=6 from .fcversion", proc.stdout)
        self.assertEqual((fx / ".fcversion").read_text(encoding="utf-8"),
                         "base=1.0.0\ncounter=6\n")
        header = (fx / "version.h").read_text(encoding="utf-8")
        self.assertIn('kFastCloneVersion = "1.0.6"', header)
        self.assertNotIn("1.0.5", header)

    # V-02 (FR-10 / FR-16): all 4 real configure entries carry the freeze flag
    # (build-test x2, windows-arm64 x1, sanitizers x1); no package step does.
    # Counted on the comment-stripped view so a `#` line can never fake a flag.
    def test_v02_static_four_configure_flags(self):
        text = (REPO / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        lines = text.splitlines()
        code = "\n".join(
            re.sub(r"(^|\s)#.*$", "", l).rstrip() for l in lines)

        # job keys are the 2-space-indented `name:` lines under jobs: (the
        # same inventory shape test_job_inventory_unchanged pins down; the
        # on:/pull_request:/push: trigger keys must not count) — split
        # exactly per job, never past the next job key.
        body_start = next(i for i, l in enumerate(lines) if l.rstrip() == "jobs:")
        job_keys = [i for i, l in enumerate(lines) if i > body_start
                    and re.match(r"^  [A-Za-z0-9_-]+:$", l)]
        self.assertEqual(len(job_keys), 3)

        def block(job):
            start = next(i for i in job_keys if lines[i].rstrip() == "  %s:" % job)
            end = next((j for j in job_keys if j > start), len(lines))
            return "\n".join(re.sub(r"(^|\s)#.*$", "", l) for l in lines[start:end])

        self.assertEqual(code.count("-DFC_VERSION_FREEZE=ON"), 4)
        self.assertEqual(block("build-test").count("-DFC_VERSION_FREEZE=ON"), 2)
        self.assertEqual(block("windows-arm64").count("-DFC_VERSION_FREEZE=ON"), 1)
        self.assertEqual(block("sanitizers").count("-DFC_VERSION_FREEZE=ON"), 1)
        pkg_lines = [l for l in code.splitlines() if "package_ci.py" in l]
        self.assertTrue(pkg_lines, "package step lines expected")
        for l in pkg_lines:
            self.assertNotIn("-DFC_VERSION_FREEZE=ON", l)

    # V-03 (FR-11/FR-12, AC-10/AC-11): ON + clean checkout -> the committed
    # value verbatim (1.0.73 in CI); no .fcversion appears, header untouched.
    def test_v03_on_clean_freezes_to_committed(self):
        fx = self._fixture("v03", header_value="1.0.73")
        before = (fx / "version.h").read_bytes()
        proc = self._configure(fx, ["-DFC_VERSION_FREEZE=ON"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self._version_line(proc), "1.0.73")
        self.assertIn("frozen 1.0.73 from committed version.h", proc.stdout)
        self.assertFalse((fx / ".fcversion").exists())
        self.assertEqual((fx / "version.h").read_bytes(), before)

    # V-04 (FR-11, AC-12): ON ignores a residual .fcversion (base != header,
    # counter arbitrary); the counter file is never read, bumped or rewritten.
    def test_v04_on_ignores_residual_fcversion(self):
        residual = "base=1.0.0\ncounter=5\n"
        fx = self._fixture("v04", header_value="1.0.73", fcversion=residual)
        before = (fx / ".fcversion").read_bytes()
        proc = self._configure(fx, ["-DFC_VERSION_FREEZE=ON"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self._version_line(proc), "1.0.73")
        self.assertEqual((fx / ".fcversion").read_bytes(), before)
        self.assertNotIn("falling back", proc.stdout + proc.stderr)

    # V-05 (FR-12, AC-11): ON leaves every pre-existing file byte-identical and
    # creates nothing outside the build tree.
    def test_v05_on_writes_no_new_files(self):
        fx = self._fixture("v05", header_value="1.0.73",
                           fcversion="base=1.0.0\ncounter=5\n")

        def snap():
            out = {}
            for p in sorted(fx.rglob("*")):
                rel = p.relative_to(fx)
                if "build" in rel.parts:
                    continue
                out[str(rel)] = p.read_bytes() if p.is_file() else "<dir>"
            return out

        before = snap()
        proc = self._configure(fx, ["-DFC_VERSION_FREEZE=ON"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self._version_line(proc), "1.0.73")
        self.assertEqual(snap(), before)

    # V-06 (FR-13, AC-13): ON + missing header -> deterministic FATAL; never
    # the 1.0.0 WARNING fallback, no .fcversion, configure never completes.
    def test_v06_on_missing_header_fatals(self):
        fx = self._fixture("v06", with_header=False)
        proc = self._configure(fx, ["-DFC_VERSION_FREEZE=ON"])
        self.assertNotEqual(proc.returncode, 0)
        combined = proc.stdout + proc.stderr
        self.assertIn("FC_VERSION_FREEZE=ON: version header not found", combined)
        self.assertNotIn("falling back", combined)
        self.assertIsNone(self._version_line(proc))
        self.assertFalse((fx / ".fcversion").exists())

    # V-07 (FR-13): ON rejects any non-quoted-MAJOR.MINOR.PATCH value — the
    # digit group is anchored to the closing quote (17 §3.1 D-03: one regex).
    def test_v07_on_invalid_version_strings_fatal(self):
        for bad in ("1.0.73-rc1", "1.0", "v1.0.73"):
            with self.subTest(bad=bad):
                fx = self._fixture("v07_%s" % bad.replace(".", "_"), header_value=bad)
                proc = self._configure(fx, ["-DFC_VERSION_FREEZE=ON"])
                self.assertNotEqual(proc.returncode, 0)
                combined = proc.stdout + proc.stderr
                self.assertIn("kFastCloneVersion must be a quoted MAJOR.MINOR.PATCH",
                              combined)
                self.assertNotIn("falling back", combined)
                self.assertIsNone(self._version_line(proc))
                self.assertFalse((fx / ".fcversion").exists())

    # V-08 (FR-09/FR-17): (a) explicit OFF == no flag at all (no bool
    # distortion) and the option statically defaults OFF; (b) clean + default
    # OFF + FC_GIT_EXE= short-circuits find_program -> the ORIGINAL 1.0.0
    # WARNING fallback + counter=1 + .fcversion written, kept verbatim (O-1).
    def test_v08_off_flag_equals_default_plus_clean_fallback(self):
        results = []
        for name, flags in (("v08a", []), ("v08b", ["-DFC_VERSION_FREEZE=OFF"])):
            fx = self._fixture(name, header_value="1.0.5",
                               fcversion="base=1.0.0\ncounter=5\n")
            proc = self._configure(fx, flags)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            results.append((self._version_line(proc),
                            (fx / ".fcversion").read_text(encoding="utf-8"),
                            (fx / "version.h").read_text(encoding="utf-8")))
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0][0], "1.0.6")
        self.assertEqual(results[0][1], "base=1.0.0\ncounter=6\n")
        src = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(
            len(re.findall(r'^option\(FC_VERSION_FREEZE[^\n]*OFF\)$', src, re.M)), 1)

        fx = self._fixture("v08c", header_value="1.0.5")  # clean: no .fcversion
        proc = self._configure(fx, ["-DFC_GIT_EXE="])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        combined = proc.stdout + proc.stderr
        # CMake wraps long message() text; compare on the whitespace-normalized
        # view so the verbatim fallback wording is asserted robustly.
        flat = re.sub(r"\s+", " ", combined)
        self.assertIn("falling back to base=1.0.0", flat)
        self.assertEqual(self._version_line(proc), "1.0.1")
        self.assertEqual((fx / ".fcversion").read_text(encoding="utf-8"),
                         "base=1.0.0\ncounter=1\n")
        self.assertIn('kFastCloneVersion = "1.0.1"',
                      (fx / "version.h").read_text(encoding="utf-8"))

    # V-09 (AC-10 stability): two ON configures against the same committed
    # header never accumulate (both 1.0.73; the old P0-2 double-accumulate mode
    # is structurally unreachable under freeze).
    def test_v09_on_repeat_does_not_accumulate(self):
        fx = self._fixture("v09", header_value="1.0.73")
        before = (fx / "version.h").read_bytes()
        for build in ("build1", "build2"):
            proc = self._configure(fx, ["-DFC_VERSION_FREEZE=ON"], build=build)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(self._version_line(proc), "1.0.73", build)
        self.assertEqual((fx / "version.h").read_bytes(), before)
        self.assertFalse((fx / ".fcversion").exists())

    # V-10 (AC-14..16) is REMOTE-PENDING by definition: real 4-artifact
    # download, 6/6 jobs, Draft rel_1.0.73 with the 2 old assets kept + notes,
    # no git ref (rel_ never matches the v* trigger), unpublished. Locally we
    # assert only the static contract that evidence depends on: the unchanged
    # helper still enforces the native triple equality (exit 5) and the cross
    # header-only / on_target=false / ARM64-no-exec note.
    def test_v10_remote_evidence_plan_pending(self):
        helper = (REPO / "scripts" / "package_ci.py").read_text(encoding="utf-8")
        self.assertIn('_fail(EXIT_VERSION, "version_mismatch"', helper)
        self.assertIn('version_source = "binary_version_output"', helper)
        self.assertIn('version_source = "configure_generated_version_h"', helper)
        self.assertIn('"on_target": kind == "native"', helper)
        self.assertIn("未在 ARM64 上运行", helper)
        rel = (REPO / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        self.assertIn("tags: ['v*']", rel)

    # V-11 (R-01 guard): the REAL working-tree header stays the authorized
    # 1.0.73 throughout this round — never bumped to 74 and restored, which
    # would fake a committed value different from the one being shipped.
    def test_v11_real_header_still_1_0_73(self):
        raw = (REPO / "FastClone" / "version.h").read_text(encoding="utf-8")
        m = re.search(r'kFastCloneVersion[ \t]*=[ \t]*"([^"]+)"', raw)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), "1.0.73")


import platform  # noqa: E402  (late import keeps the top import list minimal)


if __name__ == "__main__":
    unittest.main()
