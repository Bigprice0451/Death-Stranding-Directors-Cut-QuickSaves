"""Install an already tested release on Windows. Preserve existing personal settings.

The usual installation is drag-and-drop with the game closed. This maintainer
tool can also publish next-launch files while the current process keeps its
old mapped images on NTFS. It never ends or injects into a running process.
"""
import argparse
import ctypes
from ctypes import wintypes
from hashlib import sha256
import os
import re
import subprocess
import sys
from uuid import uuid4
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import ZipFile

PACKAGE = (
    "dxgi.dll",
    "DirectorCutQuickSaves.addon64",
    "DirectorCutQuickSaves/DLSSRuntime.exe",
    "DirectorCutQuickSaves/READ ME.txt",
    "DirectorCutQuickSaves/THIRD-PARTY-NOTICES.txt",
    "DirectorCutQuickSaves/NVIDIA-DLSS-LICENSE.txt",
)
GAME_SHA = "9d5306d7eb2f5d3135c63accc3e74b8f9b029f9d76112a6aae4a38fdbb4edd2b"
NVIDIA_SHA = "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"

def publish(stage, target, game_pid=None):
    try:
        os.replace(stage, target)
        return
    except PermissionError:
        if not game_pid or target.name not in {"dxgi.dll", "DirectorCutQuickSaves.addon64", "nvngx_dlss.dll"}:
            raise
    # NTFS allows renaming a mapped DLL while its running image stays alive.
    # Change the name, then publish the new file and retire exactly our old one.
    expected = sha256(target.read_bytes()).hexdigest()
    old = target.parent / f".dsqs-in-use-{uuid4().hex}.tmp"
    target.rename(old)
    try:
        os.replace(stage, target)
    except OSError:
        if not target.exists():
            old.rename(target)
        raise
    try:
        old.unlink()
    except PermissionError:
        subprocess.Popen([sys.executable, "-B", str(Path(__file__).resolve()),
                          "--retire", str(target.parent), str(game_pid), old.name, expected],
                         creationflags=subprocess.CREATE_NO_WINDOW,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def game_process(pid, game):
    """Include the full program path so PID reuse cannot target a different process."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    handle = kernel.OpenProcess(0x100000 | 0x1000, False, pid)
    if not handle:
        return None
    name = ctypes.create_unicode_buffer(32768)
    n = wintypes.DWORD(len(name))
    if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(n)) or Path(name.value).resolve() != (game/"ds.exe").resolve():
        kernel.CloseHandle(handle)
        return None
    return handle

def retire(folder, pid, basename, expected):
    if not re.fullmatch(r"\.dsqs-in-use-[0-9a-f]{32}\.tmp", basename) or not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Unexpected retired file identity")
    game = folder.resolve(strict=True)
    candidate = game / basename
    handle = game_process(pid, game)
    if handle:
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        try:
            if kernel.WaitForSingleObject(handle, 0xffffffff) != 0:
                return
        finally:
            kernel.CloseHandle(handle)
    if candidate.resolve() == candidate and candidate.is_file() and sha256(candidate.read_bytes()).hexdigest() == expected:
        candidate.unlink()

def install(zip_path, game, local_nvidia=None, game_pid=None):
    game = game.resolve(strict=True)
    if sha256((game / "ds.exe").read_bytes()).hexdigest() != GAME_SHA:
        raise ValueError("Different game executable; verify the build before installation.")
    if game_pid:
        handle = game_process(game_pid, game)
        if handle:
            kernel = ctypes.WinDLL("kernel32")
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel.CloseHandle(handle)
        else:
            game_pid = None
    with ZipFile(zip_path) as archive:
        if sorted(archive.namelist()) != sorted(PACKAGE):
            raise ValueError("Unexpected ZIP contents.")
        payloads = {name: archive.read(name) for name in PACKAGE}
    if local_nvidia:
        runtime = local_nvidia.read_bytes()
        if sha256(runtime).hexdigest() != NVIDIA_SHA:
            raise ValueError("The optional local NVIDIA runtime does not match the signed production release.")
        payloads["nvngx_dlss.dll"] = runtime
    staged = {}
    try:
        for name, data in payloads.items():
            target = game / name
            if target.resolve() != target or not target.is_relative_to(game):
                raise ValueError(f"Unexpected link or destination {name}.")
            if target.exists() and target.read_bytes() == data:
                continue
            target.parent.mkdir(exist_ok=True)
            with NamedTemporaryFile(dir=target.parent, prefix=".dsqs-install-", suffix=".tmp", delete=False) as file:
                stage = Path(file.name)
                staged[target] = stage
                file.write(data)
                file.flush()
                os.fsync(file.fileno())
            if stage.read_bytes() != data:
                raise OSError(f"Staged file verification failed for {name}.")
        # Dependencies first, main add-on last. Each file publishes atomically.
        ordered = sorted(staged, key=lambda p: p.name == "DirectorCutQuickSaves.addon64")
        for dest in ordered:
            publish(staged[dest], dest, game_pid)
        for name, data in payloads.items():
            if (game / name).read_bytes() != data:
                raise OSError(f"Installed file mismatch for {name}.")
            print(f"{sha256(data).hexdigest()}  {name}")
    finally:
        for path in staged.values():
            path.unlink(missing_ok=True)

if __name__ == "__main__":
    if len(sys.argv) == 6 and sys.argv[1] == "--retire":
        retire(Path(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5])
        sys.exit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip", type=Path)
    parser.add_argument("game", type=Path)
    parser.add_argument("--local-nvidia", type=Path, help="Previously verified optional signed DLL for this local install")
    parser.add_argument('--running-game-pid', type=int, help='Optional verified local ds.exe to preserve while updating its next-launch files')
    args = parser.parse_args()
    install(args.zip, args.game, args.local_nvidia, args.running_game_pid)
