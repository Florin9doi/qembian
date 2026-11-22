# Research registry: the model behind `tools/registry.py`

The map that came before this (`tools/workmap.py`, removed) ranked every DLL by an invented "work done"
percentage. That was wrong in a way that matters: **QtCore.dll having 229 importers is a dependency fact, not
a work order.** The registry replaces "how much is done per binary" with: what is this component, what may
its consumers rely on, what do we actually know, and which bounded question unblocks the next capability.

## Objects

| object | holds |
|---|---|
| **component** | a logical role (`gce-render-stage`, `qt`, `tsy-nokia`), its strategy, source availability, notes, and its subsystem memberships |
| **binary** | one module in one firmware: device, firmware, path, uid3, arch, code size, hash (crc32 today), extraction provenance, load base (the code address for XIP), parse error, `shadowed_by` (the same path in a higher ROM layer is the one loaded); its component is NULL until seed or curation gives it a role |
| **edge** | a *typed* relationship (`dll-import`: one per module pair, `n` = ordinals; the slots stay in the import index;
`ecom-plugin`: one per implementation, from `tools/ecom_index.py`; `ipc-client-server`: from `tools/ipc_index.py`, one
`serves !Name` row per module that starts a named server and one `client of !Name` edge per client module -> the
provider binary): `dll-import`, `ecom-plugin`, `ipc-client-server`, `ldd-pdd`, `shared-memory`, `device-protocol`, `replacement`, `adaptation`, `build-dependency`, `rom-file-override`, `byte-patch` |
| **source** | a source candidate for a component: repo, revision, build recipe, and whether it is exact / related / unknown to the shipped binary |
| **contract** | what an outside participant may rely on: participants, arguments, ownership, completion, cancellation, errors, status (`known` / `partial` / `unknown`) |
| **claim** | one statement with an exact scope (device / firmware), evidence, confidence (`verified` / `inferred`), and status; corrections **supersede** instead of erasing |
| **task** | a bounded question with acceptance criteria, the objective it unblocks, dependencies and an effort limit |
| **artifact** | a doc, test suite, decompilation, Ghidra program, patch or run log: path + hash + the command/tool that produced it |

A component is deliberately separate from its binaries: a finding on the N95 must not silently become an 808
fact. Claims carry the scope; binaries carry the firmware identity; addresses in claims are module-relative
(`0x830A`), and the load address lives on the binary row.

## Strategies (what an agent should do)

`retain` · `build-source` · `adapt-source` · `replace-interface` · `reverse-selected` · `defer` · `undecided`

`undecided` is a real answer: it means "do the short source/compatibility assessment first". Qt and Avkon are
`undecided` for that reason. `defer` requires a recorded reason (VideoCore, BT/FM). Only components whose
observable contract is needed *and* whose source cannot be used are `replace-interface` or
`reverse-selected`.

## Store

One project-wide SQLite file (`build/registry/registry.sqlite3`, WAL); every row that is device-specific
carries device+firmware, so logical components and tasks are shared while binaries and findings stay scoped.
`$REGISTRY_DB` overrides the path (tests, forks). JSON / Markdown / HTML plus a SQLite backup are exports,
never inputs.

```
tools/registry.py seed      # docs/registry/*.toml -> DB; content-hashed, so a second run is a no-op
tools/registry.py seed --migrate   # explicit rewrite of curated columns; operational state is never touched
tools/import_index.py build --device ID   # <devdir>/imports.json: every E32 image, thunks, ordinal names
tools/ecom_index.py build --device ID     # <devdir>/ecom.json: plugin DLL -> interface -> implementation
tools/ipc_index.py build --device ID      # <devdir>/ipc.json: named servers -> starter module, client modules
tools/registry.py ingest    # both indexes + resolved device profile + repo artifacts -> DB (idempotent)
tools/registry.py export    # build/registry/{registry.json,registry.md,snapshot.html,registry.backup.sqlite3}
tools/registry.py backup --out FILE     # SQLite's own consistent copy
tools/registry.py restore --from FILE [--to PATH] [--force]
```

`docs/registry/*.toml` are **seed only**. The seed records each file's sha256 in `meta`, so re-seeding is a
no-op until a file changes; a changed file only *adds* missing rows, and `--migrate` is the explicit way to
rewrite curated prose. Nothing in the seed path can reset a task lease, an owner, a review or a claim.

`ingest` records what exists and never promotes existence into understanding: a generated `.c` is a
`decompilation` artifact with the Ghidra version when it can be read from the log (else provenance
`unknown`), an importable `.elf` is a `ghidra-elf` (no saved annotations), a file in `tests/` is a
`test-suite`, never an `on-device-test`. Binaries are inserted first and edges resolved second against
firmware-scoped binaries through the same alias resolver that assigns components; regenerated edges replace
the previous set, binaries that disappeared are marked `stale` with their edges removed, artifact hashes are
refreshed into `artifact_version` history, and imports whose DLL is not a module of this firmware keep a
`NULL` destination and are counted in the export (`edges_missing_dst`).

## Agent workflows (CLI, and the MCP server `registry` in .mcp.json: tools/registry_mcp.py)

```
tools/registry.py search "haptics" --strategy reverse-selected
tools/registry.py context gce-render-stage          # compact briefing: binaries, edges, contracts, claims, tasks
tools/registry.py tasks --ready                     # bounded, unblocked, with acceptance criteria
tools/registry.py claim-task wserv-surface-update-contract --agent me --lease 1h
tools/registry.py submit wserv-surface-update-contract --statement "..." --evidence doc=... --confidence verified
tools/registry.py review 7 --verdict accepted --evidence tests/probes/surface-update.log
tools/registry.py record-test wserv-surface-update-contract --config nokia-808-rm807/113.010.1508 \
    --artifact build/runs/x/serial.log --result passed
tools/registry.py complete wserv-surface-update-contract --evidence build/runs/x/serial.log
```

`context` is the important one: it returns the binary identity, typed edges, contracts, claims (with their
evidence and disputes), the artifact list and the precise unanswered question, so an agent does not
rediscover the same background. `--json` on every command is what the MCP adapter will call.

One ruleset decides eligibility (`task_readiness`), and listing, claiming and completion all use it:
blocked/dropped tasks are not claimable, dependencies must be `done`, an expired lease is reaped back to
`ready` (the event is kept in `task_state`), claims carry a lease **token** and a revision, and `complete`
needs the token plus acceptance evidence. Two connections cannot both win a claim.

`submit --confidence verified` needs at least one evidence reference that resolves to a file or `cmd:` (a
reviewer can check it); scope is mandatory (`device/firmware`, or `device/*`); a correction must keep the old
claim's subject and scope and marks it `superseded` rather than deleting it; reviews are events with reviewer
and time, so a later verdict never erases an earlier one. A claim records the task it came from and every
component/contract of that task, so it is visible from the task briefing *and* from the component briefing.

## Review 2026-11-17 (schema 5)

The first version was checked against the 808 ROM and its own code; fixed, each with a regression test in
`tests/registry_test.py` / `tests/import_index_test.py`:

- **Data**: the import index missed 52 modules (XIP images without imports, `ekern.exe` among them; ROFS images
  whose extension was not in a list: `.drv`, `.pxy`, `.agt`, `Elocl.NN`); XIP thunk offsets were 4 bytes early;
  `build --device X` used `$SYBORG_DEVICE`'s directory; five XIP modules re-shipped in ROFS2 were two live
  binaries (now `shadowed_by`, imports go to the loaded copy).
- **Graph**: imports resolved through the component, so every import of `avkon.dll` landed on
  `AknKeyRotator.dll` (177k edges) and every Qt import on `JavascriptCore.dll`; edges had no components, so
  `context` never showed them. Now one edge per module pair with `n`, resolved by file name, components filled:
  835k rows / 294 MB became 43k rows / 17 MB, the export 355 MB -> 21 MB.
- **Components**: ingest invented one `undecided` component per module (4,286 of 4,301), which made `undecided`
  mean both "assess first" and "nobody looked". Binaries without a curated role now have no component
  (`unassigned_binaries` in the export); `migrate_4_to_5` removed the invented ones and kept every claim.
- **Workflow**: the lease token was optional (anyone could submit "verified", record tests, complete); a review
  could revive a superseded claim and a claim could be superseded twice; a re-recorded test linked its history
  to an unrelated artifact; a parse error was marked `stale` and dropped from reports; claims used the bare
  version (`113.010.1508`) and binaries the firmware key, so they never joined (now the profile's key
  everywhere); every seeded component said `seeded_from = contracts.toml`; `seed --migrate` reported 0.
- **Seeds**: the `hctl-bc02-transport` contract carried readings the research document had since corrected
  (ACL length endianness, event 0x10 = Hardware Error, DoCancel takes a mask, the config block offsets); the
  gce-factory contract named an N8 module as an 808 user; "KErrCancelled" is KErrCancel.

## What is not done yet

- **IPC edges** (2026-11-19, `tools/ipc_index.py`): protected servers only (`!Name` literals, the module importing
  `CServer2::StartL` is the provider, the ones importing `RSessionBase::CreateSession` the clients): 153 names on
  the 808, 143 with a provider. Not seen: names built at run time or passed by RootServer (`!SocketServer`,
  `!RootServer`, `!ConnectionServer`), unnamed servers, and which *subsession/opcode* a client uses. Clients of
  a server in the same component (Ws32.dll for `!Windowserver`) are not listed: the component's importers are.
- **Shared-memory edges**: no ingest path yet.
- **Served UI with write access**: `registry.py export` produces a read-only snapshot.
- **Ghidra preparation**: importing a binary, fixing `TMode`, and saving the program is still outside the registry.
- **Other devices**: the 808, the 701 (Belle FP2, 4677 binaries) and the N8 011.012 (Symbian^3, 4196) are ingested
  (2026-11-19); `import_index.py` / `ecom_index.py` / `ipc_index.py build --device ID` + `registry.py ingest
  --device ID` add one. A claim scoped `device/*` or to one firmware stays that device's; the N8's Belle firmware
  (nokia-n8-rm596-v111) is not ingested yet.
