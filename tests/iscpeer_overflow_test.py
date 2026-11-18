#!/usr/bin/env python3
"""Regression test SAServerSpinsOnOversizedIscMessage (2026-11-18).

The realigned product profile reply (1B 2E -> 2F, 148 bytes with the header) reached SAServer's ISC channel 35
receive, posted with a 128-byte buffer. Our ISC stand-in (src/standins/_shared/iscpeer.inl, TryDeliver)
completed the receive with KErrOverflow (-9) and kept the message; SAServer's RunL only grows its buffer on
KErrNoMemory (-4) (N95 SAIscService FUN_809120c2, E52 0x80a002b8; NokiaDSY FUN_808ba8fe likewise), so it
re-posted the same 128 bytes 1.5 million times and start-up starved. The N95's own NokiaISCDriver.ldd answers
-4 with the needed length and keeps the message (0x80119fb8..0x8011a230). Fixed in iscpeer.inl + iscrx.h:
the real -4 semantics, and a fail-safe that drops a message a client keeps refusing (docs/hacks.md
H-isc-rx-overflow).

  python3 tests/iscpeer_overflow_test.py
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHARED = os.path.join(ROOT, 'src/standins/_shared')
INL = open(os.path.join(SHARED, 'iscpeer.inl')).read()

# A host model of TryDeliver's loop around IscRxDecide (same structure as iscpeer.inl), driven by two clients:
# SAServer (grows its buffer to the needed length on -4, as the firmware does) and a client that never grows.
HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "iscrx.h"
enum { KErrNone = 0, KErrNoMemory = -4, KErrOverflow = -9 };
struct Q { int len[16]; int head, count; };
struct Rx { bool parked; int max; int needed; int status; int got; bool done; };
static TIscRxGuard g;
static int drops;
static void TryDeliver(Q& q, Rx& rx)
	{
	while (rx.parked && q.count)
		{
		int len = q.len[q.head];
		TIscRxAction a = IscRxDecide(g, len, rx.max);
		if (a == EIscRxDrop) { ++drops; q.head = (q.head + 1) % 16; --q.count; continue; }
		if (a == EIscRxTooSmall) { rx.needed = len; rx.status = KErrNoMemory; rx.parked = false; rx.done = true; return; }
		rx.got = len; rx.status = KErrNone; rx.parked = false; rx.done = true; q.head = (q.head + 1) % 16; --q.count;
		return;
		}
	}
// returns the number of receives posted until the queue is empty (or -1 after 1000: the old spin)
static int Run(bool grows, int* delivered, int* ndel)
	{
	Q q = {{148, 20}, 0, 2};
	memset(&g, 0, sizeof g);
	drops = 0;
	*ndel = 0;
	int buf = 128;
	for (int posts = 1; posts <= 1000; ++posts)
		{
		Rx rx = {true, buf, 0, 1, 0, false};
		TryDeliver(q, rx);
		if (!rx.done) return posts;			// parked: nothing more queued
		if (rx.status == KErrNone) { delivered[(*ndel)++] = rx.got; buf = 128; }
		else if (rx.status == KErrNoMemory && grows) buf = rx.needed;	// SAServer: HBufC8::NewL(needed)
		if (!q.count) return posts;
		}
	return -1;
	}
int main()
	{
	int d[4], n, posts;
	posts = Run(true, d, &n);
	printf("grows %d %d %d %d %d\n", posts, n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0, drops);
	posts = Run(false, d, &n);
	printf("stubborn %d %d %d %d %d\n", posts, n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0, drops);
	// the decision itself
	TIscRxGuard h = {0, 0};
	printf("fits %d\n", IscRxDecide(h, 128, 128));
	printf("bad-descriptor %d\n", IscRxDecide(h, 10, -14));
	return 0;
	}
'''


def cxx():
    for c in ('g++', 'clang++', 'c++'):
        if shutil.which(c):
            return c
    return None


class SAServerSpinsOnOversizedIscMessage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = cxx()
        if not cc:
            raise unittest.SkipTest('no host C++ compiler')
        d = tempfile.mkdtemp()
        src = os.path.join(d, 'h.cpp')
        exe = os.path.join(d, 'h')
        open(src, 'w').write(HARNESS)
        subprocess.check_call([cc, '-std=c++11', '-Wall', '-Werror', '-I', SHARED, '-o', exe, src])
        out = subprocess.check_output([exe], text=True)
        cls.out = {ln.split()[0]: [int(x) for x in ln.split()[1:]] for ln in out.splitlines()}
        shutil.rmtree(d)

    def test_client_that_grows_gets_the_148_byte_message(self):
        posts, n, first, second, drops = self.out['grows']
        self.assertEqual((n, first, second, drops), (2, 148, 20, 0))
        self.assertEqual(posts, 3)	# 128 refused (-4, needed 148), 148 delivered, then the 20-byte one

    def test_client_that_never_grows_cannot_spin(self):
        posts, n, first, second, drops = self.out['stubborn']
        self.assertNotEqual(posts, -1, 'the receive loop spun (the 2026-11-18 hang)')
        self.assertEqual((n, first, drops), (1, 20, 1))
        self.assertLessEqual(posts, 5)

    def test_decide_edges(self):
        self.assertEqual(self.out['fits'], [0])			# EIscRxDeliver at exactly MaxLength
        self.assertEqual(self.out['bad-descriptor'], [1])	# unreadable descriptor: refused, not written


class StandInUsesTheRealDriverSemantics(unittest.TestCase):
    def bodies(self):
        for fn in ('TryDeliver', 'TryDeliverData'):
            m = re.search(r'void DIscPeerChannel::%s\(\)\n\t\{(.*?)\n\t\}\n' % fn, INL, re.S)
            self.assertIsNotNone(m, fn)
            yield fn, m.group(1)

    def test_refusal_is_kerrnomemory_not_kerroverflow(self):
        for fn, body in self.bodies():
            with self.subTest(fn=fn):
                self.assertIn('IscRxDecide(', body)
                self.assertIn('KErrNoMemory', body)
                self.assertNotIn('KErrOverflow', body)

    def test_refusal_logged_once_per_channel(self):
        for fn, body in self.bodies():
            with self.subTest(fn=fn):
                self.assertRegex(body, r'if \(!i\w*RxGuard\.iWarned\)')


if __name__ == '__main__':
    unittest.main()
