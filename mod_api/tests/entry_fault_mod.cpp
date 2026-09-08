#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../include/wotb_mod_api.h"

WOTBMOD_ENTRY {
    (void)host;
    (void)mod;
    (void)out_info;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return WOTBMOD_ERROR_CALLBACK_FAULT;
}
