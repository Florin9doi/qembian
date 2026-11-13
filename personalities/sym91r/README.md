# Personality sym91r: Symbian 9.1 with an RVCT-built EUSER

Used by `models/se-p1i` (Sony Ericsson P1i, UIQ 3.0, R6E30). Everything as `sym91` (exec numbering, SWI
return through ip, 9.1 loader layout, export directories from ordinal 1), except two calls:

| | N73 (GCCE EUSER, sym91) | P1i (RVCT EUSER, sym91r) |
|---|---|---|
| `UserSvr::DllTls(handle)` (ord 1232) | `b` straight to the S4D stub: r1 not set | `push; movs r1, #0; bl stub` |
| `UserSvr::DllSetTls(handle, ptr)` (ord 1237) | `b` to the S75 stub: (handle, ptr) | `movs r2, r1; push; movs r1, #0; bl stub` |

[V] both EUser.dll disassembled (`tools/xipdis.py`). With sym91 the `DllSetTls91` shim took r1 = 0 as the
pointer, so DRTAEABI's exception globals stayed NULL and the first leave (a C++ throw in RVCT builds)
faulted in `__cxa_allocate_exception` (data abort at 0x8, 0.4 s into boot; docs/devices/se-p1i/bring-up.md).

- `exec.map`: sym91's with S4d/S75 mapped to our `DllTls`/`DllSetTls`.
- Kernel: ROM header personality 9 (`patches/0026-sym91r-personality.patch`, `kernel/eka/kernel/sexecpers.cpp`):
  sym91's loader layout and SWI return, the `KExec91R*` tables.
