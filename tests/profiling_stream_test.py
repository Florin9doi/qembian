#!/usr/bin/env python3
"""Regression test: the EGL_NOK_resource_profiling reply that the phone's graphics-memory monitor (GOOM)
parses (src/syvg/syvg_profiling.h, used by syvg_egl.cpp's EGL2_FN_QueryProfilingDataNOK).

The bug this pins down (2026-11-13, Nokia 808 / 701): the reply only carried TOTAL and USED when the
caller set EGL_PROF_QUERY_GLOBAL_BIT_NOK. GOOM asks with EGL_PROF_QUERY_MEMORY_USAGE_BIT_NOK alone and
its CMemoryMonitor::GetFreeMemory() always looks for that pair, so it read total = used = 0, free
memory = 0, declared low graphics memory and ALF went to its lowest-memory mode: the screen became a
CPU bitmap uploaded in full per dirty rect (docs/devices/nokia-808-rm807/frame-uploads.md).

    python3 tests/profiling_stream_test.py
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'syvg', 'syvg_profiling.h')
PROG = r'''
#include <stdio.h>
#include <stdint.h>
#include "syvg_profiling.h"

static int fails;
static void ck(int cond, const char* what) { printf("  %s %s\n", cond ? "ok" : "FAIL", what); if (!cond) fails++; }

int main(void)
{
    int32_t v[64];
    uint32_t pids[2] = { 0x00400001u, 0x00400002u };
    uint64_t bytes[2] = { 0x100000u, 0x200000u };

    /* GOOM's actual query: memory usage only, no per-process data asked of us here */
    int n = syvg_profiling_fill(0x2u, 134217728u, 1843200u, pids, bytes, 0, v, 64);
    ck(n == 4, "bits=0x2, no processes: four words");
    ck(v[0] == 0x3070 && v[1] == 134217728, "bits=0x2 still reports TOTAL (the bug: it did not)");
    ck(v[2] == 0x3071 && v[3] == 1843200, "bits=0x2 still reports USED");

    /* global bit alone: same pair, still no processes */
    n = syvg_profiling_fill(0x1u, 100u, 40u, pids, bytes, 2, v, 64);
    ck(n == 4 && v[1] == 100 && v[3] == 40, "bits=0x1: TOTAL/USED, no per-process entries");

    /* per process entries: three token,value pairs, one word each (no padding words) */
    n = syvg_profiling_fill(0x3u, 100u, 40u, pids, bytes, 2, v, 64);
    ck(n == 4 + 2 * SYVG_PROF_WORDS_PER_PROCESS, "bits=0x3, two processes: 4 + 12 words");
    ck(v[4] == 0x3072 && v[5] == 0x00400001u, "first entry: PROCESS_ID, one word");
    ck(v[6] == 0x3073 && v[7] == 0x100000u, "first entry: PRIVATE, one word");
    ck(v[8] == 0x3074 && v[9] == 0, "first entry: SHARED, one word");
    ck(v[10] == 0x3072 && v[11] == 0x00400002u, "second entry: PROCESS_ID");

    /* a short buffer: the caller sends the first cap words, the count still reports the full stream */
    int32_t small[6];
    n = syvg_profiling_fill(0x3u, 1u, 2u, pids, bytes, 2, small, 6);
    ck(n == 16, "short buffer: the needed length is still reported");
    ck(small[0] == 0x3070 && small[5] == 0x00400001u, "short buffer: first six words are the stream's");

    printf("%s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
'''


def main():
    tmp = tempfile.mkdtemp(prefix='profiling-test-')
    cpp = os.path.join(tmp, 'profiling_stream.c')
    exe = os.path.join(tmp, 'profiling_stream')
    open(cpp, 'w').write(PROG)
    r = subprocess.run([os.environ.get('CC', 'cc'), '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I' + os.path.dirname(SRC), '-o', exe, cpp], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout, end='')
    if r.returncode:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
