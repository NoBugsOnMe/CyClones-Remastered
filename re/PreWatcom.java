// Pre-analysis setup for Watcom DOS/4GW binaries: __CHK (stack probe) must not be treated as non-returning.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class PreWatcom extends GhidraScript {
    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
        Address a = toAddr(0x5355b);
        disassemble(a);
        Function f = getFunctionAt(a);
        if (f == null) f = createFunction(a, "__CHK");
        if (f != null) { f.setName("__CHK", ghidra.program.model.symbol.SourceType.USER_DEFINED); f.setNoReturn(false); }
        println("PreWatcom applied");
    }
}
