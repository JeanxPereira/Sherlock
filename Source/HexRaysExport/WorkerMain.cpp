// Sherlock — tools/Sherlock/Source/HexRaysExport/WorkerMain.cpp
// The layer-2 worker process: one image per run, and the only binary that links IDA (derived).

#include <pro.h>
#include <idalib.hpp>
#include <loader.hpp>
#include <hexrays.hpp>

#include <cstring>

#include <windows.h>

// pro.h defines fflush and stdout away; qprintf is the SDK's own line out.
int main(int argc, char** argv)
{
    // A fault or an abort has no dialog to show under ctest, and the default report mode opens
    // one anyway -- the run then waits on a window nobody sees, holding the .exe against the
    // next link.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    if (init_library() != 0)
    {
        qprintf("SherlockHexRays: init_library failed\n");
        return 2;
    }

    int major = 0, minor = 0, build = 0;
    get_library_version(major, minor, build);

    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0)
    {
        // The decompiler is reachable only once a database is open: the plugin is chosen by the
        // processor module, and with no database there is no processor, so load_plugin("hexarm")
        // returns null and the handshake answers 0. That 0 is the instrument not looking, not the
        // decompiler being absent -- this line says so instead of reporting an absence. The
        // pairing is checked statically by cmake/SherlockIdaSdk.cmake, and the handshake itself
        // is proven where a database exists.
        qprintf("SherlockHexRays sdk %d runtime %d.%d.%d api %d decompiler unknown-without-database\n",
                IDA_SDK_VERSION, major, minor, build,
                (int)(HEXRAYS_API_MAGIC & 0xFFFFFFFF));
        return 0;
    }

    qprintf("SherlockHexRays: usage: SherlockHexRays --version\n");
    return 2;
}
