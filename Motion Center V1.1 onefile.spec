# -*- mode: python ; coding: utf-8 -*-

a = Analysis(
    ['Motion_Center_V1_2.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('icons', 'icons'),
        ('images', 'images'),
        ('firmware', 'firmware'),
        ('Simhub profile', 'Simhub profile'),
        ('avrdude', 'avrdude'),
    ],
    hiddenimports=[],
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
    name='Motion Center V1.2',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='icons\\Motion-Center-icon.ico',
    onefile=True,
)
