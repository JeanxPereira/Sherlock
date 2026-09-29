// Sherlock — Source/HexRaysExport/WorkerMain.cpp
// The layer-2 worker process: one image per run, and the only binary that links IDA (derived).

#include <HexRaysExport/Worker.h>

#include <pro.h>
#include <idalib.hpp>
#include <loader.hpp>
#include <hexrays.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <windows.h>

namespace
{
    // pro.h defines fflush and stdout away; qprintf is the SDK's own line out.
    void Usage()
    {
        qprintf("SherlockHexRays --version\n"
                "SherlockHexRays --image <file> --store <dir> --name <image> --image-path <cache path>\n"
                "                --build <build> [--keep-database] [--resume] [--batch <functions>]\n");
    }

    bool Flag(const char* argument, const char* name)
    {
        return std::strcmp(argument, name) == 0;
    }
}

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

    if (argc >= 2 && Flag(argv[1], "--version"))
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

    Sherlock::HexRaysExport::WorkerOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const bool hasValue = i + 1 < argc;
        if (Flag(argv[i], "--image") && hasValue)
        {
            options.Image = argv[++i];
        }
        else if (Flag(argv[i], "--store") && hasValue)
        {
            options.StoreDir = argv[++i];
        }
        else if (Flag(argv[i], "--name") && hasValue)
        {
            options.ImageName = argv[++i];
        }
        else if (Flag(argv[i], "--image-path") && hasValue)
        {
            options.ImagePath = argv[++i];
        }
        else if (Flag(argv[i], "--build") && hasValue)
        {
            options.Build = argv[++i];
        }
        else if (Flag(argv[i], "--keep-database"))
        {
            options.KeepDatabase = true;
        }
        else if (Flag(argv[i], "--resume"))
        {
            options.Resume = true;
        }
        else if (Flag(argv[i], "--batch") && hasValue)
        {
            const long long value = std::atoll(argv[++i]);
            if (value <= 0)
            {
                qprintf("SherlockHexRays: --batch takes a count above zero\n");
                return 2;
            }
            options.BatchSize = static_cast<std::uint64_t>(value);
        }
        else
        {
            qprintf("SherlockHexRays: unknown argument '%s'\n", argv[i]);
            Usage();
            return 2;
        }
    }

    if (options.Image.empty() || options.StoreDir.empty() || options.ImageName.empty()
        || options.Build.empty())
    {
        qprintf("SherlockHexRays: --image, --store, --name and --build are all required\n");
        Usage();
        return 2;
    }
    if (options.ImagePath.empty())
    {
        options.ImagePath = options.Image.string();
    }

    auto report = Sherlock::HexRaysExport::RunWorker(options);
    if (!report)
    {
        const std::string text = report.error().Format();
        qprintf("SherlockHexRays: %s\n", text.c_str());
        // Exit 2 is NOT VERIFIED -- the instrument could not look. Exit 1 is a real failure of
        // the image itself, and the parent records the two differently.
        return report.error().Level == Sherlock::Foundation::Severity::NotVerified ? 2 : 1;
    }

    const std::string line = report->Format();
    qprintf("SherlockHexRays %s %s\n", options.ImageName.c_str(), line.c_str());
    return 0;
}
