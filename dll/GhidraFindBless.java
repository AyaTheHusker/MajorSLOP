// Ghidra headless script: find functions that reference bless spell offsets
// and decompile the main bless handler
// @category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.listing.Data;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

public class GhidraFindBless extends GhidraScript {

    @Override
    public void run() throws Exception {
        String outpath = "/home/bucka/AI/mudproxy/dll/ghidra_bless_output.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(outpath));

        pw.println("================================================================================");
        pw.println("BLESS FUNCTION FINDER");
        pw.println("================================================================================");
        pw.println();

        // Search all functions for references to bless-related struct offsets
        // BlessCmd1-10: 0x4F1F, 0x4F34, 0x4F49, 0x4F5E, 0x4F73, 0x4F88, 0x4F9D, 0x4FB2, 0x4FC7, 0x4FDC
        // BLESS_COMBAT: 0x3A44, BLESS_RESTING: 0x3A40
        // AUTO_BLESS: 0x4D0C
        // These are byte offsets, word indices would be: 0x13C7, 0x13CD, etc.
        // Also look for activity state 0x0C (Blessing)

        // Strategy: scan all function bodies for the instruction pattern that
        // accesses struct+0x4F1F (first bless command). Since param_1 is a
        // pointer and accesses are param_1[0x13C7] (word index), look for
        // 0x13C7 as an immediate value in instructions, or the byte offset 0x4F1C/0x4F1F.

        // Actually, let's just decompile all functions in the 0x406000-0x40C000 range
        // (core automation) that we haven't seen yet, plus find any function containing
        // the string "Blessing" or referencing 0x4F1F.

        // First, let's find functions that contain known bless-related strings
        pw.println("--- Searching for functions referencing bless strings ---");
        pw.println();

        // Search for string references to "Blessing", "auto-bless", "bless"
        // by scanning all defined strings
        Set<Function> blessFuncs = new LinkedHashSet<>();

        // Search for functions that access offsets in the 0x4F1F-0x4FDC range
        // by looking at references to those struct offsets
        // This is hard in headless mode, so let's take the catalog approach:
        // decompile functions in the combat area that are large and undocumented

        // Key candidates from the catalog:
        // 0x00406B40 - 1794 bytes, called from step advancement area, 43 callees
        // 0x0040AA30 - 1599 bytes, 2 callers, 12 callees
        // 0x0040B2D0 - 1563 bytes, 1 caller, 9 callees (right before combat handler!)
        // 0x00408320 - 4083 bytes, 1 caller, 33 callees (BIG)
        // 0x00402740 - 746 bytes, called from 0x0040bad0 (combat handler!)

        long[] addrs = {
            0x00406B40L,  // 1794 bytes, 43 callees - likely main automation dispatcher
            0x0040AA30L,  // 1599 bytes - near combat handler
            0x0040B2D0L,  // 1563 bytes - right before combat_handler (0x0040bad0)
            0x00408320L,  // 4083 bytes - huge, likely important
            0x00402740L,  // 746 bytes - called FROM combat handler
            0x0040FEB0L,  // 2936 bytes - core area, 0 callers (maybe entry point)
            0x00409370L,  // near combat area
        };

        String[] names = {
            "FUN_00406b40_AUTOMATION_DISPATCH",
            "FUN_0040aa30_COMBAT_AREA",
            "FUN_0040b2d0_PRE_COMBAT",
            "FUN_00408320_BIG_HANDLER",
            "FUN_00402740_FROM_COMBAT",
            "FUN_0040feb0_CORE_ENTRY",
            "FUN_00409370_COMBAT_AREA2",
        };

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        for (int i = 0; i < addrs.length; i++) {
            Address addr = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(addrs[i]);
            Function func = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (func == null) {
                func = getFunctionAt(addr);
            }
            if (func == null) {
                pw.printf("=== NO FUNCTION AT 0x%08X (%s) ===\n\n", addrs[i], names[i]);
                continue;
            }

            pw.println("================================================================================");
            pw.printf("=== %s (VA 0x%08X) ===\n", names[i], addrs[i]);
            pw.printf("=== Size: %d bytes ===\n", func.getBody().getNumAddresses());
            pw.println("================================================================================");
            pw.println();

            DecompileResults result = decomp.decompileFunction(func, 180, monitor);
            if (result != null && result.decompileCompleted()) {
                String code = result.getDecompiledFunction().getC();
                pw.println(code);

                // Flag if it contains bless-related offsets
                if (code.contains("0x13c7") || code.contains("0x13cd") ||
                    code.contains("0x4f1f") || code.contains("0x4f34") ||
                    code.contains("Bless") || code.contains("bless") ||
                    code.contains("0xe91") || code.contains("0x3a44") ||
                    code.contains("0x3a40")) {
                    pw.println("*** CONTAINS BLESS-RELATED REFERENCES ***");
                }
            } else {
                pw.println("DECOMPILATION FAILED");
                if (result != null) pw.println("Error: " + result.getErrorMessage());
            }
            pw.println();
        }

        // Also decompile any function in the 0x409000-0x40B000 range we haven't seen
        pw.println("================================================================================");
        pw.println("ADDITIONAL FUNCTIONS IN COMBAT AREA (0x409000-0x40B000)");
        pw.println("================================================================================");
        pw.println();

        FunctionIterator iter = currentProgram.getFunctionManager().getFunctions(true);
        while (iter.hasNext()) {
            Function f = iter.next();
            long a = f.getEntryPoint().getOffset();
            if (a >= 0x00409000L && a < 0x0040B000L) {
                long size = f.getBody().getNumAddresses();
                if (size < 100) continue;

                pw.printf("--- 0x%08X  %s  %d bytes ---\n", a, f.getName(), size);
                DecompileResults result = decomp.decompileFunction(f, 120, monitor);
                if (result != null && result.decompileCompleted()) {
                    String code = result.getDecompiledFunction().getC();
                    pw.println(code);
                    if (code.contains("0x13c7") || code.contains("Bless") ||
                        code.contains("bless") || code.contains("0xe91")) {
                        pw.println("*** CONTAINS BLESS-RELATED REFERENCES ***");
                    }
                }
                pw.println();
            }
        }

        decomp.dispose();
        pw.close();
        println("Output written to " + outpath);
    }
}
