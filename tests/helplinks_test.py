#!/usr/bin/env python3
"""tools/helplinks.py: URL parsing and extraction from a synthetic help tree (plain unittest).

The rules mirror cshelp.exe (S60 5th, 5800 31.0.101): it counts '/' in the URL (4 = plain, >= 6 = with a
message uid + message, anything else is ignored), numbers with "0x" are hex, others decimal.
"""
import io, os, sys, unittest, zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import helplinks  # noqa: E402


class ParseUrl(unittest.TestCase):
    def test_cmd_plain(self):
        d = helplinks.parse_url('app://cmd@localhost/Mce.exe/0x100058C5')
        self.assertEqual((d['kind'], d['exe'], d['app_uid']), ('cmd', 'Mce.exe', '0x100058c5'))
        self.assertNotIn('invalid', d)
        self.assertNotIn('msg', d)

    def test_vsa_decimal_view(self):
        d = helplinks.parse_url('app://vsa@localhost/0x101F85A0/1')
        self.assertEqual((d['app_uid'], d['view_uid']), ('0x101f85a0', '0x00000001'))

    def test_vsa_view_zero(self):
        self.assertEqual(helplinks.parse_url('app://vsa@localhost/0x10005A32/0')['view_uid'], '0x00000000')

    def test_cmd_message_keeps_slashes(self):
        # N97: the message is itself a URL with '/' in it
        d = helplinks.parse_url('app://cmd@localhost/matrixmenu.exe/0x101f4cd2/0x10003a39/mm://root/musicsuite#0')
        self.assertEqual((d['app_uid'], d['msg_uid'], d['msg']), ('0x101f4cd2', '0x10003a39', 'mm://root/musicsuite#0'))
        self.assertNotIn('invalid', d)

    def test_short_uid_typo_kept_as_written(self):
        # 5800 RealPlayer link has a digit missing (0x1005A3E); cshelp parses it as is and falls back to the exe
        self.assertEqual(helplinks.parse_url('app://cmd@localhost/MediaPlayer.exe/0x1005A3E')['app_uid'], '0x01005a3e')

    def test_five_slashes_invalid(self):
        self.assertIn('invalid', helplinks.parse_url('app://vsa@localhost/0x10005951/0x1020745C/0x1'))

    def test_non_app_links_ignored(self):
        self.assertIsNone(helplinks.parse_url('../0x0112233/GLOS_HLP_AP.html#d1'))
        self.assertIsNone(helplinks.parse_url('http://www.nokia.com/'))


class Extract(unittest.TestCase):
    def tree(self):
        z = io.BytesIO()
        with zipfile.ZipFile(z, 'w') as zf:
            zf.writestr('BT_HLP_MAIN.html', '<html><head><title>Bluetooth</title></head><body><P>Open '
                        '<a href="app://vsa@localhost/0x10005951/0x1020745C"><span>Bluetooth</span></a>. See '
                        '<a href="../0x0112233/GLOS.html">glossary</a>.</P></body></html>')
        return [('resource/xhtml/01/index.xml',
                 b'<collections><collection FeatureID="-1" id="0x10005951" navtitle="Bluetooth"></collection></collections>'),
                ('resource/xhtml/01/0x10005951/contents.zip', z.getvalue()),
                ('resource/xhtml/03/0x10005951/contents.zip', z.getvalue())]

    def test_app_links_only_one_language(self):
        out = helplinks.extract(self.tree(), '01')
        self.assertEqual(len(out), 1)
        d = out[0]
        self.assertEqual((d['text'], d['page_title'], d['module_title'], d['view_uid']),
                         ('Bluetooth', 'Bluetooth', 'Bluetooth', '0x1020745c'))

    def test_all_links(self):
        self.assertEqual(len(helplinks.extract(self.tree(), '01', all_links=True)), 2)


if __name__ == '__main__':
    unittest.main()
