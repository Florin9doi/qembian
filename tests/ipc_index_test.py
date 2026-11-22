#!/usr/bin/env python3
"""tools/ipc_index.py: the IPC server index (named servers -> the module that starts them, their clients).

2026-11-19: written for the registry's ipc-client-server edges (docs/registry/README.md listed them as the
first missing ingest path). The literal scanner and the classification run on hand-made inputs; the 808 checks
run when its index is built."""
import os, struct, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ipc_index as I

DEV = 'nokia-808-rm807'
DEV_DIR = os.path.join(ROOT, 'build', 'roms', DEV, 'RM-807_113.010.1508_04.01+C00.01')


def lit(s):
    """a TLitC16: u32 length then the characters"""
    return struct.pack('<I', len(s)) + s.encode('utf-16le')


def bare(s):
    return s.encode('utf-16le') + b'\0\0'


class Literals(unittest.TestCase):
    def test_tlitc_and_bare_names(self):
        img = b'\0' * 8 + lit('!Windowserver') + b'\xff\xff' + bare('!EtelServer') + b'\0' * 4
        self.assertEqual(I.literals(img), [('!Windowserver', 'lit'), ('!EtelServer', 'bare')])

    def test_junk_is_not_a_server_name(self):
        """the 808 scan (2026-11-19) first returned "!:\\private\\..." (a drive placeholder), "!aBitmap.Handle()"
        (an assert text), "!h-" and "!#$?%&*+-/" (tables in euser.dll) as servers"""
        img = b''.join(b'\0' * 4 + bare(s) for s in ['!:\\private\\10282BC4\\Rules\\', '!aBitmap.Handle()', '!h-',
                                                     '!#$?%&*+-/', '!"!"!"!"', '!iAnimating'])
        self.assertEqual(I.literals(img), [])
        self.assertEqual(I.literals(b'\0' * 4 + lit('!:\\sys\\bin')), [])      # even as a TLitC

    def test_literal_followed_by_printable_bytes_keeps_its_length(self):
        img = b'\0' * 4 + lit('!HWRMServer') + 'AB'.encode('utf-16le')
        self.assertEqual(I.literals(img), [('!HWRMServer', 'lit')])

    def test_wildcard_find_pattern_is_kept(self):
        self.assertEqual(I.literals(b'\0' * 4 + lit('!MdSServer*')), [('!MdSServer*', 'lit')])


IDX = {'dlls': {'euser.dll': {'1285': '_ZN8CServer25StartERK7TDesC16', '1286': '_ZN8CServer26StartLERK7TDesC16',
                              '211': '_ZN12RSessionBase13CreateSessionERK7TDesC16RK8TVersioni',
                              '1992': '_ZN12RSessionBase13CreateSessionERK7TDesC16RK8TVersioni15TIpcSessionType'
                                      'PK15TSecurityPolicyP14TRequestStatus',
                              '210': '_ZN12RSessionBase13CreateSessionE8RServer2RK8TVersioni', '7': 'memclr'}},
       'modules': {
           'sys/bin/ewsrv.exe': {'imports': [['EUSER.DLL', 1286, 0, 'code']]},
           'Sys/Bin/ws32.dll': {'imports': [['euser.dll', 211, 0, 'code']]},
           'sys/bin/app.exe': {'imports': [['euser.dll', 1992, 0, 'code'], ['euser.dll', 7, 4, 'code']]},
           'sys/bin/hwrmserver.exe': {'imports': [['euser.dll', 1286, 0, 'code'], ['euser.dll', 211, 4, 'code']]},
           'Sys/Bin/sqlsrv.exe': {'imports': [['euser.dll', 1285, 0, 'code']], 'shadowed_by': 'rofs2@0x30'},
           'sys/bin/sqlsrv.exe': {'imports': [['euser.dll', 1285, 0, 'code']]},
           'sys/bin/sqldb.dll': {'imports': [['euser.dll', 211, 0, 'code']]},
           'sys/bin/help.dll': {'imports': []},
       }}
NAMES = {'sys/bin/ewsrv.exe': [('!Windowserver', 'lit')], 'Sys/Bin/ws32.dll': [('!Windowserver', 'lit')],
         'sys/bin/app.exe': [('!Windowserver', 'bare'), ('!Windowserver', 'lit'), ('!SocketServer', 'lit')],
         # a server EXE that names another server it connects to
         'sys/bin/hwrmserver.exe': [('!HWRMServer', 'lit'), ('!Windowserver', 'lit')],
         'Sys/Bin/sqlsrv.exe': [('!SQL Server', 'lit')], 'sys/bin/sqlsrv.exe': [('!SQL Server', 'lit')],
         'sys/bin/sqldb.dll': [('!SQL Server', 'lit')], 'sys/bin/help.dll': [('!Windowserver', 'lit')]}


class Classify(unittest.TestCase):
    def setUp(self):
        self.start, self.conn = I.euser_ordinals(IDX)
        self.mods, self.servers = I.classify(IDX['modules'], NAMES, self.start, self.conn)

    def test_euser_ordinals_come_from_the_index_by_name(self):
        self.assertEqual((self.start, self.conn), ({1285, 1286}, {211, 1992}))   # not the RServer2 overload

    def test_provider_and_clients(self):
        ws = self.servers['!Windowserver']
        self.assertEqual(ws['provider'], 'sys/bin/ewsrv.exe')
        self.assertEqual(ws['clients'], ['Sys/Bin/ws32.dll', 'sys/bin/app.exe', 'sys/bin/hwrmserver.exe'])
        self.assertEqual(ws['mentions'], ['sys/bin/help.dll'])     # neither starts nor connects

    def test_server_exe_naming_another_server_is_its_client_not_its_provider(self):
        """hwrmserver.exe holds "!HWRMServer" and "!Windowserver" and imports both StartL and CreateSession: it
        provides the name resembling its file name and is a client of the other"""
        self.assertEqual(self.servers['!HWRMServer']['provider'], 'sys/bin/hwrmserver.exe')
        self.assertIn('sys/bin/hwrmserver.exe', self.servers['!Windowserver']['clients'])
        self.assertEqual(self.servers['!Windowserver']['candidates'], ['sys/bin/ewsrv.exe', 'sys/bin/hwrmserver.exe'])

    def test_shadowed_copy_is_not_the_provider(self):
        """the 808's sqlsrv.exe is in XIP and again in ROFS2: the loaded copy provides "!SQL Server" """
        self.assertEqual(self.servers['!SQL Server']['provider'], 'sys/bin/sqlsrv.exe')

    def test_no_provider_stays_visible(self):
        self.assertIsNone(self.servers['!SocketServer']['provider'])
        self.assertEqual(self.servers['!SocketServer']['clients'], ['sys/bin/app.exe'])

    def test_one_entry_per_name_lit_wins(self):
        self.assertEqual(self.mods['sys/bin/app.exe']['names'], [('!SocketServer', 'lit'), ('!Windowserver', 'lit')])


@unittest.skipUnless(os.path.exists(os.path.join(DEV_DIR, 'ipc.json')), 'needs ipc_index.py build (808)')
class Built808(unittest.TestCase):
    def test_known_servers(self):
        idx = I.load(DEV)
        sv = idx['servers']
        self.assertEqual(sv['!Windowserver']['provider'], 'sys/bin/EwSrv.exe')
        self.assertIn('Sys/Bin/Ws32.dll', sv['!Windowserver']['clients'])
        self.assertEqual(sv['!SURFACE_UPDATE_SERVER']['provider'], 'Sys/Bin/surfaceupdateserver.dll')
        self.assertEqual(sv['!SURFACE_UPDATE_SERVER']['clients'], ['Sys/Bin/surfaceupdateclient.dll'])
        self.assertEqual(sv['!Fontbitmapserver']['provider'], 'Sys/Bin/fbserv.exe')
        self.assertEqual(sv['!SQL Server']['provider'], 'sys/bin/sqlsrv.exe')    # the ROFS2 copy, not the XIP one
        self.assertGreater(len(sv), 140)
        self.assertNotIn('!:\\sys\\bin', sv)


if __name__ == '__main__':
    unittest.main()
