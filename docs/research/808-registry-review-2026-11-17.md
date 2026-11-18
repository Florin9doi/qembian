# The 808 research registry: review, fixes, and what the work is worth (2026-11-17)

The research registry is the agent workflow built while the Claude limits were out: the registry itself,
the import index, the 808 research and the seeds. This page answers four questions.

1. Is the registry sound?
2. Is the 808 data in it right?
3. Did the research give us knowledge?
4. What should we do next, and what does it buy us?

Commits: 9899829 (the fixes and the new parts) and this page.

## 1. The registry (workflow and code)

**The design is good and is kept:**
- A component is a logical role, not one DLL.
- Contracts state what an outside participant may rely on.
- Claims carry a scope (device and firmware) and evidence, and a correction supersedes a claim instead of
  erasing it.
- Tasks are bounded questions with acceptance criteria, leases and dependencies.
- The seeds can be applied twice without change, and a backup is SQLite's own copy.

This is the right shape for agents working in parallel without repeating each other's research.

**The implementation had real defects.** They are listed in `docs/registry/README.md`, "Review 2026-11-17".
The ones that mattered:
- **The import graph was wrong.** Every import of `avkon.dll` pointed at `AknKeyRotator.dll`, and every Qt
  import at `JavascriptCore.dll`. A component briefing never showed import edges.
- **The data was inflated.** There was one row per import slot (835k rows, a 294 MB database and a 355 MB
  export), and one invented component per module (4,286 of them). That made `undecided` mean both
  "assess first" and "nobody looked".
- **The workflow rules existed on paper only.**
  - The lease token was optional, so anyone could post a "verified" finding to another agent's task and
    complete it.
  - A review could bring a superseded claim back.
  - A re-recorded test was linked to the wrong artifact.
  - Claims and binaries used two different firmware names and never joined.

All of these are fixed, each with its regression test. The database was migrated in place: the 59 claims
and the task history the agent recorded only in the database are kept. It is now 17 MB, the export 21 MB.

**Finished from the agent's "not done" list:**
- the ECom plugin index (`tools/ecom_index.py`): ECom plugins become edges in the registry;
- the MCP server (`tools/registry_mcp.py`, registered as `registry` in `.mcp.json`), so agents use the
  registry as tools, with the same rules.

Task `ecom-namespace-map` was closed through the registry's own workflow (claim 70). Its acceptance text
had a wrong UID: 0x20031CB6 is clonerenderstage's DLL UID, not an implementation UID.

## 2. The 808 data

- **Binaries.** The list was 52 modules short of the ROM:
  - XIP images without imports, `ekern.exe` itself among them;
  - ROFS images with extensions that were not on a fixed list (`.drv`, `.pxy`, `.pxt`, `.agt`, `Elocl.NN`).

  Now there are 4701, checked against `core.json` and every E32 header in ROFS1-3. The 5 XIP modules that
  ROFS2 ships again are marked as shadowed, and imports go to the copy the loader actually uses.
- **Import offsets.** XIP thunk offsets were 4 bytes early (ROFS offsets were right). Fixed and tested on
  real thunks.
- **Components.** Only 364 of the 4701 binaries have a curated role, across 19 components. The other 4337
  are honestly "unassigned" rather than 4,286 fake components.
- **Seeds.**
  - The BC02 transport contract still had the readings that the research document itself had corrected:
    the ACL length's byte order, event 0x10 (it is Hardware Error), DoCancel taking a mask, and the config
    offsets. Re-synced.
  - The gce-factory contract presented an N8 module as an 808 user. Fixed.
  - "KErrCancelled" does not exist; it is KErrCancel. Fixed.
- **Spot checks.** About 15 of the "verified" byte-level claims were checked against the ROM and the
  decompilations, and all held: the gcerenderstage ECom table, End(), the refcount, the import order, the
  HCTL strings, TVersion, unit 4, the TCommConfigV01 writes, and the BC02.LDD vtable. The ECom index
  independently confirms the render-stage UID 0x20031CBA.

## 3. Did the research give us knowledge?

| piece | new? | value |
|---|---|---|
| **HCTL/BC02** (`hctl-bc02-808.md`) | all new | **The useful one.** BC02.LDD is the standard serial LDD (DComm/ECOMM, d32comm.h numbering). The Bluetooth stack opens unit 4 and speaks H4. The controller must be configured with EBpsSpecial plus a special rate (120k, 921.6k or 4M), raise CTS within 1 s, and answer a type-6 baud packet within 4 s. A stand-in PDD under the name "BC02.x" behind a QEMU UART with a small HCI controller is therefore enough. That is the path to removing H-fmradio-none (3 byte patches) and H-bt-ondemand on the 808, N8 and 701. |
| **gcerenderstage** (`gcerenderstage-808.md`) | partly (the N8 composition notes had the outline) | It confirms the boundary: gcerenderstage is the only importer of gce ordinal 1, and End() completes at once when nothing changed. So gcebe *is* the whole compositor surface. It removes no patch, but it is the contract a port must implement. |
| **Surface-update contract** (registry claims 11-14) | new | It confirms two assumptions gcebe was built on (Extension_ 0x10286498, the hand-off in slot 2). That makes gcebe safer; it changes nothing at run time. |
| ABServer notes (`fuzz/`) | new | They concern the N95's security server and capability probing, unrelated to either goal. |

**Cost.** About half of the effort went into annotation beyond "just enough to mock": naming 200
functions, the LDD's internal completion pump (HCTL §11-§16), and Ghidra work on haptics with no write-up.
That is against the project's rule for research agents. The documents also kept contradicting sections
(fixed in place now; see the review note at the top of the HCTL document).

**Verdict.** One real piece of new knowledge (the Bluetooth transport contract), one confirmation (the
compositor boundary), and a registry worth keeping now that it is correct.

## 4. "Fewer patches": what is realistic on the 808

What the 808 needs today:
- 15 kernel-side stand-ins;
- 34 firmware kernel drivers left out (`missing_drivers`);
- 3 byte patches (FM radio);
- the gcebe compositor in place of VideoCore's backend;
- 3 fake-modem reply lines and one `ESTARTCOMP.TXT` override;
- the sym102 personality.

The user side is already almost as-is: those 3 FM byte patches are the only user-side byte patches.

So "run as is" is a kernel-side question. Nokia's own LDDs can run on our kernel when their PDD talks to
hardware that QEMU emulates. Nokia's PDDs cannot run, because they drive OMAP3630 and BCM2763 (VideoCore)
registers. The pattern that removes patches is therefore:

**keep the phone's LDD, write a PDD for a QEMU device, and model the device in QEMU.**

Candidates on the 808, cheapest first:

1. **Bluetooth/FM: BC02.** A PDD derived from the EPL Syborg serial PDD, plus a QEMU UART with a minimal
   HCI controller: reset, the baud handshake, read BD_ADDR, and enough to make the stack say "on".
   - Removes: H-fmradio-none (3 patches on 3 phones), H-bt-ondemand, and 2 entries from
     `missing_drivers`.
   - The same controller model later serves other phones with H4 transports.
   - Registry objective: `bt-hci-transport`.
   - First check: whether the original BC02.LDD binds to our kernel (HCTL document §9F stage 1).
2. **Vibra.** The 808 drives the motor over ISI (1C 9E), not through a driver, so a `syborg-vibra` device
   behind the fake modem's handler is small. Tasks `haptics-isi-packet` → `vibra-qemu-device`.
3. **Sensors** (accelerometer, then proximity and magnetometer): the phone's LDD over a PDD for a QEMU
   sensor device, replacing today's stand-in pair. Task `sensor-ldd-replacement-pdd`.
4. **VideoCore stays replaced** (gcebe and our GL path). Its strategy is `defer`, for a reason: emulating
   BCM2763 is out of proportion.

## 5. Next work, and its value

| next | why | value |
|---|---|---|
| BC02 PDD + QEMU HCI controller (`bt-hci-transport`) | the research already gives the contract | 4+ hacks gone on the 808/N8/701; the original Bluetooth stack starts; the first "original LDD over a QEMU device" proof, which is the template for the rest |
| IPC edges in the registry (server names ↔ client `CreateSession` literals) | stand-in work keeps asking "who talks to which server" | answers that question in seconds, and shows which servers a port has to provide |
| Ingest the N8 and 701 (both ECom indexes are built) | they share Belle with the 808 | one claim scoped to three firmwares instead of three investigations |
| `sensor-ldd-replacement-pdd`, `vibra-qemu-device` | small and bounded | fewer stand-ins; real devices you can drive from sydb |
| **Android port (goal b)**: write down the seams as contracts | hosted EKA2 already boots the 808 in an aarch64 VM | a port is the hosted kernel plus a backend per seam: the compositor (gcebe → Android EGL/SurfaceView), the modem (ISI model → offline or RIL), audio (RAPIDO → AAudio), sensors (PDD → the Android sensor API), input. The registry is where these contracts live; the compositor and surface-update contracts are already the most complete. Bluetooth is not realistic there without root. |

Housekeeping found on the way:
- `models/nokia-808-rm807/alfappservercore-nolowmem.dll` is a firmware binary that was one `git add` away
  from a commit. `.gitignore` now excludes DLLs and EXEs under `models/`.
- Left untracked and not reviewed: `fuzz/` (Ghidra projects and the capask probe), `.pi/` (the agent
  harness's task files), `tmp/`, `config-temp/`.
