// hitchsym -- resolve the "[HITCH]" lines the frame profiler writes to FFDebug.log.
//
// The game samples the sim thread's call stack when a VR frame runs 40 ms or more and logs raw frames as
// "module+0xoffset" (it cannot afford symbol lookup on a frozen frame, and must not take locks while the thread
// is suspended). This turns them back into function names and source lines using the exe's PDB.
//
//   build:  cl /nologo /O2 /EHsc /Fe:hitchsym.exe hitchsym.cpp dbghelp.lib
//   run  :  hitchsym.exe <FFDebug.log> <FFViper.exe> <dir with RedViper.pdb> [maxFramesPerHitch]
//
// The PDB must be the one built WITH that exe (the exe's symbols are RedViper.pdb, not FFViper.pdb). The tool
// loads with SYMOPT_EXACT_SYMBOLS, so a stale PDB resolves nothing instead of resolving the wrong names, and
// says so. After the per-hitch listing it prints a tally of the first game frame of every hitch: the function
// the sim thread was sitting in.
//
// Frames in other modules (d3d12.dll, the GPU driver, ntdll, kernel32, ...) are printed as module+offset: they
// say WHAT it was waiting on (a fence, a lock, a file read), the exe frames below them say WHO asked.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

static const DWORD64 kBase = 0x140000000ULL;

static std::string Resolve(HANDLE proc, DWORD64 addr)
{
    char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
    ZeroMemory(sym, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 500;
    DWORD64 disp = 0;
    char out[700];

    if (!SymFromAddr(proc, addr, &disp, sym))
        return "";

    IMAGEHLP_LINE64 ln;
    ZeroMemory(&ln, sizeof(ln));
    ln.SizeOfStruct = sizeof(ln);
    DWORD ld = 0;

    if (SymGetLineFromAddr64(proc, addr, &ld, &ln))
    {
        const char* f = ln.FileName;

        for (const char* p = ln.FileName; *p; ++p)
            if (*p == '\\' || *p == '/')
                f = p + 1;

        _snprintf(out, sizeof(out) - 1, "%s  (%s:%lu)", sym->Name, f, ln.LineNumber);
    }
    else
        _snprintf(out, sizeof(out) - 1, "%s+0x%llx", sym->Name, (unsigned long long)disp);

    out[sizeof(out) - 1] = 0;
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: hitchsym <FFDebug.log> <FFViper.exe> <dir with RedViper.pdb> [maxFrames]\n");
        return 2;
    }

    const char* logPath = argv[1];
    const char* exePath = argv[2];
    const char* pdbDir = argv[3];
    const int maxFrames = argc > 4 ? atoi(argv[4]) : 14;
    HANDLE proc = GetCurrentProcess();

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_EXACT_SYMBOLS);

    if (!SymInitialize(proc, pdbDir, FALSE))
    {
        fprintf(stderr, "SymInitialize failed (%lu)\n", GetLastError());
        return 1;
    }

    const DWORD64 mod = SymLoadModuleEx(proc, NULL, exePath, NULL, kBase, 0, NULL, 0);
    IMAGEHLP_MODULE64 mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.SizeOfStruct = sizeof(mi);
    const bool haveInfo = mod && SymGetModuleInfo64(proc, mod, &mi);

    if (!mod || !haveInfo || mi.SymType != SymPdb)
        fprintf(stderr,
                "WARNING: no matching PDB loaded for %s (SymType=%d). Use the RedViper.pdb built with THIS exe;\n"
                "         exe frames will print as raw offsets.\n",
                exePath, haveInfo ? (int)mi.SymType : -1);
    else
        printf("symbols: %s\n", mi.LoadedPdbName);

    FILE* f = fopen(logPath, "rb");

    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", logPath);
        return 1;
    }

    std::map<std::string, int> tally;
    int hitches = 0;
    static char line[16384];
    const char* exeName = strrchr(exePath, '\\');
    exeName = exeName ? exeName + 1 : exePath;

    while (fgets(line, sizeof(line), f))
    {
        const char* h = strstr(line, "[HITCH]");

        if (!h)
            continue;

        ++hitches;
        const char* colon = strstr(h, "ms:");
        printf("\n%.*s\n", (int)(colon ? (colon + 3 - h) : 60), h);

        if (!colon)
            continue;

        const char* p = colon + 3;
        int shown = 0;
        bool firstGame = true;

        while (*p && shown < maxFrames)
        {
            while (*p == ' ')
                ++p;

            const char* plus = strstr(p, "+0x");
            if (!plus)
                break;

            std::string name(p, plus);
            const unsigned long long off = _strtoui64(plus + 3, NULL, 16);
            while (*p && *p != ' ' && *p != '\r' && *p != '\n')
                ++p;

            std::string text;
            const bool game = _stricmp(name.c_str(), exeName) == 0;

            if (game)
                text = Resolve(proc, kBase + off);

            if (text.empty())
            {
                char tmp[300];
                _snprintf(tmp, sizeof(tmp) - 1, "%s+0x%llx", name.c_str(), off);
                tmp[sizeof(tmp) - 1] = 0;
                text = tmp;
            }

            printf("   %2d  %s\n", shown, text.c_str());

            if (game && firstGame)
            {
                ++tally[text];
                firstGame = false;
            }

            ++shown;
        }
    }

    fclose(f);
    printf("\n==== %d hitch samples. First game frame of each (where the sim thread was): ====\n", hitches);
    std::vector<std::pair<int, std::string> > v;

    for (std::map<std::string, int>::const_iterator it = tally.begin(); it != tally.end(); ++it)
        v.push_back(std::make_pair(it->second, it->first));

    // largest first
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j)
            if (v[j].first > v[i].first)
                std::swap(v[i], v[j]);

    for (size_t i = 0; i < v.size() && i < 25; ++i)
        printf("%5d  %s\n", v[i].first, v[i].second.c_str());

    SymCleanup(proc);
    return 0;
}
