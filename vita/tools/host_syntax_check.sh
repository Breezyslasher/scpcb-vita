#!/bin/sh
# Syntax-checks vita/src/main.c on the host with generated stub
# headers for vitaGL/psp2 (no vitasdk needed). Catches the recurring
# declaration-order and typo build breaks before a CI round trip.
# Run from the repo root:  sh vita/tools/host_syntax_check.sh
set -e
STUB=$(mktemp -d)
trap 'rm -rf "$STUB"' EXIT
python3 - "$STUB" <<'EOF'
import re, os, sys
stub = sys.argv[1]
src = open('vita/src/main.c').read()
os.makedirs(stub + '/psp2/kernel', exist_ok=True)
funcs = sorted(set(re.findall(r'\b(gl[A-Z]\w*|vgl[A-Z]\w*|sce[A-Z]\w*)\s*\(', src)))
consts = sorted(set(re.findall(r'\b(GL_[A-Z0-9_]+|VGL_[A-Z0-9_]+|SCE_[A-Z0-9_]+)\b', src)))
types = sorted(set(re.findall(r'\b(GL[a-z]\w*)\b', src)))
with open(stub + '/vitaGL.h', 'w') as f:
    f.write('#ifndef STUB_VITAGL_H\n#define STUB_VITAGL_H\n')
    f.write('#include <stdint.h>\n#include <stddef.h>\n')
    for t in types:
        if t in ('GLsizeiptr', 'GLintptr'):
            f.write('typedef long %s;\n' % t)
        elif t in ('GLfloat', 'GLclampf'):
            f.write('typedef float %s;\n' % t)
        elif t in ('GLboolean', 'GLubyte'):
            f.write('typedef unsigned char %s;\n' % t)
        elif t == 'GLdouble':
            f.write('typedef double GLdouble;\n')
        elif t == 'GLuint':
            f.write('typedef unsigned int GLuint;\n')
        elif t == 'GLvoid':
            f.write('typedef void GLvoid;\n')
        else:
            f.write('typedef int %s;\n' % t)
    for i, c in enumerate(consts):
        f.write('#define %s %d\n' % (c, i + 1))
    for fn in funcs:
        if not fn.startswith('sce'):
            f.write('long %s();\n' % fn)
    f.write('#endif\n')
with open(stub + '/psp2/kernel/processmgr.h', 'w') as f:
    f.write('#pragma once\n#include <stdint.h>\n')
    for fn in funcs:
        if not fn.startswith('sce'):
            continue
        if fn == 'sceKernelGetProcessTimeWide':
            f.write('unsigned long long sceKernelGetProcessTimeWide();\n')
        else:
            f.write('long %s();\n' % fn)
    f.write('typedef int SceUID; typedef unsigned int SceSize;\n')
open(stub + '/psp2/kernel/threadmgr.h', 'w').write(
    '#pragma once\n#include "processmgr.h"\n')
EOF
gcc -fsyntax-only -Wall -Wno-deprecated-declarations \
    -I "$STUB" -I vita/src vita/src/main.c
echo "main.c syntax OK"
