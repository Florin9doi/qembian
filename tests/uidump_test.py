#!/usr/bin/env python3
"""Unit tests for tools/sydb/uidump.py (plain unittest): tests/uidump_test.py

Guest-side helpers (page-table walk, RTTI, descriptors) run on a fake `gdb` module with fake virtual and
physical memory; the tree/summary/select logic runs on synthetic snapshots. No phone is booted."""
import os, struct, sys, types, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sydb'))
import uidump as U


class FakeMem:
    def __init__(self):
        self.virt, self.phys, self.physmode = {}, {}, False

    def put(self, space, addr, data):
        m = self.phys if space == 'p' else self.virt
        for i, b in enumerate(data):
            m[addr + i] = b

    def words(self, space, addr, *ws):
        self.put(space, addr, b''.join(struct.pack('<I', w & 0xffffffff) for w in ws))

    def read_memory(self, a, n):
        if self.physmode:                                  # RAM: every address reads
            b = bytes(self.phys.get(a + i, 0) for i in range(n))
        else:
            try:
                b = bytes(self.virt[a + i] for i in range(n))
            except KeyError:
                raise RuntimeError('unmapped %#x' % a)
        return types.SimpleNamespace(tobytes=lambda: b)


def helpers(mem):
    """GDB_HELPERS executed against `mem`"""
    g = types.ModuleType('gdb')
    g.selected_inferior = lambda: mem

    def execute(cmd, to_string=False):
        if 'PhyMemMode:1' in cmd:
            mem.physmode = True
        elif 'PhyMemMode:0' in cmd:
            mem.physmode = False
        return ''
    g.execute = execute
    sys.modules['gdb'] = g
    ns = {}
    exec(U.GDB_HELPERS.replace('%%', '%'), ns)
    return ns


def utf16(s):
    return s.encode('utf-16-le')


class GuestHelpersTest(unittest.TestCase):
    def setUp(self):
        m = self.m = FakeMem()
        # process page directory at phys 0x100000: VA 0x00600000 -> page table at 0x200000 -> page 0x300000;
        # VA 0x00700000 -> 1 MB section at phys 0x400000
        m.words('p', 0x100000 + 6 * 4, 0x200000 | 1)
        m.words('p', 0x100000 + 7 * 4, 0x400000 | 2)
        m.words('p', 0x200000, 0x300000 | 2)
        m.put('p', 0x300000, bytes(0x1000))
        m.put('p', 0x400000, bytes(0x2000))
        self.h = helpers(m)
        self.h['PD'] = 0x100000

    def test_user_memory_through_page_table_and_section(self):
        """a process's user memory is read by walking its own page directory (any process, not the current one)"""
        self.m.words('p', 0x300010, 0x11223344)
        self.m.words('p', 0x400123 & ~3, 0xcafef00d)
        self.assertEqual(self.h['u'](0x600010), 0x11223344)
        self.assertEqual(self.h['u'](0x700120), 0xcafef00d)
        self.assertIsNone(self.h['u'](0x800000))           # no PDE: unmapped, not an exception
        self.assertIsNone(self.h['u'](0x600011))           # unaligned

    def test_hbufc_text_and_rejects(self):
        m, h = self.m, self.h
        # heap cell: length word (cell size incl. header) at p-4, then HBufC16 {length, text}
        m.words('p', 0x3000fc, 4 + 4 + 2 * 9 + 2)
        m.words('p', 0x300100, 9)
        m.put('p', 0x300104, utf16('Messaging'))
        self.assertEqual(h['hbufc'](0x600100), 'Messaging')
        m.words('p', 0x300200 - 4, 16)
        m.words('p', 0x300200, 50)                         # longer than its cell
        self.assertIsNone(h['hbufc'](0x600200))

    def test_descriptor_strings_tbuf_and_tbufc(self):
        """custom list arrays: rows are found as TBuf/TBufC inside the objects (Call settings, Messaging)"""
        m, h = self.m, self.h
        row = '\tAutomatic redial\t\tOff'
        blob = struct.pack('<II', 0x30000000 | len(row), 64) + utf16(row)
        blob += bytes((-len(blob)) % 4) + struct.pack('<I', 5) + utf16('Inbox') + b'\0\0'
        blob += struct.pack('<I', 0x00720072) + struct.pack('<I', 0x7000a0)   # pointer bytes, not text
        m.put('p', 0x300400, blob)
        self.assertEqual(h['desc_strings'](0x600400, len(blob)), [row, 'Inbox'])

    def test_plausible_rejects_pointer_bytes(self):
        """2026-11-19: the first Messaging harvest listed pointers read as UTF-16 ("Ꝍ聥㷸q")"""
        p = self.h['plausible']
        self.assertTrue(p('New message'))
        self.assertTrue(p('Сообщения'))
        self.assertFalse(p('Ꝍ聥㷸q'))
        self.assertFalse(p('z:\\resource\\apps\\muiu.mbm'))
        self.assertFalse(p('a1'))

    def test_demangle(self):
        d = self.h['demangle']
        self.assertEqual(d('14CCarouselAppUi'), 'CCarouselAppUi')
        self.assertEqual(d('N9CCoeAppUi6CExtraE'), 'CCoeAppUi::CExtra')
        self.assertEqual(d('13CArrayFixFlatI13RResourceFileE'), 'CArrayFixFlat<..>')

    def test_rtti_lineage_and_isa(self):
        """class names and bases from the ROM's typeinfo (vtable[-1]); no symbols"""
        m, h = self.m, self.h
        names = {0x81000000: b'N10__cxxabiv117__class_type_infoE\0', 0x81000040: b'N10__cxxabiv120__si_class_type_infoE\0',
                 0x81000080: b'11CCoeControl\0', 0x810000a0: b'11CEikListBox\0', 0x810000c0: b'8CAknGrid\0'}
        for a, s in names.items():
            m.put('v', a, s)
        # the typeinfo classes' own vtables, each with its typeinfo at [-1]
        m.words('v', 0x81000200, 0x81000300)               # [-1] of class_type_info's vtable 0x81000204
        m.words('v', 0x81000300, 0, 0x81000000)
        m.words('v', 0x81000210, 0x81000310)               # [-1] of si_class_type_info's vtable 0x81000214
        m.words('v', 0x81000310, 0, 0x81000040)
        m.words('v', 0x81000400, 0x81000204, 0x81000080)                 # typeinfo CCoeControl
        m.words('v', 0x81000410, 0x81000214, 0x810000a0, 0x81000400)     # CEikListBox : CCoeControl
        m.words('v', 0x81000420, 0x81000214, 0x810000c0, 0x81000410)     # CAknGrid : CEikListBox
        m.words('v', 0x81000500, 0, 0x81000420, 0)                       # vtable: [-2] 0, [-1] ti, [0]..
        m.words('p', 0x300600, 0x81000508)                               # object -> vtable
        m.words('p', 0x3005fc, 0x40)
        ti = h['otype'](0x600600)
        self.assertEqual(ti, 0x81000420)
        self.assertEqual([h['demangle'](n) for n in h['lineage'](ti)], ['CAknGrid', 'CEikListBox', 'CCoeControl'])
        self.assertTrue(h['isa'](ti, 'CEikListBox'))
        self.assertFalse(h['isa'](ti, 'ListBox'))          # whole class names only


def ctl(addr, cls, pos, size, flags=0x10, parent=None, via=None, seq=0, **kw):
    c = {'addr': addr, 'class': cls, 'kind': kw.pop('kind', None), 'pos': list(pos), 'size': list(size), 'flags': flags,
         'win': 0, 'parent': parent, 'via': via, 'components': [], 'texts': kw.pop('texts', []), 'seq': seq}
    c.update(kw)
    return c


class TreeTest(unittest.TestCase):
    def test_menu_pane_is_a_root_not_hidden_under_its_menu_bar(self):
        """2026-11-19: the open Options menu (window-owning CEikMenuPane, no CONE parent) was hung under the
        invisible CEikMenuBar whose memory points to it, so it came out hidden and the summary had no menu"""
        d = {'controls': [
            ctl('0x1', 'CEikMenuBar', (0, 0), (0, 0), flags=0x24),
            ctl('0x2', 'CEikMenuPane', (0, 128), (240, 167), flags=0x1c, via='0x1', seq=1,
                menu={'selected': 2, 'items': [{'text': t, 'cmd': 1, 'cascade': 0} for t in ('Open', 'Move', 'Change Menu view')]}),
        ], 'stack': []}
        roots, nodes = U.build_tree(d)
        self.assertTrue(nodes['0x2']['visible'])
        self.assertIn(nodes['0x2'], roots)
        s = U.summary(d, roots, nodes)
        self.assertEqual(s['menu']['selected'], 'Change Menu view')

    def test_screen_rect_from_window_owning_ancestors(self):
        """non-window-owning controls are placed relative to the window they draw in"""
        d = {'controls': [
            ctl('0x1', 'CViewControl', (0, 54), (240, 241), flags=0x14),
            ctl('0x2', 'CXnGridAdapter', (1, 1), (238, 239), parent='0x1', seq=1),
            ctl('0x3', 'CXn3DGrid', (1, 1), (238, 239), parent='0x2', seq=2),
        ], 'stack': []}
        roots, nodes = U.build_tree(d)
        self.assertEqual(nodes['0x3']['rect'], [1, 55, 238, 239])   # +window, not +every ancestor
        self.assertEqual(nodes['0x1']['rect'], [0, 54, 240, 241])

    def test_hidden_parent_hides_children(self):
        d = {'controls': [ctl('0x1', 'A', (0, 0), (10, 10), flags=0x34), ctl('0x2', 'B', (0, 0), (5, 5), parent='0x1')],
             'stack': []}
        roots, nodes = U.build_tree(d)
        self.assertFalse(nodes['0x2']['visible'])

    def test_parent_cycle_does_not_hang(self):
        d = {'controls': [ctl('0x1', 'A', (0, 0), (1, 1), parent='0x2'), ctl('0x2', 'B', (0, 0), (1, 1), parent='0x1')],
             'stack': []}
        roots, nodes = U.build_tree(d)
        self.assertEqual(len(nodes), 2)

    def test_softkeys_skip_the_middle_softkey_label(self):
        """2026-11-19: Messaging gave softkeys left="Select" and the Bluetooth name query right="OK": the middle
        softkey's button (x=0 in one, centred in the other) is not drawn on 3rd FP1 and is never activated"""
        def btn(a, pos, size, text, seq, flags=0x10):
            return [ctl(a, 'CEikCbaButton', pos, size, flags=flags, parent='0xc', seq=seq),
                    ctl(a + '1', 'CEikLabel', (0, 0), size, parent=a, texts=[text], seq=seq)]
        d = {'controls': [ctl('0xc', 'CEikCba', (0, 295), (240, 25), flags=0x14, kind='CEikCba')]
             + btn('0xa', (1, 3), (115, 20), 'Options', 1) + btn('0xb', (124, 3), (115, 20), 'Exit', 2)
             + btn('0xd', (0, 0), (80, 17), 'Select', 3, flags=0), 'stack': [{'ctl': '0xc'}]}
        roots, nodes = U.build_tree(d)
        s = U.summary(d, roots, nodes)
        self.assertEqual(s['softkeys'], {'left': 'Options', 'right': 'Exit', 'middle': 'Select'})
        d['controls'][-2]['pos'] = [78, 3]                 # centred, same row (the query dialog's CBA)
        d['controls'][-2]['size'] = [85, 20]
        roots, nodes = U.build_tree(d)
        s = U.summary(d, roots, nodes)
        self.assertEqual(s['softkeys'], {'left': 'Options', 'right': 'Exit', 'middle': 'Select'})

    def test_softkeys_empty_editor_query_shows_none(self):
        """2026-11-19: the Bluetooth name query (empty editor: no softkeys on screen) gave left=right="OK":
        one hidden label was taken for both keys"""
        d = {'controls': [ctl('0xc', 'CEikCba', (0, 295), (240, 25), flags=0x14, kind='CEikCba'),
                          ctl('0xa', 'CEikCbaButton', (1, 3), (115, 20), parent='0xc', seq=1),
                          ctl('0xa1', 'CEikLabel', (0, 0), (115, 20), flags=0x30, parent='0xa', texts=['OK'], seq=1),
                          ctl('0xb', 'CEikCbaButton', (124, 3), (115, 20), parent='0xc', seq=2),
                          ctl('0xb1', 'CEikLabel', (0, 0), (115, 20), parent='0xb', texts=['Cancel'], seq=2)],
             'stack': [{'ctl': '0xc'}]}
        roots, nodes = U.build_tree(d)
        self.assertEqual(U.summary(d, roots, nodes)['softkeys'], {'left': None, 'right': 'Cancel'})

    def test_list_summary_and_grid_flag(self):
        d = {'controls': [ctl('0x1', 'CXn3DGrid', (0, 54), (240, 240), flags=0x1c, kind='CAknGrid',
                              list={'current': 4, 'items': ['1\tContacts', '1\tMessaging', '\tMaps', '\tMusic', '\tGallery']})],
             'stack': []}
        roots, nodes = U.build_tree(d)
        s = U.summary(d, roots, nodes)
        self.assertEqual((s['list']['selected'], s['list']['count'], s['list']['grid']), ('Gallery', 5, True))
        self.assertEqual(U.target_list(s), ('CXn3DGrid', ['Contacts', 'Messaging', 'Maps', 'Music', 'Gallery'], 4, True))

    def test_item_label_and_find(self):
        self.assertEqual(U.item_label('\tAutomatic redial\t\tOff'), 'Automatic redial | Off')
        self.assertEqual(U.item_label('0\tNew message'), 'New message')
        items = ['General', 'Phone', 'Connection', 'Applications']
        self.assertEqual(U.find_item(items, 'phone'), 1)
        self.assertEqual(U.find_item(items, 'App'), 3)
        self.assertEqual(U.find_item(items, 'nect'), 2)
        self.assertIsNone(U.find_item(items, 'Camera'))


class ProbeParseTest(unittest.TestCase):
    """the in-app probe's lines (src/n95apps/uiprobe/uiprobe.cpp) -> the same snapshot the gdb reader gives"""
    LINES = ['G\trecovered', 'A\tZ:\\\\sys\\\\bin\\\\GSApp.exe\t0x100058ec\t5CGSUi',
             'C\t0\t-1\t22CGSCallPluginContainer\t-\t0\t54\t240\t241\t0x1c',
             'C\t1\t0\t23CAknSettingStyleListBox\tCEikListBox\t0\t0\t240\t241\t0x18',
             'L\t1\t2\t1\t0\t1\t0\t29CAknFilteredTextListBoxModel',
             'I\t1\t0\t\\tSend my caller ID\\t\\tSet by network', 'I\t1\t1\t\\tAutomatic redial\\t\\tOff',
             'C\t2\t-1\t7CEikCba\tCEikCba\t0\t295\t240\t25\t0x14',
             'C\t3\t2\t13CEikCbaButton\tCEikCbaButton\t1\t3\t115\t20\t0x10', 'T\t3\tOptions',
             'C\t4\t2\t13CEikCbaButton\tCEikCbaButton\t124\t3\t115\t20\t0x10', 'T\t4\tBack',
             'C\t5\t-1\t9CEikEdwin\tCEikEdwin\t15\t50\t202\t22\t0x1c', 'E\t5\tQ\\u00e9mbian',
             'S\t2\t0']

    def test_parse_and_summary(self):
        d = U.parse_probe(self.LINES)
        self.assertEqual((d['exe'], d['uid3'], d['appui_class']), ('Z:\\sys\\bin\\GSApp.exe', '0x100058ec', 'CGSUi'))
        roots, nodes = U.build_tree(d)
        self.assertEqual(nodes['1']['rect'], [0, 54, 240, 241])     # inside its window (the container)
        s = U.summary(d, roots, nodes)
        self.assertEqual(s['list']['selected'], 'Automatic redial | Off')
        self.assertEqual(s['softkeys'], {'left': 'Options', 'right': 'Back'})
        self.assertEqual(s['editor'], 'Q\u00e9mbian')
        self.assertNotIn('bad_lines', d)

    def test_unescape(self):
        self.assertEqual(U.unescape('a\\tb\\\\c\\u2029d\\n'), 'a\tb\\c\u2029d\n')
        self.assertEqual(U.unescape('end\\'), 'end\\')

    def test_demangle_name(self):
        self.assertEqual(U.demangle_name('18CAknTitlePaneLabel'), 'CAknTitlePaneLabel')
        self.assertEqual(U.demangle_name('N9CCoeAppUi6CExtraE'), 'CCoeAppUi::CExtra')
        self.assertEqual(U.demangle_name('?'), '?')

    def test_cut_dump_and_bad_lines_are_reported(self):
        d = U.parse_probe(['C\t0\t-1\t5CFooX\t-\t0\t0', 'X\tbuffer full'])
        self.assertEqual(d['truncated'], 'buffer full')
        self.assertEqual(len(d['bad_lines']), 1)

    def test_zero_size_parent_does_not_hide_its_cba(self):
        """2026-11-19: the keypad-lock screen showed no softkeys: akncapserver's CAknKeyLockControl is 0x0 and
        its CBA (a component) is on screen; a zero size hides only the control itself"""
        d = U.parse_probe(['C\t0\t-1\t18CAknKeyLockControl\t-\t0\t0\t0\t0\t0x14',
                           'C\t1\t0\t7CEikCba\tCEikCba\t0\t295\t240\t25\t0x14',
                           'C\t2\t1\t13CEikCbaButton\tCEikCbaButton\t1\t3\t115\t20\t0x10', 'T\t2\tUnlock'])
        roots, nodes = U.build_tree(d)
        self.assertFalse(nodes['0']['visible'])
        self.assertEqual(U.summary(d, roots, nodes)['softkeys']['left'], 'Unlock')


class PicturesAndDrawnTextTest(unittest.TestCase):
    LINES = ['A\tZ:\\\\sys\\\\bin\\\\Idle.exe\t0x101fd64c\t10CIdleAppUi',
             'C\t0\t-1\t13CIdleMainView\t-\t0\t54\t240\t266\t0x1c',
             'C\t1\t0\t19CShortcutPluginGrid\tCAknGrid\t4\t2\t232\t32\t0x18',
             'L\t1\t2\t1\t0\t1\t7\t9CAknGridM', 'I\t1\t0\t0', 'I\t1\t1\t1',
             'B\t1\ticon[0]\t32\t32\t12\t0x1a2b\tz:\\\\resource\\\\apps\\\\phonebook_aif.mif\t16384\t16385',
             'B\t1\ticon[1]\t32\t32\t12\t0xbeef\t\t-1\t-1',
             'C\t2\t0\t9CEikLabel\tCEikLabel\t23\t77\t212\t17\t0x10',
             'C\t3\t0\t9CEikLabel\tCEikLabel\t23\t96\t212\t17\t0x10',
             'W\t0\t23\t77\t212\t17\tNo calendar entries today', 'W\t0\t30\t200\t0\t0\t12:05',
             'W\t-1\t0\t0\t0\t0\tnowhere']

    def setUp(self):
        self.saved = dict(U.ICON_LABELS)
        U.ICON_LABELS.clear()
        U.ICON_LABELS.update({'app:phonebook': 'Contacts', '32x32:0000beef': 'Mystery', 'avkon2.mif#16414': 'qgn_x'})

    def tearDown(self):
        U.ICON_LABELS.clear()
        U.ICON_LABELS.update(self.saved)

    def test_icon_only_grid_items_are_named_by_their_icons(self):
        """the idle screen's shortcut grid has items "0".."6" (icon indices): named by the icon's app"""
        d = U.parse_probe(self.LINES)
        roots, nodes = U.build_tree(d)
        s = U.summary(d, roots, nodes)
        self.assertEqual(s['list']['items'], ['Contacts', 'Mystery'])
        self.assertEqual(U.icon_id(nodes['1']['icons'][0]), 'phonebook_aif.mif#16384')
        self.assertEqual(U.icon_id(nodes['1']['icons'][1]), '32x32:0000beef')

    def test_drawn_text_lands_on_the_control_under_it(self):
        """the screensaver/calculator case: text drawn straight into a window, no member string"""
        d = U.parse_probe(self.LINES)
        roots, nodes = U.build_tree(d)
        self.assertEqual([t['text'] for t in nodes['2']['drawn']], ['No calendar entries today'])
        self.assertEqual(nodes['0']['drawn'][0]['text'], '12:05')        # a point outside both labels
        s = U.summary(d, roots, nodes)
        self.assertEqual(s['screen_text'], ['No calendar entries today', '12:05'])

    def test_paths_are_stable_names(self):
        d = U.parse_probe(self.LINES)
        roots, nodes = U.build_tree(d)
        self.assertEqual(nodes['1']['path'], '/CIdleMainView/CShortcutPluginGrid')
        self.assertEqual(nodes['3']['path'], '/CIdleMainView/CEikLabel[1]')

    def test_avkon_labels_from_the_sdk_header(self):
        self.assertEqual(U.icon_label({'file': 'z:\\resource\\apps\\avkon2.mif', 'bmp': 16414, 'size': [1, 1], 'hash': '0'}), 'qgn_x')


class SelectTest(unittest.TestCase):
    def setUp(self):
        self.saved = U.capture, U.press, U.time.sleep
        U.time.sleep = lambda s: None

    def tearDown(self):
        U.capture, U.press, U.time.sleep = self.saved

    def fake(self, items, start, lose_first=0, grid=False):
        st = {'cur': start, 'lost': lose_first, 'keys': []}

        def capture(base, tid=None):
            return {'summary': {'list': {'class': 'L', 'items': items, 'current': st['cur'], 'grid': grid}}}

        def press(base, keys, delay=0.15):
            st['keys'] += keys
            if st['lost'] > 0:
                st['lost'] -= 1
                return
            for k in keys:
                if k in ('down', 'right'):
                    st['cur'] = (st['cur'] + 1) % len(items)
                elif k in ('up', 'left'):
                    st['cur'] = (st['cur'] - 1) % len(items)
        U.capture, U.press = capture, press
        return st

    def test_select_moves_and_opens(self):
        st = self.fake(['General', 'Phone', 'Connection', 'Applications'], 0)
        U.select([], 'Connection', open_item=True)
        self.assertEqual(st['keys'], ['down', 'down', 'ok'])

    def test_select_grid_uses_left_right(self):
        st = self.fake(['Contacts', 'Messaging', 'Maps', 'Music', 'Gallery'], 4, grid=True)
        U.select([], 'Messaging')
        self.assertEqual(st['keys'], ['left'] * 3)

    def test_select_retries_when_a_starting_app_drops_keys(self):
        """2026-11-19: `select Phone` right after Settings opened failed: GSApp, still starting, dropped the
        presses; select backs off and presses again instead of giving up"""
        st = self.fake(['General', 'Phone', 'Connection'], 0, lose_first=3)
        s = U.select([], 'Phone')
        self.assertEqual(s['list']['current'], 1)

    def test_select_grid_with_known_columns_moves_by_rows(self):
        """the probe reports the grid's columns: Gallery (4) -> Applications (10) in a 3-column grid is down, down"""
        st = self.fake(['Contacts', 'Messaging', 'Maps', 'Music', 'Gallery', 'Video', 'Search', 'Web', 'Games',
                        'Tools', 'Applications', 'Download'], 4, grid=3)
        moves = []

        def press(base, keys, delay=0.15):
            moves.extend(keys)
            for k in keys:
                st['cur'] += {'down': 3, 'up': -3, 'right': 1, 'left': -1}.get(k, 0)
        U.press = press
        U.select([], 'Applications')
        self.assertEqual(moves, ['down', 'down'])

    def test_select_waits_for_a_busy_app(self):
        """2026-11-19: `select Phone` in Settings failed with "no reply": GSApp, loading its plugins, reached its
        scheduler after the agent's 3 s; select retries the snapshot instead of failing"""
        st = self.fake(['General', 'Phone'], 0)
        good = U.capture
        calls = {'n': 0}

        def capture(base, tid=None):
            calls['n'] += 1
            return {'error': 'no reply'} if calls['n'] <= 2 else good(base)
        U.capture = capture
        s = U.select([], 'Phone')
        self.assertEqual(s['list']['current'], 1)

    def test_select_open_settles_after_moving(self):
        """2026-11-19: `select Settings --open` highlighted Settings but the Menu's 3D grid, still animating
        the last move, dropped OK; select waits 0.3 s before OK when it moved"""
        st = self.fake(['Tools', 'Settings'], 0)
        slept = []
        U.time.sleep = lambda s: slept.append(s)
        U.select([], 'Settings', open_item=True)
        self.assertEqual(st['keys'], ['down', 'ok'])
        self.assertIn(0.3, slept)

    def test_select_unknown_item_says_what_is_there(self):
        self.fake(['General', 'Phone'], 0)
        with self.assertRaisesRegex(RuntimeError, "'Camera' not among"):
            U.select([], 'Camera')


if __name__ == '__main__':
    unittest.main()
