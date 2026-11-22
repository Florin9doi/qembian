// Headless Ghidra post-script: write the disassembly listing (address, bytes, instruction) of the whole
// program to the file given as the first argument (qembian; tools/ghidra/rawdec.sh LISTING=1).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportListing extends GhidraScript {
	@Override
	public void run() throws Exception {
		String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "listing.txt";
		try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
			InstructionIterator it = currentProgram.getListing().getInstructions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Instruction i = it.next();
				Function f = getFunctionContaining(i.getAddress());
				w.printf("%s  %-8s %s%s%n", i.getAddress(), toHex(i.getBytes()), i.toString(),
					f != null && f.getEntryPoint().equals(i.getAddress()) ? "   ; <" + f.getName() + ">" : "");
			}
		}
	}

	private static String toHex(byte[] b) {
		StringBuilder s = new StringBuilder();
		for (int i = b.length - 1; i >= 0; i--) s.append(String.format("%02x", b[i]));
		return s.toString();
	}
}
