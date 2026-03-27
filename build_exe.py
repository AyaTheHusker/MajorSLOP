"""Build MajorSLOP Windows .exe using PyInstaller under Wine.

Run: cd /home/bucka/AI/mudproxy && wine C:\\users\\bucka\\AppData\\Local\\Programs\\Python\\Python312\\python.exe build_exe.py
"""
import PyInstaller.__main__
import os
import sys

# Paths relative to this script
BASE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(BASE, 'dist')

PyInstaller.__main__.run([
    os.path.join(BASE, 'mudproxy-gui', '__main__.py'),
    '--name', 'MajorSLOP',
    '--onefile',
    '--windowed',
    '--noconfirm',
    '--clean',
    # Include the mudproxy package
    '--paths', os.path.join(BASE),
    '--paths', os.path.join(BASE, 'mudproxy-web'),
    # Hidden imports that PyInstaller might miss
    '--hidden-import', 'mudproxy',
    '--hidden-import', 'mudproxy.config',
    '--hidden-import', 'mudproxy.paths',
    '--hidden-import', 'mudproxy.proxy',
    '--hidden-import', 'mudproxy.parser',
    '--hidden-import', 'mudproxy.gamedata',
    '--hidden-import', 'mudproxy.hp_tracker',
    '--hidden-import', 'mudproxy.entity_db',
    '--hidden-import', 'mudproxy.ansi',
    '--hidden-import', 'mudproxy.textblock',
    '--hidden-import', 'event_bus',
    '--hidden-import', 'slop_loader',
    '--hidden-import', 'orchestrator',
    '--hidden-import', 'server',
    '--hidden-import', 'uvicorn',
    '--hidden-import', 'uvicorn.logging',
    '--hidden-import', 'uvicorn.loops',
    '--hidden-import', 'uvicorn.loops.auto',
    '--hidden-import', 'uvicorn.protocols',
    '--hidden-import', 'uvicorn.protocols.http',
    '--hidden-import', 'uvicorn.protocols.http.auto',
    '--hidden-import', 'uvicorn.protocols.http.h11_impl',
    '--hidden-import', 'uvicorn.protocols.websockets',
    '--hidden-import', 'uvicorn.protocols.websockets.auto',
    '--hidden-import', 'uvicorn.protocols.websockets.websockets_impl',
    '--hidden-import', 'uvicorn.lifespan',
    '--hidden-import', 'uvicorn.lifespan.on',
    '--hidden-import', 'fastapi',
    '--hidden-import', 'starlette',
    '--hidden-import', 'websockets',
    '--hidden-import', 'access_parser',
    '--hidden-import', 'multipart',
    '--hidden-import', 'python_multipart',
    '--hidden-import', 'construct',
    # Include static web files
    '--add-data', os.path.join(BASE, 'mudproxy-web', 'static') + os.pathsep + 'static',
    # Include export scripts
    '--add-data', os.path.join(BASE, 'scripts') + os.pathsep + 'scripts',
    # Output
    '--distpath', os.path.join(BASE, 'dist'),
    '--workpath', os.path.join(BASE, 'build'),
    '--specpath', os.path.join(BASE),
])

# Create portable directory structure
for folder in ['slop', 'gamedata', 'config', 'scripts']:
    os.makedirs(os.path.join(DIST, folder), exist_ok=True)

# Copy export script for MDB loading
import shutil
src_script = os.path.join(BASE, 'scripts', 'export_gamedata.py')
if os.path.exists(src_script):
    shutil.copy2(src_script, os.path.join(DIST, 'scripts', 'export_gamedata.py'))

print(f"\nPortable build ready at: {DIST}")
print("  MajorSLOP.exe")
print("  slop/        <- drop .slop files here")
print("  gamedata/    <- exported MDB data")
print("  config/      <- settings.json")
