#!/usr/bin/env python3
"""FastClone CI packaging helper.

Frozen implementation basis: docs/tasks/ci-multiplatform-packaging/
  - 02-design.md (A1..A7 trunk, exit codes)
  - 04-design-v2.md (C-1..C-6, D-09..D-13, full text frozen)
  - 05-gate-design-v2.md §4 (frozen overlay, overrides descriptive wording)

Stdlib only (zero third-party deps). Subprocesses always run via Runner with an
explicit timeout; argv lists, never a shell string. Exit codes (frozen):
  0 ok | 2 usage/san/cross+tests=passed/missing_env/bad_output_value
  3 stage whitelist violation | 4 arch (unknown/mismatch/fat/macho32/differ/
    cross_binary_matches_host) | 5 version mismatch | 6 hash/member/traversal
  7 IO/collision/timeout/subprocess failure.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import platform
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

SCHEMA = "fastclone.provenance/1"

# ---------------------------------------------------------------- exit codes
EXIT_OK = 0
EXIT_USAGE = 2        # usage, sanitizer, cross+tests=passed, missing_env, bad_output_value
EXIT_WHITELIST = 3    # stage whitelist missing/extra
EXIT_ARCH = 4         # arch unknown/mismatch/unsupported_fat_binary/unsupported_macho32/
                      # binaries_differ/cross_binary_matches_host
EXIT_VERSION = 5      # version mismatch
EXIT_HASH = 6         # hash/unpack/member count/traversal
EXIT_IO = 7           # IO, output collision, timeout, subprocess failure


class PackageError(Exception):
    def __init__(self, code: int, reason: str, detail: str = ""):
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.code = code
        self.reason = reason
        self.detail = detail


def _fail(code: int, reason: str, detail: str = "") -> None:
    raise PackageError(code, reason, detail)


# ---------------------------------------------------------------- Runner
class Runner:
    """Subprocess seam. run(argv, timeout) -> (rc, stdout, stderr).

    May raise subprocess.TimeoutExpired / OSError; callers map those to exit 7.
    Unit tests inject fakes here: no fixture binary is ever executed."""

    def run(self, argv, timeout):
        p = subprocess.run(
            [str(a) for a in argv], capture_output=True, timeout=timeout, check=False
        )
        return p.returncode, p.stdout, p.stderr


SUBPROCESS_RUNNER = Runner()  # default seam


# ---------------------------------------------------------------- arch canon
_X64_ALIASES = {"x64", "amd64", "x86_64", "x86-64"}
_ARM64_ALIASES = {"arm64", "aarch64"}


def norm_arch(value):
    """Canonicalize {x64,arm64} alias set; unknown -> None (never a guess)."""
    if value is None:
        return None
    v = str(value).strip().lower()
    if v in _X64_ALIASES:
        return "x64"
    if v in _ARM64_ALIASES:
        return "arm64"
    return None


PE_MACHINE = {0x8664: "x64", 0xAA64: "arm64"}
ELF_MACHINE = {0x3E: "x64", 0xB7: "arm64"}
MACHO_CPUTYPE = {0x01000007: "x64", 0x0100000C: "arm64"}
MACHO_FAT_MAGICS = (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf")
MACHO32_MAGICS = (b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce")
MACHO64_LE = b"\xcf\xfa\xed\xfe"
MACHO64_BE = b"\xfe\xed\xfa\xcf"


def _u16le(b, off):
    return struct.unpack_from("<H", b, off)[0]


def _u32le(b, off):
    return struct.unpack_from("<I", b, off)[0]


def parse_pe(data: bytes):
    """PE: MZ, e_lfanew bounds-checked, PE\\0\\0 signature, Machine@e_lfanew+4."""
    if len(data) < 0x40:
        return None, "pe_too_short"
    if data[0:2] != b"MZ":
        return None, "pe_bad_mz"
    e_lfanew = _u32le(data, 0x3C)
    if not (0x40 <= e_lfanew <= len(data) - 24):
        return None, "pe_bad_lfanew"
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        return None, "pe_bad_signature"
    machine = _u16le(data, e_lfanew + 4)
    arch = PE_MACHINE.get(machine)
    return {
        "format": "PE", "raw": f"0x{machine:04X}", "pemachine": f"0x{machine:04X}",
        "endianness": "LE", "parse_status": "ok" if arch else "unknown",
        "arch": arch,
    }, None


def parse_elf(data: bytes):
    """ELF: magic, EI_CLASS=2 (64-bit), EI_DATA 1=LE/2=BE, e_machine@18."""
    if len(data) < 20:
        return None, "elf_too_short"
    if data[0:4] != b"\x7fELF":
        return None, "elf_bad_magic"
    ei_class = data[4]
    ei_data = data[5]
    if ei_class != 2:
        return None, "unsupported_elf32" if ei_class == 1 else "elf_bad_class"
    if ei_data not in (1, 2):
        return None, "elf_bad_data"
    endianness = "LE" if ei_data == 1 else "BE"
    fmt = "<H" if ei_data == 1 else ">H"
    machine = struct.unpack_from(fmt, data, 18)[0]
    arch = ELF_MACHINE.get(machine)
    return {
        "format": "ELF", "raw": f"0x{machine:04X}", "elfmachine": f"0x{machine:04X}",
        "endianness": endianness, "parse_status": "ok" if arch else "unknown",
        "arch": arch,
    }, None


def parse_macho(data: bytes):
    """Mach-O: thin 64-bit LE/BE variants; 32-bit and fat/universal rejected."""
    if len(data) < 16:
        return None, "macho_too_short"
    magic = data[0:4]
    if magic in MACHO_FAT_MAGICS:
        return None, "unsupported_fat_binary"
    if magic in MACHO32_MAGICS:
        return None, "unsupported_macho32"
    if magic == MACHO64_LE:
        endianness, fmt = "LE", "<"
    elif magic == MACHO64_BE:
        endianness, fmt = "BE", ">"
    else:
        return None, "macho_bad_magic"
    cputype = struct.unpack_from(fmt + "I", data, 4)[0]
    arch = MACHO_CPUTYPE.get(cputype)
    return {
        "format": "Mach-O", "raw": f"0x{cputype:08X}", "machocputype": f"0x{cputype:08X}",
        "endianness": endianness, "parse_status": "ok" if arch else "unknown",
        "arch": arch,
    }, None


_PARSERS = (parse_pe, parse_elf, parse_macho)


def read_binary_evidence(path: Path):
    """Parse one binary's header. Dispatch on magic bytes; a recognized-but-
    broken structure (bad lfanew, fat, macho32...) is a hard exit 4; a blob
    matching no known format is arch_unknown (also exit 4, never guessed)."""
    try:
        data = path.read_bytes()
    except OSError as e:
        _fail(EXIT_IO, "io_error", f"read {path}: {e.__class__.__name__}")
    if len(data) < 4:
        _fail(EXIT_ARCH, "arch_unknown", f"too short: {path}")
    head = data[0:4]
    if data[0:2] == b"MZ":
        ev, err = parse_pe(data)
    elif head == b"\x7fELF":
        ev, err = parse_elf(data)
    elif head in MACHO_FAT_MAGICS:
        _fail(EXIT_ARCH, "unsupported_fat_binary", str(path))
    elif head in MACHO32_MAGICS:
        _fail(EXIT_ARCH, "unsupported_macho32", str(path))
    elif head in (MACHO64_LE, MACHO64_BE):
        ev, err = parse_macho(data)
    else:
        _fail(EXIT_ARCH, "arch_unknown", f"no known header format: {path}")
    if err is not None:
        _fail(EXIT_ARCH, err, f"{path}")
    return ev


# ---------------------------------------------------------------- sanitizers
RE_OUTPUT_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
RE_ASCII_TOKEN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
RE_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
RE_REF = re.compile(r"^refs/(heads|pull|tags)/[A-Za-z0-9._/-]+$")
RE_RUN_TOKEN = re.compile(r"^[A-Za-z0-9._-]+$")
RE_HEADER_VERSION = re.compile(r'kFastCloneVersion\s*=\s*"([^"]+)"')
RE_VERSION_OUTPUT = re.compile(r"^(FastClone|FastCheck) (\d+\.\d+\.\d+)$")
REQUIRED_ENV = ("RUNNER_TEMP", "RUNNER_OS", "RUNNER_ARCH", "GITHUB_SHA", "GITHUB_RUN_ID")
PLATFORMS = ("windows", "linux", "macos")
PLATFORM_EXTS = {"windows": "zip", "linux": "tar.gz", "macos": "tar.gz"}
WIN_CRT_DLLS = ["MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "UCRT"]


def _bad_value(name, value, code=EXIT_USAGE, reason="bad_output_value"):
    _fail(code, reason, f"{name}={value!r}")


def check_no_newline_nul(name, value):
    if value is None:
        return value
    if "\r" in value or "\n" in value or "\0" in value:
        _bad_value(name, value)
    return value


def check_ascii_token(name, value):
    if value is None or not RE_ASCII_TOKEN.match(value) or "/" in value or ".." in value:
        _bad_value(name, value)
    return value


def posix_path(p: Path) -> str:
    """Native path -> forward-slash string (paths keep spaces/Unicode; never
    forced to ASCII — 05-gate §4.4 G2)."""
    return str(p).replace("\\", "/")


# ---------------------------------------------------------------- cache/meta
def parse_cmake_cache(path: Path):
    cache = {}
    if not path.is_file():
        return None
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^([A-Za-z0-9_]+):([A-Za-z]+)=(.*)$", line)
            if m:
                cache[m.group(1)] = m.group(3)
    except OSError:
        return None
    return cache


def _compiler_facts(build_dir: Path, cache):
    """CMAKE_CXX_COMPILER_ID/VERSION live in CMakeFiles/*/CMakeCXXCompiler.cmake
    (not in the cache); read them there, else None — recorded as-is."""
    cid = cache.get("CMAKE_CXX_COMPILER_ID") if cache else None
    cver = cache.get("CMAKE_CXX_COMPILER_VERSION") if cache else None
    if cid is None or cver is None:
        for f in sorted(build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
            try:
                text = f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            m = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]+)"\)', text)
            if m and cid is None:
                cid = m.group(1)
            m = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"\)', text)
            if m and cver is None:
                cver = m.group(1)
            break
    return cid, cver


def derive_version_header(build_dir: Path, cache):
    """Default header = <CMAKE_HOME_DIRECTORY>/FastClone/version.h, i.e. the
    configure-rewritten value (FC_NEW_VERSION). Never the raw working-tree
    value by assumption, never `git show HEAD`, never release.yml drift guard."""
    home = (cache or {}).get("CMAKE_HOME_DIRECTORY")
    if not home:
        return None
    return Path(home) / "FastClone" / "version.h"


def header_version(header: Path):
    try:
        text = header.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        _fail(EXIT_VERSION, "version_header_unreadable", f"{header}: {e.__class__.__name__}")
    m = RE_HEADER_VERSION.search(text)
    if not m or not RE_VERSION.match(m.group(1)):
        _fail(EXIT_VERSION, "version_header_unparseable", str(header))
    return m.group(1)


def binary_version(exe: Path, expect_prefix: str, runner: Runner):
    """Run `--version` through the Runner seam only (never a shell)."""
    rc, out, err = _run(runner, [str(exe), "--version"], 30, "version")
    if rc != 0:
        _fail(EXIT_VERSION, "version_binary_rc", f"{exe.name} rc={rc}")
    text = out.decode("utf-8", "replace").strip()
    for line in (l.strip() for l in text.splitlines()):
        m = RE_VERSION_OUTPUT.match(line)
        if m:
            if m.group(1) != expect_prefix:
                _fail(EXIT_VERSION, "version_mismatch",
                      f"{exe.name} reported as {m.group(1)}")
            return m.group(2)
    _fail(EXIT_VERSION, "version_binary_unparseable", f"{exe.name}: {text[:80]!r}")


# ---------------------------------------------------------------- subprocess
def _run(runner: Runner, argv, timeout, stage):
    try:
        return runner.run(argv, timeout)
    except subprocess.TimeoutExpired:
        _fail(EXIT_IO, "timeout", f"stage={stage} timeout={timeout}s argv0={argv[0]}")
    except OSError as e:
        _fail(EXIT_IO, "io_error", f"stage={stage} reason={e.__class__.__name__}")


# ---------------------------------------------------------------- fs helpers
def fsynced_write(target: Path, payload: bytes):
    """*.part -> fsync -> os.replace (D-13). Partial never visible at target."""
    part = target.with_name(target.name + ".part")
    with open(part, "wb") as f:
        f.write(payload)
        f.flush()
        os.fsync(f.fileno())
    os.replace(part, target)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- packaging
def stage_bin_names(platform_str):
    suffix = ".exe" if platform_str == "windows" else ""
    return f"FastClone{suffix}", f"FastCheck{suffix}"


def whitelist_stage(stage_dir: Path, platform_str):
    """A2: stage/bin must hold EXACTLY the two production targets (exit 3)."""
    bin_dir = stage_dir / "bin"
    want = set(stage_bin_names(platform_str))
    try:
        have = {p.name for p in bin_dir.iterdir()}
    except OSError:
        _fail(EXIT_WHITELIST, "stage_whitelist", "stage/bin missing")
    if have != want:
        _fail(EXIT_WHITELIST, "stage_whitelist",
              f"want={sorted(want)} have={sorted(have)}")
    return bin_dir


def _bad_member(name: str):
    _fail(EXIT_HASH, "archive_member", f"illegal member {name!r}")


def check_member_names(names, platform_str):
    """Frozen member contract: exactly 3; no absolute/`..`/backslash/leading
    slash/duplicates (exit 6)."""
    fc, fchk = stage_bin_names(platform_str)
    allowed = {f"bin/{fc}", f"bin/{fchk}", "provenance.json"}
    seen = set()
    for n in names:
        if not isinstance(n, str) or not n:
            _bad_member(n)
        if n.startswith("/") or n.startswith("\\") or ":" in n or ".." in n or "\\" in n:
            _bad_member(n)
        if n in seen:
            _fail(EXIT_HASH, "archive_member", f"duplicate member {n!r}")
        seen.add(n)
    if set(names) != allowed:
        _fail(EXIT_HASH, "archive_member",
              f"members must be exactly {sorted(allowed)}, got {sorted(names)}")


def build_archive(archive_path: Path, bin_dir: Path, fc_name: str,
                  fchk_name: str, inarchive_provenance: bytes, platform_str):
    """A3: Windows zip / POSIX tar.gz; fixed members bin/<exe> + provenance.json;
    exec bits written explicitly (0o755), never inherited from the fs."""
    if platform_str == "windows":
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for name in (fc_name, fchk_name):
                zi = zipfile.ZipInfo(f"bin/{name}", date_time=(1980, 1, 1, 0, 0, 0))
                zi.external_attr = 0o755 << 16
                zf.writestr(zi, (bin_dir / name).read_bytes())
            zi = zipfile.ZipInfo("provenance.json", date_time=(1980, 1, 1, 0, 0, 0))
            zi.external_attr = 0o644 << 16
            zf.writestr(zi, inarchive_provenance)
    else:
        with tarfile.open(archive_path, "w:gz") as tf:
            for name, mode in ((fc_name, 0o755), (fchk_name, 0o755),
                               ("provenance.json", 0o644)):
                ti = tarfile.TarInfo(f"bin/{name}" if name != "provenance.json"
                                     else "provenance.json")
                ti.mode = mode
                ti.mtime = 0
                ti.size = (bin_dir / name).stat().st_size \
                    if name != "provenance.json" else len(inarchive_provenance)
                payload = (bin_dir / name).read_bytes() \
                    if name != "provenance.json" else inarchive_provenance
                tf.addfile(ti, io.BytesIO(payload))


def read_archive(archive_path: Path, platform_str):
    """-> (members: dict name->bytes, mode_of(name)). Raises PackageError(6)."""
    members = {}
    modes = {}
    if platform_str == "windows":
        try:
            with zipfile.ZipFile(archive_path) as zf:
                for zi in zf.infolist():
                    if zi.is_dir():
                        _bad_member(zi.filename)
                    members[zi.filename] = zf.read(zi)
                    modes[zi.filename] = (zi.external_attr >> 16) & 0o777
        except zipfile.BadZipFile:
            _fail(EXIT_HASH, "archive_corrupt", str(archive_path))
    else:
        try:
            with tarfile.open(archive_path, "r:gz") as tf:
                for ti in tf:
                    if not ti.isfile():
                        _bad_member(ti.name)
                    f = tf.extractfile(ti)
                    members[ti.name] = f.read() if f else b""
                    modes[ti.name] = ti.mode & 0o7777
        except tarfile.TarError:
            _fail(EXIT_HASH, "archive_corrupt", str(archive_path))
    check_member_names(list(members), platform_str)
    return members, modes


# ---------------------------------------------------------------- probes
def probe_dependencies(platform_str, exe: Path, runner: Runner):
    """A6/D-07: real tools only; missing/failed -> unknown + limitation, never
    a static/portable claim. No tool output is fabricated. A missing probe tool
    (OSError/FileNotFoundError) is 'unknown' and does NOT fail packaging (R-02);
    a probe timeout remains a hard exit 7 (04 §3.5)."""
    tool = {"windows": "dumpbin /dependents",
            "linux": "readelf -d", "macos": "otool -L"}[platform_str]
    argv = {"windows": ["dumpbin", "/dependents", str(exe)],
            "linux": ["readelf", "-d", str(exe)],
            "macos": ["otool", "-L", str(exe)]}[platform_str]

    def _unknown():
        return {"probe_tool": tool, "probe_status": "unknown", "libraries": None,
                "min_system_tool": None, "min_system": None}

    try:
        rc, out, err = _run(runner, argv, 60, "probe")
    except PackageError as e:
        if e.reason == "io_error":  # tool missing -> unknown (R-02), never fail
            return _unknown()
        raise
    out_text = out.decode("utf-8", "replace")
    status = "ok" if rc == 0 and out_text.strip() else "unknown"
    libraries = None
    if status == "ok":
        if platform_str == "windows":
            libraries = sorted(set(re.findall(r"(\w+\.dll)", out_text, re.I)))
        elif platform_str == "linux":
            libraries = sorted(set(re.findall(r"NEEDED\s*\[([^\]]+)\]", out_text)))
        else:
            lines = [l.strip().split(" ")[0] for l in out_text.splitlines()
                     if l.startswith("\t") or "/usr/" in l]
            libraries = sorted(set(x for x in lines if x and x != exe.name))
    min_tool = min_val = None
    if status != "unknown":  # only probe min-system when the toolchain exists
        if platform_str == "linux":
            min_tool = "getconf GNU_LIBC_VERSION"
            try:
                rc2, out2, _ = _run(runner, ["getconf", "GNU_LIBC_VERSION"], 60, "probe")
                if rc2 != 0 or not out2.decode("utf-8", "replace").strip():
                    rc2, out2, _ = _run(runner, ["ldd", "--version"], 60, "probe")
                    min_tool = "ldd --version"
                min_val = out2.decode("utf-8", "replace").strip().splitlines()[0] \
                    if rc2 == 0 and out2.strip() else None
            except PackageError as e:
                if e.reason != "io_error":
                    raise
        elif platform_str == "macos":
            min_tool = "sw_vers -productVersion + otool -l (minos)"
            try:
                rc2, out2, _ = _run(runner, ["sw_vers", "-productVersion"], 60, "probe")
                rc3, out3, _ = _run(runner, ["otool", "-l", str(exe)], 60, "probe")
                sw = out2.decode("utf-8", "replace").strip() if rc2 == 0 else ""
                minos = " ".join(re.findall(r"minos (\d+\.\d+)",
                                            out3.decode("utf-8", "replace")))
                min_val = f"sw_vers={sw} minos={minos}".strip() or None
            except PackageError as e:
                if e.reason != "io_error":
                    raise
    else:
        min_tool = min_val = None
    return {"probe_tool": tool, "probe_status": status, "libraries": libraries,
            "min_system_tool": min_tool, "min_system": min_val}


def actually_static_from(libraries):
    """Static only when real imports are absent; a /MT flag alone never proves
    it. Unknown -> None (recorded as-is)."""
    if libraries is None:
        return None
    crt_markers = ("MSVCP", "VCRUNTIME", "ucrtbase", "api-ms-crt")
    if not libraries:
        return True
    return not any(m in l for l in libraries for m in crt_markers)


def runtime_requirements(platform_str, kind, libraries):
    if platform_str != "windows":
        return {"vcredist_required": None, "portable_static": None,
                "crt": None, "actually_static": None}
    act = actually_static_from(libraries)
    if kind == "native":
        # x64 job keeps the default /MD: no portable claim, vcredist required.
        return {"vcredist_required": True, "portable_static": False,
                "crt": list(WIN_CRT_DLLS), "actually_static": act}
    # ARM64 cross job compiles with /MT, but "static" is claimed only from the
    # parsed import table, never from the flag itself.
    return {"vcredist_required": False,
            "portable_static": act, "crt": "static(/MT)", "actually_static": act}


# ---------------------------------------------------------------- github out
def write_github_output(path: Path, values: dict):
    """Single-line k=v appends, UTF-8. Validate everything BEFORE writing: a
    rejected value never leaves a half line (D-12)."""
    for k, v in values.items():
        if not RE_OUTPUT_KEY.match(k):
            _fail(EXIT_USAGE, "bad_output_value", f"key {k!r}")
        if v is None:
            continue
        if "\r" in v or "\n" in v or "\0" in v:
            _fail(EXIT_USAGE, "bad_output_value", f"value of {k} contains CR/LF/NUL")
    lines = "".join(f"{k}={v}\n" for k, v in values.items() if v is not None)
    with open(path, "ab") as f:
        f.write(lines.encode("utf-8"))
        f.flush()
        os.fsync(f.fileno())


# ---------------------------------------------------------------- defaults
def default_stage_prefix(env):
    return Path(env["RUNNER_TEMP"]) / "fc-stage"


def default_out_dir(env, platform_str, arch):
    job = env.get("GITHUB_JOB") or "unknown-job"
    job = RE_RUN_TOKEN.match(job) and job or "unknown-job"
    return (Path(env["RUNNER_TEMP"]) / "fc-dist" / f"run-{env['GITHUB_RUN_ID']}"
            / f"job-{job}-{platform_str}-{arch}")


def dist_glob_of(dist_dir: Path):
    return posix_path(dist_dir) + "/*"


# ---------------------------------------------------------------- verify core
def verify_dist(dist_dir: Path, platform_str=None):
    """Shared by the `verify` CLI and the in-package self check (A7). All
    mismatches exit 6. Returns the archive path on success."""
    if not dist_dir.is_dir():
        _fail(EXIT_HASH, "dist_missing", str(dist_dir))
    archives = sorted(p for p in dist_dir.iterdir()
                      if p.is_file() and p.suffix in (".zip", ".gz")
                      and p.name != "SHA256SUMS.txt" and p.name != "provenance.json"
                      and not p.name.endswith(".part"))
    if len(archives) != 1:
        _fail(EXIT_HASH, "dist_archive_count", f"{[a.name for a in archives]}")
    archive = archives[0]
    platform_str = platform_str or ("windows" if archive.suffix == ".zip" else "linux")
    sums_path = dist_dir / "SHA256SUMS.txt"
    prov_path = dist_dir / "provenance.json"
    if not sums_path.is_file() or not prov_path.is_file():
        _fail(EXIT_HASH, "dist_manifest_missing")
    sums = {}
    for line in sums_path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^([0-9a-f]{64})  (\S+)$", line)
        if not m:
            _fail(EXIT_HASH, "checksums_format", line[:100])
        sums[m.group(2)] = m.group(1)
    digest = sha256_file(archive)
    if sums.get(archive.name) != digest:
        _fail(EXIT_HASH, "checksum_mismatch", archive.name)
    top_prov = json.loads(prov_path.read_text(encoding="utf-8"))
    size_bytes = top_prov.get("package", {}).get("size_bytes")
    actual = archive.stat().st_size
    if not isinstance(size_bytes, int) or size_bytes != actual:
        _fail(EXIT_HASH, "size_mismatch", f"stated={size_bytes} actual={actual}")
    members, modes = read_archive(archive, platform_str)
    fc, fchk = stage_bin_names(platform_str)
    for m in (f"bin/{fc}", f"bin/{fchk}"):
        if modes.get(m) != 0o755:
            _fail(EXIT_HASH, "execmode", f"{m} mode={oct(modes.get(m)) if modes.get(m) is not None else None}")
    files = top_prov.get("files") or []
    by_path = {f.get("path"): f for f in files}
    for m in (f"bin/{fc}", f"bin/{fchk}"):
        f = by_path.get(m)
        if not f or not f.get("executable"):
            _fail(EXIT_HASH, "provenance_files", m)
        if sha256_bytes(members[m]) != f.get("sha256"):
            _fail(EXIT_HASH, "file_hash_mismatch", m)
    inarch = json.loads(members["provenance.json"])
    if inarch.get("product", {}).get("version") != top_prov.get("product", {}).get("version") \
            or inarch.get("platform", {}).get("target_arch") != top_prov.get("platform", {}).get("target_arch"):
        _fail(EXIT_HASH, "provenance_disagree", "in-archive vs top-level")
    return archive, digest


# ---------------------------------------------------------------- package core
def run_package(ns, runner: Runner, env) -> int:
    platform_str = ns.platform
    kind = ns.kind
    tests = ns.tests
    # --- usage-level rejections (exit 2)
    if kind == "cross" and tests == "passed":
        _fail(EXIT_USAGE, "cross_tests_conflict", "cross packaging cannot claim passed tests")
    if kind == "native" and tests == "cross-build-only":
        _fail(EXIT_USAGE, "native_tests_conflict", "native packaging requires passed tests")
    if kind == "cross" and not ns.expected_arch:
        _fail(EXIT_USAGE, "expected_arch_required", "cross kind requires --expected-arch")
    expected = norm_arch(ns.expected_arch) if ns.expected_arch else None
    if ns.expected_arch and expected is None:
        _fail(EXIT_ARCH, "arch_unknown", f"--expected-arch {ns.expected_arch!r}")
    if expected and kind == "native":
        pass  # native: --expected-arch is an assertion reference only
    for name in REQUIRED_ENV:
        if not env.get(name):
            _fail(EXIT_USAGE, "missing_env", name)

    build_dir = Path(ns.build_dir).resolve()
    cache = parse_cmake_cache(build_dir / "CMakeCache.txt")
    if cache is None:
        _fail(EXIT_IO, "io_error", f"CMakeCache.txt not found under {build_dir}")
    flags = cache.get("CMAKE_CXX_FLAGS") or ""
    if "-fsanitize" in flags:
        _fail(EXIT_USAGE, "sanitizer_build", "sanitizer builds are never packaged")

    header = Path(ns.version_header).resolve() if ns.version_header \
        else derive_version_header(build_dir, cache)
    if header is None:
        _fail(EXIT_VERSION, "version_header_unresolved",
              "no --version-header and no CMAKE_HOME_DIRECTORY in cache")

    host_machine = norm_arch(platform.machine())
    runner_arch_raw = env.get("RUNNER_ARCH")
    runner_arch = norm_arch(runner_arch_raw)
    if runner_arch is None:
        _fail(EXIT_ARCH, "arch_unknown", f"RUNNER_ARCH={runner_arch_raw!r}")

    # --- A1: install into a private mkdtemp stage
    stage_prefix = Path(ns.stage_prefix).resolve() if ns.stage_prefix \
        else default_stage_prefix(env)
    stage_prefix.mkdir(parents=True, exist_ok=True)
    stage_dir = Path(tempfile.mkdtemp(prefix="stage-", dir=stage_prefix))
    rc, out, err = _run(runner,
                        ["cmake", "--install", str(build_dir), "--prefix", str(stage_dir)],
                        600, "install")
    if rc != 0:
        _fail(EXIT_IO, "install_failed",
              (out + err).decode("utf-8", "replace")[:400])

    # --- A2: exact two-target whitelist
    bin_dir = whitelist_stage(stage_dir, platform_str)
    fc_name, fchk_name = stage_bin_names(platform_str)
    fc_path = bin_dir / fc_name
    fchk_path = bin_dir / fchk_name

    # --- C-1: binary header = the only target-arch truth
    ev_fc = read_binary_evidence(fc_path)
    ev_fchk = read_binary_evidence(fchk_path)
    arch = ev_fc["arch"]
    if arch is None:
        _fail(EXIT_ARCH, "arch_unknown", f"FastClone machine {ev_fc['raw']}")
    if ev_fchk.get("arch") != arch:
        _fail(EXIT_ARCH, "binaries_differ",
              f"FastClone={ev_fc['raw']} FastCheck={ev_fchk['raw']}")
    if kind == "native":
        if host_machine is None:
            _fail(EXIT_ARCH, "arch_unknown", f"platform.machine()={platform.machine()!r}")
        if runner_arch != host_machine or arch != runner_arch:
            _fail(EXIT_ARCH, "arch_mismatch",
                  f"header={arch} RUNNER_ARCH={runner_arch} host={host_machine}")
        if expected is not None and arch != expected:
            _fail(EXIT_ARCH, "arch_mismatch", f"--expected-arch={expected} header={arch}")
        target_arch = arch
    else:
        if expected is None:
            _fail(EXIT_ARCH, "arch_unknown", "cross without expected arch")
        target_arch = expected
        if arch != target_arch:
            _fail(EXIT_ARCH, "arch_mismatch", f"header={arch} expected={target_arch}")
        if arch == runner_arch:
            _fail(EXIT_ARCH, "cross_binary_matches_host",
                  f"host binary ({ev_fc['raw']}) cannot be labeled cross target")

    # --- C-6: version cross-check
    header_val = header_version(header)
    version_source = "binary_version_output"
    if kind == "native":
        v1 = binary_version(fc_path, "FastClone", runner)
        v2 = binary_version(fchk_path, "FastCheck", runner)
        if not (v1 == v2 == header_val):
            _fail(EXIT_VERSION, "version_mismatch",
                  f"FastClone={v1} FastCheck={v2} header={header_val}")
        version = v1
    else:
        version = header_val
        version_source = "configure_generated_version_h"

    # --- A6: build metadata + dependency probes (as-is, unknown allowed)
    cid, cver = _compiler_facts(build_dir, cache)
    rt = cache.get("CMAKE_MSVC_RUNTIME_LIBRARY")
    if platform_str == "windows":
        msvc_rt = rt or "MultiThreadedDLL(default /MD)"
    else:
        msvc_rt = rt
    probe = probe_dependencies(platform_str, fc_path, runner)
    rr = runtime_requirements(platform_str, kind, probe["libraries"])

    # --- A3/A7: dist dir, collision refusal, archive + manifests
    out_dir = Path(ns.out_dir).resolve() if ns.out_dir \
        else default_out_dir(env, platform_str, target_arch)
    if out_dir.exists():
        try:
            nonempty = any(out_dir.iterdir())
        except OSError:
            nonempty = True
        if nonempty:
            # Never delete or overwrite an existing archive/checksum/manifest.
            _fail(EXIT_IO, "output_collision", str(out_dir))
    out_dir.mkdir(parents=True, exist_ok=True)
    ext = PLATFORM_EXTS[platform_str]
    stem = f"FastClone-{version}-{platform_str}-{target_arch}"
    archive_name = f"{stem}.{ext}"
    artifact_name = stem
    archive_path = out_dir / archive_name
    if archive_path.exists():
        _fail(EXIT_IO, "output_collision", str(archive_path))

    limitations = []
    if kind == "cross":
        limitations.append("未在 ARM64 上运行（cross build-only；on_target=false）")
    if probe["probe_status"] != "ok":
        limitations.append(
            f"dependency probe {probe['probe_tool']} unavailable -> unknown; "
            "no static/portable claim made from it")
    if kind == "cross" and platform_str == "windows":
        limitations.append(
            "ARM64 artifact 形态变更（原 FastClone-windows-arm64 两裸 exe 经 "
            "upload-artifact@v4 打成 zip 容器）；仓库内 grep 零命中，外部下载方/"
            "winget/外链使用未知，不据此推断无断链；旧 draft rel_1.0.2 资产不动")

    fc_hash = sha256_file(fc_path)
    fchk_hash = sha256_file(fchk_path)
    files_meta = [
        {"path": f"bin/{fc_name}", "sha256": fc_hash, "executable": True,
         "format": ev_fc["format"], "machine": arch},
        {"path": f"bin/{fchk_name}", "sha256": fchk_hash, "executable": True,
         "format": ev_fchk["format"], "machine": arch},
    ]
    evidence = dict(ev_fc)
    evidence.pop("arch", None)

    common = {
        "schema": SCHEMA,
        "package": {
            "name": stem, "artifact_name": artifact_name, "format": ext,
            "size_bytes": None, "sha256": None,
            "compat": {"changed": True, "previous_artifact_name": "FastClone-windows-arm64",
                       "previous_form": "两裸 exe 经 upload-artifact@v4 打成 zip 容器"}
            if (kind == "cross" and platform_str == "windows") else
            {"changed": False, "previous_artifact_name": None, "previous_form": None},
        },
        "product": {"name": "FastClone", "version": version,
                    "version_source": version_source,
                    "version_header_value": header_val},
        "source": provenance_source(runner, env),
        "build": {"config": cache.get("CMAKE_BUILD_TYPE"),
                  "generator": cache.get("CMAKE_GENERATOR"),
                  "compiler": cache.get("CMAKE_CXX_COMPILER"),
                  "compiler_id": cid, "compiler_version": cver,
                  "msvc_runtime_library": msvc_rt,
                  "sanitizer": "-fsanitize" in flags, "kind": kind,
                  "target": f"{platform_str}-{target_arch}"},
        "platform": {"runner_os": env.get("RUNNER_OS"),
                     "runner_arch": runner_arch_raw,
                     "host_arch": host_machine, "target_arch": target_arch,
                     "arch_evidence": evidence},
        "execution": {"on_target": kind == "native", "tests": tests,
                      "note": "未在 ARM64 上运行" if kind == "cross" else None},
        "runtime_dependencies": probe,
        "runtime_requirements": rr,
        "files": files_meta,
        "limitations": limitations,
    }
    inarch_prov = dict(common)
    inarch_prov["limitations"] = list(limitations) + [
        "包内 provenance 副本不含包级 sha256/size_bytes（自引用物理不可能）；"
        "包级摘要见同目录 SHA256SUMS.txt 与顶层 provenance.json"]
    inarch_bytes = json.dumps(inarch_prov, ensure_ascii=False,
                              indent=2, sort_keys=True).encode("utf-8") + b"\n"

    # write order (D-13): archive -> SHA256SUMS.txt -> provenance.json (last)
    part = archive_path.with_name(archive_path.name + ".part")
    build_archive(part, bin_dir, fc_name, fchk_name, inarch_bytes, platform_str)
    with open(part, "r+b") as f:  # r+b: fsync needs a writable handle on Windows
        os.fsync(f.fileno())
    os.replace(part, archive_path)

    digest = sha256_file(archive_path)
    size = archive_path.stat().st_size
    sums_bytes = f"{digest}  {archive_name}\n".encode("utf-8")
    fsynced_write(out_dir / "SHA256SUMS.txt", sums_bytes)

    top_prov = dict(common)
    top_prov["package"] = dict(common["package"])
    top_prov["package"]["size_bytes"] = size
    top_prov["package"]["sha256"] = digest
    fsynced_write(out_dir / "provenance.json",
                  json.dumps(top_prov, ensure_ascii=False, indent=2,
                             sort_keys=True).encode("utf-8") + b"\n")

    # A7: self-verify before declaring success
    verify_dist(out_dir, platform_str)

    # outputs (validated single-line k=v; ASCII tokens only for the four
    # identifier keys, paths merely CR/LF/NUL-checked)
    dist_dir_s = posix_path(out_dir)
    dist_glob = dist_dir_s + "/*"
    outs = {
        "artifact_name": check_ascii_token("artifact_name", artifact_name),
        "archive": check_ascii_token("archive", archive_name),
        "version": check_ascii_token("version", version),
        "arch": check_ascii_token("arch", target_arch),
        "dist_dir": check_no_newline_nul("dist_dir", dist_dir_s),
        "dist_glob": check_no_newline_nul("dist_glob", dist_glob),
    }
    out_path = Path(ns.github_output).resolve() if ns.github_output \
        else (Path(env["GITHUB_OUTPUT"]) if env.get("GITHUB_OUTPUT") else None)
    if out_path is not None:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        write_github_output(out_path, outs)

    print(f"artifact_name={artifact_name} version={version} "
          f"arch={target_arch} dist_dir={dist_dir_s}")
    if ns.print_paths:
        print(f"[paths] stage={posix_path(stage_dir)} dist_dir={dist_dir_s} "
              f"dist_glob={dist_glob} version_header={posix_path(header)} "
              f"github_output={out_path if out_path is None else posix_path(out_path)}")
    return EXIT_OK


def provenance_source(runner: Runner, env):
    rc, out, err = _run(runner, ["git", "rev-parse", "HEAD"], 60, "source")
    commit = out.decode("utf-8", "replace").strip() if rc == 0 and out.strip() else None
    rc, out, err = _run(runner, ["git", "status", "--porcelain"], 60, "source")
    dirty = out.decode("utf-8", "replace") if rc == 0 and out else None

    def tok(name):
        v = env.get(name)
        return v if v is not None and RE_RUN_TOKEN.match(v) else \
            ("unknown" if v is not None else None)

    ref = env.get("GITHUB_REF")
    ref = ref if ref is not None and RE_REF.match(ref) else "unknown"
    return {"commit": commit, "dirty": dirty, "run_id": tok("GITHUB_RUN_ID"),
            "run_attempt": tok("GITHUB_RUN_ATTEMPT"), "ref": ref,
            "workflow": tok("GITHUB_WORKFLOW"), "job": tok("GITHUB_JOB")}


# ---------------------------------------------------------------- verify CLI
def run_verify(ns) -> int:
    dist_dir = Path(ns.dist_dir).resolve()
    archive, digest = verify_dist(dist_dir)
    print(f"verify ok: {archive.name} sha256={digest}")
    return EXIT_OK


# ---------------------------------------------------------------- main
def main(argv=None, runner=None, env=None):
    runner = runner or SUBPROCESS_RUNNER
    env = os.environ if env is None else env
    ap = argparse.ArgumentParser(prog="package_ci.py")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("package")
    p.add_argument("--build-dir", required=True)
    p.add_argument("--stage-prefix")
    p.add_argument("--out-dir")
    p.add_argument("--platform", required=True, choices=PLATFORMS)
    p.add_argument("--kind", required=True, choices=("native", "cross"))
    p.add_argument("--expected-arch")
    p.add_argument("--tests", required=True,
                   choices=("passed", "cross-build-only"))
    p.add_argument("--version-header")
    p.add_argument("--github-output")
    p.add_argument("--print-paths", action="store_true")

    v = sub.add_parser("verify")
    v.add_argument("--dist-dir", required=True)

    ns = ap.parse_args(argv)
    try:
        if ns.cmd == "package":
            return run_package(ns, runner, env)
        return run_verify(ns)
    except PackageError as e:
        print(f"error: exit {e.code} ({e.reason})"
              + (f" {e.detail}" if e.detail else ""), file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
