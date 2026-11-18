#!/usr/bin/env python3
"""Research registry MCP server (stdio, JSON-RPC 2.0, newline-delimited): tools/registry.py for agents.

Run: python3 tools/registry_mcp.py   (registered in .mcp.json as "registry")

Every tool is one function of tools/registry.py (or tools/import_index.py for per-ordinal questions) on a fresh
connection; results are compact JSON text. The rules are the registry's own: a task must be claimed
(registry_claim_task returns the lease token) before findings, test records or completion; "verified" needs
evidence that resolves to a file or cmd:; scope is device/firmware. Reviews are for people: no review tool.
"""
import contextlib, io, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import registry as R            # noqa: E402
import import_index as II       # noqa: E402

PROTO = '2024-11-05'
STR, INT, BOOL = {'type': 'string'}, {'type': 'integer'}, {'type': 'boolean'}
ARR = {'type': 'array', 'items': {'type': 'string'}}


def obj(props, req=()):
    return {'type': 'object', 'properties': props, 'required': list(req)}


TOOLS = [
    ('registry_search', 'Find components (logical roles: gce-render-stage, tsy-nokia, qt, ...) by text, strategy '
     '(retain/build-source/adapt-source/replace-interface/reverse-selected/defer/undecided) or subsystem.',
     obj({'query': STR, 'strategy': STR, 'subsystem': STR, 'device': STR, 'limit': INT})),
    ('registry_context', 'The briefing to read before any work: for a task key or a component key, its components, '
     'binaries, import graph summary, contracts, claims with evidence and reviews, artifacts and the open question.',
     obj({'needle': STR, 'budget': INT, 'device': STR}, ['needle'])),
    ('registry_contracts', 'Contracts (what an outside participant may rely on: args, completion, cancellation, '
     'errors, status known/partial/unknown) of a component or a task.', obj({'component': STR, 'task': STR})),
    ('registry_tasks', 'Bounded research tasks with acceptance criteria; ready=true lists only claimable ones.',
     obj({'ready': BOOL, 'objective': STR, 'all': BOOL})),
    ('registry_claim_task', 'Claim a ready task for a lease (30m, 2h). Returns the lease token every later '
     'submit/record_test/complete needs.', obj({'task': STR, 'agent': STR, 'lease': STR}, ['task', 'agent'])),
    ('registry_submit', 'Record a finding (claim) on a claimed task. scope = device/firmware (or device/*); '
     'evidence = ["doc=path", "decompilation=path", "cmd:..."]; confidence verified needs resolving evidence; '
     'supersedes = the id of the claim this corrects (same subject and scope).',
     obj({'task': STR, 'token': STR, 'statement': STR, 'scope': STR, 'evidence': ARR, 'confidence': STR,
          'author': STR, 'supersedes': INT}, ['task', 'token', 'statement', 'scope'])),
    ('registry_record_test', 'Attach a run log (test result passed/failed/blocked) to a claimed task.',
     obj({'task': STR, 'token': STR, 'config': STR, 'artifact': STR, 'result': STR},
         ['task', 'token', 'config', 'artifact', 'result'])),
    ('registry_complete', 'Complete a claimed task with acceptance evidence (a path or cmd:...).',
     obj({'task': STR, 'token': STR, 'evidence': STR, 'agent': STR}, ['task', 'token', 'evidence'])),
    ('imports_module', "One module's import table from the firmware's import index: dll, ordinal, thunk offset, "
     'name (from the device SDK; unnamed ordinals stay null).', obj({'module': STR, 'device': STR}, ['module'])),
    ('imports_dll', 'What ordinal N of a DLL is, and which modules import it (or all of its ordinals).',
     obj({'dll': STR, 'ordinal': INT, 'grep': STR, 'device': STR}, ['dll'])),
]


def _printed(fn, *a):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        fn(*a)
    return {'text': buf.getvalue()}


class Server:
    def __init__(self, db=None):
        self.db = db

    def tool(self, name, a):
        if name.startswith('imports_'):
            idx = json.load(open(II.index_path(a.get('device'))))
            if name == 'imports_module':
                return _printed(II.command_module, idx, a['module'])
            return _printed(II.command_dll, idx, a['dll'], a.get('ordinal'), a.get('grep'))
        conn = R.connect(self.db or R.store_path(), create=False)
        try:
            if name == 'registry_search':
                rows = R.q_components(conn, a.get('query'), a.get('strategy'), a.get('device'), a.get('subsystem'),
                                      None, a.get('limit') or 40)
                return [{k: r[k] for k in ('key', 'title', 'strategy', 'role', 'binaries', 'in_edges', 'claims')}
                        for r in rows]
            if name == 'registry_context':
                return R.context(conn, a['needle'], a.get('budget') or 8000, a.get('device'))
            if name == 'registry_contracts':
                cid = tid = None
                if a.get('component'):
                    r = conn.execute('SELECT id FROM component WHERE key=?', (a['component'],)).fetchone()
                    cid = r['id'] if r else -1
                if a.get('task'):
                    r = conn.execute('SELECT id FROM task WHERE key=?', (a['task'],)).fetchone()
                    tid = r['id'] if r else -1
                return R.q_contracts(conn, cid, tid)
            if name == 'registry_tasks':
                return [{k: r[k] for k in ('key', 'title', 'state', 'ready', 'ready_reason', 'effort_limit',
                                           'acceptance', 'objective_key', 'components', 'contracts')}
                        for r in R.q_tasks(conn, a.get('objective'), bool(a.get('ready')), bool(a.get('all')))]
            if name == 'registry_claim_task':
                return R.claim_task(conn, a['task'], a['agent'], a.get('lease') or '2h')
            if name == 'registry_submit':
                return R.submit(conn, a['task'], a['statement'], a.get('evidence') or [], a['scope'],
                                a.get('confidence') or 'inferred', a.get('author') or 'agent', a.get('supersedes'),
                                a['token'])
            if name == 'registry_record_test':
                return R.record_test(conn, a['task'], a['config'], a['artifact'], a['result'], a['token'])
            if name == 'registry_complete':
                return R.complete(conn, a['task'], a['evidence'], a['token'], a.get('agent'))
        finally:
            conn.close()
        raise KeyError(name)

    def handle(self, msg):
        m, mid = msg.get('method'), msg.get('id')
        if m == 'initialize':
            return {'protocolVersion': PROTO, 'capabilities': {'tools': {}},
                    'serverInfo': {'name': 'registry', 'version': '0.1'}}
        if m == 'tools/list':
            return {'tools': [{'name': n, 'description': d, 'inputSchema': s} for n, d, s in TOOLS]}
        if m == 'tools/call':
            p = msg.get('params', {})
            try:
                r = self.tool(p['name'], p.get('arguments') or {})
                text = r['text'] if isinstance(r, dict) and set(r) == {'text'} else json.dumps(r, separators=(',', ':'))
                return {'content': [{'type': 'text', 'text': text}]}
            except SystemExit as e:         # the registry's refusals (not claimed, wrong token, ...) are answers
                return {'content': [{'type': 'text', 'text': 'refused: %s' % e}], 'isError': True}
            except Exception as e:
                return {'content': [{'type': 'text', 'text': 'error: %s: %s' % (type(e).__name__, e)}],
                        'isError': True}
        if m == 'ping':
            return {}
        if mid is None:
            return None
        raise KeyError(m)


def serve(db=None):
    srv = Server(db)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        mid = msg.get('id')
        try:
            res = srv.handle(msg)
            if mid is None:
                continue
            out = {'jsonrpc': '2.0', 'id': mid, 'result': res}
        except KeyError as e:
            out = {'jsonrpc': '2.0', 'id': mid, 'error': {'code': -32601, 'message': 'method not found: %s' % e}}
        except Exception as e:
            out = {'jsonrpc': '2.0', 'id': mid, 'error': {'code': -32603, 'message': str(e)}}
        sys.stdout.write(json.dumps(out) + '\n')
        sys.stdout.flush()


if __name__ == '__main__':
    serve()
