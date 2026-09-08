#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

extern "C" __declspec(dllexport) uint32_t WotbNotAMod(void) {
    return 0x4E4F5045u;
}
