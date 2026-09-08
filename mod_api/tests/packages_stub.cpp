// Stand-in for wotbmod.exe in the wotb.packages host tests.
//
// Prints its arguments, one per line, to stdout, and the two launcher
// variables the bridge is required to set; exits 3 (with a stderr line) when
// any argument is the word "fail", so a test can see the exit code and the
// stderr channel travel back separately.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    bool fail = false;
    for (int index = 1; index < argc; ++index) {
        std::printf("arg:%s\n", argv[index]);
        if (std::strcmp(argv[index], "fail") == 0) fail = true;
    }
    char yes[64] = {};
    char pause[64] = {};
    GetEnvironmentVariableA("WOTBMOD_LAUNCHER_YES", yes, sizeof(yes));
    GetEnvironmentVariableA("WOTBMOD_LAUNCHER_NO_PAUSE", pause, sizeof(pause));
    std::printf("env:WOTBMOD_LAUNCHER_YES=%s\n", yes);
    std::printf("env:WOTBMOD_LAUNCHER_NO_PAUSE=%s\n", pause);
    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    std::printf("cwd:%s\n", cwd);
    std::fflush(stdout);
    if (fail) {
        std::fprintf(stderr, "wotbmod: error: the stub was told to fail\n");
        return 3;
    }
    return 0;
}
