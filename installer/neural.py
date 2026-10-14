"""Neural rendering (lmxxf's HIP port of DLSS 5's neural rendering, on AMD RDNA 4): its
HIP kernels, compiled on the player's PC by the AMD driver's own compiler
(amd_comgr_3.dll, which validated them; no SDK), the recipe of lmxxf's
hip/build-modules.ps1. The network's weights aren't ours: the player points the game at
the package they downloaded from lmxxf.
"""
import concurrent.futures
import ctypes
import hashlib
import json
import os
import re
import shutil
from pathlib import Path

from .util import IS_WINDOWS, REPO, Cancelled, InstallError

# The GPUs the kernels are for: RX 9060 (gfx1200) and RX 9070 (gfx1201) series.
TARGETS = ("gfx1200", "gfx1201")

# lmxxf's source (scripts/vendor_lmxxf.py): the kernels' recipe and source, the types
# header a module includes, the shaders the runtime compiles, the licenses.
LMXXF = REPO / "thirdparty" / "lmxxf-nr"
HIP_DIR = LMXXF / "hip"
TYPES_HEADER = LMXXF / "Development" / "HIP" / "swin_persistent_types.h"
SHADERS_DIR = LMXXF / "shaders"
# The settings template lmxxf made for this runtime: it turns on the kernels' fast paths
# and lets inputs above 1920x1080 in (fitted to the network). The runtime reads it as
# default-config.txt in CONFIG_FOLDER; the game's settings override it.
FLAGS = LMXXF / "scripts" / "hip-re9-flags.txt"
# The runtime's own folder next to its DLL: its settings, and its logs (logs\). Without
# a config there it falls back to lmxxf's development folder, D:\DLSSNR-Lab.
CONFIG_FOLDER = "DLSS5-AMD"
LICENSES = {"LICENSE": "lmxxf_nr_license.txt", "hip/HIP-API-LICENSE.txt": "lmxxf_nr_hip_api_license.txt"}
# The game's folder for them (xenia's dao_upscale_neural.cc): HIP/<target>/*.hsaco
# listed in HIP/SHA256SUMS, where the runtime finds them, and shaders/. The launcher and
# the game check for HIP/SHA256SUMS (configure_view.cpp, graphics_menu.cpp).
FOLDER = "neural"
# The compiler's processes (it serializes one process's threads).
MAX_PROCESSES = 8

# A row of hip/build-modules.ps1's recipe: @{ name = '...'; defines = @('...', ...);
# sources = @('...', ...); ... }. Rows lmxxf builds with LLVM 23 (bit-exact speed-ups,
# its -RowOpts) are compiled by COMGR here, as that script does by default.
_ROW = re.compile(r"@\{\s*name\s*=\s*'([^']+)';\s*defines\s*=\s*@\(([^)]*)\);\s*sources\s*=\s*@\(([^)]*)\)")


def _system32():
    """Windows' System32 folder, as Windows reports it."""
    buffer = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetSystemDirectoryW(buffer, len(buffer))
    if not 0 < length < len(buffer):
        raise InstallError("Windows didn't say where System32 is")
    return Path(buffer.value)


def recipe(hip_dir):
    """build-modules.ps1's modules: (name, defines, sources)."""
    text = (Path(hip_dir) / "build-modules.ps1").read_text(encoding="utf-8-sig")
    modules = [(name, re.findall(r"'([^']*)'", defines), re.findall(r"'([^']*)'", sources))
               for name, defines, sources in _ROW.findall(text)]
    if not modules or len({m[0] for m in modules}) != len(modules):
        raise InstallError("hip/build-modules.ps1: its module rows weren't found")
    return modules


def module_source(name, defines, sources, hip_dir, types_header):
    """A module's source text, as build-modules.ps1 writes its .generated.hip."""
    # HIP_BARRIER_FENCE: the driver's compiler is LLVM 22 or later, which no longer waits
    # for LDS writes before gfx12's split barriers; without the fences the kernels that
    # rely on bare barriers race (whole frames of the wrong colour now and then). lmxxf
    # sets it only for the modules it builds with LLVM 23 (l23defines).
    text = "#define HIP_ISA_HALF 1\n#define HIP_BARRIER_FENCE 1\n"
    if name.endswith("-packed"):
        text += "#define HIP_PREPACKED_WEIGHTS 1\n"
    for d in defines:
        text += f"#define {d}\n"
    for part in sources:
        if part == "@swin-persistent-types":
            text += Path(types_header).read_text(encoding="utf-8").replace("#pragma once", "") + "\n"
        elif part == "@wave-owned-attention-body":
            core = (Path(hip_dir) / "wave_owned_mh.inc").read_text(encoding="utf-8")
            start, end = core.find(" // One wave owns all keys"), core.find("#define W2_KERNEL")
            if start < 0 or end <= start:
                raise InstallError("wave_owned_mh.inc: the attention body's anchors are missing")
            text += core[start:end] + "\n"
        else:
            text += (Path(hip_dir) / part).read_text(encoding="utf-8") + "\n"
    return text


class _Handle(ctypes.Structure):
    _fields_ = [("value", ctypes.c_uint64)]


class Comgr:
    """The AMD driver's code object manager (amd_comgr_3.dll in System32)."""

    # amd_comgr_data_kind_t, amd_comgr_action_kind_t, AMD_COMGR_LANGUAGE_HIP
    _SOURCE, _DIAGNOSTIC, _LOG, _EXECUTABLE = 1, 4, 5, 8
    _COMPILE_TO_BC, _CODEGEN_TO_RELOCATABLE, _LINK_TO_EXECUTABLE = 2, 4, 7
    _HIP = 3

    def __init__(self):
        if not IS_WINDOWS:
            raise InstallError("Neural rendering's kernels are compiled on Windows.")
        path = _system32() / "amd_comgr_3.dll"
        try:
            self.dll = ctypes.WinDLL(str(path))
            stat = path.stat()
        except OSError as e:
            raise InstallError(f"{path.name} isn't there (an AMD driver with HIP 7 is needed): {e}") from e
        # Which compiler this is (a driver update changes it, and with it the kernels).
        self.identity = f"{stat.st_size}:{stat.st_mtime_ns}"
        H, P = _Handle, ctypes.POINTER
        self._api = {}
        for name, args in [
            ("get_isa_count", [P(ctypes.c_size_t)]),
            ("get_isa_name", [ctypes.c_size_t, P(ctypes.c_char_p)]),
            ("create_data", [ctypes.c_int, P(H)]), ("set_data", [H, ctypes.c_size_t, ctypes.c_char_p]),
            ("set_data_name", [H, ctypes.c_char_p]), ("release_data", [H]),
            ("create_data_set", [P(H)]), ("destroy_data_set", [H]), ("data_set_add", [H, H]),
            ("create_action_info", [P(H)]), ("destroy_action_info", [H]),
            ("action_info_set_language", [H, ctypes.c_int]),
            ("action_info_set_isa_name", [H, ctypes.c_char_p]),
            ("action_info_set_option_list", [H, P(ctypes.c_char_p), ctypes.c_size_t]),
            ("action_info_set_logging", [H, ctypes.c_bool]),
            ("do_action", [ctypes.c_int, H, H, H]),
            ("action_data_count", [H, ctypes.c_int, P(ctypes.c_size_t)]),
            ("action_data_get_data", [H, ctypes.c_int, ctypes.c_size_t, P(H)]),
            ("get_data", [H, P(ctypes.c_size_t), ctypes.c_char_p]),
        ]:
            f = getattr(self.dll, "amd_comgr_" + name)
            f.argtypes, f.restype = args, ctypes.c_int
            self._api[name] = f

    def _call(self, name, *args):
        status = self._api[name](*args)
        if status:
            raise InstallError(f"COMGR {name} failed (status {status})")

    def targets(self):
        """The GPU targets it can compile for, of TARGETS."""
        count = ctypes.c_size_t()
        self._call("get_isa_count", ctypes.byref(count))
        names = []
        for i in range(count.value):
            name = ctypes.c_char_p()
            self._call("get_isa_name", i, ctypes.byref(name))
            names.append((name.value or b"").decode())
        return [t for t in TARGETS if any(t in n for n in names)]

    def _bytes(self, data_set, kind):
        count = ctypes.c_size_t()
        if self._api["action_data_count"](data_set, kind, ctypes.byref(count)):
            return []
        out = []
        for i in range(count.value):
            data = _Handle()
            self._call("action_data_get_data", data_set, kind, i, ctypes.byref(data))
            size = ctypes.c_size_t()
            self._call("get_data", data, ctypes.byref(size), None)
            buffer = ctypes.create_string_buffer(size.value)
            self._call("get_data", data, ctypes.byref(size), buffer)
            self._api["release_data"](data)
            out.append(buffer.raw[:size.value])
        return out

    def compile(self, source, target):
        """The code object (ELF) for `source` on `target`, as lmxxf's rtc_compile.exe
        builds it (-O3, no device libraries)."""
        sets = []

        def new_set():
            s = _Handle()
            self._call("create_data_set", ctypes.byref(s))
            sets.append(s)
            return s

        def options(action, opts):
            array = (ctypes.c_char_p * len(opts))(*[o.encode() for o in opts]) if opts else None
            self._call("action_info_set_option_list", action, array, len(opts))

        action = _Handle()
        self._call("create_action_info", ctypes.byref(action))
        try:
            inputs, compiled, relocatable, executable = new_set(), new_set(), new_set(), new_set()
            data = _Handle()
            self._call("create_data", self._SOURCE, ctypes.byref(data))
            text = source.encode("utf-8")
            self._call("set_data", data, len(text), text)
            self._call("set_data_name", data, b"probe.hip")
            self._call("data_set_add", inputs, data)
            self._api["release_data"](data)
            self._call("action_info_set_language", action, self._HIP)
            self._call("action_info_set_isa_name", action, f"amdgcn-amd-amdhsa--{target}".encode())
            self._call("action_info_set_logging", action, True)
            options(action, ["-O3", "-nogpuinc", "-nogpulib"])
            status = self._api["do_action"](self._COMPILE_TO_BC, action, inputs, compiled)
            result = compiled
            if not status:
                options(action, ["-O3"])
                status = self._api["do_action"](self._CODEGEN_TO_RELOCATABLE, action, compiled, relocatable)
                result = relocatable
                if not status:
                    options(action, [])
                    status = self._api["do_action"](self._LINK_TO_EXECUTABLE, action, relocatable, executable)
                    result = executable
            if status:
                log = b"\n".join(self._bytes(result, self._DIAGNOSTIC) + self._bytes(result, self._LOG))
                raise InstallError(f"COMGR couldn't compile for {target}: "
                                   f"{log.decode('utf-8', 'replace').strip()[-2000:]}")
            code = self._bytes(executable, self._EXECUTABLE)
            if len(code) != 1 or not code[0].startswith(b"\x7fELF"):
                raise InstallError("COMGR made no code object")
            return code[0]
        finally:
            self._api["destroy_action_info"](action)
            for s in sets:
                self._api["destroy_data_set"](s)


def gpu_targets():
    """The architectures of TARGETS this PC's GPUs have, as AMD's HIP runtime (part of
    its driver) reports them; none without it."""
    try:
        hip = ctypes.WinDLL(str(_system32() / "amdhip64_7.dll"))
    except (OSError, InstallError):
        return []
    # hipDeviceProp_tR0600 (1472 bytes; the buffer has room to spare) and its gcnArchName
    # (256 characters at 1160), such as "gfx1201" or "gfx1201:sramecc-:xnack-".
    size, arch_at, arch_size = 4096, 1160, 256
    count = ctypes.c_int()
    if hip.hipInit(0) or hip.hipGetDeviceCount(ctypes.byref(count)):
        return []
    found = set()
    for i in range(count.value):
        props = ctypes.create_string_buffer(size)
        if hip.hipGetDevicePropertiesR0600(props, i):
            continue
        arch = props.raw[arch_at:arch_at + arch_size].split(b"\0")[0].split(b":")[0].decode("ascii", "replace")
        if arch in TARGETS:
            found.add(arch)
    return sorted(found)


_worker_comgr = None


def _compile_job(job):
    """A compiler process's work: (source, target) -> the code object."""
    global _worker_comgr
    if _worker_comgr is None:
        _worker_comgr = Comgr()
    return _worker_comgr.compile(*job)


def build_modules(reporter, hip_dir, types_header, out_dir, targets=None):
    """Compiles the recipe's modules for `targets` (the driver's of TARGETS by default) into
    out_dir/<target>/, skipping a module whose source is unchanged since it was built
    (modules.json) with the same compiler. Returns the targets built."""
    comgr = Comgr()
    supported = comgr.targets()
    targets = [t for t in (targets or TARGETS) if t in supported]
    if not targets:
        raise InstallError("The AMD driver's compiler can't build for RDNA 4 (update the AMD driver).")
    modules = recipe(hip_dir)
    for target in targets:
        folder = Path(out_dir) / target
        folder.mkdir(parents=True, exist_ok=True)
        manifest_path = folder / "modules.json"
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            manifest = {}
        # The modules whose source changed since they were built: {name: (source, its hash)}.
        jobs = {}
        for name, defines, sources in modules:
            text = module_source(name, defines, sources, hip_dir, types_header)
            key = hashlib.sha256(f"{comgr.identity}\n{text}".encode("utf-8")).hexdigest()
            if manifest.get(name) != key or not (folder / f"{name}.hsaco").is_file():
                jobs[name] = (text, key)
        if not jobs:
            continue
        reporter.log(f"Neural rendering: compiling {len(jobs)} kernel modules for {target} (a minute or two)")

        def done(name, code):
            (folder / f"{name}.hsaco").write_bytes(code)
            manifest[name] = jobs[name][1]
            manifest_path.write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", encoding="utf-8")

        _compile_all(reporter, comgr, jobs, target, done)
    return targets


def _compile_all(reporter, comgr, jobs, target, done):
    """Compiles jobs ({name: (source, key)}) in processes, or in this one if they can't
    start, calling done(name, code) for each."""
    remaining = dict(jobs)

    def finished(name, code):
        done(name, code)
        del remaining[name]
        reporter.progress((len(jobs) - len(remaining)) / len(jobs))

    processes = min(MAX_PROCESSES, os.cpu_count() or 1, len(jobs))
    if processes > 1:
        try:
            with concurrent.futures.ProcessPoolExecutor(processes) as pool:
                futures = {pool.submit(_compile_job, (text, target)): name for name, (text, _) in jobs.items()}
                try:
                    for future in concurrent.futures.as_completed(futures):
                        reporter.check_cancel()
                        finished(futures[future], future.result())
                except BaseException:
                    pool.shutdown(wait=False, cancel_futures=True)
                    raise
        except (OSError, concurrent.futures.process.BrokenProcessPool) as e:
            reporter.log(f"Neural rendering: compiling in one process ({e})")
    for name, (text, _) in list(remaining.items()):
        reporter.check_cancel()
        finished(name, comgr.compile(text, target))


def install(reporter, built_dir, dest):
    """Puts neural rendering's kernels (compiled for this PC's RDNA 4 GPU), its shaders
    and settings in dest, and lmxxf's licenses (its DLL is in every install). Without such
    a GPU and AMD's driver it's left out (the game runs without it), and an earlier
    install's kernels and shaders are removed (the launcher would offer it, and it
    wouldn't start); if they can't be built, an earlier install's stay. built_dir keeps
    the compiled kernels between installs."""
    for source, name in LICENSES.items():
        shutil.copy2(LMXXF / source, Path(dest) / name)
    out = Path(dest) / FOLDER

    def remove_installed():
        # What this installs there, the list (the launcher's and the game's check) first;
        # anything else in the folder stays.
        (out / "HIP" / "SHA256SUMS").unlink(missing_ok=True)
        for folder in ("HIP", "shaders"):
            shutil.rmtree(out / folder, ignore_errors=True)

    if not IS_WINDOWS:
        return
    targets = gpu_targets()
    if not targets:
        reporter.log("Neural rendering not installed: no AMD RDNA 4 GPU (RX 9000 series) with AMD's driver")
        remove_installed()
        return
    try:
        targets = build_modules(reporter, HIP_DIR, TYPES_HEADER, built_dir, targets)
    except Cancelled:
        raise
    except InstallError as e:
        reporter.log(f"Neural rendering not installed: {e}"
                     + (" (the earlier install's kept)" if (out / "HIP" / "SHA256SUMS").is_file() else ""))
        return
    try:
        remove_installed()
        hip = out / "HIP"
        sums = []
        for target in targets:
            (hip / target).mkdir(parents=True)
            # The recipe's modules (not ones it no longer has).
            for name, _, _ in recipe(HIP_DIR):
                data = (Path(built_dir) / target / f"{name}.hsaco").read_bytes()
                (hip / target / f"{name}.hsaco").write_bytes(data)
                sums.append(f"{hashlib.sha256(data).hexdigest()}  {target}/{name}.hsaco")
        (out / "shaders").mkdir()
        for shader in sorted(SHADERS_DIR.glob("*.hlsl")):
            shutil.copy2(shader, out / "shaders" / shader.name)
        # The player's own settings (custom-config.txt) beside it stay.
        config = Path(dest) / CONFIG_FOLDER
        config.mkdir(exist_ok=True)
        shutil.copy2(FLAGS, config / "default-config.txt")
        # Last: the launcher and the game take the list as neural rendering installed.
        (hip / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
    except OSError as e:
        reporter.log(f"Neural rendering not installed: {e}")
        remove_installed()
        return
    reporter.log(f"Neural rendering: kernels for {', '.join(targets)} installed")
