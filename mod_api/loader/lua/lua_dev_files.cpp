#include "lua_dev_files.h"

#include "lua_watcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <utility>

namespace wotbmod {
namespace lua {
namespace {

// A script bigger than this is not a script anyone is hot-reloading; it is a
// file that landed in the folder by mistake, or a device pretending to be one.
// Refusing it is cheaper than compiling it.
constexpr uint64_t kMaxScriptBytes = 4u * 1024u * 1024u;

// An address inside this DLL, and nothing else. GetModuleHandleExW's
// FROM_ADDRESS form needs one to find the module it belongs to, and the
// obvious candidates are all wrong for one reason or another: a data address
// can be folded into a shared section, and the address of a function that has
// overloads will not even convert. A function that exists purely to be pointed
// at cannot acquire either problem later.
void ModuleAnchor() noexcept {}

// An environment variable of whatever length it happens to be, rather than one
// that happens to fit.
//
// GetEnvironmentVariableW does not fail when the buffer is too small; it
// returns the size it needs, terminator included, and writes nothing. The first
// version of this code asked for MAX_PATH and treated "needs more" exactly as
// it treated "not set" - so an author whose dev folder sat past 260 characters
// silently got the fallback instead of the folder they had named.
//
// Two calls, not a retry loop: the second cannot need more than the first said
// unless the variable changed in between, and a loop would not settle that race
// either. False means unset, empty, or unreadable: all mean "try next source".
bool ReadEnvironmentPath(const wchar_t* name, std::wstring* out) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0u);
    if (needed == 0u) return false;
    std::wstring value(static_cast<size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, &value[0], needed);
    if (written == 0u || written >= needed) return false;
    value.resize(static_cast<size_t>(written));
    if (value.empty()) return false;
    *out = std::move(value);
    return true;
}

// This module's own file, at whatever length. A game installed under a long
// path is not a reason for hot reload to quietly not exist.
bool ThisModulePath(std::wstring* out) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleAnchor), &self)) {
        return false;
    }
    for (size_t capacity = MAX_PATH; capacity <= 32768u; capacity *= 2u) {
        std::wstring path(capacity, L'\0');
        const DWORD written =
            GetModuleFileNameW(self, &path[0], static_cast<DWORD>(capacity));
        if (written == 0u) return false;
        if (written < capacity) {
            path.resize(static_cast<size_t>(written));
            *out = std::move(path);
            return true;
        }
    }
    return false;
}

}  // namespace

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string narrow(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), &narrow[0], needed,
                        nullptr, nullptr);
    return narrow;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
                            static_cast<int>(text.size()), &wide[0], needed) !=
        needed) {
        return std::wstring();
    }
    return wide;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The chunk name a script's errors are reported against: the file name without
// its extension, so hot.lua raises hot:3 rather than naming a path the author
// has to read past.
std::string ScriptIdOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0u) name = name.substr(0, dot);
    return Narrow(name);
}

bool HasLuaExtension(const std::wstring& path) {
    if (path.size() < 4u) return false;
    const std::wstring tail = path.substr(path.size() - 4u);
    return FoldPathKey(tail) == L".lua";
}

// Every share flag on the read, because the editor that just wrote this file
// may still have it open, and a reload that fights the editor for the file is a
// reload that fails for a reason that has nothing to do with the code.
bool ReadWholeFile(const std::wstring& path, std::string* out) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxScriptBytes) {
        CloseHandle(file);
        return false;
    }
    std::string data;
    try {
        data.resize(static_cast<size_t>(size.QuadPart));
    } catch (...) {
        CloseHandle(file);
        return false;
    }
    DWORD read = 0u;
    BOOL ok = TRUE;
    if (!data.empty()) {
        ok = ReadFile(file, &data[0], static_cast<DWORD>(data.size()), &read,
                      nullptr);
    }
    CloseHandle(file);
    if (!ok || read != data.size()) return false;
    // Lua's lexer does not recognise a UTF-8 BOM, while Windows editors can
    // add one routinely. Strip exactly that prefix before compiling.
    if (data.size() >= 3u &&
        static_cast<unsigned char>(data[0]) == 0xEFu &&
        static_cast<unsigned char>(data[1]) == 0xBBu &&
        static_cast<unsigned char>(data[2]) == 0xBFu) {
        data.erase(0u, 3u);
    }
    *out = std::move(data);
    return true;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

std::wstring ResolveDevFolder(const std::wstring& override_folder) {
    if (!override_folder.empty()) return override_folder;

    std::wstring from_environment;
    if (ReadEnvironmentPath(L"WOTBMOD_LUA_DEV_DIR", &from_environment)) {
        return from_environment;
    }

    std::wstring path;
    if (!ThisModulePath(&path)) return std::wstring();
    for (int level = 0; level < 2; ++level) {  // drop file, then mods/<id>
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return std::wstring();
        path.resize(slash);
    }
    return path + L"\\lua-dev";
}

std::wstring ResolveLuaModsFolder(const std::wstring& override_folder,
                                  const std::wstring& game_directory) {
    if (!override_folder.empty()) return override_folder;

    std::wstring from_environment;
    if (ReadEnvironmentPath(L"WOTBMOD_LUA_MOD_DIR", &from_environment)) {
        return from_environment;
    }

    if (!game_directory.empty()) {
        return game_directory + L"\\mods\\lua";
    }

    std::wstring path;
    if (!ThisModulePath(&path)) return std::wstring();
    for (int level = 0; level < 2; ++level) {  // drop file, then mods/<id>
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return std::wstring();
        path.resize(slash);
    }
    return path + L"\\lua";
}

}  // namespace lua
}  // namespace wotbmod
