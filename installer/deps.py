"""Everything the build downloads, pinned to exact commits and checksums."""
from dataclasses import dataclass

# The game this recompilation was made from: Dragon Age: Origins (Xbox 360),
# title 454108C0. The function and switch-table addresses in config/ belong to
# exactly this default.xex.
GAME_XEX_SHA1 = "532c0ec164f44bc8784cc0e8f6f67f72c1ed3751"
GAME_TITLE = "Dragon Age: Origins (Xbox 360, title 454108C0)"


@dataclass(frozen=True)
class GitSource:
    path: str          # relative to the repository root
    url: str
    commit: str
    # Only these paths are checked out (sparse checkout); empty = everything.
    sparse: tuple = ()
    # Submodules to check out too (all of them, at the commits the parent records).
    submodules: bool = False


XENIA_TP = "tools/xenia-canary/third_party"

GIT_SOURCES = [
    # Xenia (BSD): the vendored sources in xenia/src come from this commit; only
    # its in-tree third-party code is used from the checkout.
    GitSource("tools/xenia-canary", "https://github.com/xenia-canary/xenia-canary.git",
              "1c81b867473806b2845a1946b2d185c9d888a319",
              sparse=("LICENSE", "third_party/dxbc", "third_party/ffmpeg-xenia", "third_party/stb",
                      "third_party/microprofile", "third_party/renderdoc")),
    # Xenia's submodules that the build uses, at the commits xenia-canary records.
    GitSource(f"{XENIA_TP}/DirectX-Headers", "https://github.com/microsoft/DirectX-Headers.git",
              "33374754f65baac0500dda6187e371136357246f"),
    GitSource(f"{XENIA_TP}/DirectXShaderCompiler", "https://github.com/microsoft/DirectXShaderCompiler.git",
              "6e0fab36dc9ac8c423e1b08fed3aeb0e739d9e54",
              sparse=("LICENSE.TXT", "include/dxc", "projects/dxilconv/include")),
    GitSource(f"{XENIA_TP}/FFmpeg", "https://github.com/has207/FFmpeg.git",
              "d980192e175e6ff95bcd287af77e16fcb6597974"),
    GitSource(f"{XENIA_TP}/cxxopts", "https://github.com/jarro2783/cxxopts.git",
              "f029892dab60526184ee42ae63a7f6d19a6b0e49"),
    GitSource(f"{XENIA_TP}/date", "https://github.com/HowardHinnant/date.git",
              "f94b8f36c6180be0021876c4a397a054fe50c6f2"),
    GitSource(f"{XENIA_TP}/disruptorplus", "https://github.com/xenia-canary/disruptorplus.git",
              "bf9c70c0cc92b6435f7edec75e12751ea37e5c2f"),
    GitSource(f"{XENIA_TP}/fmt", "https://github.com/fmtlib/fmt.git",
              "407c905e45ad75fc29bf0f9bb7c5c2fd3475976f"),
    GitSource(f"{XENIA_TP}/imgui", "https://github.com/ocornut/imgui.git",
              "4806a1924ff6181180bf5e4b8b79ab4394118875"),
    GitSource(f"{XENIA_TP}/snappy", "https://github.com/google/snappy.git",
              "6af9287fbdb913f0794d0148c6aa43b58e63c8e3"),
    GitSource(f"{XENIA_TP}/tomlplusplus", "https://github.com/marzer/tomlplusplus.git",
              "708fff700f36ab3c2ab107b984ec9f3b8be5f055"),
    GitSource(f"{XENIA_TP}/utfcpp", "https://github.com/nemtrif/utfcpp.git",
              "65701fe00700577f9f4e9b96d682bd7cfcc0b76e"),
    GitSource(f"{XENIA_TP}/xbyak", "https://github.com/herumi/xbyak.git",
              "4e44f4614ddbf038f2a6296f5b906d5c72691e0f"),
    GitSource(f"{XENIA_TP}/xxhash", "https://github.com/Cyan4973/xxHash.git",
              "e626a72bc2321cd320e953a0ccf1584cad60f363"),
    # The recompiler (patched by scripts/patch_recompiler.py) and its utilities.
    GitSource("tools/XenonRecomp", "https://github.com/hedge-dev/XenonRecomp.git",
              "ddd128bcca99fe8bfbb99bea583c972351fa6ace", submodules=True),
    # SDL 3.4.18: input (incl. DualSense), audio, the launcher window.
    GitSource("thirdparty/SDL", "https://github.com/libsdl-org/SDL.git",
              "829a65d769d935c4852f8159e964312c0957260a"),
    # AMD FidelityFX SDK 2.3.0: API headers and the signed FSR 3.1/4 DLLs.
    GitSource("tools/FidelityFX-SDK", "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git",
              "60f4ea81909200d8542eca14dccb2628b763a9a3",
              sparse=("Kits/FidelityFX/docs/license.md", "Kits/FidelityFX/api/include", "Kits/FidelityFX/upscalers/include",
                      "Kits/FidelityFX/signedbin")),
]


@dataclass(frozen=True)
class Download:
    url: str
    sha256: str
    size: int
    top_dir: str = ""  # the archive's single top-level directory


LLVM_VERSION = "22.1.8"
# Oldest system LLVM accepted instead of downloading the pinned one.
LLVM_MIN_MAJOR = 18
_LLVM_RELEASE = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}"
LLVM = {
    "windows": Download(f"{_LLVM_RELEASE}/clang%2Bllvm-{LLVM_VERSION}-x86_64-pc-windows-msvc.tar.xz",
                        "d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234", 862053924,
                        f"clang+llvm-{LLVM_VERSION}-x86_64-pc-windows-msvc"),
    "linux": Download(f"{_LLVM_RELEASE}/LLVM-{LLVM_VERSION}-Linux-X64.tar.xz",
                      "df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384", 1938859476,
                      f"LLVM-{LLVM_VERSION}-Linux-X64"),
}

XWIN_VERSION = "0.10.0"
_XWIN_RELEASE = f"https://github.com/Jake-Shadle/xwin/releases/download/{XWIN_VERSION}"
XWIN = {
    "windows": Download(f"{_XWIN_RELEASE}/xwin-{XWIN_VERSION}-x86_64-pc-windows-msvc.tar.gz",
                        "96e83665ef5c1406aabd0be603a8d08e67e604f36fda630b6807ee948a614e26", 3074443,
                        f"xwin-{XWIN_VERSION}-x86_64-pc-windows-msvc"),
    "linux": Download(f"{_XWIN_RELEASE}/xwin-{XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz",
                      "d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88", 3416863,
                      f"xwin-{XWIN_VERSION}-x86_64-unknown-linux-musl"),
}
# MSVC CRT and Windows SDK versions xwin splats (from the VS 17 manifest).
MSVC_CRT_VERSION = "14.44.17.14"
WINDOWS_SDK_VERSION = "10.0.26100"
MSVC_LICENSE_URL = "https://go.microsoft.com/fwlink/?LinkId=2086102"

# Build tools installed into a private virtual environment.
PIP_PACKAGES = ["cmake==4.4.4", "ninja==1.13.2"]
