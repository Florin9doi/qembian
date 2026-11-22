// Headless Ghidra pre-script for raw EKA1/EKA2 module bytes (qembian, tools/ghidra/rawdec.sh):
// argument "thumb": set TMode over the whole image and create a function at every Thumb push {.., lr}
// (0xB5xx) before analysis (Ghidra finds no Thumb functions in a raw ARM image by itself);
// argument "arm": create a function at every ARM stmfd sp!, {.., lr} (0xE92D4xxx).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.mem.*;
import java.math.BigInteger;

public class RawSeed extends GhidraScript {
	@Override
	public void run() throws Exception {
		String mode = getScriptArgs().length > 0 ? getScriptArgs()[0] : "arm";
		MemoryBlock b = currentProgram.getMemory().getBlocks()[0];
		Address s = b.getStart(), e = b.getEnd();
		int n = 0;
		for (int i = 1; i < getScriptArgs().length; i++) {      // further arguments: entry points (hex)
			Address a = toAddr(Long.parseLong(getScriptArgs()[i].replace("0x", ""), 16));
			disassemble(a);
			if (getFunctionAt(a) == null && createFunction(a, null) != null) n++;
		}
		if (mode.equals("thumb")) {
			Register t = currentProgram.getRegister("TMode");
			currentProgram.getProgramContext().setValue(t, s, e, BigInteger.ONE);
			for (Address a = s; a.compareTo(e) < 0; a = a.add(2)) {
				int h = getShort(a) & 0xffff;
				if ((h & 0xff00) == 0xb500 && getFunctionAt(a) == null) {
					disassemble(a);
					if (createFunction(a, null) != null) n++;
				}
			}
		} else {
			for (Address a = s; a.add(4).compareTo(e) < 0; a = a.add(4)) {
				int w = getInt(a);
				if ((w & 0xffffc000) == 0xe92d4000 && getFunctionAt(a) == null) {
					disassemble(a);
					if (createFunction(a, null) != null) n++;
				}
			}
		}
		println("RawSeed " + mode + ": " + n + " functions");
	}
}
