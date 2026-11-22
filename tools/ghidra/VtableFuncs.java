// Headless Ghidra pre-script: create (Thumb) functions at every code pointer of the given vtables, so the
// virtual methods auto-analysis never reaches (nothing calls them directly) get decompiled too.
// Args: vtable addresses, hex. A slot ends the table when it does not point into program memory.
//   (qembian; tools/ghidra/decompile.sh runs it when VTABLES="0x... 0x..." is set.)
// 2026-11-19: hwrmhapticsvibepacketizerplugin.dll had 38 functions found, 78 with its 2 vtables walked.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.math.BigInteger;

public class VtableFuncs extends GhidraScript {
	@Override
	public void run() throws Exception {
		Memory mem = currentProgram.getMemory();
		Listing lst = currentProgram.getListing();
		for (String a : getScriptArgs()) {
			Address vt = toAddr(Long.decode(a));
			int made = 0;
			for (int i = 0; i < 256; i++) {
				Address slot = vt.add(i * 4);
				if (!mem.contains(slot)) break;
				long p = mem.getInt(slot) & 0xffffffffL;
				Address f = toAddr(p & ~1L);
				if (p == 0 || !mem.contains(f)) break;
				if (lst.getFunctionAt(f) != null) continue;
				try {
					if ((p & 1) == 1) {
						currentProgram.getProgramContext().setValue(currentProgram.getRegister("TMode"), f, f, BigInteger.ONE);
					}
					new DisassembleCommand(f, null, true).applyTo(currentProgram, monitor);
					if (createFunction(f, null) != null) made++;
					else println("VtableFuncs: no function at " + f + " (slot " + slot + ")");
				} catch (Exception e) {
					println("VtableFuncs: " + f + ": " + e);
				}
			}
			println("VtableFuncs: vtable " + vt + ": " + made + " functions created");
		}
	}
}
