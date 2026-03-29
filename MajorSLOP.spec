# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['Z:\\home\\bucka\\AI\\mudproxy\\mudproxy-gui\\__main__.py'],
    pathex=['Z:\\home\\bucka\\AI\\mudproxy', 'Z:\\home\\bucka\\AI\\mudproxy\\mudproxy-web'],
    binaries=[],
    datas=[('Z:\\home\\bucka\\AI\\mudproxy\\mudproxy-web\\static', 'static'), ('Z:\\home\\bucka\\AI\\mudproxy\\scripts', 'scripts')],
    hiddenimports=['mudproxy', 'mudproxy.config', 'mudproxy.paths', 'mudproxy.proxy', 'mudproxy.parser', 'mudproxy.gamedata', 'mudproxy.hp_tracker', 'mudproxy.entity_db', 'mudproxy.ansi', 'mudproxy.textblock', 'mudproxy.mem_reader', 'mudproxy.megamud_offsets', 'event_bus', 'slop_loader', 'orchestrator', 'server', 'uvicorn', 'uvicorn.logging', 'uvicorn.loops', 'uvicorn.loops.auto', 'uvicorn.protocols', 'uvicorn.protocols.http', 'uvicorn.protocols.http.auto', 'uvicorn.protocols.http.h11_impl', 'uvicorn.protocols.websockets', 'uvicorn.protocols.websockets.auto', 'uvicorn.protocols.websockets.websockets_impl', 'uvicorn.lifespan', 'uvicorn.lifespan.on', 'fastapi', 'starlette', 'websockets', 'access_parser', 'multipart', 'python_multipart', 'construct'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='MajorSLOP',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
