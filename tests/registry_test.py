#!/usr/bin/env python3
"""tools/registry.py: store-integrity guarantees (plain unittest): tests/registry_test.py

Written for the 2026-11-15 review, which found that several advertised guarantees were not implemented. Each
test here reproduces one of those failures against a temporary store:

  1. seed twice does not overwrite operational edits, and does not duplicate source rows;
  2. ingest twice is identical - no duplicated edges - and destinations resolve through the alias resolver
     (a patch to fmradioaudiocontrol.dll must land on bt-fm, not on a component invented for the file name);
  3. ingest of a changed fixture retires stale generated facts;
  4. task lifecycle: blocked tasks are not claimable, dependencies gate readiness, expired leases are reaped
     and claimable again, completion needs the lease token and revision, and two connections cannot both win;
  5. a finding submitted against a task is retrievable from the task *and* from the component, with scope;
  6. --device selects the target everywhere, and an index labelled with another device is refused;
  7. evidence honesty: verified needs resolving evidence, reviews are events, superseding validates subject and
     scope; artifact versions are kept;
  8. backup/restore round-trips, and the export contains every fact (including superseded claims).
"""
import json, os, sqlite3, subprocess, sys, tempfile, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import registry as R

DEV = 'nokia-808-rm807'
DEV_DIR = os.path.join(ROOT, 'build', 'roms', DEV, 'RM-807_113.010.1508_04.01+C00.01')
HAVE_PROFILE = os.path.isdir(os.path.join(ROOT, 'models', DEV))

FIXTURE = {
    'schema': 2, 'device': DEV, 'firmware': 'RM-807/113.010.1508/04.01+C00.01', 'built': '2026-11-15T00:00:00',
    'dlls': {'gce.dll': {'1': None}, 'surfacemanager.dll': {'2': '_ZN15RSurfaceManager11OpenSurfaceERK10TSurfaceId'}},
    'modules': {
        'sys/bin/gcerenderstage.dll': {
            'origin': 'rofs1@0x5c2dcc', 'size': 5333, 'crc32': 1, 'codesize': 6408, 'uid3': '0x20031cb9',
            'base': '0x8000', 'imports': [['gce.dll', 1, 0xc8, 'code'],
                                          ['surfacemanager.dll', 2, 0x1a8, 'code']]},
        'sys/bin/surfacemanager.dll': {
            'origin': 'xip@0x80b33408', 'size': 780, 'crc32': 4, 'codesize': 780, 'uid3': '0x10286a9d',
            'base': '0x80b33408', 'imports': []},
        'sys/bin/fmradioaudiocontrol.dll': {
            'origin': 'rofs1@0x2', 'size': 111, 'crc32': 3, 'codesize': 200, 'uid3': '0x2000aaaa',
            'base': '0x8000', 'imports': []},
        'sys/bin/unparsed.dll': {'origin': 'rofs1@0x1', 'size': 999, 'crc32': 2, 'codesize': None,
                                 'error': 'ValueError: not an E32 image', 'imports': []},
        # 2026-11-17 review: a component with several binaries (avkon: avkon.dll + akn*.dll), a module with no
        # component, and a path in two ROM layers
        'Sys/Bin/AknKeyRotator.dll': {'origin': 'xip@0x80100000', 'size': 64, 'crc32': 5, 'codesize': 64,
                                      'uid3': '0x1', 'base': '0x80100000', 'imports': []},
        'sys/bin/avkon.dll': {'origin': 'rofs1@0x10', 'size': 64, 'crc32': 6, 'codesize': 64, 'uid3': '0x2',
                              'base': '0x8000', 'imports': []},
        'sys/bin/someapp.exe': {'origin': 'rofs1@0x20', 'size': 64, 'crc32': 7, 'codesize': 64, 'uid3': '0x3',
                                'base': '0x8000', 'imports': [['avkon.dll', 10, 0x10, 'code'],
                                                              ['avkon.dll', 11, 0x18, 'code'],
                                                              ['sqldb.dll', 1, 0x20, 'code']]},
        'Sys/Bin/sqldb.dll': {'origin': 'xip@0x80200000', 'size': 64, 'crc32': 8, 'codesize': 64, 'uid3': '0x4',
                              'base': '0x80200000', 'imports': [], 'shadowed_by': 'rofs2@0x30'},
        'sys/bin/sqldb.dll': {'origin': 'rofs2@0x30', 'size': 64, 'crc32': 9, 'codesize': 64, 'uid3': '0x4',
                              'base': '0x8000', 'imports': []},
    }}


@unittest.skipUnless(HAVE_PROFILE, 'needs the device profile')
class StoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.db = os.path.join(self.tmp, 'registry.sqlite3')
        os.environ['REGISTRY_DB'] = self.db
        os.environ['SYBORG_DEVICE'] = DEV
        self.target = R.resolve_target(DEV)
        self.target['store'] = self.db
        self.fixture = os.path.join(self.tmp, 'imports.json')
        json.dump(FIXTURE, open(self.fixture, 'w'))
        self.conn = R.connect(self.db)
        R.seed(self.conn, self.target, quiet=True)
        R.ingest(self.conn, self.target, quiet=True, index_path=self.fixture)

    def tearDown(self):
        self.conn.close()
        os.environ.pop('REGISTRY_DB', None)

    # ---------------------------------------------------------------- 1. seed
    def test_seed_twice_is_a_noop_and_keeps_operational_edits(self):
        before = self.conn.execute('SELECT count(*) c FROM source').fetchone()['c']
        self.conn.execute("UPDATE component SET notes='OPERATIONAL EDIT' WHERE key='qt'")
        self.conn.execute("UPDATE task SET owner='agent-x', state='claimed', lease_token='t',"
                          " lease_until='2999-01-01T00:00:00Z' WHERE key='haptics-packetizer-encoding'")
        self.conn.commit()
        R.seed(self.conn, self.target, quiet=True)                      # unchanged files: nothing happens
        self.assertEqual(self.conn.execute('SELECT count(*) c FROM source').fetchone()['c'], before,
                         'sources must not be duplicated by a second seed')
        self.assertEqual(self.conn.execute("SELECT notes FROM component WHERE key='qt'").fetchone()['notes'],
                         'OPERATIONAL EDIT')
        R.seed(self.conn, self.target, migrate=True, quiet=True)         # explicit migration
        self.assertEqual(self.conn.execute("SELECT owner FROM task WHERE key='haptics-packetizer-encoding'"
                                           ).fetchone()['owner'], 'agent-x',
                         'migration must not reset a lease')
        self.assertEqual(self.conn.execute("SELECT count(*) c FROM source").fetchone()['c'], before)

    def test_seed_rejects_dangling_references(self):
        bad = os.path.join(self.tmp, 'bad.toml')
        open(bad, 'w').write('[[task]]\nkey="t"\ntitle="T"\ncomponents=["nope"]\n')
        orig, R.seed_files = R.seed_files, lambda: [bad]
        try:
            with self.assertRaises(SystemExit):
                R.seed(self.conn, self.target, quiet=True)
        finally:
            R.seed_files = orig

    # ---------------------------------------------------------------- 2. ingest idempotence
    def snapshot(self):
        return {t: self.conn.execute('SELECT count(*) c FROM "%s"' % t).fetchone()['c']
                for t in ('edge', 'binary', 'artifact', 'source', 'mention')}

    def test_ingest_twice_gives_identical_results(self):
        first = self.snapshot()
        R.ingest(self.conn, self.target, quiet=True, index_path=self.fixture)
        self.assertEqual(self.snapshot(), first, 'repeated ingest must not duplicate edges/artifacts')

    def test_patch_edge_lands_on_the_aliased_component(self):
        row = self.conn.execute("SELECT dst_component FROM edge WHERE type='byte-patch'"
                                ' AND detail=?', ('fmradioaudiocontrol.dll',)).fetchone()
        self.assertIsNotNone(row)
        bt_fm = self.conn.execute("SELECT id FROM component WHERE key='bt-fm'").fetchone()['id']
        self.assertEqual(row['dst_component'], bt_fm,
                         'a patch to fmradioaudiocontrol.dll belongs to bt-fm, not to a component named after the file')
        self.assertIsNone(self.conn.execute("SELECT id FROM component WHERE key='fmradioaudiocontrol'"
                                            ).fetchone())

    def test_import_destinations_resolve_to_firmware_binaries(self):
        edge = self.conn.execute("SELECT dst FROM edge WHERE type='dll-import' AND detail LIKE 'surfacemanager%'"
                                 ).fetchone()
        self.assertIsNotNone(edge['dst'], 'destination must resolve when the DLL is in this firmware')
        self.assertIsNone(self.conn.execute("SELECT dst FROM edge WHERE type='dll-import' AND detail LIKE"
                                            " 'gce%'").fetchone()['dst'],
                          'a DLL that is not a module of this firmware keeps a NULL destination (and is counted)')

    def test_missing_destinations_are_counted_not_silent(self):
        data = R.export(self.conn, os.path.join(self.tmp, 'out'), self.target)
        self.assertGreaterEqual(data['edges_missing_dst'], 1)

    # ---------------------------------------------------------------- 3. reconciliation
    def test_changed_fixture_retires_stale_facts(self):
        changed = dict(FIXTURE)
        changed['modules'] = {k: v for k, v in FIXTURE['modules'].items() if 'unparsed' not in k}
        p = os.path.join(self.tmp, 'imports2.json')
        json.dump(changed, open(p, 'w'))
        R.ingest(self.conn, self.target, quiet=True, index_path=p)
        self.assertEqual(self.conn.execute("SELECT stale FROM binary WHERE path LIKE '%unparsed%'"
                                           ).fetchone()['stale'], 1, 'vanished binaries are retired, not left live')
        self.assertEqual(self.conn.execute("SELECT count(*) c FROM edge e JOIN binary b ON b.id=e.src"
                                           " WHERE b.stale=1").fetchone()['c'], 0)

    # ---------------------------------------------------------------- 4. task lifecycle
    def test_blocked_task_is_not_claimable(self):
        with self.assertRaises(SystemExit):
            R.claim_task(self.conn, 'haptics-packetizer-encoding', 'agent-a', '1h')

    def test_dependencies_gate_readiness(self):
        rows = {r['key']: r for r in R.q_tasks(self.conn)}
        self.assertFalse(rows['tsy-request-oracle']['ready'])
        self.assertIn('modem-lifecycle-probe', rows['tsy-request-oracle']['ready_reason'])
        self.assertTrue(rows['modem-lifecycle-probe']['ready'])

    def test_expired_lease_is_reaped_and_reclaimable(self):
        first = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        self.assertTrue(first['token'])
        self.conn.execute("UPDATE task SET lease_until='2000-01-01T00:00:00Z' WHERE key='modem-lifecycle-probe'")
        self.conn.commit()
        ready = {r['key']: r for r in R.q_tasks(self.conn, ready_only=True)}
        self.assertIn('modem-lifecycle-probe', ready, 'an expired claim must return to the pool')
        second = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-b', '1h')
        self.assertEqual(second['agent'], 'agent-b')

    def test_two_connections_cannot_both_claim(self):
        other = R.connect(self.db)
        try:
            R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
            with self.assertRaises(SystemExit):
                R.claim_task(other, 'modem-lifecycle-probe', 'agent-b', '1h')
        finally:
            other.close()

    def test_completion_needs_token_and_evidence(self):
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        with self.assertRaises(SystemExit):
            R.complete(self.conn, 'modem-lifecycle-probe', 'docs/tools.md', token='wrong')
        ev = os.path.join(self.tmp, 'run.log')
        open(ev, 'w').write('passed')
        with self.assertRaises(SystemExit):
            R.complete(self.conn, 'modem-lifecycle-probe', '', token=c['token'])
        R.complete(self.conn, 'modem-lifecycle-probe', ev, token=c['token'])
        self.assertEqual(self.conn.execute("SELECT state FROM task WHERE key='modem-lifecycle-probe'"
                                           ).fetchone()['state'], 'done')

    def test_reopen_returns_a_task_to_the_pool_with_history(self):
        """A task closed on premature evidence must come back with the reason recorded, not silently vanish."""
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        ev = os.path.join(self.tmp, 'run.log')
        open(ev, 'w').write('passed')
        R.complete(self.conn, 'modem-lifecycle-probe', ev, token=c['token'])
        R.reopen(self.conn, 'modem-lifecycle-probe', 'claim 17 was incomplete')
        row = self.conn.execute("SELECT state, owner, lease_token FROM task WHERE key='modem-lifecycle-probe'"
                                ).fetchone()
        self.assertEqual((row['state'], row['owner'], row['lease_token']), ('ready', None, None))
        notes = [r['note'] for r in self.conn.execute(
            'SELECT note FROM task_state WHERE state=?', ('reopened',))]
        self.assertIn('claim 17 was incomplete', notes)
        with self.assertRaises(SystemExit):
            R.reopen(self.conn, 'modem-lifecycle-probe', '')

    def test_stale_revision_cannot_complete(self):
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        self.conn.execute("UPDATE task SET revision=revision+1 WHERE key='modem-lifecycle-probe'")
        self.conn.commit()
        with self.assertRaises(SystemExit):
            R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-b', '1h')

    # ---------------------------------------------------------------- 5. claims wired to tasks+components
    def test_finding_is_visible_from_task_and_component(self):
        c = R.claim_task(self.conn, 'wserv-surface-update-contract', 'agent-a', '1h')
        out = R.submit(self.conn, 'wserv-surface-update-contract',
                       'NotifyContentReady completes after the update count is reached.',
                       ['doc=docs/registry/README.md'], 'nokia-808-rm807/113.010.1508', 'verified',
                       'agent-a', token=c['token'])
        by_task = R.context(self.conn, 'wserv-surface-update-contract')
        self.assertIn(out['claim'], [x['id'] for x in by_task['claims']])
        self.assertTrue(by_task['components'], 'a task must know its components')
        self.assertTrue(any(c['binaries'] for c in by_task['components']),
                        'the task briefing must reach the binaries of its components')
        self.assertTrue(by_task['contracts'], 'the task briefing must include its contracts')
        small = R.context(self.conn, 'wserv-surface-update-contract', budget=2500)
        self.assertIn(out['claim'], [x['id'] for x in small['claims']],
                      'a tight budget trims detail, never the claims')
        self.assertTrue(small['contracts'], 'a tight budget keeps the contracts')
        by_comp = R.context(self.conn, 'gce-render-stage')
        self.assertIn(out['claim'], [x['id'] for x in by_comp['claims']])
        claim = self.conn.execute('SELECT task_id, subject_component, scope_device, scope_firmware FROM claim'
                                  ' WHERE id=?', (out['claim'],)).fetchone()
        self.assertIsNotNone(claim['task_id'])
        self.assertIsNotNone(claim['subject_component'])
        # one firmware naming: the bare version becomes the profile's firmware key, as on binaries
        self.assertEqual((claim['scope_device'], claim['scope_firmware']),
                         ('nokia-808-rm807', 'RM-807/113.010.1508/04.01+C00.01'))

    # ---------------------------------------------------------------- 6. explicit device
    def test_other_device_index_is_refused(self):
        wrong = dict(FIXTURE)
        wrong['device'] = 'nokia-n95-8gb-rm320'
        p = os.path.join(self.tmp, 'wrong.json')
        json.dump(wrong, open(p, 'w'))
        with self.assertRaises(SystemExit):
            R.ingest(self.conn, self.target, quiet=True, index_path=p)

    def test_cli_device_selects_the_target(self):
        env = dict(os.environ, SYBORG_DEVICE='nokia-n95-8gb-rm320', REGISTRY_DB=self.db)
        out = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'registry.py'), 'search',
                              'gce-render-stage', '--device', DEV, '--json'],
                             capture_output=True, text=True, env=env)
        self.assertEqual(out.returncode, 0, out.stderr)
        rows = json.loads(out.stdout)
        self.assertTrue(rows and rows[0]['key'] == 'gce-render-stage')

    # ---------------------------------------------------------------- 7. evidence honesty
    def test_verified_requires_resolving_evidence(self):
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        with self.assertRaises(SystemExit):
            R.submit(self.conn, 'modem-lifecycle-probe', 'trust me', [], 'nokia-808-rm807/*', 'verified',
                     'a', token=c['token'])
        with self.assertRaises(SystemExit):
            R.submit(self.conn, 'modem-lifecycle-probe', 'trust me', ['doc=does/not/exist.md'],
                     'nokia-808-rm807/*', 'verified', 'a', token=c['token'])
        with self.assertRaises(SystemExit):
            R.submit(self.conn, 'modem-lifecycle-probe', 'no scope', ['doc=docs/tools.md'], '', 'inferred',
                     'a', token=c['token'])

    def test_review_is_an_event_not_an_overwrite(self):
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        out = R.submit(self.conn, 'modem-lifecycle-probe', 'claim', ['doc=docs/tools.md'],
                       'nokia-808-rm807/*', 'inferred', 'a', token=c['token'])
        R.review(self.conn, out['claim'], 'disputed', 'reviewer-1', 'ordering unclear')
        R.review(self.conn, out['claim'], 'accepted', 'reviewer-2', 'probe agrees')
        rows = self.conn.execute('SELECT verdict, reviewer FROM review WHERE claim_id=? ORDER BY at',
                                 (out['claim'],)).fetchall()
        self.assertEqual([(r['verdict'], r['reviewer']) for r in rows],
                         [('disputed', 'reviewer-1'), ('accepted', 'reviewer-2')])

    def test_supersede_validates_subject_and_scope(self):
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        first = R.submit(self.conn, 'modem-lifecycle-probe', 'old', ['doc=docs/tools.md'],
                         'nokia-808-rm807/113.010.1508', 'inferred', 'a', token=c['token'])
        with self.assertRaises(SystemExit):
            R.submit(self.conn, 'modem-lifecycle-probe', 'new', ['doc=docs/tools.md'],
                     'nokia-n95-8gb-rm320/35.0.001', 'inferred', 'a', token=c['token'],
                     supersedes=first['claim'])
        second = R.submit(self.conn, 'modem-lifecycle-probe', 'new', ['doc=docs/tools.md'],
                          'nokia-808-rm807/113.010.1508', 'inferred', 'a', token=c['token'],
                          supersedes=first['claim'])
        self.assertEqual(self.conn.execute('SELECT status FROM claim WHERE id=?', (first['claim'],)).fetchone()['status'],
                         'superseded')
        self.assertEqual(self.conn.execute('SELECT supersedes FROM claim WHERE id=?', (second['claim'],)
                                           ).fetchone()['supersedes'], first['claim'])

    # ---------------------------------------------------------------- 2026-11-17 review: one test per defect
    def bid(self, path):
        return self.conn.execute('SELECT id FROM binary WHERE path=?', (path,)).fetchone()['id']

    def test_imports_land_on_the_named_binary_not_the_components_first(self):
        """bug: every import of avkon.dll landed on AknKeyRotator.dll (the avkon component's first binary)"""
        rows = self.conn.execute("SELECT dst, n, ordinal FROM edge WHERE type='dll-import' AND src=?",
                                 (self.bid('sys/bin/someapp.exe'),)).fetchall()
        got = {r['dst']: r['n'] for r in rows}
        self.assertEqual(got.get(self.bid('sys/bin/avkon.dll')), 2, 'one edge per module pair, n = ordinals')
        self.assertNotIn(self.bid('Sys/Bin/AknKeyRotator.dll'), got)
        self.assertTrue(all(r['ordinal'] is None for r in rows), 'no per-slot rows (the import index has them)')

    def test_import_goes_to_the_copy_the_loader_gets(self):
        """a path in XIP and ROFS2: the ROFS2 copy is loaded, the XIP one is marked shadowed"""
        got = {r['dst'] for r in self.conn.execute("SELECT dst FROM edge WHERE type='dll-import' AND src=?",
                                                   (self.bid('sys/bin/someapp.exe'),))}
        self.assertIn(self.bid('sys/bin/sqldb.dll'), got)
        self.assertNotIn(self.bid('Sys/Bin/sqldb.dll'), got)
        self.assertEqual(self.conn.execute("SELECT shadowed_by FROM binary WHERE path='Sys/Bin/sqldb.dll'"
                                           ).fetchone()['shadowed_by'], 'rofs2@0x30')

    def test_context_shows_import_edges(self):
        """bug: import edges had NULL src/dst components, so a component briefing never showed them"""
        ctx = R.context(self.conn, 'avkon', budget=20000)
        ib = ctx['components'][0]['edges']['imported_by']
        self.assertEqual((ib['modules'], ib['ordinals']), (1, 2))

    def test_no_component_is_invented_per_module(self):
        """bug: ingest made one 'undecided' component per module (4,286 on the 808)"""
        self.assertIsNone(self.conn.execute("SELECT component_id FROM binary WHERE path='sys/bin/someapp.exe'"
                                            ).fetchone()['component_id'])
        self.assertIsNone(self.conn.execute("SELECT id FROM component WHERE key='someapp'").fetchone())
        data = R.export(self.conn, os.path.join(self.tmp, 'out'), self.target)
        self.assertGreaterEqual(data['unassigned_binaries'], 1)

    def test_mutations_need_the_lease_token(self):
        """bug: without --token anyone could submit to, record tests on and complete another agent's task"""
        R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        with self.assertRaises(SystemExit):
            R.submit(self.conn, 'modem-lifecycle-probe', 'x', ['doc=docs/tools.md'], 'nokia-808-rm807/*',
                     'verified', 'intruder')
        with self.assertRaises(SystemExit):
            R.record_test(self.conn, 'modem-lifecycle-probe', 'cfg', 'docs/tools.md', 'passed')
        with self.assertRaises(SystemExit):
            R.complete(self.conn, 'modem-lifecycle-probe', 'docs/tools.md')

    def test_superseded_claim_cannot_be_revived_or_forked(self):
        """bug: accepting a superseded claim set it back to 'accepted'; superseding it twice forked history"""
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        a = R.submit(self.conn, 'modem-lifecycle-probe', 'old', ['doc=docs/tools.md'], 'nokia-808-rm807/*',
                     'inferred', 'a', token=c['token'])
        R.submit(self.conn, 'modem-lifecycle-probe', 'new', ['doc=docs/tools.md'], 'nokia-808-rm807/*',
                 'inferred', 'a', token=c['token'], supersedes=a['claim'])
        with self.assertRaises(SystemExit):
            R.review(self.conn, a['claim'], 'accepted')
        with self.assertRaises(SystemExit):
            R.submit(self.conn, 'modem-lifecycle-probe', 'fork', ['doc=docs/tools.md'], 'nokia-808-rm807/*',
                     'inferred', 'a', token=c['token'], supersedes=a['claim'])
        self.assertEqual(self.conn.execute('SELECT status FROM claim WHERE id=?', (a['claim'],)
                                           ).fetchone()['status'], 'superseded')

    def test_rerecorded_test_keeps_its_own_history(self):
        """bug: on a re-record lastrowid was stale and the version row went to an unrelated artifact"""
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        log = os.path.join(self.tmp, 'serial.log')
        open(log, 'w').write('one')
        a1 = R.record_test(self.conn, 'modem-lifecycle-probe', 'cfg', log, 'failed', token=c['token'])
        open(log, 'w').write('two, longer')
        a2 = R.record_test(self.conn, 'modem-lifecycle-probe', 'cfg', log, 'passed', token=c['token'])
        self.assertEqual(a1['artifacts'], a2['artifacts'])
        aid = a1['artifacts'][0]
        self.assertEqual(self.conn.execute('SELECT count(*) c FROM artifact_version WHERE artifact_id=?',
                                           (aid,)).fetchone()['c'], 2)
        self.assertEqual(self.conn.execute('SELECT bytes FROM artifact WHERE id=?', (aid,)).fetchone()['bytes'],
                         len('two, longer'))

    def test_parse_error_is_not_stale(self):
        """bug: a parse error set stale=1 and the export (stale=0 only) never listed it"""
        self.assertEqual(self.conn.execute("SELECT stale FROM binary WHERE path='sys/bin/unparsed.dll'"
                                           ).fetchone()['stale'], 0)
        data = R.export(self.conn, os.path.join(self.tmp, 'out'), self.target)
        self.assertIn('sys/bin/unparsed.dll', [r['path'] for r in data['parse_errors']])

    def test_migrate_reports_what_it_rewrote(self):
        """bug: seed --migrate always reported 0 migrated"""
        rep = R.seed(self.conn, self.target, migrate=True, quiet=True)
        self.assertGreater(rep['migrated'], 0)

    def test_seed_records_each_components_own_file(self):
        """bug: every seeded component said seeded_from = contracts.toml (the loop's last file)"""
        self.assertEqual(self.conn.execute("SELECT seeded_from FROM component WHERE key='qt'").fetchone()[0],
                         'docs/registry/components.toml')

    def test_reap_counts_reaped_tasks_and_zero_lease_refused(self):
        R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        self.conn.execute("UPDATE task SET lease_until='2000-01-01T00:00:00Z' WHERE key='modem-lifecycle-probe'")
        self.assertEqual(R.reap_expired(self.conn), 1)
        self.assertEqual(R.reap_expired(self.conn), 0)
        with self.assertRaises(SystemExit):
            R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '0h')

    def test_schema4_store_migrates_keeping_claims(self):
        """the live store (schema 4) holds 59 agent claims and task history: migration keeps them, drops the
        invented components and the per-slot edges"""
        c = R.claim_task(self.conn, 'modem-lifecycle-probe', 'agent-a', '1h')
        cl = R.submit(self.conn, 'modem-lifecycle-probe', 'kept', ['doc=docs/tools.md'],
                      'nokia-808-rm807/113.010.1508', 'inferred', 'a', token=c['token'])
        x = self.conn
        x.execute("UPDATE claim SET scope_firmware='113.010.1508' WHERE id=?", (cl['claim'],))
        auto = x.execute("INSERT INTO component(key,title) VALUES('someapp','someapp.exe')").lastrowid
        x.execute("UPDATE binary SET component_id=? WHERE path='sys/bin/someapp.exe'", (auto,))
        x.execute("INSERT INTO edge(key,type,src,ordinal,offset) VALUES('slot1','dll-import',1,5,16)")
        x.execute('ALTER TABLE edge DROP COLUMN n')
        x.execute('ALTER TABLE binary DROP COLUMN shadowed_by')
        x.execute("UPDATE meta SET v='4' WHERE k='schema'")
        x.close()
        self.conn = R.connect(self.db)
        x = self.conn
        self.assertEqual(x.execute("SELECT v FROM meta WHERE k='schema'").fetchone()[0], str(R.SCHEMA))
        self.assertIsNone(x.execute("SELECT id FROM component WHERE key='someapp'").fetchone())
        self.assertIsNone(x.execute("SELECT component_id FROM binary WHERE path='sys/bin/someapp.exe'"
                                    ).fetchone()[0])
        self.assertIsNone(x.execute("SELECT 1 FROM edge WHERE key='slot1'").fetchone())
        row = x.execute('SELECT statement, scope_firmware FROM claim WHERE id=?', (cl['claim'],)).fetchone()
        self.assertEqual(tuple(row), ('kept', 'RM-807/113.010.1508/04.01+C00.01'))
        self.assertIsNotNone(x.execute("SELECT id FROM component WHERE key='qt'").fetchone())   # seeded: kept
        R.ingest(x, self.target, quiet=True, index_path=self.fixture)           # and it works after
        self.assertEqual(x.execute("SELECT count(*) FROM edge WHERE type='dll-import' AND ordinal IS NOT NULL"
                                   ).fetchone()[0], 0)

    def test_ecom_plugins_become_edges(self):
        """tools/ecom_index.py output: one ecom-plugin edge per implementation, from the plugin's binary; shown
        in its component's briefing"""
        ec = {'device': DEV, 'firmware': FIXTURE['firmware'], 'plugins': {'gcerenderstage': {
            'name': 'gcerenderstage', 'format': 1, 'dll_uid': '0x20031cb9', 'interfaces': [
                {'uid': '0x10285a29', 'implementations': [{'uid': '0x20031cba', 'version': 1,
                                                           'name': 'gcerenderstage'}]}]}}}
        p = os.path.join(self.tmp, 'ecom.json')
        json.dump(ec, open(p, 'w'))
        st = R.ingest(self.conn, self.target, quiet=True, index_path=self.fixture, ecom_path=p)
        self.assertEqual(st['ecom'], 1)
        row = self.conn.execute("SELECT src, detail FROM edge WHERE type='ecom-plugin'").fetchone()
        self.assertEqual(row['src'], self.bid('sys/bin/gcerenderstage.dll'))
        ctx = R.context(self.conn, 'gce-render-stage', budget=20000)
        self.assertIn('iface 0x10285a29 impl 0x20031cba gcerenderstage', ctx['components'][0]['edges']['ecom'])

    def test_mcp_adapter(self):
        """tools/registry_mcp.py: the agent-facing tools over the same functions; refusals come back as tool
        errors (a submit without the lease token), not as a dead server"""
        msgs = [{'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {}},
                {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
                {'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call',
                 'params': {'name': 'registry_search', 'arguments': {'query': 'gce'}}},
                {'jsonrpc': '2.0', 'id': 4, 'method': 'tools/call',
                 'params': {'name': 'registry_claim_task', 'arguments': {'task': 'modem-lifecycle-probe',
                                                                         'agent': 'mcp-test'}}},
                {'jsonrpc': '2.0', 'id': 5, 'method': 'tools/call',
                 'params': {'name': 'registry_submit', 'arguments': {
                     'task': 'modem-lifecycle-probe', 'token': 'wrong', 'statement': 'x',
                     'scope': 'nokia-808-rm807/*'}}},
                {'jsonrpc': '2.0', 'id': 6, 'method': 'tools/call',
                 'params': {'name': 'registry_context', 'arguments': {'needle': 'avkon'}}}]
        self.conn.commit()
        env = dict(os.environ, REGISTRY_DB=self.db)
        out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'registry_mcp.py')],
                             input='\n'.join(json.dumps(m) for m in msgs) + '\n', capture_output=True, text=True,
                             env=env, timeout=60)
        res = {r['id']: r['result'] for r in map(json.loads, out.stdout.splitlines())}
        self.assertIn('registry_submit', [t['name'] for t in res[2]['tools']])
        self.assertIn('gce-render-stage', res[3]['content'][0]['text'])
        self.assertTrue(json.loads(res[4]['content'][0]['text'])['token'])
        self.assertTrue(res[5].get('isError'))
        self.assertIn('wrong lease token', res[5]['content'][0]['text'])
        self.assertEqual(json.loads(res[6]['content'][0]['text'])['components'][0]['key'], 'avkon')

    def test_artifact_versions_are_kept(self):
        doc = os.path.join(self.tmp, 'note.md')
        open(doc, 'w').write('v1')
        os.environ['REGISTRY_DB'] = self.db
        art = 'tests/registry_test.py'
        before = self.conn.execute('SELECT sha1 FROM artifact WHERE path=?', (art,)).fetchone()['sha1']
        open(os.path.join(ROOT, art), 'a').write('\n# touch\n')
        try:
            R.ingest(self.conn, self.target, quiet=True, index_path=self.fixture)
        finally:
            txt = open(os.path.join(ROOT, art)).read()
            open(os.path.join(ROOT, art), 'w').write(txt[:-len('\n# touch\n')])
        versions = self.conn.execute('SELECT count(*) c FROM artifact_version av JOIN artifact a ON a.id=av.artifact_id'
                                     ' WHERE a.path=?', (art,)).fetchone()['c']
        self.assertGreaterEqual(versions, 2, 'a changed artifact must keep its previous version')
        self.assertIsNotNone(before)

    # ---------------------------------------------------------------- 8. backup/restore + complete export
    def test_backup_restore_round_trip(self):
        dst = os.path.join(self.tmp, 'copy.sqlite3')
        R.backup(self.conn, dst)
        copy = R.connect(dst)
        try:
            for t in ('component', 'binary', 'edge', 'claim', 'task', 'artifact'):
                self.assertEqual(copy.execute('SELECT count(*) c FROM "%s"' % t).fetchone()['c'],
                                 self.conn.execute('SELECT count(*) c FROM "%s"' % t).fetchone()['c'], t)
        finally:
            copy.close()

    def test_export_contains_every_table(self):
        data = R.export(self.conn, os.path.join(self.tmp, 'out'), self.target)
        for t in ('component', 'binary', 'edge', 'source', 'membership', 'claim', 'evidence', 'review', 'task',
                  'task_state', 'artifact', 'artifact_version', 'contract', 'objective', 'target'):
            self.assertIn(t, data['dump'], t)
            self.assertEqual(data['counts'][t], len(data['dump'][t]))
        self.assertNotIn('progress', json.dumps(data['components'])[:2000])


if __name__ == '__main__':
    unittest.main()
