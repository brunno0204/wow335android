#!/usr/bin/env python3
"""Embeds ui/*.lua (sorted by name) as C++ raw strings (UiPrelude.inc). Run from Tools/AndroidPort.

Each file runs as its own chunk, in order, before Interface\\FrameXML\\FrameXML.toc."""
import glob, os
parts = []
for path in sorted(glob.glob('ui/*.lua')):
    src = open(path, encoding='utf-8').read()
    assert ')LUA"' not in src, path
    parts.append('    {"Android\\\\%s", R"LUA(%s)LUA"},\n' % (os.path.basename(path), src))
open('UiPrelude.inc', 'w', encoding='utf-8').write(
    '// Generated from ui/*.lua by tools/gen_ui_prelude.py. Do not edit.\n'
    'struct UiScript { const char *name, *source; };\n'
    'static const UiScript kUiScripts[] = {\n' + ''.join(parts) + '};\n')
