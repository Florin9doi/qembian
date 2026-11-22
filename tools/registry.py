#!/usr/bin/env python3
"""Research registry: components, contracts, claims, tasks - the operational store for understanding Symbian.

Guarantees this file is responsible for (each one has an integration test in tests/registry_test.py):

  * seed is idempotent. Files under docs/registry/ are hashed; an unchanged file is a no-op, a changed file
    only adds missing rows, and `seed --migrate` is the explicit way to rewrite curated columns - it never
    touches operational state (task leases/state, claims, reviews, artifact history).
  * ingest is idempotent and reconciling. Binaries are inserted first, then edges are resolved against
    firmware-scoped binaries through the same alias resolver that assigns components. Generated edges are
    rebuilt per target, so a changed firmware retires stale facts instead of accumulating them.
  * tasks have one lifecycle. Listing, claiming and completion all use task_ready(); claims carry a lease
    token and a revision; expired leases are reaped and the task becomes claimable again.
  * context is complete and scoped. A task knows its components and contracts; a claim records the task it
    came from; `context` returns binaries, typed edges, contracts, claims and artifacts within a budget.
  * the target is explicit. `--device` selects the profile, the database rows and the index; an import index
    whose device/firmware does not match the selected target is refused.
  * evidence is honest. "verified" needs evidence that resolves to something identifiable; reviews are events,
    not overwrites; a superseding claim must have the same subject and scope; artifact versions are kept.
  * the store is recoverable. It is one project-wide SQLite file with target-scoped records, and
    `backup`/`restore` use SQLite's own consistent copy. JSON/Markdown/HTML exports are complete snapshots,
    never inputs.
"""
import argparse, fnmatch, glob, hashlib, html, json, os, re, secrets, shutil, sqlite3, sys, time, tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import import_index as II        # noqa: E402

SCHEMA = 5          # 5 (2026-11-17): nullable binary.component_id, binary.shadowed_by, one import edge per
                    # module pair with edge.n (migrate_4_to_5 keeps every claim, task and review)
STRATEGIES = ('retain', 'build-source', 'adapt-source', 'replace-interface', 'reverse-selected',
              'defer', 'undecided')
STRATEGY_HELP = {
    'retain': 'keep the original binary; explore only when a compatibility issue demands it',
    'build-source': 'available source: pin version, build recipe, ABI compatibility, behaviour',
    'adapt-source': 'available source: find the platform dependencies and the changes needed',
    'replace-interface': 'recover and validate the externally observable contract, then replace it',
    'reverse-selected': 'answer one bounded question that blocks an objective',
    'defer': 'recorded reason, no automatic research task',
    'undecided': 'needs a short source/compatibility assessment first',
}
# kinds of evidence that can carry a "verified" claim: they must resolve to something identifiable
RESOLVING_KINDS = ('doc', 'test-suite', 'decompilation', 'ghidra-elf', 'ghidra-project', 'run-log',
                   'import-index', 'backup')
EVIDENCE_KINDS = RESOLVING_KINDS + ('on-device-test', 'audit', 'review')
EDGE_TYPES = ('dll-import', 'ipc-client-server', 'ecom-plugin', 'ldd-pdd', 'shared-memory', 'device-protocol',
              'replacement', 'adaptation', 'build-dependency', 'rom-file-override', 'byte-patch')

DDL = """
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
CREATE TABLE IF NOT EXISTS component(
  id INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, title TEXT NOT NULL DEFAULT '',
  role TEXT NOT NULL DEFAULT '', strategy TEXT NOT NULL DEFAULT 'undecided',
  source_availability TEXT NOT NULL DEFAULT 'unknown', notes TEXT NOT NULL DEFAULT '',
  seeded_from TEXT NOT NULL DEFAULT '', seeded_sha TEXT NOT NULL DEFAULT '', updated TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS membership(component_id INT NOT NULL REFERENCES component(id), subsystem TEXT NOT NULL,
  PRIMARY KEY(component_id, subsystem));
CREATE TABLE IF NOT EXISTS component_alias(pattern TEXT PRIMARY KEY, component_key TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS binary(
  id INTEGER PRIMARY KEY, component_id INT REFERENCES component(id),   -- NULL: no logical role assigned yet
  device TEXT NOT NULL, firmware TEXT NOT NULL, path TEXT NOT NULL,
  uid3 TEXT, arch TEXT, codesize INT, filesize INT, hash_algo TEXT, hash TEXT,
  origin TEXT, base TEXT, extractor TEXT, extractor_version TEXT, parse_error TEXT,
  stale INT NOT NULL DEFAULT 0, seen TEXT NOT NULL DEFAULT '',
  shadowed_by TEXT,                         -- the same path in a higher ROM layer: that one is loaded
  UNIQUE(device, firmware, path));
CREATE TABLE IF NOT EXISTS edge(
  key TEXT PRIMARY KEY,                     -- stable identity; NULLs must not weaken uniqueness
  origin TEXT NOT NULL DEFAULT 'ingest',    -- ingest | curated
  device TEXT NOT NULL DEFAULT '', firmware TEXT NOT NULL DEFAULT '',
  src INT REFERENCES binary(id), dst INT REFERENCES binary(id),
  src_component INT REFERENCES component(id), dst_component INT REFERENCES component(id),
  type TEXT NOT NULL, detail TEXT NOT NULL DEFAULT '', ordinal INT, offset INT,
  evidence TEXT NOT NULL DEFAULT '',
  n INT);                                   -- dll-import: how many ordinals src imports from dst
CREATE INDEX IF NOT EXISTS edge_src_comp ON edge(src_component); CREATE INDEX IF NOT EXISTS edge_dst_comp ON edge(dst_component);
CREATE INDEX IF NOT EXISTS edge_src ON edge(src); CREATE INDEX IF NOT EXISTS edge_dst ON edge(dst);
CREATE INDEX IF NOT EXISTS edge_target ON edge(device, firmware, origin);
CREATE TABLE IF NOT EXISTS source(
  id INTEGER PRIMARY KEY, component_id INT NOT NULL REFERENCES component(id), repo TEXT NOT NULL,
  revision TEXT NOT NULL DEFAULT '', recipe TEXT NOT NULL DEFAULT '', relation TEXT NOT NULL DEFAULT 'unknown',
  notes TEXT NOT NULL DEFAULT '', UNIQUE(component_id, repo, revision, recipe));
CREATE TABLE IF NOT EXISTS contract(
  id INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, component_id INT REFERENCES component(id),
  title TEXT NOT NULL, kind TEXT NOT NULL DEFAULT '', participants TEXT NOT NULL DEFAULT '',
  args TEXT NOT NULL DEFAULT '', ownership TEXT NOT NULL DEFAULT '', completion TEXT NOT NULL DEFAULT '',
  cancellation TEXT NOT NULL DEFAULT '', errors TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'unknown',
  source TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS objective(id INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, title TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'open', notes TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS task(
  id INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, title TEXT NOT NULL, question TEXT NOT NULL DEFAULT '',
  objective INT REFERENCES objective(id), acceptance TEXT NOT NULL DEFAULT '',
  dependencies TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(dependencies)),
  owner TEXT, lease_token TEXT, lease_until TEXT, effort_limit TEXT,
  state TEXT NOT NULL DEFAULT 'ready' CHECK (state IN ('ready','claimed','blocked','done','dropped')),
  revision INT NOT NULL DEFAULT 1, created TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS task_component(task_id INT NOT NULL REFERENCES task(id),
  component_id INT NOT NULL REFERENCES component(id), PRIMARY KEY(task_id, component_id));
CREATE TABLE IF NOT EXISTS task_contract(task_id INT NOT NULL REFERENCES task(id),
  contract_id INT NOT NULL REFERENCES contract(id), PRIMARY KEY(task_id, contract_id));
CREATE TABLE IF NOT EXISTS task_state(id INTEGER PRIMARY KEY, task_id INT NOT NULL REFERENCES task(id),
  at TEXT, actor TEXT, state TEXT, note TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS claim(
  id INTEGER PRIMARY KEY, task_id INT REFERENCES task(id),
  subject_component INT REFERENCES component(id), subject_contract INT REFERENCES contract(id),
  subject_binary INT REFERENCES binary(id), statement TEXT NOT NULL,
  scope_device TEXT NOT NULL DEFAULT '', scope_firmware TEXT NOT NULL DEFAULT '', scope_detail TEXT,
  confidence TEXT NOT NULL DEFAULT 'inferred' CHECK (confidence IN ('verified','inferred')),
  status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ('open','accepted','rejected','disputed','superseded')),
  supersedes INT REFERENCES claim(id), author TEXT NOT NULL DEFAULT '', created TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS evidence(id INTEGER PRIMARY KEY, claim_id INT REFERENCES claim(id),
  artifact_id INT REFERENCES artifact(id), kind TEXT NOT NULL, ref TEXT NOT NULL DEFAULT '',
  resolved INT NOT NULL DEFAULT 0, note TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS claim_component(claim_id INT NOT NULL REFERENCES claim(id),
  component_id INT NOT NULL REFERENCES component(id), PRIMARY KEY(claim_id, component_id));
CREATE TABLE IF NOT EXISTS claim_contract(claim_id INT NOT NULL REFERENCES claim(id),
  contract_id INT NOT NULL REFERENCES contract(id), PRIMARY KEY(claim_id, contract_id));
CREATE TABLE IF NOT EXISTS review(id INTEGER PRIMARY KEY, claim_id INT NOT NULL REFERENCES claim(id),
  verdict TEXT NOT NULL, reviewer TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS artifact(
  id INTEGER PRIMARY KEY, kind TEXT NOT NULL, path TEXT NOT NULL, sha1 TEXT, bytes INT,
  command TEXT, tool TEXT, tool_version TEXT, provenance TEXT NOT NULL DEFAULT 'unknown',
  target TEXT, result TEXT, task_id INT REFERENCES task(id), created TEXT NOT NULL DEFAULT '',
  UNIQUE(kind, path));
CREATE TABLE IF NOT EXISTS artifact_version(id INTEGER PRIMARY KEY, artifact_id INT NOT NULL REFERENCES artifact(id),
  sha1 TEXT, bytes INT, at TEXT NOT NULL, note TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS target(device TEXT PRIMARY KEY, firmware TEXT, level TEXT, updated TEXT,
  open TEXT NOT NULL DEFAULT '[]', milestones TEXT NOT NULL DEFAULT '{}', notes TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS mention(component_id INT NOT NULL REFERENCES component(id),
  artifact_id INT NOT NULL REFERENCES artifact(id), PRIMARY KEY(component_id, artifact_id));
CREATE VIRTUAL TABLE IF NOT EXISTS fts_component USING fts5(key, title, role, notes, content='component',
  content_rowid='id');
"""


def now():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def _dev_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location('device', os.path.join(HERE, 'device.py'))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def store_path():
    """One project-wide store; records are target-scoped inside it. $REGISTRY_DB overrides (tests, forks)."""
    return os.environ.get('REGISTRY_DB') or os.path.join(ROOT, 'build', 'registry', 'registry.sqlite3')


def resolve_target(device_id=None):
    """Resolve the target ONCE: profile, firmware, store, import index. Nothing downstream reads the env."""
    d = _dev_module()
    dev = device_id or os.environ.get('SYBORG_DEVICE') or d.get(d.load(), 'id', '')
    prof = d.load(dev)
    paths = d.paths(prof)
    fw = d.get(prof, 'firmware.default') or ''
    key = d.paths_v2(prof).get('firmware_key', '') if hasattr(d, 'paths_v2') else ''
    return {'device': dev, 'profile': prof, 'firmware': key or fw, 'store': store_path(),
            'index': paths['imports'], 'devdir': paths['dir'], 'device_py': d}


def connect(path=None, create=True):
    p = path or store_path()
    if not create and not os.path.exists(p):
        raise SystemExit('no registry yet: %s (run: registry.py seed + ingest)' % p)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    c = sqlite3.connect(p, timeout=30, isolation_level=None)
    c.row_factory = sqlite3.Row
    c.execute('PRAGMA journal_mode=WAL')
    c.execute('PRAGMA foreign_keys=ON')
    has_meta = c.execute("SELECT 1 FROM sqlite_master WHERE name='meta'").fetchone()
    row = c.execute('SELECT v FROM meta WHERE k=?', ('schema',)).fetchone() if has_meta else None
    if row is not None and int(row['v']) == 4:
        migrate_4_to_5(c)
        row = c.execute('SELECT v FROM meta WHERE k=?', ('schema',)).fetchone()
    c.executescript(DDL)
    if row is None:
        c.execute('INSERT INTO meta(k,v) VALUES(?,?)', ('schema', str(SCHEMA)))
    elif int(row['v']) != SCHEMA:
        raise SystemExit('store schema %s, this tool speaks %d: migrate or use a fresh store (%s)'
                         % (row['v'], SCHEMA, p))
    return c


def migrate_4_to_5(c):
    """schema 4 -> 5 in place, keeping every claim, task, review and artifact:
      * binary.component_id becomes nullable and the auto-made one-binary components go: 4,286 of 4,301 on the
        808 were invented per module ('undecided' then meant both "assess first" and "nobody looked");
        a component stays when anything curated refers to it (seed, alias, claim, contract, task, source)
      * binary.shadowed_by, edge.n; the per-slot import edges are dropped (ingest writes one per module pair)
      * claim firmware scopes in the device profile's firmware key form"""
    c.execute('PRAGMA foreign_keys=OFF')
    c.execute('BEGIN IMMEDIATE')
    try:
        c.execute("DELETE FROM edge WHERE type='dll-import' AND origin='ingest'")
        c.execute('ALTER TABLE edge ADD COLUMN n INT')
        c.execute('''CREATE TABLE binary_v5(
  id INTEGER PRIMARY KEY, component_id INT REFERENCES component(id),
  device TEXT NOT NULL, firmware TEXT NOT NULL, path TEXT NOT NULL,
  uid3 TEXT, arch TEXT, codesize INT, filesize INT, hash_algo TEXT, hash TEXT,
  origin TEXT, base TEXT, extractor TEXT, extractor_version TEXT, parse_error TEXT,
  stale INT NOT NULL DEFAULT 0, seen TEXT NOT NULL DEFAULT '', shadowed_by TEXT,
  UNIQUE(device, firmware, path))''')
        c.execute('INSERT INTO binary_v5(id,component_id,device,firmware,path,uid3,arch,codesize,filesize,hash_algo,'
                  'hash,origin,base,extractor,extractor_version,parse_error,stale,seen) SELECT id,component_id,'
                  'device,firmware,path,uid3,arch,codesize,filesize,hash_algo,hash,origin,base,extractor,'
                  'extractor_version,parse_error,stale,seen FROM binary')
        c.execute('DROP TABLE binary')
        c.execute('ALTER TABLE binary_v5 RENAME TO binary')
        auto = [r['id'] for r in c.execute(
            "SELECT id FROM component c WHERE seeded_from='' AND NOT EXISTS (SELECT 1 FROM component_alias a"
            " WHERE a.component_key=c.key) AND NOT EXISTS (SELECT 1 FROM claim k WHERE k.subject_component=c.id)"
            " AND NOT EXISTS (SELECT 1 FROM claim_component k WHERE k.component_id=c.id)"
            " AND NOT EXISTS (SELECT 1 FROM contract k WHERE k.component_id=c.id)"
            " AND NOT EXISTS (SELECT 1 FROM task_component k WHERE k.component_id=c.id)"
            " AND NOT EXISTS (SELECT 1 FROM source k WHERE k.component_id=c.id)"
            " AND NOT EXISTS (SELECT 1 FROM membership k WHERE k.component_id=c.id)")]
        for i in range(0, len(auto), 500):
            ids = json.dumps(auto[i:i + 500])
            c.execute('UPDATE binary SET component_id=NULL WHERE component_id IN (SELECT value FROM json_each(?))', (ids,))
            c.execute('UPDATE edge SET src_component=NULL WHERE src_component IN (SELECT value FROM json_each(?))', (ids,))
            c.execute('UPDATE edge SET dst_component=NULL WHERE dst_component IN (SELECT value FROM json_each(?))', (ids,))
            c.execute('DELETE FROM mention WHERE component_id IN (SELECT value FROM json_each(?))', (ids,))
            c.execute('DELETE FROM component WHERE id IN (SELECT value FROM json_each(?))', (ids,))
        c.execute('UPDATE binary SET stale=0 WHERE parse_error IS NOT NULL')   # stale = gone, nothing else
        for r in c.execute('SELECT DISTINCT scope_device, scope_firmware FROM claim').fetchall():
            fw = canon_firmware(r['scope_device'], r['scope_firmware'])
            if fw != r['scope_firmware']:
                c.execute('UPDATE claim SET scope_firmware=? WHERE scope_device=? AND scope_firmware=?',
                          (fw, r['scope_device'], r['scope_firmware']))
        c.execute("UPDATE meta SET v='5' WHERE k='schema'")
        c.execute('COMMIT')
    except BaseException:
        c.execute('ROLLBACK')
        raise
    finally:
        c.execute('PRAGMA foreign_keys=ON')
    c.execute("INSERT INTO fts_component(fts_component) VALUES('rebuild')")
    c.execute('VACUUM')                     # the 835k slot rows were most of the file


_FW_CACHE = {}


def canon_firmware(device, fw):
    """one firmware naming everywhere: the device profile's firmware key (RM-807/113.010.1508/04.01+C00.01),
    as binaries carry it. Seeds and agents wrote the bare version (113.010.1508), which never joined.
    '' and '*' stay; an unknown device or version is kept as given."""
    if not device or fw in ('', '*') or '/' in (fw or ''):
        return fw
    k = (device, fw)
    if k not in _FW_CACHE:
        try:
            d = _dev_module()
            prof = d.load('%s@%s' % (device, fw))
            p = d.paths_v2(prof) if hasattr(d, 'paths_v2') else d.paths(prof)
            _FW_CACHE[k] = p.get('firmware_key') or fw
        except BaseException:
            _FW_CACHE[k] = fw
    return _FW_CACHE[k]


def tx(conn):
    return _Tx(conn)


class _Tx:
    def __init__(self, conn):
        self.c = conn

    def __enter__(self):
        self.c.execute('BEGIN IMMEDIATE')
        return self.c

    def __exit__(self, exc, *_):
        self.c.execute('ROLLBACK' if exc else 'COMMIT')
        return False


# ------------------------------------------------------------------ identity helpers
def component_key_from_module(name):
    return os.path.splitext(os.path.basename(name))[0].lower()


def aliases(conn):
    return {r['pattern'].lower(): r['component_key'] for r in conn.execute(
        'SELECT pattern, component_key FROM component_alias')}


def resolve_component_key(al, path):
    """basename globs from component_alias, then the module stem. The ONE resolver: ingest, patches, stand-ins
    and destinations all use it, so a component is never invented for a module that belongs to another one.

    Deterministic and specific-first: an exact basename beats any glob, and among globs the longest pattern wins.
    (Without this a broad bucket such as `hci*.dll` silently stole `hciaccess.dll` from its own component.)"""
    base = os.path.basename(path).lower()
    if base in al:
        return al[base]
    best = None
    for pattern, key in al.items():
        if ('*' in pattern or '?' in pattern) and fnmatch.fnmatchcase(base, pattern):
            if best is None or len(pattern) > len(best[0]):
                best = (pattern, key)
    return best[1] if best else component_key_from_module(base)


def component_id(conn, al, path, create=False):
    """the component a module belongs to, or None. Components are logical roles made by seed or curation;
    ingest never invents one per module (create=True is for curation tools only)"""
    key = resolve_component_key(al, path)
    r = conn.execute('SELECT id FROM component WHERE key=?', (key,)).fetchone()
    if r:
        return r['id']
    if not create:
        return None
    cur = conn.execute("INSERT INTO component(key,title,strategy,source_availability,updated)"
                       " VALUES(?,?, 'undecided','unknown',?)", (key, os.path.basename(path), now()))
    return cur.lastrowid


def _sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def _file_sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


# ------------------------------------------------------------------ seed (idempotent, explicit migration)
SEED_DIR = 'docs/registry'


def seed_files():
    return sorted(glob.glob(os.path.join(ROOT, SEED_DIR, '*.toml')))


def seed(conn, target, migrate=False, quiet=False):
    """Idempotent by content hash. Unchanged file -> no-op. Changed file -> additive only. --migrate rewrites
    curated columns but never operational state. All files are validated together and applied in dependency
    order, so a claim may reference a component declared in another file."""
    report = {'files': 0, 'skipped': 0, 'components': 0, 'contracts': 0, 'tasks': 0, 'claims': 0,
              'migrated': 0}
    pending = []
    for path in seed_files():
        rel = os.path.relpath(path, ROOT)
        sha = _file_sha(path)
        prev = conn.execute('SELECT v FROM meta WHERE k=?', ('seed:sha:' + rel,)).fetchone()
        if prev and prev['v'] == sha and not migrate:
            report['skipped'] += 1
            continue
        with open(path, 'rb') as f:
            pending.append((rel, sha, tomllib.load(f)))
    if pending:
        merged = {'component': [], 'objective': [], 'contract': [], 'task': [], 'claim': []}
        origin = {}                 # id(item) -> (file, sha): a component records the file it came from
        for rel, sha_, data in pending:
            for k in merged:
                for it in data.get(k, []):
                    merged[k].append(it)
                    origin[id(it)] = (rel, sha_)
        bad = _validate_seed(conn, merged, ', '.join(rel for rel, _s, _d in pending))
        if bad:
            raise SystemExit('unknown references: %s' % ', '.join(bad))
        if migrate:                 # curated rows that exist and get rewritten (the counter was never set)
            report['migrated'] = sum(1 for tbl in ('component', 'contract', 'task') for it in merged[tbl]
                                     if conn.execute('SELECT 1 FROM %s WHERE key=?' % tbl, (it['key'],)).fetchone())
        with tx(conn) as c:
            for it in merged['component']:
                report['components'] += _seed_component(c, it, *origin[id(it)], migrate)
            for it in merged['objective']:
                c.execute('INSERT INTO objective(key,title,state,notes) VALUES(?,?,?,?)'
                          ' ON CONFLICT(key) DO UPDATE SET title=excluded.title, notes=excluded.notes',
                          (it['key'], it['title'], it.get('state', 'open'), (it.get('notes') or '').strip()))
            for it in merged['contract']:
                report['contracts'] += _seed_contract(c, it, migrate)
            for it in merged['task']:
                report['tasks'] += _seed_task(c, it, migrate)
            for it in merged['claim']:
                report['claims'] += _seed_claim(c, it)
            for rel, sha, _data in pending:
                c.execute('INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)', ('seed:sha:' + rel, sha))
                c.execute('INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)', ('seed:at:' + rel, now()))
        report['files'] = len(pending)
    conn.execute('INSERT INTO fts_component(fts_component) VALUES(\'rebuild\')')
    if not quiet:
        print('seed: %(files)d file(s) applied, %(skipped)d skipped, %(components)d components, '
              '%(contracts)d contracts, %(tasks)d tasks, %(claims)d claims, %(migrated)d migrated' % report)
    return report


def _validate_seed(conn, data, rel):
    comps = {c['key'] for c in data.get('component', [])} | {
        r['key'] for r in conn.execute('SELECT key FROM component')}
    contracts = {c['key'] for c in data.get('contract', [])} | {
        r['key'] for r in conn.execute('SELECT key FROM contract')}
    tasks = {t['key'] for t in data.get('task', [])} | {r['key'] for r in conn.execute('SELECT key FROM task')}
    objs = {o['key'] for o in data.get('objective', [])} | {r['key'] for r in conn.execute('SELECT key FROM objective')}
    bad = []
    for t in data.get('task', []):
        if t.get('objective') and t['objective'] not in objs:
            bad.append('%s: objective %s' % (rel, t['objective']))
        for dep in t.get('dependencies', []):
            if dep not in tasks:
                bad.append('%s: task %s depends on unknown task %s' % (rel, t['key'], dep))
        for comp in t.get('components', []):
            if comp not in comps:
                bad.append('%s: task %s names unknown component %s' % (rel, t['key'], comp))
        for con in t.get('contracts', []):
            if con not in contracts:
                bad.append('%s: task %s names unknown contract %s' % (rel, t['key'], con))
    for c in data.get('contract', []):
        if c.get('component') and c['component'] not in comps:
            bad.append('%s: contract %s names unknown component %s' % (rel, c['key'], c['component']))
    for cl in data.get('claim', []):
        if cl.get('component') and cl['component'] not in comps:
            bad.append('%s: claim names unknown component %s' % (rel, cl['component']))
        if cl.get('contract') and cl['contract'] not in contracts:
            bad.append('%s: claim names unknown contract %s' % (rel, cl['contract']))
        if cl.get('task') and cl['task'] not in tasks:
            bad.append('%s: claim names unknown task %s' % (rel, cl['task']))
    return bad


def _seed_component(c, it, rel, sha, migrate):
    key = it['key']
    row = c.execute('SELECT * FROM component WHERE key=?', (key,)).fetchone()
    created = 0
    vals = (it.get('title', key), it.get('role', ''), it.get('strategy', 'undecided'),
            it.get('source_availability', 'unknown'), (it.get('notes') or '').strip())
    if row is None:
        cid = c.execute('INSERT INTO component(key,title,role,strategy,source_availability,notes,seeded_from,'
                        'seeded_sha,updated) VALUES(?,?,?,?,?,?,?,?,?)',
                        (key,) + vals + (rel, sha, now())).lastrowid
        created = 1
    else:
        cid = row['id']
        if migrate:
            c.execute('UPDATE component SET title=?,role=?,strategy=?,source_availability=?,notes=?,'
                      'seeded_from=?,seeded_sha=?,updated=? WHERE id=?', vals + (rel, sha, now(), cid))
        else:
            # additive: adopt curation only for fields that are still empty/derived
            c.execute("UPDATE component SET title=CASE WHEN title='' OR title=key THEN ? ELSE title END,"
                      " role=CASE WHEN role='' THEN ? ELSE role END,"
                      " strategy=CASE WHEN strategy='undecided' THEN ? ELSE strategy END,"
                      " source_availability=CASE WHEN source_availability='unknown' THEN ? ELSE"
                      " source_availability END,"
                      " notes=CASE WHEN notes='' THEN ? ELSE notes END, seeded_sha=? WHERE id=?",
                      (it.get('title', key), it.get('role', ''), it.get('strategy', 'undecided'),
                       it.get('source_availability', 'unknown'), (it.get('notes') or '').strip(), sha, cid))
    for s in it.get('subsystems', []):
        c.execute('INSERT OR IGNORE INTO membership(component_id,subsystem) VALUES(?,?)', (cid, s))
    for name in it.get('binaries', []):
        c.execute('INSERT OR REPLACE INTO component_alias(pattern,component_key) VALUES(?,?)', (name.lower(), key))
    for s in it.get('sources', []):
        c.execute('INSERT OR IGNORE INTO source(component_id,repo,revision,recipe,relation,notes)'
                  ' VALUES(?,?,?,?,?,?)', (cid, s.get('repo', ''), s.get('revision', ''), s.get('recipe', ''),
                                           s.get('relation', 'unknown'), s.get('notes', '')))
    return created


def _seed_contract(c, it, migrate):
    cid = None
    if it.get('component'):
        r = c.execute('SELECT id FROM component WHERE key=?', (it['component'],)).fetchone()
        cid = r['id'] if r else None
    vals = (cid, it.get('title', it['key']), it.get('kind', ''), it.get('participants', ''), it.get('args', ''),
            it.get('ownership', ''), it.get('completion', ''), it.get('cancellation', ''), it.get('errors', ''),
            it.get('status', 'unknown'), it.get('source', ''))
    row = c.execute('SELECT id FROM contract WHERE key=?', (it['key'],)).fetchone()
    if row is None:
        c.execute('INSERT INTO contract(key,component_id,title,kind,participants,args,ownership,completion,'
                  'cancellation,errors,status,source) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)', (it['key'],) + vals)
        return 1
    if migrate:
        c.execute('UPDATE contract SET component_id=?,title=?,kind=?,participants=?,args=?,ownership=?,'
                  'completion=?,cancellation=?,errors=?,status=?,source=? WHERE id=?', vals + (row['id'],))
    return 0


def _seed_task(c, it, migrate):
    obj = None
    if it.get('objective'):
        r = c.execute('SELECT id FROM objective WHERE key=?', (it['objective'],)).fetchone()
        obj = r['id'] if r else None
    row = c.execute('SELECT * FROM task WHERE key=?', (it['key'],)).fetchone()
    created = 0
    if row is None:
        tid = c.execute('INSERT INTO task(key,title,question,objective,acceptance,dependencies,effort_limit,'
                        'state,created) VALUES(?,?,?,?,?,?,?,?,?)',
                        (it['key'], it['title'], it.get('question', ''), obj, it.get('acceptance', ''),
                         json.dumps(it.get('dependencies', [])), it.get('effort_limit', ''),
                         it.get('state', 'ready'), now())).lastrowid
        created = 1
    else:
        tid = row['id']
        if migrate:      # curated columns only; operational state (owner/lease/state/revision) is untouched
            c.execute('UPDATE task SET title=?,question=?,objective=?,acceptance=?,dependencies=?,effort_limit=?,'
                      'state=CASE WHEN state IN (\'ready\',\'blocked\') THEN ? ELSE state END WHERE id=?',
                      (it['title'], it.get('question', ''), obj, it.get('acceptance', ''),
                       json.dumps(it.get('dependencies', [])), it.get('effort_limit', ''),
                       it.get('state', 'ready'), tid))
    for comp in it.get('components', []):
        r = c.execute('SELECT id FROM component WHERE key=?', (comp,)).fetchone()
        c.execute('INSERT OR IGNORE INTO task_component(task_id,component_id) VALUES(?,?)', (tid, r['id']))
    for con in it.get('contracts', []):
        r = c.execute('SELECT id FROM contract WHERE key=?', (con,)).fetchone()
        c.execute('INSERT OR IGNORE INTO task_contract(task_id,contract_id) VALUES(?,?)', (tid, r['id']))
    return created


def _seed_claim(c, it):
    existing = c.execute('SELECT id, subject_component, subject_contract FROM claim WHERE statement=?',
                         (it['statement'],)).fetchone()
    if existing:
        comp = cont = None
        if it.get('component'):
            r = c.execute('SELECT id FROM component WHERE key=?', (it['component'],)).fetchone()
            comp = r['id'] if r else None
        if it.get('contract'):
            r = c.execute('SELECT id FROM contract WHERE key=?', (it['contract'],)).fetchone()
            cont = r['id'] if r else None
        if (existing['subject_component'] is None and comp) or (existing['subject_contract'] is None and cont):
            c.execute('UPDATE claim SET subject_component=COALESCE(subject_component, ?),'
                      ' subject_contract=COALESCE(subject_contract, ?) WHERE id=?',
                      (comp, cont, existing['id']))
        return 0
    comp = cont = None
    if it.get('component'):
        r = c.execute('SELECT id FROM component WHERE key=?', (it['component'],)).fetchone()
        comp = r['id'] if r else None
    if it.get('contract'):
        r = c.execute('SELECT id FROM contract WHERE key=?', (it['contract'],)).fetchone()
        cont = r['id'] if r else None
    task = None
    if it.get('task'):
        r = c.execute('SELECT id FROM task WHERE key=?', (it['task'],)).fetchone()
        task = r['id'] if r else None
    cid = c.execute('INSERT INTO claim(task_id,subject_component,subject_contract,statement,scope_device,'
                    'scope_firmware,scope_detail,confidence,status,author,created)'
                    ' VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                    (task, comp, cont, it['statement'], it.get('device', ''),
                     canon_firmware(it.get('device', ''), it.get('firmware', '')),
                     it.get('detail', ''), it.get('confidence', 'inferred'), it.get('status', 'open'),
                     it.get('author', 'seed'), now())).lastrowid
    for e in it.get('evidence', []):
        ref = e.get('ref', '')
        c.execute('INSERT INTO evidence(claim_id,kind,ref,resolved,note) VALUES(?,?,?,?,?)',
                  (cid, e.get('kind', 'doc'), ref, _resolves(ref), e.get('note', '')))
    return 1


# ------------------------------------------------------------------ ingest (idempotent, reconciling)
WORD = re.compile(r'\b[A-Za-z0-9_@$.*+-]{2,}\.(?:dll|exe|ldd|pdd|app|fsy|csy|tsy|dsy|esock|prt|drv)\b',
                  re.IGNORECASE)
GHIDRA_VERSION_RE = re.compile(r'Ghidra Version:\s*([0-9][0-9.]*)')


def _resolves(ref):
    """can this evidence reference be checked by someone else?"""
    if not ref:
        return 0
    if ref.startswith('cmd:'):
        return 1
    p = ref if os.path.isabs(ref) else os.path.join(ROOT, ref)
    return 1 if os.path.exists(p) else 0


def ingest(conn, target, quiet=False, index_path=None, limit=0, ecom_path=None, ipc_path=None):
    """Rebuild the generated facts for one target. Binaries first, then edges, so destinations resolve;
    regenerated edges replace the previous set (no accumulation, no stale leftovers)."""
    cfg = target['device_py']
    prof = target['profile']
    idx_path = index_path or target['index']
    if not os.path.exists(idx_path):
        raise SystemExit('no import index: tools/import_index.py build (%s)' % idx_path)
    idx = json.load(open(idx_path))
    if idx.get('device') and idx['device'] != target['device']:
        raise SystemExit('import index belongs to %r, target is %r' % (idx['device'], target['device']))
    if idx.get('firmware') and target['firmware'] and idx['firmware'] != target['firmware']:
        raise SystemExit('import index is firmware %r, target is %r'
                         % (idx['firmware'], target['firmware']))
    dev, fw = target['device'], target['firmware']
    al = aliases(conn)
    mods = sorted(idx['modules'].items())
    if limit:
        mods = mods[:limit]
    stats = {'binaries': 0, 'edges': 0, 'missing_dst': 0, 'stale': 0, 'artifacts': 0, 'refreshed': 0}

    # pass 1: components + binaries (and their current aliases)
    with tx(conn) as c:
        by_path = {}
        for path, m in mods:
            cid = component_id(c, al, path)
            row = c.execute('SELECT id FROM binary WHERE device=? AND firmware=? AND path=?',
                            (dev, fw, path)).fetchone()
            xip = (m.get('origin') or '').startswith('xip')
            # stale means "gone from the firmware" only: a parse error is its own column (bug 2026-11-17: the
            # errors were marked stale and then left out of every report). XIP has no file size of its own.
            vals = (cid, m.get('uid3'), 'arm', m.get('codesize'), None if xip else m.get('size'), 'crc32',
                    str(m.get('crc32') or ''), m.get('origin'), m.get('base'), 'tools/import_index.py',
                    str(idx.get('schema')), m.get('error'), 0, now(), m.get('shadowed_by'))
            if row:
                bid = row['id']
                c.execute('UPDATE binary SET component_id=?,uid3=?,arch=?,codesize=?,filesize=?,hash_algo=?,'
                          'hash=?,origin=?,base=?,extractor=?,extractor_version=?,parse_error=?,stale=?,'
                          'seen=?,shadowed_by=? WHERE id=?', vals + (bid,))
            else:
                bid = c.execute('INSERT INTO binary(component_id,uid3,arch,codesize,filesize,hash_algo,hash,'
                                'origin,base,extractor,extractor_version,parse_error,stale,seen,shadowed_by,'
                                'device,firmware,path) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                                vals + (dev, fw, path)).lastrowid
            by_path[path] = bid
            stats['binaries'] += 1
        # binaries that are no longer in the index: retire their edges, keep the row for old claims
        gone = [r['id'] for r in c.execute('SELECT id FROM binary WHERE device=? AND firmware=? AND path NOT IN'
                                           ' (SELECT value FROM json_each(?))',
                                           (dev, fw, json.dumps(list(by_path))))]
        for bid in gone:
            c.execute('DELETE FROM edge WHERE src=? OR dst=?', (bid, bid))
            c.execute('UPDATE binary SET stale=1 WHERE id=?', (bid,))
        stats['stale'] = len(gone)

    # pass 2: edges, resolved against the binaries we just wrote. One dll-import edge per (module, imported
    # module) with n = the number of ordinals; the slots themselves stay in the import index
    # (tools/import_index.py module/at/dll), which answers per-ordinal questions. (Schema 4 had one row per
    # slot: 835k rows, 294 MB, and destinations through the component, so every import of avkon.dll landed on
    # AknKeyRotator.dll, the component's first binary.)
    comp_of, by_base = {}, {}
    for r in conn.execute('SELECT id, path, component_id, shadowed_by FROM binary WHERE device=? AND firmware=?'
                          ' AND stale=0 ORDER BY shadowed_by IS NULL', (dev, fw)):
        comp_of[r['id']] = r['component_id']
        by_base[os.path.basename(r['path']).lower()] = r['id']     # the loaded copy (not shadowed) last wins

    names = idx.get('dlls', {})
    with tx(conn) as c:
        c.execute("DELETE FROM edge WHERE origin='ingest' AND device=? AND firmware=?", (dev, fw))
        rows = []
        for path, m in mods:
            src = by_path[path]
            per = {}
            for dll, ordn, off, kind in m.get('imports', []):
                base = os.path.basename(dll)
                per.setdefault(base.lower(), [base, 0])[1] += 1
            for low, (base, n) in sorted(per.items()):
                dst = by_base.get(low)
                if dst is None:
                    stats['missing_dst'] += 1
                k = 'ingest:%s:dll-import:%s' % (src, low)
                rows.append((hashlib.sha1(k.encode()).hexdigest(), 'ingest', dev, fw, src, dst, comp_of.get(src),
                             comp_of.get(dst), 'dll-import', base, None, None, 'import-index', n))
        c.executemany('INSERT OR IGNORE INTO edge(key,origin,device,firmware,src,dst,src_component,'
                      'dst_component,type,detail,ordinal,offset,evidence,n)'
                      ' VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)', rows)
        stats['edges'] = len(rows)
        stats['import_slots'] = sum(len(m.get('imports', [])) for _p, m in mods)

        # ECom: plugin DLL -> the interfaces it implements (tools/ecom_index.py, <devdir>/ecom.json), one edge per
        # implementation; detail = "iface 0x... impl 0x... <display name>"
        stats['ecom'] = 0
        # (a fixture index brings its own ECom file or none: the device's would not match its modules)
        ecom_path = ecom_path or (None if index_path else os.path.join(target['devdir'], 'ecom.json'))
        if ecom_path and os.path.exists(ecom_path):
            ec = json.load(open(ecom_path))
            if ec.get('device') == dev and (not ec.get('firmware') or ec['firmware'] == fw):
                erows = []
                for name, pl in ec['plugins'].items():
                    src = by_base.get(name + '.dll')
                    for itf in pl['interfaces']:
                        for im in itf['implementations']:
                            detail = 'iface %s impl %s %s' % (itf['uid'], im['uid'], im['name'][:60])
                            # device+firmware in the key (bug 2026-11-19: the 701's rows collided with the 808's)
                            k = 'ingest:ecom:%s:%s:%s:%s:%s' % (dev, fw, name, itf['uid'], im['uid'])
                            erows.append((hashlib.sha1(k.encode()).hexdigest(), 'ingest', dev, fw, src, None,
                                          comp_of.get(src), None, 'ecom-plugin', detail, None, None,
                                          'ecom-index', None))
                c.executemany('INSERT OR IGNORE INTO edge(key,origin,device,firmware,src,dst,src_component,'
                              'dst_component,type,detail,ordinal,offset,evidence,n)'
                              ' VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)', erows)
                stats['ecom'] = len(erows)

        # IPC: named servers (tools/ipc_index.py, <devdir>/ipc.json). One 'serves NAME' row per provider (src =
        # the module that starts it, no dst) and one 'client of NAME' edge per client module -> provider binary
        # (dst NULL when no module was seen starting it: run-time names, RootServer-hosted servers).
        stats['ipc'] = 0
        ipc_path = ipc_path or (None if index_path else os.path.join(target['devdir'], 'ipc.json'))
        if ipc_path and os.path.exists(ipc_path):
            ip = json.load(open(ipc_path))
            if ip.get('device') == dev and (not ip.get('firmware') or ip['firmware'] == fw):
                irows = []
                for name, sv in ip['servers'].items():
                    prov = by_path.get(sv.get('provider')) if sv.get('provider') else None
                    if prov is not None:
                        k = 'ingest:ipc:serves:%s:%s:%s:%s' % (dev, fw, sv['provider'], name)
                        irows.append((hashlib.sha1(k.encode()).hexdigest(), 'ingest', dev, fw, prov, None,
                                      comp_of.get(prov), None, 'ipc-client-server', 'serves ' + name, None,
                                      None, 'ipc-index', None))
                    for cl in sv.get('clients', []):
                        src = by_path.get(cl)
                        if src is None:
                            continue
                        k = 'ingest:ipc:client:%s:%s:%s:%s' % (dev, fw, cl, name)
                        irows.append((hashlib.sha1(k.encode()).hexdigest(), 'ingest', dev, fw, src, prov,
                                      comp_of.get(src), comp_of.get(prov), 'ipc-client-server',
                                      'client of ' + name, None, None, 'ipc-index', None))
                c.executemany('INSERT OR IGNORE INTO edge(key,origin,device,firmware,src,dst,src_component,'
                              'dst_component,type,detail,ordinal,offset,evidence,n)'
                              ' VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)', irows)
                stats['ipc'] = len(irows)

        # curated relationships: stand-ins, byte patches, rom overrides (component-scoped, no binaries)
        c.execute("DELETE FROM edge WHERE origin='curated' AND device=? AND firmware=?", (dev, fw))
        cur = []
        for s in (cfg.get(prof, 'standins') or []):
            name, kind = s.get('name') or '', s.get('kind') or ''
            r = c.execute('SELECT id FROM component WHERE key=?',
                          (resolve_component_key(al, name + '.dll'),)).fetchone()
            cur.append(('replacement', '%s (%s stand-in)' % (name, kind), r['id'] if r else None))
        for _pkey, pdetail, _pfile in _patch_entries(cfg, prof, dev):
            key = resolve_component_key(al, pdetail)      # fmradioaudiocontrol.dll -> bt-fm
            r = c.execute('SELECT id FROM component WHERE key=?', (key,)).fetchone()
            cur.append(('byte-patch', pdetail, r['id'] if r else None))
        for f in (cfg.get(prof, 'rom.files') or []):
            cur.append(('rom-file-override', f.get('rom', ''), None))
        for typ, detail, cid in cur:
            k = 'curated:%s:%s:%s:%s' % (dev, fw, typ, detail)
            c.execute('INSERT OR IGNORE INTO edge(key,origin,device,firmware,dst_component,type,detail,evidence)'
                      ' VALUES(?,?,?,?,?,?,?,?)',
                      (hashlib.sha1(k.encode()).hexdigest(), 'curated', dev, fw, cid, typ, detail,
                       'models/%s/model.toml' % dev))

    # artifacts: docs, test suites, decompilations, ELFs - hashed, versioned, provenance recorded
    def upsert_artifact(kind, path, **kw):
        p = os.path.relpath(path, ROOT) if os.path.isabs(path) else path
        full = path if os.path.isabs(path) else os.path.join(ROOT, p)
        sha = _sha1_file(full) if os.path.isfile(full) else None
        size = os.path.getsize(full) if os.path.isfile(full) else None
        row = conn.execute('SELECT id, sha1 FROM artifact WHERE kind=? AND path=?', (kind, p)).fetchone()
        if row is None:
            aid = conn.execute('INSERT INTO artifact(kind,path,sha1,bytes,command,tool,tool_version,provenance,'
                               'target,result,created) VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                               (kind, p, sha, size, kw.get('command'), kw.get('tool'),
                                kw.get('tool_version'), kw.get('provenance', 'unknown'), kw.get('target'),
                                kw.get('result', ''), now())).lastrowid
            conn.execute('INSERT INTO artifact_version(artifact_id,sha1,bytes,at,note) VALUES(?,?,?,?,?)',
                         (aid, sha, size, now(), 'first seen'))
            stats['artifacts'] += 1
        elif row['sha1'] != sha:
            conn.execute('UPDATE artifact SET sha1=?,bytes=? WHERE id=?', (sha, size, row['id']))
            conn.execute('INSERT INTO artifact_version(artifact_id,sha1,bytes,at,note) VALUES(?,?,?,?,?)',
                         (row['id'], sha, size, now(), 'changed'))
            stats['refreshed'] += 1

    devdir = target['devdir']
    for p in glob.glob(os.path.join(devdir, 'ghidra', '*.c')):
        log = p[:-2] + '.ghidra.log'
        ver = None
        prov = 'unknown'
        if os.path.exists(log):
            m = GHIDRA_VERSION_RE.search(open(log, errors='replace').read())
            ver, prov = (m.group(1) if m else None), ('recorded' if m else 'unknown')
        upsert_artifact('decompilation', p, tool='Ghidra', tool_version=ver, provenance=prov, target=dev,
                        command='tools/ghidra/decompile.sh ' + os.path.basename(p)[:-2])
    for p in glob.glob(os.path.join(devdir, 'ghidra', '*.elf')):
        upsert_artifact('ghidra-elf', p, tool=None, tool_version=None, provenance='unknown', target=dev,
                        command=None)      # an importable ELF, NOT a saved Ghidra program with annotations
    for p in glob.glob(os.path.join(devdir, 'ghidra', '*.gpr')) + glob.glob(os.path.join(ROOT, '*.gpr')):
        upsert_artifact('ghidra-project', p, tool='Ghidra', target=dev)
    for p in glob.glob(os.path.join(ROOT, 'docs', '**', '*.md'), recursive=True):
        upsert_artifact('doc', p)
    for pat in ('*_test.py', 'test_*.py'):
        for p in glob.glob(os.path.join(ROOT, 'tests', '**', pat), recursive=True):
            upsert_artifact('test-suite', p, tool='unittest', provenance='recorded',
                            command='python3 -m unittest %s' % os.path.relpath(p, ROOT).replace('/', '.')[:-3])
    # mentions
    conn.execute('DELETE FROM mention')
    comps = {r['key']: r['id'] for r in conn.execute('SELECT id, key FROM component')}
    for r in conn.execute('SELECT id, path FROM artifact'):
        full = os.path.join(ROOT, r['path'])
        if not os.path.isfile(full):
            continue
        try:
            txt = open(full, errors='replace').read()
        except OSError:
            continue
        for mo in set(x.lower() for x in WORD.findall(txt)):
            key = resolve_component_key(al, mo)
            if key in comps:
                conn.execute('INSERT OR IGNORE INTO mention(component_id,artifact_id) VALUES(?,?)',
                             (comps[key], r['id']))
    # device status
    st = cfg.get(prof, 'status', {}) or {}
    conn.execute('INSERT INTO target(device,firmware,level,updated,open,milestones,notes) VALUES(?,?,?,?,?,?,?)'
                 ' ON CONFLICT(device) DO UPDATE SET firmware=excluded.firmware, level=excluded.level,'
                 ' updated=excluded.updated, open=excluded.open, milestones=excluded.milestones,'
                 ' notes=excluded.notes',
                 (dev, fw, st.get('level', ''), st.get('updated', ''), json.dumps(st.get('open', []) or []),
                  json.dumps(cfg.get(prof, 'milestones', {}) or {}),
                  '; '.join('%s: %s' % kv for kv in (st.get('smoke_skip') or {}).items())))
    conn.execute('INSERT INTO fts_component(fts_component) VALUES(\'rebuild\')')
    if not quiet:
        print('ingest %s/%s: %d binaries, %d import edges (%d without a destination), %d retired, '
              '%d new artifacts, %d refreshed'
              % (dev, fw, stats['binaries'], stats['edges'], stats['missing_dst'], stats['stale'],
                 stats['artifacts'], stats['refreshed']))
        print('  ecom-plugin edges %d, ipc-client-server edges %d' % (stats.get('ecom', 0), stats.get('ipc', 0)))
    return stats


def _patch_entries(cfg, prof, dev):
    out = []
    for pf in (cfg.get(prof, 'rom.code_patches') or []):
        p = os.path.join(ROOT, 'models', dev, pf)
        if not os.path.exists(p):
            continue
        for line in open(p, errors='replace'):
            if line.lstrip().startswith('#'):
                continue
            m = re.match(r'\s*([A-Za-z0-9_.@-]+\.(?:dll|exe|ldd|pdd|app|fsy|csy|tsy|dsy|prt|drv))', line,
                         re.IGNORECASE)
            if m:
                out.append((component_key_from_module(m.group(1)), m.group(1), os.path.relpath(p, ROOT)))
    return out


def resolved_standins(device):
    d = _dev_module()
    prof = d.load(device)
    return d.get(prof, 'standins') or []


def patched_component_keys(device, conn=None):
    """component keys (after alias resolution) named by this device's byte-patch lists"""
    d = _dev_module()
    prof = d.load(device)
    al = aliases(conn) if conn is not None else {}
    return {resolve_component_key(al, detail) for _k, detail, _f in _patch_entries(d, prof, device)}


# ------------------------------------------------------------------ queries
def q_components(conn, query=None, strategy=None, device=None, subsystem=None, kind=None, limit=200):
    sql = ('SELECT c.*,'
           ' (SELECT count(*) FROM binary b WHERE b.component_id=c.id AND b.stale=0) AS binaries,'
           ' (SELECT sum(b.codesize) FROM binary b WHERE b.component_id=c.id AND b.stale=0) AS codesize,'
           ' (SELECT count(*) FROM binary b WHERE b.component_id=c.id AND b.codesize IS NULL AND b.stale=0)'
           '   AS unknown_sizes,'
           ' (SELECT count(*) FROM membership m WHERE m.component_id=c.id) AS subsystems,'
           ' (SELECT count(*) FROM mention x WHERE x.component_id=c.id) AS mentions,'
           ' (SELECT count(*) FROM edge e WHERE e.dst_component=c.id AND e.origin=\'curated\') AS curated_edges,'
           ' (SELECT count(DISTINCT e.src) FROM edge e WHERE e.dst_component=c.id AND e.type=\'dll-import\''
           '   AND (e.src_component IS NULL OR e.src_component!=c.id)) AS in_edges,'
           ' (SELECT count(*) FROM claim k WHERE k.subject_component=c.id AND k.status!=\'superseded\') AS claims'
           ' FROM component c WHERE 1=1')
    args = []
    if strategy:
        sql += ' AND c.strategy=?'; args.append(strategy)
    if device:
        sql += ' AND EXISTS (SELECT 1 FROM binary b WHERE b.component_id=c.id AND b.device=?)'; args.append(device)
    if subsystem:
        sql += ' AND EXISTS (SELECT 1 FROM membership m WHERE m.component_id=c.id AND m.subsystem=?)'
        args.append(subsystem)
    if kind:
        sql += (' AND EXISTS (SELECT 1 FROM binary b WHERE b.component_id=c.id AND lower(b.path) LIKE ?)')
        args.append('%.' + kind.lower().lstrip('.'))
    if query:
        sql += (' AND (c.key LIKE ? OR c.title LIKE ? OR c.role LIKE ? OR c.notes LIKE ? OR EXISTS'
                ' (SELECT 1 FROM membership m WHERE m.component_id=c.id AND m.subsystem LIKE ?))')
        like = '%' + query + '%'
        args += [like] * 5
    sql += ' ORDER BY c.key LIMIT %d' % max(1, int(limit))
    return [dict(r) for r in conn.execute(sql, args)]


def q_binaries(conn, component_id, device=None):
    sql = 'SELECT * FROM binary WHERE component_id=? AND stale=0'
    args = [component_id]
    if device:
        sql += ' AND device=?'; args.append(device)
    return [dict(r) for r in conn.execute(sql + ' ORDER BY device, firmware, path', args)]


def q_edges(conn, component_id=None, binary_id=None, device=None):
    if binary_id is not None:
        rows = conn.execute('SELECT * FROM edge WHERE src=? OR dst=? ORDER BY type, ordinal',
                            (binary_id, binary_id))
    elif component_id is not None:
        rows = conn.execute('SELECT * FROM edge WHERE src_component=? OR dst_component=? ORDER BY type, detail',
                            (component_id, component_id))
    else:
        rows = conn.execute('SELECT * FROM edge ORDER BY type')
    out = {}
    for r in rows:
        if device and r['device'] and r['device'] != device:
            continue
        out.setdefault(r['type'], []).append(dict(r))
    return out


def q_edge_summary(conn, component_id, device=None):
    """a component's place in the import graph, by component: whom its modules import (and how many ordinals),
    who imports them, and its curated relations (stand-ins, byte patches). Modules without a component show
    as their file name."""
    dev_sql, dev_args = (' AND e.device=?', [device]) if device else ('', [])
    imports = [dict(r) for r in conn.execute(
        "SELECT COALESCE(c.key, '(' || b.path || ')') AS target, count(DISTINCT e.dst) AS modules,"
        ' sum(e.n) AS ordinals FROM edge e LEFT JOIN component c ON c.id=e.dst_component'
        " LEFT JOIN binary b ON b.id=e.dst WHERE e.type='dll-import' AND e.src_component=?"
        ' AND (e.dst_component IS NULL OR e.dst_component!=e.src_component)' + dev_sql +
        ' GROUP BY target ORDER BY ordinals DESC LIMIT 8', [component_id] + dev_args)]
    by = conn.execute("SELECT count(DISTINCT e.src) AS modules, sum(e.n) AS ordinals FROM edge e"
                      " WHERE e.type='dll-import' AND e.dst_component=? AND (e.src_component IS NULL OR"
                      ' e.src_component!=e.dst_component)' + dev_sql, [component_id] + dev_args).fetchone()
    top = [dict(r) for r in conn.execute(
        "SELECT COALESCE(c.key, '(unassigned)') AS source, count(DISTINCT e.src) AS modules, sum(e.n) AS ordinals"
        ' FROM edge e LEFT JOIN component c ON c.id=e.src_component'
        " WHERE e.type='dll-import' AND e.dst_component=? AND (e.src_component IS NULL OR"
        ' e.src_component!=e.dst_component)' + dev_sql + ' GROUP BY source ORDER BY ordinals DESC LIMIT 8',
        [component_id] + dev_args)]
    curated = [{'type': r['type'], 'detail': r['detail']} for r in conn.execute(
        "SELECT type, detail FROM edge e WHERE e.origin='curated' AND e.dst_component=?" + dev_sql +
        ' ORDER BY type, detail', [component_id] + dev_args)]
    plugins = [r['detail'] for r in conn.execute(
        "SELECT e.detail FROM edge e WHERE e.type='ecom-plugin' AND e.src_component=?" + dev_sql +
        ' ORDER BY e.detail LIMIT 12', [component_id] + dev_args)]
    serves = [r['detail'][7:] for r in conn.execute(
        "SELECT e.detail FROM edge e WHERE e.type='ipc-client-server' AND e.detail LIKE 'serves %'"
        ' AND e.src_component=?' + dev_sql + ' ORDER BY e.detail LIMIT 12', [component_id] + dev_args)]
    client_of = [r['detail'][10:] for r in conn.execute(
        "SELECT DISTINCT e.detail FROM edge e WHERE e.type='ipc-client-server' AND e.detail LIKE 'client of %'"
        ' AND e.src_component=? AND (e.dst_component IS NULL OR e.dst_component!=e.src_component)' + dev_sql +
        ' ORDER BY e.detail LIMIT 12', [component_id] + dev_args)]
    served = [{'server': r['server'], 'clients': r['c']} for r in conn.execute(
        "SELECT substr(e.detail, 11) AS server, count(DISTINCT e.src) AS c FROM edge e"
        " WHERE e.type='ipc-client-server' AND e.detail LIKE 'client of %' AND e.dst_component=?"
        ' AND (e.src_component IS NULL OR e.src_component!=e.dst_component)' + dev_sql +
        ' GROUP BY server ORDER BY c DESC LIMIT 12', [component_id] + dev_args)]
    out = {'imports': {'top': imports},
           'imported_by': {'modules': by['modules'] or 0, 'ordinals': by['ordinals'] or 0, 'top': top}}
    if curated:
        out['curated'] = curated
    if plugins:
        out['ecom'] = plugins
    if serves or client_of or served:
        out['ipc'] = {'serves': serves, 'client_of': client_of, 'served_clients': served}
    return out


def q_contracts(conn, component_id=None, task_id=None):
    if task_id is not None:
        rows = conn.execute('SELECT c.* FROM contract c JOIN task_contract tc ON tc.contract_id=c.id'
                            ' WHERE tc.task_id=? ORDER BY c.key', (task_id,))
    elif component_id is not None:
        rows = conn.execute('SELECT * FROM contract WHERE component_id=? ORDER BY key', (component_id,))
    else:
        rows = conn.execute('SELECT * FROM contract ORDER BY key')
    return [dict(r) for r in rows]


def q_claims(conn, component_id=None, contract_id=None, task_id=None, include_superseded=False):
    sql = 'SELECT * FROM claim WHERE 1=1'
    args = []
    if component_id is not None:
        sql += (' AND (subject_component=? OR id IN (SELECT claim_id FROM claim_component WHERE component_id=?))')
        args += [component_id, component_id]
    if contract_id is not None:
        sql += (' AND (subject_contract=? OR id IN (SELECT claim_id FROM claim_contract WHERE contract_id=?))')
        args += [contract_id, contract_id]
    if task_id is not None:
        sql += ' AND task_id=?'; args.append(task_id)
    if not include_superseded:
        sql += " AND status!='superseded'"
    rows = [dict(r) for r in conn.execute(sql + ' ORDER BY created DESC, id DESC', args)]
    for r in rows:
        r['evidence'] = [dict(e) for e in conn.execute(
            'SELECT kind, ref, resolved, note FROM evidence WHERE claim_id=?', (r['id'],))]
        r['reviews'] = [dict(e) for e in conn.execute(
            'SELECT verdict, reviewer, note, at FROM review WHERE claim_id=? ORDER BY at', (r['id'],))]
    return rows


# ------------------------------------------------------------------ task lifecycle (one ruleset)
def reap_expired(conn, actor='registry'):
    """Expired claims return to the pool; the event is recorded, not silently dropped."""
    rows = conn.execute("SELECT * FROM task WHERE state='claimed' AND lease_until IS NOT NULL"
                        ' AND lease_until<?', (now(),)).fetchall()
    for r in rows:
        conn.execute("UPDATE task SET state='ready', owner=NULL, lease_token=NULL, lease_until=NULL,"
                     ' revision=revision+1 WHERE id=?', (r['id'],))
        conn.execute('INSERT INTO task_state(task_id,at,actor,state,note) VALUES(?,?,?,?,?)',
                     (r['id'], now(), actor, 'ready', 'lease expired (%s)' % (r['owner'] or '?')))
    return len(rows)                        # the tasks reaped (total_changes counted the connection's lifetime)


def task_readiness(conn, row):
    """(ready, reason). Used by listing, claiming and completion - one rule, no drift."""
    if row['state'] == 'done':
        return False, 'done'
    if row['state'] == 'dropped':
        return False, 'dropped'
    if row['state'] == 'blocked':
        return False, 'blocked in the registry'
    if row['state'] == 'claimed' and row['lease_until'] and row['lease_until'] >= now():
        return False, 'claimed by %s until %s' % (row['owner'], row['lease_until'])
    deps = json.loads(row['dependencies'] or '[]')
    if deps:
        unmet = []
        for dep in deps:
            d = conn.execute('SELECT state FROM task WHERE key=?', (dep,)).fetchone()
            if d is None:
                unmet.append('%s (unknown task)' % dep)
            elif d['state'] != 'done':
                unmet.append('%s (%s)' % (dep, d['state']))
        if unmet:
            return False, 'dependencies not done: ' + ', '.join(unmet)
    return True, 'ready'


def q_tasks(conn, objective=None, ready_only=False, include_done=False, device=None):
    """tasks are project-wide (a task names components, not firmwares); `device` is accepted for the CLI's
    symmetry and does not filter"""
    reap_expired(conn)
    sql = ('SELECT t.*, o.key AS objective_key, o.title AS objective_title FROM task t'
           ' LEFT JOIN objective o ON o.id=t.objective WHERE 1=1')
    args = []
    if objective:
        sql += ' AND o.key=?'; args.append(objective)
    if not include_done:
        sql += " AND t.state NOT IN ('done','dropped')"
    rows = [dict(r) for r in conn.execute(sql + ' ORDER BY o.key, t.key', args)]
    for r in rows:
        ok, why = task_readiness(conn, r)
        r['ready'], r['ready_reason'] = ok, why
        r['components'] = [x['key'] for x in conn.execute(
            'SELECT c.key FROM component c JOIN task_component tc ON tc.component_id=c.id WHERE tc.task_id=?',
            (r['id'],))]
        r['contracts'] = [x['key'] for x in conn.execute(
            'SELECT c.key FROM contract c JOIN task_contract tc ON tc.contract_id=c.id WHERE tc.task_id=?',
            (r['id'],))]
    return [r for r in rows if r['ready']] if ready_only else rows


def claim_task(conn, key, agent, lease='2h'):
    if not re.fullmatch(r'\d+[hm]', lease):
        raise SystemExit('lease must look like 30m or 2h')
    secs = int(lease[:-1]) * (3600 if lease.endswith('h') else 60)
    if secs <= 0:
        raise SystemExit('a lease of 0 expires at once: give 30m, 2h, ...')
    reap_expired(conn)
    with tx(conn) as c:
        r = c.execute('SELECT * FROM task WHERE key=?', (key,)).fetchone()
        if not r:
            raise SystemExit('no such task: ' + key)
        ok, why = task_readiness(c, r)
        if not ok:
            raise SystemExit('%s is not claimable: %s' % (key, why))
        token = secrets.token_hex(8)
        until = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + secs))
        cur = c.execute("UPDATE task SET state='claimed', owner=?, lease_token=?, lease_until=?,"
                        ' revision=revision+1 WHERE id=? AND revision=?', (agent, token, until, r['id'],
                                                                          r['revision']))
        if cur.rowcount != 1:
            raise SystemExit('claim lost a race (revision changed); retry')
        c.execute('INSERT INTO task_state(task_id,at,actor,state,note) VALUES(?,?,?,?,?)',
                  (r['id'], now(), agent, 'claimed', 'lease %s' % lease))
        return {'task': key, 'agent': agent, 'lease_until': until, 'token': token,
                'revision': r['revision'] + 1}


def _require_claim(conn, key, token, agent=None):
    r = conn.execute('SELECT * FROM task WHERE key=?', (key,)).fetchone()
    if not r:
        raise SystemExit('no such task: ' + key)
    if r['state'] != 'claimed':
        raise SystemExit('%s is %s, not claimed' % (key, r['state']))
    if r['lease_until'] and r['lease_until'] < now():
        reap_expired(conn)
        raise SystemExit('%s: lease expired' % key)
    # the token is what claim-task handed out: without it anyone could submit "verified" findings to, record
    # tests on and complete a task another agent holds (bug 2026-11-17: it was optional)
    if not token:
        raise SystemExit('%s: the lease token from claim-task is required (--token)' % key)
    if r['lease_token'] != token:
        raise SystemExit('%s: wrong lease token' % key)
    if agent and r['owner'] != agent:
        raise SystemExit('%s: owned by %s' % (key, r['owner']))
    return r


def submit(conn, key, statement, evidence, scope, confidence='inferred', author='agent', supersedes=None,
           token=None):
    if confidence not in ('verified', 'inferred'):
        raise SystemExit('confidence must be verified or inferred')
    if not scope or scope == '/':
        raise SystemExit('scope is required, e.g. nokia-808-rm807/113.010.1508 (or device/*)')
    dev, _, fw = scope.partition('/')
    fw = canon_firmware(dev, fw)
    r = _require_claim(conn, key, token)
    resolved = []
    for e in evidence:
        kind, _, ref = e.partition('=')
        resolved.append((kind or 'ref', ref or e, _resolves(ref or e)))
    if confidence == 'verified':
        ids = [(k, x) for k, x, ok in resolved if ok and k in RESOLVING_KINDS]
        if not ids:
            raise SystemExit('verified needs at least one %s evidence that resolves to a file or cmd:'
                             ' (%s)' % ('/'.join(RESOLVING_KINDS), ', '.join(e for e in evidence) or 'none'))
    comp = conn.execute('SELECT component_id FROM task_component WHERE task_id=? ORDER BY component_id LIMIT 1',
                        (r['id'],)).fetchone()
    cont = conn.execute('SELECT contract_id FROM task_contract WHERE task_id=? ORDER BY contract_id LIMIT 1',
                        (r['id'],)).fetchone()
    old = None
    if supersedes:
        old = conn.execute('SELECT * FROM claim WHERE id=?', (supersedes,)).fetchone()
        if not old:
            raise SystemExit('no claim %s to supersede' % supersedes)
        if old['status'] == 'superseded':      # one line of history, never a fork
            later = conn.execute('SELECT id FROM claim WHERE supersedes=?', (supersedes,)).fetchone()
            raise SystemExit('claim %s is already superseded%s: correct the newest one'
                             % (supersedes, ' by %s' % later['id'] if later else ''))
        mine_c = {x['component_id'] for x in conn.execute(
            'SELECT component_id FROM task_component WHERE task_id=?', (r['id'],))}
        old_c = {x['component_id'] for x in conn.execute(
            'SELECT component_id FROM claim_component WHERE claim_id=?', (supersedes,))}
        old_c.add(old['subject_component'])
        mine_k = {x['contract_id'] for x in conn.execute(
            'SELECT contract_id FROM task_contract WHERE task_id=?', (r['id'],))}
        old_k = {x['contract_id'] for x in conn.execute(
            'SELECT contract_id FROM claim_contract WHERE claim_id=?', (supersedes,))}
        old_k.add(old['subject_contract'])
        if (old_c & mine_c) == set() and (old_k & mine_k) == set():
            raise SystemExit('claim %s has a different subject; a correction must keep it' % supersedes)
        if (old['scope_device'], old['scope_firmware']) != (dev, fw):
            raise SystemExit('claim %s has scope %s/%s, this correction is %s/%s'
                             % (supersedes, old['scope_device'], old['scope_firmware'], dev, fw))
    with tx(conn) as c:
        cid = c.execute('INSERT INTO claim(task_id,subject_component,subject_contract,statement,scope_device,'
                        'scope_firmware,confidence,status,supersedes,author,created)'
                        ' VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                        (r['id'], comp['component_id'] if comp else None,
                         cont['contract_id'] if cont else None, statement, dev, fw, confidence, 'open',
                         supersedes, author, now())).lastrowid
        for kind, ref, ok in resolved:
            c.execute('INSERT INTO evidence(claim_id,kind,ref,resolved) VALUES(?,?,?,?)', (cid, kind, ref, ok))
        for comp_row in c.execute('SELECT component_id FROM task_component WHERE task_id=?', (r['id'],)):
            c.execute('INSERT OR IGNORE INTO claim_component(claim_id,component_id) VALUES(?,?)',
                      (cid, comp_row['component_id']))
        for cont_row in c.execute('SELECT contract_id FROM task_contract WHERE task_id=?', (r['id'],)):
            c.execute('INSERT OR IGNORE INTO claim_contract(claim_id,contract_id) VALUES(?,?)',
                      (cid, cont_row['contract_id']))
        if supersedes:
            c.execute("UPDATE claim SET status='superseded' WHERE id=?", (supersedes,))
        return {'claim': cid, 'task': key, 'confidence': confidence, 'evidence_resolved': sum(
            1 for _k, _r, ok in resolved if ok)}


def review(conn, claim_id, verdict, reviewer='human', note='', evidence=None):
    if verdict not in ('accepted', 'rejected', 'disputed'):
        raise SystemExit('verdict must be accepted, rejected or disputed')
    row = conn.execute('SELECT id, status FROM claim WHERE id=?', (claim_id,)).fetchone()
    if not row:
        raise SystemExit('no claim %s' % claim_id)
    if row['status'] == 'superseded':          # a verdict must not bring a corrected claim back
        raise SystemExit('claim %s is superseded: review the claim that replaced it' % claim_id)
    with tx(conn) as c:
        c.execute('INSERT INTO review(claim_id,verdict,reviewer,note,at) VALUES(?,?,?,?,?)',
                  (claim_id, verdict, reviewer, note or '', now()))
        c.execute('UPDATE claim SET status=? WHERE id=?', (verdict, claim_id))
        for e in evidence or []:
            c.execute('INSERT INTO evidence(claim_id,kind,ref,resolved) VALUES(?,?,?,?)',
                      (claim_id, 'review', e, _resolves(e)))
    return {'claim': claim_id, 'status': verdict, 'reviewer': reviewer}


def record_test(conn, key, config, artifact, result, token=None, tool_version=None):
    if result not in ('passed', 'failed', 'blocked'):
        raise SystemExit('result must be passed, failed or blocked')
    r = _require_claim(conn, key, token)
    ids = []
    with tx(conn) as c:
        for p in [x.strip() for x in artifact.split(',') if x.strip()]:
            full = p if os.path.isabs(p) else os.path.join(ROOT, p)
            sha = _sha1_file(full) if os.path.isfile(full) else None
            size = os.path.getsize(full) if sha else None
            c.execute('INSERT INTO artifact(kind,path,sha1,bytes,tool,tool_version,provenance,target,'
                      'result,task_id,created) VALUES(?,?,?,?,?,?,?,?,?,?,?)'
                      ' ON CONFLICT(kind,path) DO UPDATE SET result=excluded.result, sha1=excluded.sha1,'
                      ' bytes=excluded.bytes, target=excluded.target, task_id=excluded.task_id',
                      ('run-log', p, sha, size, None, tool_version, 'recorded' if sha else 'unknown', config,
                       result, r['id'], now()))
            # the row's own id: lastrowid is stale after the conflict branch (bug 2026-11-17: a re-recorded
            # test's history went to an unrelated artifact)
            aid = c.execute("SELECT id FROM artifact WHERE kind='run-log' AND path=?", (p,)).fetchone()['id']
            c.execute('INSERT INTO artifact_version(artifact_id,sha1,bytes,at,note) VALUES(?,?,?,?,?)',
                      (aid, sha, size, now(), 'on-device test %s' % result))
            ids.append(aid)
    return {'artifacts': ids, 'config': config, 'result': result}


def reopen(conn, key, reason, actor='cli'):
    """Put a task back into the pool (it was completed on premature evidence, or the work resumed).
    The reason is recorded in task_state; claims, evidence and history stay untouched."""
    if not reason:
        raise SystemExit('reopening needs a reason (--reason)')
    r = conn.execute('SELECT * FROM task WHERE key=?', (key,)).fetchone()
    if not r:
        raise SystemExit('no such task: ' + key)
    with tx(conn) as c:
        c.execute("UPDATE task SET state='ready', owner=NULL, lease_token=NULL, lease_until=NULL,"
                  ' revision=revision+1 WHERE id=?', (r['id'],))
        c.execute('INSERT INTO task_state(task_id,at,actor,state,note) VALUES(?,?,?,?,?)',
                  (r['id'], now(), actor, 'reopened', reason))
    return {'task': key, 'state': 'ready', 'reason': reason}


def complete(conn, key, evidence, token=None, agent=None):
    if not evidence:
        raise SystemExit('completion needs acceptance evidence (--evidence path or cmd:...)')
    r = _require_claim(conn, key, token, agent)
    if not _resolves(evidence.split(',')[0].strip()):
        raise SystemExit('acceptance evidence %r does not resolve' % evidence)
    ok, why = task_readiness(conn, r)
    if not ok and why != 'claimed by %s until %s' % (r['owner'], r['lease_until']):
        raise SystemExit('%s cannot be completed: %s' % (key, why))
    with tx(conn) as c:
        c.execute("UPDATE task SET state='done', owner=NULL, lease_token=NULL, lease_until=NULL,"
                  ' revision=revision+1 WHERE id=?', (r['id'],))
        c.execute('INSERT INTO task_state(task_id,at,actor,state,note) VALUES(?,?,?,?,?)',
                  (r['id'], now(), r['owner'] or 'registry', 'done', evidence))
    return {'task': key, 'state': 'done'}


# ------------------------------------------------------------------ context (complete, budgeted)
def context(conn, needle, budget=12000, device=None):
    """A self-contained briefing: task, its components, contracts, binaries, typed edges, claims (with evidence
    and reviews), artifacts, and the precise open question.

    The core (identity, contracts, claims, the question) is always included; binaries/edges/sources/artifacts
    are added while the JSON stays inside `budget`. Trimming therefore removes detail, never the answer."""
    reap_expired(conn)
    task = conn.execute('SELECT * FROM task WHERE key=? OR title LIKE ?', (needle, '%' + needle + '%')).fetchone()
    if task is not None:
        comps = [dict(r) for r in conn.execute(
            'SELECT c.* FROM component c JOIN task_component tc ON tc.component_id=c.id WHERE tc.task_id=?'
            ' ORDER BY c.key', (task['id'],))]
        contracts = q_contracts(conn, task_id=task['id'])
        claims = q_claims(conn, task_id=task['id'], include_superseded=True)
    else:
        row = conn.execute('SELECT * FROM component WHERE key=? OR title LIKE ? OR key LIKE ?',
                           (needle, '%' + needle + '%', '%' + needle.lower() + '%')).fetchone()
        if row is None:
            raise SystemExit('nothing matches %r' % needle)
        comps = [dict(row)]
        contracts = q_contracts(conn, row['id'])
        claims = q_claims(conn, component_id=row['id'], include_superseded=True)

    core = {'budget': budget, 'device': device or '', 'strategy_help': STRATEGY_HELP}
    if task is not None:
        core['task'] = {k: task[k] for k in ('key', 'title', 'question', 'acceptance', 'effort_limit', 'state',
                                             'owner', 'lease_until', 'dependencies', 'revision')}
        core['task']['contracts'] = [c['key'] for c in contracts]
        core['task']['objectives'] = [r['key'] for r in conn.execute(
            'SELECT o.key FROM objective o WHERE o.id=?', (task['objective'],))] if task['objective'] else []
    core['components'] = []
    for c in comps:
        core['components'].append({
            'key': c['key'], 'title': c['title'], 'role': c['role'], 'strategy': c['strategy'],
            'source_availability': c['source_availability'],
            'notes': (c['notes'][:600] + '…') if len(c['notes']) > 600 else c['notes'],
            'subsystems': [r['subsystem'] for r in conn.execute(
                'SELECT subsystem FROM membership WHERE component_id=?', (c['id'],))]})
    core['contracts'] = [{k: (c[k][:400] + '…' if isinstance(c[k], str) and len(c[k]) > 400 else c[k])
                          for k in ('key', 'title', 'kind', 'status', 'participants', 'args', 'ownership',
                                    'completion', 'cancellation', 'errors', 'source')} for c in contracts]
    core['claims'] = [{'id': c['id'], 'task_id': c['task_id'], 'statement': c['statement'],
                       'confidence': c['confidence'], 'status': c['status'],
                       'scope': '%s/%s' % (c['scope_device'], c['scope_firmware']),
                       'supersedes': c['supersedes'], 'evidence': c['evidence'], 'reviews': c['reviews']}
                      for c in claims]
    core['next_question'] = (core.get('task', {}).get('question') or '').strip()

    # optional detail, added only while it fits
    out = json.loads(json.dumps(core))
    for i, src in enumerate(comps):
        c = out['components'][i]
        c['binaries'] = [{k: b[k] for k in ('device', 'firmware', 'path', 'uid3', 'codesize', 'origin',
                                            'base', 'parse_error')}
                         for b in q_binaries(conn, src['id'], device)]
        c['edges'] = q_edge_summary(conn, src['id'], device)
        c['sources'] = [dict(r) for r in conn.execute(
            'SELECT repo, revision, recipe, relation FROM source WHERE component_id=?', (src['id'],))]
        c['mentioned_in'] = [r['path'] for r in conn.execute(
            'SELECT a.path FROM mention m JOIN artifact a ON a.id=m.artifact_id WHERE m.component_id=? LIMIT 6',
            (src['id'],))]
        for drop in ('mentioned_in', 'sources', None, 'edges', 'binaries'):
            if len(json.dumps(out)) <= budget:
                break
            if drop is None:
                for k in ('imports', 'imported_by'):
                    if isinstance(c['edges'].get(k), dict) and 'top' in c['edges'][k]:
                        c['edges'][k]['top'] = c['edges'][k]['top'][:2]
            elif drop == 'binaries':
                c['binaries'] = c['binaries'][:1]
            else:
                c.pop(drop, None)
    # artifacts about THIS subject: the ones that mention its components, and the task's run logs (it was the
    # first 40 artifacts of the whole project)
    ids = json.dumps([c['id'] for c in comps])
    arts = [dict(r) for r in conn.execute(
        'SELECT DISTINCT a.kind, a.path, a.sha1, a.tool, a.tool_version, a.provenance, a.result FROM artifact a'
        ' LEFT JOIN mention m ON m.artifact_id=a.id'
        ' WHERE m.component_id IN (SELECT value FROM json_each(?)) OR (a.task_id IS NOT NULL AND a.task_id=?)'
        " ORDER BY a.kind='doc', a.kind, a.path LIMIT 40", (ids, task['id'] if task is not None else -1))]
    out['artifacts'] = arts
    while len(json.dumps(out)) > budget and out['artifacts']:
        out['artifacts'].pop()
    if len(json.dumps(out)) > budget:
        for c in out['components']:
            c['notes'] = c['notes'][:300]
        out['trimmed'] = True
    return out


# ------------------------------------------------------------------ backup / restore / export
def backup(conn, out_path):
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    dst = sqlite3.connect(out_path)
    with dst:
        conn.backup(dst)
    dst.close()
    return out_path


def restore(src_path, dst_path=None, force=False):
    dst_path = dst_path or store_path()
    if os.path.exists(dst_path) and not force:
        raise SystemExit('%s exists; pass --force to replace it' % dst_path)
    if os.path.exists(dst_path):
        os.unlink(dst_path)
    for suffix in ('-wal', '-shm'):
        if os.path.exists(dst_path + suffix):
            os.unlink(dst_path + suffix)
    os.makedirs(os.path.dirname(os.path.abspath(dst_path)), exist_ok=True)
    shutil.copyfile(src_path, dst_path)
    return dst_path


def export(conn, out_dir, target=None):
    """Complete snapshot: every table that carries a fact, including superseded claims and history."""
    os.makedirs(out_dir, exist_ok=True)
    tables = ['component', 'membership', 'component_alias', 'binary', 'edge', 'source', 'contract', 'objective',
              'task', 'task_component', 'task_contract', 'task_state', 'claim', 'evidence', 'review', 'artifact',
              'artifact_version', 'target', 'meta']
    dump = {t: [dict(r) for r in conn.execute('SELECT * FROM "%s"' % t)] for t in tables}
    reap_expired(conn)
    data = {'schema': SCHEMA, 'generated': now(), 'strategies': STRATEGY_HELP,
            'device': (target or {}).get('device', ''), 'firmware': (target or {}).get('firmware', ''),
            'components': q_components(conn, device=(target or {}).get('device'), limit=100000),
            'contracts': q_contracts(conn), 'claims': q_claims(conn, include_superseded=True),
            'tasks': q_tasks(conn, include_done=True, device=(target or {}).get('device')),
            'objectives': [dict(r) for r in conn.execute('SELECT * FROM objective ORDER BY key')],
            'targets': [{**dict(r), 'open': json.loads(r['open'] or '[]'),
                         'milestones': json.loads(r['milestones'] or '{}')}
                        for r in conn.execute('SELECT * FROM target ORDER BY device')],
            'blocking_contracts': [dict(r) for r in conn.execute(
                "SELECT key,title,status,completion,cancellation FROM contract WHERE status!='known' ORDER BY key")],
            'ready_tasks': [{k: r[k] for k in ('key', 'title', 'acceptance', 'effort_limit', 'objective_key')}
                            for r in q_tasks(conn, ready_only=True,
                                             device=(target or {}).get('device'))],
            'artifacts': [dict(r) for r in conn.execute(
                'SELECT kind,path,sha1,tool,tool_version,provenance,target,result FROM artifact ORDER BY kind,path')],
            'edges_by_type': {r['type']: r['c'] for r in conn.execute(
                'SELECT type, count(*) c FROM edge GROUP BY type ORDER BY c DESC')},
            'counts': {t: len(v) for t, v in dump.items()},
            'unknown_codesize': conn.execute('SELECT count(*) c FROM binary WHERE codesize IS NULL AND stale=0'
                                             ).fetchone()['c'],
            'edges_missing_dst': conn.execute("SELECT count(*) c FROM edge WHERE type='dll-import' AND dst IS NULL"
                                              ).fetchone()['c'],
            'parse_errors': [dict(r) for r in conn.execute(
                'SELECT device,firmware,path,parse_error FROM binary WHERE parse_error IS NOT NULL AND stale=0')],
            'unassigned_binaries': conn.execute('SELECT count(*) c FROM binary WHERE component_id IS NULL'
                                                ' AND stale=0').fetchone()['c']}
    data['dump'] = dump
    json.dump(data, open(os.path.join(out_dir, 'registry.json'), 'w'), indent=1)
    open(os.path.join(out_dir, 'snapshot.html'), 'w').write(
        PAGE.replace('__DATA__', json.dumps(
            {k: v for k, v in data.items() if k != 'dump'}, separators=(',', ':')).replace('</', '<\\/')))
    open(os.path.join(out_dir, 'registry.md'), 'w').write(markdown(data))
    backup(conn, os.path.join(out_dir, 'registry.backup.sqlite3'))
    return data


def markdown(d):
    L = ['# Research registry', '', 'Generated %s (schema %d)' % (d['generated'], d['schema']), '']
    L += ['## Objectives', '']
    for o in d['objectives']:
        L += ['### %s - %s' % (o['title'], o['state']), '']
        for t in [x for x in d['tasks'] if x['objective_key'] == o['key']]:
            L += ['- **%s** (%s) - %s' % (t['title'], t['state'], t['acceptance'] or 'no acceptance criteria yet'),
                  '  - question: %s' % (t['question'] or '-'),
                  '  - effort: %s%s' % (t['effort_limit'] or 'unset',
                                        ', depends on ' + t['dependencies'] if t['dependencies'] != '[]' else '')]
        L += ['']
    L += ['## Contracts', '']
    for c in d['contracts']:
        L += ['### %s (%s) - %s' % (c['title'], c['kind'] or '?', c['status']), '']
        for k in ('participants', 'args', 'ownership', 'completion', 'cancellation', 'errors'):
            if c[k]:
                L += ['- *%s*: %s' % (k, c[k])]
        if c['source']:
            L += ['- *source*: %s' % c['source']]
        L += ['']
    L += ['## Components by strategy', '']
    for s in STRATEGIES:
        rows = [c for c in d['components'] if c['strategy'] == s]
        if not rows:
            continue
        L += ['### %s - %s (%d)' % (s, STRATEGY_HELP[s], len(rows)), '',
              '| component | role | binaries | code KiB | subsystem |', '|---|---|---|---|---|']
        for c in sorted(rows, key=lambda x: -(x['codesize'] or 0))[:40]:
            L += ['| %s | %s | %d | %s | %s |' % (c['key'], (c['role'] or '')[:60], c['binaries'],
                                                  round((c['codesize'] or 0) / 1024) or '?', c['subsystems'])]
        L += ['']
    L += ['## Claims', '']
    for c in d['claims']:
        L += ['- [%s/%s] %s (%s, scope %s/%s)%s' % (c['confidence'], c['status'], c['statement'],
                                                    c['author'] or '?', c['scope_device'] or '*',
                                                    c['scope_firmware'] or '*',
                                                    '' if not c['evidence'] else
                                                    '  \n  evidence: ' + ', '.join(
                                                        '%s=%s' % (e['kind'], e['ref']) for e in c['evidence']))]
    L += ['', '## Data quality', '',
          '- binaries with unknown code size: %d' % d['unknown_codesize'],
          '- parse failures: %d' % len(d['parse_errors']),
          '- edges by type: %s' % ', '.join('%s %d' % (k, v) for k, v in d['edges_by_type'].items()), '']
    return '\n'.join(L) + '\n'


PAGE = r'''<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>qembian research registry</title><style>
:root{--bg:#11141a;--bg2:#191d26;--bg3:#222836;--fg:#e2e6ef;--dim:#8b93a7;--acc:#7fb0ff;--line:#2b3240}
*{box-sizing:border-box}body{margin:0;font:13px/1.5 ui-sans-serif,system-ui,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:12px 16px;border-bottom:1px solid var(--line);background:var(--bg2)}
h1{margin:0;font-size:15px}.sub{color:var(--dim);font-size:12px}
nav{display:flex;gap:4px;padding:8px 16px;border-bottom:1px solid var(--line);background:var(--bg2)}
nav button{background:var(--bg3);border:1px solid var(--line);color:var(--fg);padding:4px 10px;border-radius:6px;cursor:pointer}
nav button.on{border-color:var(--acc);color:var(--acc)}
main{padding:14px 16px}table{width:100%;border-collapse:collapse}
th{text-align:left;font-size:11px;color:var(--dim);padding:5px 6px;border-bottom:1px solid var(--line);cursor:pointer}
td{padding:4px 6px;border-bottom:1px solid #1d222c;vertical-align:top}
tr:hover{background:#1b202b}code{font-family:ui-monospace,monospace;font-size:11px;color:#a8bde0}
input[type=search]{background:var(--bg3);border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:5px 8px;min-width:260px}
select{background:var(--bg3);border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:5px 8px}
.st{font-size:10px;padding:1px 6px;border-radius:9px;border:1px solid var(--line);color:var(--dim);white-space:nowrap}
.st.retain{color:#8ce8c6;border-color:#2f6b57}.st.build-source{color:#8fc4ff;border-color:#2d5b8f}
.st.adapt-source{color:#b9a6ff;border-color:#4b3d8f}.st.replace-interface{color:#e0c47a;border-color:#7d6529}
.st.reverse-selected{color:#f0a3a8;border-color:#8f3038}.st.defer{color:#8b93a7}.st.undecided{color:#c9d1e3}
.note{white-space:pre-wrap;background:var(--bg3);border-left:2px solid var(--acc);padding:8px 10px;border-radius:0 6px 6px 0}
.chip{display:inline-block;font-size:11px;padding:1px 7px;border:1px solid var(--line);border-radius:9px;color:#c3cbe0;margin:1px}
.warn{color:#e0c47a}.bad{color:#f0a3a8}.ok{color:#8ce8c6}
</style></head><body>
<header><h1>qembian research registry</h1>
<div class="sub" id="meta"></div></header>
<nav><button data-v="home" class="on">status</button><button data-v="objectives">objectives &amp; tasks</button><button data-v="components">components</button>
<button data-v="contracts">contracts</button><button data-v="claims">claims</button><button data-v="data">data quality</button></nav>
<main id="main"></main>
<script type="application/json" id="data">__DATA__</script>
<script>
const D=JSON.parse(document.getElementById('data').textContent);
const e=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const KiB=n=>n==null?'?':(n>=1048576?(n/1048576).toFixed(1)+' MiB':(n/1024).toFixed(1)+' KiB');
document.getElementById('meta').textContent=D.components.length+' components · '+D.tasks.length+' tasks · '+
  D.claims.length+' claims · '+D.contracts.length+' contracts · generated '+D.generated+
  ' · unknown sizes: '+D.unknown_codesize+' · parse failures: '+D.parse_errors.length;
let view='home', q='', strat='', sort='key', dir=1;
const main=document.getElementById('main');
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{view=b.dataset.v;
  document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('on',x===b));render();});
function comps(){
  let rows=D.components.filter(c=>{
    if(strat&&c.strategy!==strat)return false;
    if(!q)return true; const s=q.toLowerCase();
    return (c.key+' '+c.title+' '+c.role+' '+c.notes).toLowerCase().includes(s);});
  rows.sort((a,b)=>{const k=sort;const x=a[k]??'',y=b[k]??'';
    return (typeof x==='number'&&typeof y==='number'?x-y:String(x).localeCompare(String(y)))*dir;});
  return rows;
}
function renderComponents(){
  const rows=comps();
  main.innerHTML=`<div style="margin-bottom:10px;display:flex;gap:8px;flex-wrap:wrap">
    <input type="search" id="q" placeholder="search component / role / note" value="${e(q)}">
    <select id="strat"><option value="">all strategies</option>${D.components.length?
      Object.keys(D.strategies).map(s=>`<option ${s===strat?'selected':''}>${s}</option>`).join(''):''}</select>
    <span class="sub">${rows.length} shown</span></div>
    <table><thead><tr>${['key','strategy','role','binaries','codesize','in_edges','mentions','subsystems']
      .map(k=>`<th data-k="${k}">${k}</th>`).join('')}</tr></thead><tbody>
    ${rows.map(c=>`<tr><td><code>${e(c.key)}</code></td>
      <td><span class="st ${e(c.strategy)}">${e(c.strategy)}</span></td>
      <td>${e((c.role||'').slice(0,90))}</td><td>${c.binaries}</td><td>${KiB(c.codesize)}</td>
      <td>${c.in_edges}</td><td>${c.mentions}</td><td>${c.subsystems}</td></tr>`).join('')}
    </tbody></table>`;
  const qi=document.getElementById('q');
  qi.oninput=()=>{q=qi.value;render();}; qi.focus(); qi.setSelectionRange(q.length,q.length);
  document.getElementById('strat').onchange=ev=>{strat=ev.target.value;render();};
  main.querySelectorAll('th').forEach(th=>th.onclick=()=>{
    const k=th.dataset.k; if(sort===k)dir=-dir; else {sort=k;dir=1;} render();});
}
function renderObjectives(){
  main.innerHTML=D.objectives.map(o=>`<section style="margin-bottom:18px">
    <h3 style="margin:0 0 6px">${e(o.title)} <span class="st">${e(o.state)}</span></h3>
    ${o.notes?`<div class="note">${e(o.notes)}</div>`:''}
    <table><thead><tr><th>task</th><th>state</th><th>acceptance criteria</th><th>effort</th><th>deps</th></tr></thead><tbody>
    ${D.tasks.filter(t=>t.objective_key===o.key).map(t=>`<tr>
      <td><code>${e(t.key)}</code><br><span class="sub">${e(t.question||'')}</span></td>
      <td><span class="st">${e(t.state)}</span>${t.owner?'<br><span class="sub">'+e(t.owner)+'</span>':''}</td>
      <td>${t.acceptance?e(t.acceptance):'<span class="warn">no acceptance criteria yet</span>'}</td>
      <td>${e(t.effort_limit||'')}</td><td><code>${e(t.dependencies)}</code></td></tr>`).join('')}
    </tbody></table></section>`).join('');
}
function renderContracts(){
  main.innerHTML=D.contracts.map(c=>`<section style="margin-bottom:16px">
    <h3 style="margin:0 0 4px"><code>${e(c.key)}</code> <span class="st">${e(c.status)}</span></h3>
    <div class="sub">${e(c.title)} — ${e(c.kind)}</div>
    <table>${['participants','args','ownership','completion','cancellation','errors','source']
      .filter(k=>c[k]).map(k=>`<tr><td class="sub" style="width:120px">${k}</td><td>${e(c[k])}</td></tr>`).join('')}
    </table></section>`).join('') || '<div class="sub">no contracts recorded</div>';
}
function renderClaims(){
  main.innerHTML=`<table><thead><tr><th>claim</th><th>confidence</th><th>status</th><th>scope</th><th>evidence</th></tr></thead><tbody>
  ${D.claims.map(c=>`<tr><td>${e(c.statement)}</td>
    <td class="${c.confidence==='verified'?'ok':''}">${e(c.confidence)}</td>
    <td>${e(c.status)}</td><td><code>${e(c.scope_device)}/${e(c.scope_firmware)}</code></td>
    <td>${(c.evidence||[]).map(x=>`<span class="chip">${e(x.kind)}: ${e(x.ref)}</span>`).join('')}</td></tr>`).join('')}
  </tbody></table>`;
}
function renderData(){
  main.innerHTML=`<p>Binaries with unknown code size: <b class="${D.unknown_codesize?'warn':'ok'}">${D.unknown_codesize}</b><br>
  Parse failures: <b class="${D.parse_errors.length?'bad':'ok'}">${D.parse_errors.length}</b></p>
  <p>Edges by type: ${Object.entries(D.edges_by_type).map(([k,v])=>`<span class="chip">${e(k)} ${v}</span>`).join('')}</p>
  <p class="sub">Artifacts referenced (never stored): ${D.artifacts.length}</p>`;
}
function renderHome(){
  const targets=D.targets.map(t=>`<section style="margin-bottom:14px">
    <h3 style="margin:0 0 4px">${e(t.device)} <span class="st">${e(t.level||'?')}</span>
      <span class="sub">${e(t.firmware)} ${t.updated?'· '+e(t.updated):''}</span></h3>
    ${t.open&&t.open.length?`<div class="sub">open:</div><ul>${t.open.map(x=>`<li>${e(x)}</li>`).join('')}</ul>`:''}
    ${Object.keys(t.milestones||{}).length?`<div class="sub">milestones: ${Object.entries(t.milestones).map(([k,v])=>`<span class="chip">${e(k)}: ${e(v)}</span>`).join('')}</div>`:''}
    </section>`).join('') || '<div class="sub">no device status recorded</div>';
  const blockers=D.blocking_contracts.map(c=>`<li><code>${e(c.key)}</code> <span class="st">${e(c.status)}</span>
    — ${e(c.title)}<br><span class="sub">completion: ${e((c.completion||'').slice(0,240))}</span></li>`).join('');
  const pick=D.ready_tasks.map(t=>`<li><code>${e(t.key)}</code> <span class="sub">${e(t.objective_key||'')} · ${e(t.effort_limit||'')}</span>
    <br>${e(t.title)}<br><span class="sub">accept: ${e((t.acceptance||'none').slice(0,200))}</span></li>`).join('');
  const disputed=D.claims.filter(c=>c.status==='disputed'||c.confidence==='inferred').slice(0,12)
    .map(c=>`<li>${e(c.statement)} <span class="chip">${e(c.confidence)}</span>`+
            (c.evidence||[]).map(x=>`<span class="chip">${e(x.kind)}: ${e(x.ref)}</span>`).join('')+'</li>').join('');
  main.innerHTML=`<div style="display:grid;grid-template-columns:1fr 1fr;gap:18px">
    <div><h4 style="margin:0 0 6px">targets</h4>${targets}</div>
    <div><h4 style="margin:0 0 6px">contracts that block the next capability</h4><ul>${blockers||'<li class="sub">none recorded</li>'}</ul>
      <h4 style="margin:12px 0 6px">bounded tasks an agent can pick up now</h4><ul>${pick||'<li class="sub">none ready</li>'}</ul>
      <h4 style="margin:12px 0 6px">known but not verified / disputed</h4><ul>${disputed||'<li class="sub">none</li>'}</ul></div>
  </div>`;
}
function render(){({home:renderHome,components:renderComponents,objectives:renderObjectives,
  contracts:renderContracts,claims:renderClaims,data:renderData}[view])();}
render();
</script></body></html>
'''


def KiB(n):
    return '?' if n is None else ('%.1fM' % (n / 1048576) if n >= 1048576 else '%.0fK' % (n / 1024))


def _context_text(c):
    L = []
    if 'task' in c:
        t = c['task']
        L += ['TASK %s [%s]  effort %s  revision %s' % (t['key'], t['state'], t['effort_limit'], t['revision']),
              '  %s' % t['title'], '  question: %s' % t['question'], '  accept: %s' % t['acceptance'],
              '  components: %s   contracts: %s' % (', '.join(x['key'] for x in c['components']) or '-',
                                                    ', '.join(t['contracts']) or '-'), '']
    for m in c['components']:
        L += ['COMPONENT %s  strategy=%s  source=%s' % (m['key'], m['strategy'], m['source_availability']),
              '  %s' % m['title'], '  role: %s' % m['role'], '  subsystems: %s' % ', '.join(m['subsystems'])]
        if m['notes']:
            L += ['  note: %s' % m['notes']]
        for b in m['binaries']:
            L += ['  binary %s/%s %s  code %s  uid3 %s  %s' % (b['device'], b['firmware'], b['path'],
                                                              KiB(b['codesize']), b['uid3'], b['origin'])]
        for s in m.get('sources', []):
            L += ['  source %s @%s (%s)' % (s['repo'], s['revision'], s['relation'])]
        e = m.get('edges', {})
        if e.get('imports', {}).get('top'):
            L += ['  imports: %s' % ', '.join('%s (%d ord)' % (x['target'], x['ordinals'] or 0)
                                               for x in e['imports']['top'])]
        if e.get('imported_by'):
            ib = e['imported_by']
            L += ['  imported by %d modules (%d ord): %s' % (ib['modules'], ib['ordinals'], ', '.join(
                '%s %d' % (x['source'], x['modules']) for x in ib.get('top', [])))]
        for x in e.get('curated', []):
            L += ['  %s: %s' % (x['type'], x['detail'])]
        for x in e.get('ecom', []):
            L += ['  ecom plugin: %s' % x]
        if e.get('ipc'):
            ipc = e['ipc']
            if ipc.get('serves'):
                L += ['  serves: %s' % ', '.join(ipc['serves'])]
            if ipc.get('served_clients'):
                L += ['  clients: %s' % ', '.join('%s (%d modules)' % (x['server'], x['clients'])
                                                   for x in ipc['served_clients'])]
            if ipc.get('client_of'):
                L += ['  client of: %s' % ', '.join(ipc['client_of'])]
        if m.get('mentioned_in'):
            L += ['  mentioned in: %s' % ', '.join(m['mentioned_in'])]
    for k in c['contracts']:
        L += ['CONTRACT %s [%s] completion=%s' % (k['key'], k['status'], (k['completion'] or '')[:200])]
    for cl in c['claims']:
        L += ['CLAIM %s [%s/%s] %s' % (cl['id'], cl['confidence'], cl['status'], cl['statement']),
              '      evidence: %s' % ', '.join('%s=%s%s' % (e['kind'], e['ref'], '' if e['resolved'] else ' (unresolved)')
                                              for e in cl['evidence'] or [])]
    if c.get('next_question'):
        L += ['', 'NEXT: %s' % c['next_question']]
    return '\n'.join(L)


# ------------------------------------------------------------------ CLI
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('seed', help='import docs/registry/*.toml (idempotent)')
    p.add_argument('--device'); p.add_argument('--migrate', action='store_true',
                                               help='explicitly rewrite curated columns (never operational state)')
    p.add_argument('--json', action='store_true')
    p = sub.add_parser('ingest'); p.add_argument('--device'); p.add_argument('--index')
    p.add_argument('--limit', type=int, default=0); p.add_argument('--json', action='store_true')
    p = sub.add_parser('export'); p.add_argument('--device'); p.add_argument('--out', default='build/registry')
    p = sub.add_parser('backup'); p.add_argument('--out', default='build/registry/registry.backup.sqlite3')
    p.add_argument('--device')
    p = sub.add_parser('restore'); p.add_argument('--from', dest='src', required=True)
    p.add_argument('--to'); p.add_argument('--force', action='store_true')
    p = sub.add_parser('search'); p.add_argument('query', nargs='?'); p.add_argument('--strategy')
    p.add_argument('--device'); p.add_argument('--kind'); p.add_argument('--subsystem')
    p.add_argument('--json', action='store_true'); p.add_argument('--limit', type=int, default=80)
    p = sub.add_parser('context'); p.add_argument('needle'); p.add_argument('--budget', type=int, default=4000)
    p.add_argument('--device'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('contracts'); p.add_argument('component', nargs='?')
    p.add_argument('--task'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('tasks'); p.add_argument('--objective'); p.add_argument('--ready', action='store_true')
    p.add_argument('--all', action='store_true'); p.add_argument('--device'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('claim-task'); p.add_argument('task'); p.add_argument('--agent', required=True)
    p.add_argument('--lease', default='2h'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('submit'); p.add_argument('task'); p.add_argument('--statement', required=True)
    p.add_argument('--evidence', action='append', default=[]); p.add_argument('--scope', default='')
    p.add_argument('--confidence', default='inferred'); p.add_argument('--author', default='agent')
    p.add_argument('--token'); p.add_argument('--supersedes', type=int); p.add_argument('--json', action='store_true')
    p = sub.add_parser('review'); p.add_argument('claim', type=int); p.add_argument('--verdict', required=True)
    p.add_argument('--reviewer', default='human'); p.add_argument('--note', default='')
    p.add_argument('--evidence', action='append', default=[]); p.add_argument('--json', action='store_true')
    p = sub.add_parser('record-test'); p.add_argument('task'); p.add_argument('--config', required=True)
    p.add_argument('--artifact', required=True); p.add_argument('--result', required=True)
    p.add_argument('--token'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('reopen'); p.add_argument('task'); p.add_argument('--reason', required=True)
    p.add_argument('--json', action='store_true')
    p = sub.add_parser('complete'); p.add_argument('task'); p.add_argument('--evidence', required=True)
    p.add_argument('--token'); p.add_argument('--agent'); p.add_argument('--json', action='store_true')
    a = ap.parse_args()

    target = resolve_target(getattr(a, 'device', None))
    conn = connect(target['store'])
    try:
        if a.cmd == 'seed':
            out = seed(conn, target, migrate=a.migrate)
        elif a.cmd == 'ingest':
            out = ingest(conn, target, index_path=a.index,
                         limit=a.limit) if a.index or a.limit else ingest(conn, target)
        elif a.cmd == 'export':
            out = export(conn, a.out if os.path.isabs(a.out) else os.path.join(ROOT, a.out), target)
            out = {'files': ['registry.json', 'registry.md', 'snapshot.html', 'registry.backup.sqlite3'],
                   'counts': out['counts']}
        elif a.cmd == 'backup':
            out = {'backup': backup(conn, a.out if os.path.isabs(a.out) else os.path.join(ROOT, a.out))}
        elif a.cmd == 'restore':
            out = {'restored': restore(a.src, a.to, a.force)}
        elif a.cmd == 'search':
            rows = q_components(conn, a.query, a.strategy, a.device or target['device'], a.subsystem, a.kind,
                                a.limit)
            out = rows
            if not a.json:
                for r in rows:
                    print('%-28s %-18s %-46s %3d bin %8s %s' % (
                        r['key'], r['strategy'], (r['role'] or '')[:46], r['binaries'], KiB(r['codesize']),
                        ','.join(x['subsystem'] for x in conn.execute(
                            'SELECT subsystem FROM membership WHERE component_id=?', (r['id'],))) or '-'))
                return
        elif a.cmd == 'context':
            out = context(conn, a.needle, a.budget, a.device or target['device'])
            if not a.json:
                print(_context_text(out))
                return
        elif a.cmd == 'contracts':
            tid = None
            if a.task:
                r = conn.execute('SELECT id FROM task WHERE key=?', (a.task,)).fetchone()
                tid = r['id'] if r else None
            cid = None
            if a.component:
                r = conn.execute('SELECT id FROM component WHERE key=?', (a.component,)).fetchone()
                cid = r['id'] if r else None
            out = q_contracts(conn, cid, tid)
            if not a.json:
                for r in out:
                    print('%s (%s) %s\n  args: %s\n  completion: %s\n  cancellation: %s\n  errors: %s'
                          % (r['key'], r['kind'], r['status'], r['args'], r['completion'], r['cancellation'],
                             r['errors']))
                return
        elif a.cmd == 'tasks':
            out = q_tasks(conn, a.objective, a.ready, a.all, a.device or target['device'])
            if not a.json:
                for r in out:
                    print('%-32s %-8s %-10s %-22s %s' % (r['key'], r['state'], r['effort_limit'] or '-',
                                                         r['ready_reason'], r['objective_key'] or '-'))
                return
        elif a.cmd == 'claim-task':
            out = claim_task(conn, a.task, a.agent, a.lease)
        elif a.cmd == 'submit':
            out = submit(conn, a.task, a.statement, a.evidence, a.scope, a.confidence, a.author, a.supersedes,
                         a.token)
        elif a.cmd == 'review':
            out = review(conn, a.claim, a.verdict, a.reviewer, a.note, a.evidence)
        elif a.cmd == 'record-test':
            out = record_test(conn, a.task, a.config, a.artifact, a.result, a.token)
        elif a.cmd == 'reopen':
            out = reopen(conn, a.task, a.reason)
        elif a.cmd == 'complete':
            out = complete(conn, a.task, a.evidence, a.token, a.agent)
        else:
            raise SystemExit('unknown command')
        print(json.dumps(out, indent=1))
    finally:
        conn.close()


if __name__ == '__main__':
    main()
