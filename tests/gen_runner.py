#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Autonomy®
"""Write a Unity main() that runs every `void test_*(void)` in a test file."""
import re
import sys

src, out = sys.argv[1], sys.argv[2]
tests = re.findall(r'^void (test_\w+)\(void\)', open(src, encoding='utf-8').read(), re.M)
with open(out, 'w', encoding='utf-8') as f:
    f.write('#include "unity.h"\nvoid setUp(void);\nvoid tearDown(void);\n')
    f.writelines(f'void {t}(void);\n' for t in tests)
    f.write('int main(void)\n{\n    UNITY_BEGIN();\n')
    f.writelines(f'    RUN_TEST({t});\n' for t in tests)
    f.write('    return UNITY_END();\n}\n')
