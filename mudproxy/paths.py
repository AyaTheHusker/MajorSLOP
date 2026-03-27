"""Cross-platform default paths for MajorSLOP.

Portable mode: if a 'slop/' folder exists next to the exe, all data lives
in the exe's directory tree. Otherwise falls back to OS standard dirs.

On Linux:
    config  -> ~/.config/majorslop
    cache   -> ~/.cache/majorslop
    data    -> ~/.local/share/majorslop
    megamud -> ~/Megamud
    slop    -> ~/.local/share/majorslop/slop

On Windows (installed):
    config  -> %APPDATA%/MajorSLOP
    cache   -> %LOCALAPPDATA%/MajorSLOP/cache
    data    -> %LOCALAPPDATA%/MajorSLOP
    megamud -> C:/Megamud
    slop    -> %LOCALAPPDATA%/MajorSLOP/slop

On Windows (portable):
    Everything lives next to MajorSLOP.exe:
    ./slop/  ./gamedata/  ./config/  ./cache/
"""

import os
import platform
import sys
from pathlib import Path

IS_WINDOWS = platform.system() == "Windows"
_home = Path.home()

_APP_NAME = "MajorSLOP" if IS_WINDOWS else "majorslop"


def _exe_dir() -> Path:
    """Directory containing the exe (PyInstaller) or the script."""
    if getattr(sys, '_MEIPASS', None):
        # PyInstaller onedir: exe is one level up from _MEIPASS (_internal)
        return Path(sys.executable).parent
    return Path(__file__).resolve().parent.parent


def _is_portable() -> bool:
    """Portable mode if a 'slop' or 'portable' marker exists next to the exe."""
    d = _exe_dir()
    return (d / "slop").exists() or (d / "portable").exists()


def _win_appdata() -> Path:
    return Path(os.environ.get("APPDATA", _home / ".config"))


def _win_localappdata() -> Path:
    return Path(os.environ.get("LOCALAPPDATA", _home / ".local" / "share"))


# ── Default directories ──

def default_config_dir() -> Path:
    if _is_portable():
        return _exe_dir() / "config"
    if IS_WINDOWS:
        return _win_appdata() / _APP_NAME
    return _home / ".config" / _APP_NAME


def default_cache_dir() -> Path:
    if _is_portable():
        return _exe_dir() / "cache"
    if IS_WINDOWS:
        return _win_localappdata() / _APP_NAME / "cache"
    return _home / ".cache" / _APP_NAME


def default_data_dir() -> Path:
    if _is_portable():
        return _exe_dir()
    if IS_WINDOWS:
        return _win_localappdata() / _APP_NAME
    return _home / ".local" / "share" / _APP_NAME


def default_slop_dir() -> Path:
    if _is_portable():
        return _exe_dir() / "slop"
    return default_data_dir() / "slop"


def default_megamud_dir() -> Path:
    if IS_WINDOWS:
        return Path("C:/Megamud")
    return _home / "Megamud"


def default_gamedata_dir() -> Path:
    if _is_portable():
        return _exe_dir() / "gamedata"
    return default_cache_dir() / "gamedata"


def default_dat_dir() -> Path:
    if IS_WINDOWS:
        return Path("C:/Megamud")
    return _home / "Megamud"


def default_pictures_dir() -> Path:
    return _home / "Pictures"


def default_downloads_dir() -> Path:
    return _home / "Downloads"


# ── Search paths ──

def mdb_search_paths() -> list[Path]:
    paths = [default_megamud_dir(), default_downloads_dir(), _home]
    if IS_WINDOWS:
        paths.insert(0, Path("C:/Program Files (x86)/MMUD Explorer"))
        paths.insert(1, Path("C:/Program Files/MMUD Explorer"))
    else:
        paths.insert(0, _home / ".wine" / "drive_c" / "Program Files (x86)" / "MMUD Explorer")
        paths.insert(1, _home / ".wine" / "drive_c" / "Program Files" / "MMUD Explorer")
    return paths


def slop_search_paths() -> list[Path]:
    paths = [default_slop_dir()]
    if _is_portable():
        return paths
    paths.append(default_cache_dir() / "slop")
    paths.append(default_downloads_dir())
    paths.append(_home)
    return paths
