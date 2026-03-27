# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['mudproxy-gui\\__main__.py'],
    pathex=['.', 'mudproxy-web'],
    binaries=[],
    datas=[('mudproxy-web/static', 'static')],
    hiddenimports=['mudproxy', 'mudproxy.config', 'mudproxy.paths', 'mudproxy.proxy', 'mudproxy.parser', 'mudproxy.gamedata', 'mudproxy.hp_tracker', 'mudproxy.entity_db', 'mudproxy.ansi', 'event_bus', 'slop_loader', 'orchestrator', 'server', 'uvicorn', 'uvicorn.logging', 'uvicorn.loops', 'uvicorn.loops.auto', 'uvicorn.protocols', 'uvicorn.protocols.http', 'uvicorn.protocols.http.auto', 'uvicorn.protocols.http.h11_impl', 'uvicorn.protocols.websockets', 'uvicorn.protocols.websockets.auto', 'uvicorn.protocols.websockets.websockets_impl', 'uvicorn.lifespan', 'uvicorn.lifespan.on', 'fastapi', 'starlette', 'websockets'],
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
    name='MajorSLOP_test',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
