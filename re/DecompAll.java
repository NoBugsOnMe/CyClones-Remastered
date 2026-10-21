// Decompile every function in the program to out/decomp.c and list strings with xrefs to out/strings.txt
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.io.*;

public class DecompAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp";
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/decomp.c"));
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            pw.println("// ===== " + f.getName() + " @ " + f.getEntryPoint());
            if (r != null && r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
            else pw.println("// decompile failed");
            n++;
        }
        pw.close();
        PrintWriter ps = new PrintWriter(new FileWriter(outDir + "/strings.txt"));
        DataIterator dit = currentProgram.getListing().getDefinedData(true);
        while (dit.hasNext()) {
            Data dd = dit.next();
            if (dd.hasStringValue()) {
                StringBuilder refs = new StringBuilder();
                for (Reference ref : getReferencesTo(dd.getAddress())) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    refs.append(" ").append(ref.getFromAddress()).append(f != null ? "(" + f.getName() + ")" : "");
                }
                ps.println(dd.getAddress() + "\t" + dd.getValue() + "\t" + refs);
            }
        }
        ps.close();
        println("decompiled " + n + " functions");
    }
}
