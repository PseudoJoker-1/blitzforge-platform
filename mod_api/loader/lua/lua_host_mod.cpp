#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../../include/wotb_mod_api_v3.h"
#include "lua_bindings.h"
#include "lua_dev_files.h"
#include "lua_manifest_scanner.h"
#include "lua_permissions.h"
#include "lua_script.h"
#include "lua_watcher.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

#include <algorithm>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using wotbmod::lua::LuaScript;
using wotbmod::lua::LuaScriptStage;
using wotbmod::lua::Watcher;
using wotbmod::lua::FileExists;
using wotbmod::lua::FileNameOf;
using wotbmod::lua::HasLuaExtension;
using wotbmod::lua::Narrow;
using wotbmod::lua::ReadWholeFile;
using wotbmod::lua::ResolveDevFolder;
using wotbmod::lua::ResolveLuaModsFolder;
using wotbmod::lua::ScriptIdOf;
using wotbmod::lua::Widen;

// Filled in on load and read by every binding. The bootstrap pointer belongs
// to the runtime and stays valid for the life of the mod.
const WotbModV3Bootstrap* g_bootstrap = nullptr;
WotbModV3Handle g_mod = WOTBMOD_V3_INVALID_HANDLE;

// ---------------------------------------------------------------------------
// The set of live scripts, and the one lock over it
// ---------------------------------------------------------------------------
//
// Task 8 left this host owning no set of live scripts, so on_disable unloaded
// nothing and said so. This is that set. Deleting a LuaScript is a complete
// revocation - ~LuaScript calls OwnershipRegistry::RevokeAll, which takes back
// subscriptions, controls and transactions without asking the script - so a
// walk that deletes each entry cannot forget a resource kind. That is the
// whole reason this file does not reimplement teardown: there is nothing here
// to get wrong that RevokeAll has not already got right.
//
// One lock covers this map, the watcher pointer and the enabled flag, and it
// is a leaf held for as short a time as the operation allows. In particular it
// is never held across:
//
//   * LuaScript::Create or CallGlobal - which run script code, which calls the
//     ABI, which may call back;
//   * delete script - which is RevokeAll, which calls the ABI and *waits* for
//     in-flight event deliveries on other threads. Holding a host lock across
//     that wait is an ABBA deadlock waiting for a delivery to want the lock;
//   * file I/O, which can block for as long as a disk feels like.
//
// So every operation below takes it, moves a pointer out of or into the map,
// and drops it. The pattern to copy: detach under the lock, act outside it.
//
// Three places take a second lock under this one and are named rather than
// left to be discovered:
//
//   * OnFrame                              -> ChangeQueue's lock, via
//                                             Watcher::TakeChanged;
//   * WotbLuaHost_DevWatcherDebounceMsForTests
//                                          -> ChangeQueue's lock, via
//                                             Watcher::Debounce;
//   * WotbLuaHost_LoadedOwnedCountForTests -> OwnershipRegistry's lock, via
//                                             Count.
//
// All three inner locks are leaves that call nothing at all while held - not
// the ABI, not Lua, not back into this file - so none can close a cycle with
// this one, and the order is always this one first. Nothing else may join that
// list without the same argument. (The list said two until a review found the
// third, which had joined silently - so the list is worth keeping only if it is
// checked against the code rather than trusted.)
struct Loaded {
    std::wstring path;         // as first seen, for logs
    std::string id;            // canonical storage/diagnostic identity
    std::shared_ptr<LuaScript> script;
};

std::mutex& HostLock() {
    static std::mutex lock;
    return lock;
}

// Keyed by the folded path - see FoldPathKey. Two spellings of one file
// must be one entry, or an author who typed `Panel.lua` where the folder holds
// `panel.lua` would get two live scripts from one file, which is the leak this
// task exists to prevent wearing a different hat.
std::map<std::wstring, Loaded>& Scripts() {
    static std::map<std::wstring, Loaded> scripts;
    return scripts;
}

Watcher* g_watcher = nullptr;
bool g_enabled = false;
uint32_t g_frames_in_flight = 0u;

std::condition_variable& FramesIdle() {
    static std::condition_variable idle;
    return idle;
}

// Test-only overrides, both under HostLock. Empty/zero means "use the real
// thing", which is what every non-test caller gets.
std::wstring& DevFolderOverride() {
    static std::wstring folder;
    return folder;
}
std::wstring& LuaModsFolderOverride() {
    static std::wstring folder;
    return folder;
}
uint32_t g_debounce_override_ms = 0u;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
//
// A reload that fails has to say so somewhere the author can read, and it
// cannot say it through the failing script's own print(): the script it would
// print from is the one that did not compile. So this goes straight at
// wotbmod.core, with the same OutputDebugStringA fallback print() itself uses
// when core is unavailable.
//
// Category "lua.host" rather than "lua": a message from the host about a
// script is a different thing from a message the script itself printed, and an
// author reading a log wants to tell them apart at a glance.
// Two overloads, and the const char* one is not a convenience. Every caller in
// a noexcept teardown path passes a literal, and `HostLog(LEVEL, "text")`
// against a std::string parameter constructs one - an allocation, on the error
// path, inside a function whose whole job is to not throw. The overload makes
// those call sites allocation-free; the std::string one is for the messages
// that genuinely have a script's name or Lua's own text in them, and those
// callers already sit inside a try block.
void HostLog(uint32_t level, const char* message) noexcept {
    if (!message) return;
    const WotbModV3CoreApiV1* core =
        wotbmod::lua::QueryCoreApi(g_bootstrap, g_mod);
    if (core && core->log) {
        core->log(g_mod, level, "lua.host", message);
        return;
    }
    char line[2048] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[wotbmod.lua.host] %s\n",
                message);
    OutputDebugStringA(line);
}

void HostLog(uint32_t level, const std::string& message) noexcept {
    HostLog(level, message.c_str());
}

// ---------------------------------------------------------------------------
// Paths and files
// ---------------------------------------------------------------------------

std::wstring DevFolder() {
    std::wstring override_folder;
    {
        std::lock_guard<std::mutex> guard(HostLock());
        override_folder = DevFolderOverride();
    }
    return ResolveDevFolder(override_folder);
}

std::wstring LuaModsFolder() {
    std::wstring override_folder;
    {
        std::lock_guard<std::mutex> guard(HostLock());
        override_folder = LuaModsFolderOverride();
    }
    std::wstring game_directory;
    const WotbModV3CoreApiV1* core =
        wotbmod::lua::QueryCoreApi(g_bootstrap, g_mod);
    if (core && core->get_game_directory) {
        uint32_t size = 0u;
        const WotbModV3Result measured =
            core->get_game_directory(g_mod, nullptr, &size);
        if ((measured == WOTBMOD_V3_E_BUFFER_TOO_SMALL ||
             measured == WOTBMOD_V3_OK) &&
            size > 1u && size <= 32768u) {
            std::vector<char> utf8(size, '\0');
            uint32_t written = size;
            if (core->get_game_directory(g_mod, utf8.data(), &written) ==
                    WOTBMOD_V3_OK &&
                written > 0u && written <= size &&
                std::memchr(utf8.data(), '\0', written) != nullptr) {
                game_directory = Widen(std::string(utf8.data()));
            }
        }
    }
    return ResolveLuaModsFolder(override_folder, game_directory);
}

std::string FoldIdKey(const std::string& id) {
    std::string folded = id;
    for (char& c : folded) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z') c = static_cast<char>(byte + ('a' - 'A'));
    }
    return folded;
}

bool HasOtherIdLocked(const std::wstring& path_key, const std::string& id) {
    const std::string wanted = FoldIdKey(id);
    for (const auto& entry : Scripts()) {
        if (entry.first == path_key) continue;
        if (FoldIdKey(entry.second.id) == wanted) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Load, unload, reload
// ---------------------------------------------------------------------------

// Takes a script out of the set and hands it back, or null. The caller
// destroys it with no lock held - see the note on HostLock.
std::shared_ptr<LuaScript> Detach(const std::wstring& key) noexcept {
    std::lock_guard<std::mutex> guard(HostLock());
    std::map<std::wstring, Loaded>& scripts = Scripts();
    const auto found = scripts.find(key);
    if (found == scripts.end()) return {};
    std::shared_ptr<LuaScript> script = std::move(found->second.script);
    scripts.erase(found);
    return script;
}

// Removes key only when it still names the script the caller inspected. A
// concurrent reload may already have installed a replacement under the same
// path; a failing old frame must not detach that new script.
std::shared_ptr<LuaScript> DetachIfSame(
    const std::wstring& key, const std::shared_ptr<LuaScript>& expected) noexcept {
    std::lock_guard<std::mutex> guard(HostLock());
    std::map<std::wstring, Loaded>& scripts = Scripts();
    const auto found = scripts.find(key);
    if (found == scripts.end() || found->second.script != expected) return {};
    std::shared_ptr<LuaScript> script = std::move(found->second.script);
    scripts.erase(found);
    return script;
}

// A script's last word, and then the end of it. The only way a loaded script is
// ever destroyed - reload, disable and unload all come through here.
//
// on_disable is offered because an author who defines it expects it to run:
// this host calls on_enable, and a lifecycle with one half of a pair is a trap.
// A script that wants to write its state to storage before it goes has exactly
// one moment to do it, and this is that moment.
//
// It is offered and not relied on, and the difference is the whole design.
// Everything after this call happens identically whether on_disable ran,
// raised, was never defined, or deliberately hoarded every control it made:
// `delete` is OwnershipRegistry::RevokeAll, which takes it all back without
// asking. So a failure here is logged at warning and then ignored - it is
// information for the author, not a branch in the teardown. Anything
// on_disable creates is created while the ledger is still open (RevokeAll has
// not run yet), so it is recorded and revoked like anything else.
void DestroyScript(std::shared_ptr<LuaScript> script,
                   const std::string& name) noexcept {
    if (!script) return;
    if (script->InstructionLimitExceeded()) {
        try {
            HostLog(WOTBMOD_V3_LOG_WARNING,
                    name + ": disabled after exceeding its instruction "
                           "budget");
        } catch (...) {
        }
        script->Deactivate(false, nullptr);
        return;
    }
    try {
        std::string error;
        if (!script->Deactivate(true, &error)) {
            HostLog(WOTBMOD_V3_LOG_WARNING,
                    name + ": on_disable failed: " + error);
        }
    } catch (...) {
        // Running the courtesy call, or reporting that it failed, ran out of
        // memory. The line below is the part that matters and it allocates
        // nothing, so it still runs. This is the shape of the whole file: the
        // script's cooperation is optional, its destruction is not.
    }
}

// A count hook trips while LuaScript::Entry is holding the script's recursive
// lock. Destruction there would call RevokeAll under that same lock, violating
// its lock-order contract and potentially waiting for a delivery that cannot
// finish. Event delivery therefore does only the lock-free atomic transition;
// the frame pump detaches faulted scripts here and destroys them afterwards,
// with neither HostLock nor the script lock held.
void ReapInstructionLimitedScripts() noexcept {
    std::map<std::wstring, Loaded> faulted;
    {
        std::lock_guard<std::mutex> guard(HostLock());
        std::map<std::wstring, Loaded>& scripts = Scripts();
        for (auto it = scripts.begin(); it != scripts.end();) {
            if (!it->second.script ||
                !it->second.script->InstructionLimitExceeded()) {
                ++it;
                continue;
            }
            const auto doomed = it++;
            faulted.insert(scripts.extract(doomed));
        }
    }
    for (std::pair<const std::wstring, Loaded>& entry : faulted) {
        std::string name;
        try {
            name = Narrow(FileNameOf(entry.second.path));
        } catch (...) {
        }
        DestroyScript(entry.second.script, name);
    }
}

// One file, reloaded. Runs on the thread the client calls on_frame on and
// nowhere else.
//
// The order is the contract, and it is deliberately not "compile the new one,
// and swap only if it worked". The old script goes first, unconditionally,
// because *that* is what takes back its subscriptions, controls and open
// transactions; a host that kept the old one running as a fallback would be
// running code the author has already deleted, still drawing panels they can
// see and still holding a transaction they cannot commit. On a compile error
// the honest state is "nothing loaded, and here is why", which is what an
// author staring at their own syntax error expects.
void ReloadOne(const std::wstring& path) noexcept {
    try {
        const std::wstring key = wotbmod::lua::FoldPathKey(path);
        const std::string name = Narrow(FileNameOf(path));

        // Nothing runs against a ceiling this host could not read. on_enable
        // already refuses in that state and never sets g_enabled, so this is
        // unreachable today - and it is here anyway because this is the only
        // function in the host that creates a LuaScript, which makes it the one
        // place the property can be stated locally instead of inferred from a
        // flag set three functions away. A future caller that reaches here by
        // some other route inherits the refusal rather than having to know
        // about it.
        if (!wotbmod::lua::HostCeilingMeasured()) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    name + ": not loaded - this host has no measured "
                           "permission ceiling to run it under");
            return;
        }

        // 1. The old one, gone. Destroying is a complete revocation, whatever
        //    its on_disable did or did not manage to do.
        std::shared_ptr<LuaScript> previous = Detach(key);
        // Recorded before it is destroyed. After the delete inside
        // DestroyScript the pointer's own *value* is no longer something this
        // code may examine, not merely something it may not dereference - and
        // "it was not null a moment ago" is all the line below wants to know.
        const bool had_one = static_cast<bool>(previous);
        DestroyScript(previous, name);

        // 2. A file that is no longer there unloads its script and stops.
        //    Deleting a script from the dev folder is a legitimate edit.
        if (!HasLuaExtension(path) || !FileExists(path)) {
            if (had_one) HostLog(WOTBMOD_V3_LOG_INFO, name + ": unloaded");
            return;
        }

        std::string source;
        if (!ReadWholeFile(path, &source)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    name + ": could not be read; left unloaded");
            return;
        }

        // 3. Compile and run the body. A failure here logs and stops: the old
        //    script is already gone and the new one never existed, so there is
        //    nothing to take back that LuaScript::Create has not taken back
        //    itself (its own failure path deletes the script, which revokes
        //    whatever the top-level body created before it raised).
        std::string error;
        LuaScriptStage stage = LuaScriptStage::kOk;
        const std::string id = ScriptIdOf(path);
        bool duplicate_id_before_run = false;
        {
            std::lock_guard<std::mutex> guard(HostLock());
            duplicate_id_before_run = HasOtherIdLocked(key, id);
        }
        if (duplicate_id_before_run) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    name + ": not loaded - another Lua mod already uses id " +
                        id);
            return;
        }
        // Derived here, on every reload, rather than carried over from the
        // script that just went: a reload is a fresh script that happens to
        // have the same name, and re-deriving is the only way the fence cannot
        // drift across a save. A dev-folder script is written by the machine's
        // owner, so it gets the ceiling - which is itself re-read from the
        // cache the last enable measured, so a host that holds less than it
        // used to gives its scripts less too, without anything here knowing
        // what changed.
        LuaScript* script = LuaScript::Create(
            id.c_str(), source.c_str(), g_bootstrap, g_mod,
            wotbmod::lua::ScriptPermissions::DevCeiling(), &error, &stage);
        if (!script) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    name + (stage == LuaScriptStage::kRuntimeFailed
                                ? ": error while loading: "
                                : ": compile error: ") +
                        error);
            return;
        }

        // Everything from here to the publish below allocates: two HostLog
        // calls that build their message by concatenation, a map insert, a
        // std::wstring copy into the entry. A std::bad_alloc from any of them
        // reached the catch at the bottom of this function with `script` alive
        // and unreferenced - holding its subscriptions, its controls, its open
        // transaction and its lua_State, with nothing left in the process that
        // could ever destroy it. It is the one leak path the host's seven
        // counters cannot see, because it is neither a live script nor a
        // destroyed one.
        //
        // A guard rather than a catch-and-rethrow, because the *right*
        // destruction changes as this block proceeds and a guard can carry
        // that: before on_enable succeeds a plain delete is owed (see step 4),
        // after it DestroyScript is (see step 5), and once the script is
        // published neither is - it belongs to Scripts() and the map's own
        // entry is what destroys it. All three states are one member each.
        //
        // /EHsc, so this destructor really does run on a bad_alloc unwind; the
        // frames it would not run on are SEH unwinds to an __except, and there
        // is no __try anywhere on this path.
        struct ScriptGuard {
            std::shared_ptr<LuaScript> script;
            const std::string* name = nullptr;
            bool enabled = false;
            ~ScriptGuard() {
                if (!script) return;
                if (enabled) {
                    DestroyScript(std::move(script), *name);
                }
            }
        } unpublished;
        // Build the shared lifetime before on_enable. If allocating its
        // control block fails, std::shared_ptr deletes the still-unenabled raw
        // script and the outer catch reports the allocation failure. Once
        // on_enable succeeds this guard changes the owed teardown to
        // DestroyScript, exactly as the old raw-pointer guard did.
        unpublished.script = std::shared_ptr<LuaScript>(script);
        unpublished.name = &name;

        // 4. on_enable. A script whose on_enable raises is not an enabled
        //    script, so it is destroyed rather than left half-started - and
        //    destroying it takes back whatever its body created before the
        //    failure, which is the case a "log it and carry on" would leak.
        std::string call_error;
        if (!script->CallGlobal("on_enable", &call_error)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    name + ": on_enable failed: " + call_error +
                        "; left unloaded");
            // delete, not DestroyScript: a script whose on_enable raised was
            // never enabled, and calling on_disable on it would be telling it
            // to undo a start that never finished. Revocation is unaffected -
            // that is what delete does, and it does not consult the script.
            //
            // Left to `unpublished` rather than written out, so there is one
            // deletion for this state and not two that have to agree.
            return;
        }
        // From here the script has been enabled, so what it is owed on any
        // failure changes: DestroyScript, which runs on_disable, not delete.
        unpublished.enabled = true;

        // 5. Published, last. Between step 1 and here a disable may have run
        //    on another thread and walked a set this script was not yet in;
        //    inserting it now would leave a live script behind a disable that
        //    believed it had unloaded everything. So the flag is re-checked
        //    under the same lock that the insert takes, and a script that lost
        //    the race is destroyed rather than published.
        //
        //    The displaced entry, if there is one, is taken out and destroyed
        //    rather than overwritten. `Scripts()[key] = ...` on its own would
        //    drop a raw LuaScript* on the floor: nothing would revoke its
        //    subscriptions, its controls or its open transaction, and its
        //    lua_State would never be closed - a live subscription into a state
        //    nobody will ever look at again. Step 1 makes that key normally
        //    absent, so this is the second line of defence rather than the
        //    first; but "normally" rests on the ABI calling on_frame from one
        //    thread, and one lookup is a cheaper way to be sure than an
        //    argument is.
        bool published = false;
        bool duplicate_id = false;
        std::shared_ptr<LuaScript> displaced;
        {
            std::lock_guard<std::mutex> guard(HostLock());
            if (g_enabled) {
                duplicate_id = HasOtherIdLocked(key, id);
                if (!duplicate_id) {
                    Loaded entry;
                    entry.path = path;
                    entry.id = id;
                    entry.script = unpublished.script;
                    std::map<std::wstring, Loaded>& scripts = Scripts();
                    const auto existing = scripts.find(key);
                    if (existing != scripts.end()) {
                        displaced = std::move(existing->second.script);
                        existing->second = std::move(entry);
                    } else {
                        scripts.emplace(key, std::move(entry));
                    }
                    published = true;
                    // Handed over under the same lock that took ownership of
                    // it. The map's entry now destroys this script.
                    unpublished.script.reset();
                }
            }
        }
        // Outside the lock, like every other destruction in this file.
        if (displaced) DestroyScript(std::move(displaced), name);
        if (!published) {
            // DestroyScript here and plain delete above, and the difference is
            // real: this script's on_enable succeeded, so it was enabled, so it
            // is owed the other half of the pair even though it never reached
            // the set. Both spellings live in the guard now; this is only the
            // early return.
            if (duplicate_id) {
                HostLog(WOTBMOD_V3_LOG_ERROR,
                        name + ": not loaded - another Lua mod already uses id " +
                            id);
            }
            return;
        }
        HostLog(WOTBMOD_V3_LOG_INFO, name + ": loaded");
    } catch (...) {
        // A boundary this file owns: on_frame is noexcept, and everything
        // above allocates. std::bad_alloc crossing a client callback is a
        // terminated game; a missed reload is a missed reload.
        HostLog(WOTBMOD_V3_LOG_ERROR, "reload failed: out of memory");
    }
}

// Everything already in the folder when the host is enabled. Without this the
// dev folder would only load a script once the author edited it, which is a
// surprising thing to have to do to start a session.
void LoadFolder(const std::wstring& folder) noexcept {
    try {
        WIN32_FIND_DATAW found = {};
        // Enumerate everything and filter, rather than asking for "*.lua":
        // that pattern also matches a file whose 8.3 short name ends in .lua,
        // which is a class of surprise nobody needs from a loader.
        HANDLE search = FindFirstFileW(
            wotbmod::lua::JoinPath(folder, L"*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> files;
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            // The same join the watcher uses, and that is the point rather than
            // convenience: this path becomes a key in Scripts(), and a
            // notification for the same file arriving later has to produce the
            // identical key or the script is loaded twice and unloaded never.
            const std::wstring path =
                wotbmod::lua::JoinPath(folder, found.cFileName);
            if (HasLuaExtension(path)) files.push_back(path);
        } while (FindNextFileW(search, &found));
        FindClose(search);
        // Collected first, loaded after: FindClose before running script code
        // means no search handle is held open across a chunk that could take
        // any amount of time.
        for (const std::wstring& path : files) ReloadOne(path);
    } catch (...) {
        HostLog(WOTBMOD_V3_LOG_ERROR, "the dev folder could not be scanned");
    }
}

bool IsValidInstalledId(const char* id) noexcept {
    if (!id || !*id) return false;
    size_t length = 0u;
    for (; id[length] != '\0'; ++length) {
        const unsigned char c = static_cast<unsigned char>(id[length]);
        const bool allowed = (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                             c == '-';
        if (!allowed) return false;
    }
    if (length == 0u || length >= wotbmod::lua::LuaManifest::kIdCapacity) {
        return false;
    }
    const auto edge_ok = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    if (!edge_ok(id[0]) || !edge_ok(id[length - 1u])) return false;
    return std::strstr(id, "..") == nullptr;
}

bool IsValidInstalledEntrypoint(const char* entrypoint) noexcept {
    if (!entrypoint || !*entrypoint) return false;
    size_t length = 0u;
    for (; entrypoint[length] != '\0'; ++length) {
        const unsigned char c = static_cast<unsigned char>(entrypoint[length]);
        const bool allowed = (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                             c == '-';
        if (!allowed) return false;
    }
    if (length < 5u ||
        length >= wotbmod::lua::LuaManifest::kEntrypointCapacity) {
        return false;
    }
    const char* tail = entrypoint + length - 4u;
    const bool lua_extension =
        tail[0] == '.' && (tail[1] == 'l' || tail[1] == 'L') &&
        (tail[2] == 'u' || tail[2] == 'U') &&
        (tail[3] == 'a' || tail[3] == 'A');
    return lua_extension && std::strstr(entrypoint, "..") == nullptr;
}

// Loads one installed mod once. Unlike ReloadOne this never replaces an old
// generation: installed files are immutable for an enable cycle, while the dev
// watcher is the explicit edit/reload surface.
void LoadInstalledOne(const std::wstring& directory,
                      const std::wstring& directory_name) noexcept {
    try {
        const std::string folder_label = Narrow(directory_name);
        const std::wstring manifest_path =
            wotbmod::lua::JoinPath(directory, L"manifest.json");
        std::string manifest_json;
        if (!ReadWholeFile(manifest_path, &manifest_json)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    folder_label + ": installed Lua mod has no readable "
                                   "manifest.json");
            return;
        }

        wotbmod::lua::LuaManifest manifest;
        const char* manifest_error = nullptr;
        if (!wotbmod::lua::ScanLuaManifest(
                manifest_json.data(), manifest_json.size(),
                &wotbmod::lua::PermissionBitFor, &manifest, &manifest_error)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    folder_label + ": manifest refused: " +
                        (manifest_error ? manifest_error : "unknown error"));
            return;
        }
        if (!manifest.has_id || !IsValidInstalledId(manifest.id)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    folder_label +
                        ": manifest refused: id is required and may contain "
                        "only letters, digits, '.', '_' and '-', must start "
                        "and end with a letter or digit, and cannot contain "
                        "'..'");
            return;
        }
        if (FoldIdKey(folder_label) != FoldIdKey(manifest.id)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    folder_label + ": manifest refused: directory name must "
                                   "match id " +
                        manifest.id);
            return;
        }

        const char* entrypoint =
            manifest.has_entrypoint ? manifest.entrypoint : "main.lua";
        if (!IsValidInstalledEntrypoint(entrypoint)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    std::string(manifest.id) +
                        ": manifest refused: entrypoint must be one .lua file "
                        "inside the mod directory");
            return;
        }
        const std::wstring script_path =
            wotbmod::lua::JoinPath(directory, Widen(entrypoint));
        std::string source;
        if (!ReadWholeFile(script_path, &source)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    std::string(manifest.id) + ": entrypoint " + entrypoint +
                        " could not be read");
            return;
        }

        const std::wstring key = wotbmod::lua::FoldPathKey(script_path);
        bool duplicate = false;
        {
            std::lock_guard<std::mutex> guard(HostLock());
            if (!g_enabled) return;
            duplicate = Scripts().find(key) != Scripts().end() ||
                        HasOtherIdLocked(key, manifest.id);
        }
        if (duplicate) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    std::string(manifest.id) +
                        ": not loaded - duplicate Lua mod id or path");
            return;
        }

        std::string error;
        LuaScriptStage stage = LuaScriptStage::kOk;
        LuaScript* raw = LuaScript::Create(
            manifest.id, source.c_str(), g_bootstrap, g_mod,
            wotbmod::lua::ScriptPermissions::FromRequestedBits(
                manifest.requested_permissions),
            &error, &stage);
        if (!raw) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    std::string(manifest.id) +
                        (stage == LuaScriptStage::kRuntimeFailed
                             ? ": error while loading: "
                             : ": compile error: ") +
                        error);
            return;
        }

        struct InstalledGuard {
            std::shared_ptr<LuaScript> script;
            std::string name;
            bool enabled = false;
            ~InstalledGuard() {
                if (!script) return;
                if (enabled) DestroyScript(std::move(script), name);
            }
        } unpublished;
        unpublished.script = std::shared_ptr<LuaScript>(raw);
        unpublished.name = manifest.id;

        std::string call_error;
        if (!raw->CallGlobal("on_enable", &call_error)) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    std::string(manifest.id) + ": on_enable failed: " +
                        call_error + "; left unloaded");
            return;
        }
        unpublished.enabled = true;

        bool published = false;
        {
            std::lock_guard<std::mutex> guard(HostLock());
            if (g_enabled && Scripts().find(key) == Scripts().end() &&
                !HasOtherIdLocked(key, manifest.id)) {
                Loaded entry;
                entry.path = script_path;
                entry.id = manifest.id;
                entry.script = unpublished.script;
                Scripts().emplace(key, std::move(entry));
                unpublished.script.reset();
                published = true;
            }
        }
        if (published) {
            HostLog(WOTBMOD_V3_LOG_INFO,
                    std::string(manifest.id) + ": installed Lua mod loaded");
        } else {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    std::string(manifest.id) +
                        ": not loaded - host stopped or id became duplicate");
        }
    } catch (...) {
        HostLog(WOTBMOD_V3_LOG_ERROR,
                "an installed Lua mod could not be loaded");
    }
}

void LoadInstalledFolder(const std::wstring& folder) noexcept {
    HANDLE search = INVALID_HANDLE_VALUE;
    try {
        WIN32_FIND_DATAW found = {};
        search = FindFirstFileW(
            wotbmod::lua::JoinPath(folder, L"*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> directories;
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                continue;
            }
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            directories.push_back(name);
        } while (FindNextFileW(search, &found));
        FindClose(search);
        search = INVALID_HANDLE_VALUE;
        std::sort(directories.begin(), directories.end());
        for (const std::wstring& name : directories) {
            LoadInstalledOne(wotbmod::lua::JoinPath(folder, name), name);
        }
    } catch (...) {
        if (search != INVALID_HANDLE_VALUE) FindClose(search);
        HostLog(WOTBMOD_V3_LOG_ERROR,
                "the installed Lua mod folder could not be scanned");
    }
}

// Stops the watcher and unloads every script, in that order. The order is not
// cosmetic: a watcher still running could queue a change that a frame arriving
// mid-teardown would act on, loading a script into a host that is shutting
// down. Shared by disable and unload, because they mean the same thing here.
void ShutDown() noexcept {
    Watcher* watcher = nullptr;
    {
        std::unique_lock<std::mutex> guard(HostLock());
        g_enabled = false;
        watcher = g_watcher;
        g_watcher = nullptr;
        // OnFrame owns shared references after dropping HostLock. Wait with a
        // condition variable (which releases this lock while sleeping) so no
        // script object, state or DLL code can remain in use after shutdown
        // returns. A frame already inside may still need HostLock to detach a
        // faulted script or drain watcher changes; the released wait lets it.
        FramesIdle().wait(guard, []() { return g_frames_in_flight == 0u; });
    }
    // Outside the lock: Stop joins a worker thread. That worker touches
    // nothing this lock guards, so holding it would not deadlock today - but a
    // host lock held across a thread join is a rule that only has to change
    // once to become one.
    //
    // The answer is acted on rather than dropped, and this is the whole reason
    // the watcher is a `new Watcher` rather than a member: false means the
    // worker could not be joined and may still be writing into the buffer this
    // object owns, so freeing it would be worse than the leak. Leaking one
    // watcher costs a thread and four handles for the life of the process;
    // deleting it costs whatever the kernel writes into freed memory next. See
    // Watcher::Stop.
    if (watcher) {
        if (watcher->Stop()) {
            delete watcher;
        } else {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    "the dev-folder watcher could not be stopped; it has been "
                    "left running rather than freed under its own worker");
        }
    }

    // F7. Reaping faulted scripts used to be reachable from exactly two call
    // sites, both inside OnFrame and both below `if (!g_enabled) return;`. A
    // script whose instruction budget tripped on the render thread or a worker
    // therefore depended entirely on another frame arriving to be retired, and
    // the disable path - which is the moment frames stop - had no reap in it at
    // all.
    //
    // Being exact about what that did and did not cost, because the fix is
    // worth only what is true of it: the general walk below *does* destroy a
    // faulted script, so one did not survive a disable. What it did not do is
    // retire it through the faulted path first. Two consequences, and the
    // second is the one with teeth:
    //
    //   * ordering. `taken` is a map walked in path order, so a healthy
    //     script's on_disable - real Lua code, entitled to post events, read
    //     storage and enumerate entities - could run while a faulted zombie
    //     was still in Scripts() holding its whole ownership ledger:
    //     subscriptions the client still delivers into, controls still on
    //     screen. Reaping first means every on_disable runs in a host whose
    //     faulted scripts are already gone.
    //   * reachability. A reaper that can only be entered from behind
    //     `if (!g_enabled) return;` is one call-site change away from never
    //     running at all. Now it is entered from the teardown path too, which
    //     is the path that always runs.
    //
    // Locking, per the contract at the top of this file: called with HostLock
    // *not* held - ReapInstructionLimitedScripts takes it itself, extracts
    // under it, and destroys outside it. It is deliberately placed after the
    // frames-idle wait, so no frame is concurrently walking Scripts(), and
    // before the swap below, so the two cannot both claim the same entry.
    //
    // A fault that trips *after* this call - an event delivery on a worker
    // thread is not a frame and is not covered by the frames-idle wait - is
    // still caught: DestroyScript below tests InstructionLimitExceeded itself
    // and takes the same no-on_disable path. This is a second gate, not the
    // only one, and reaping at trip time remains impossible for the reason
    // given above ReapInstructionLimitedScripts.
    ReapInstructionLimitedScripts();

    // Swapped out, then deleted with nothing held. Each delete is a RevokeAll
    // that calls the ABI and waits for in-flight deliveries on other threads;
    // a delivery that wanted this lock while this thread waited for it would
    // be a deadlock with the game's render thread on one side of it.
    std::map<std::wstring, Loaded> taken;
    {
        std::lock_guard<std::mutex> guard(HostLock());
        taken.swap(Scripts());
    }
    for (std::pair<const std::wstring, Loaded>& entry : taken) {
        // Narrow allocates, and this function is noexcept. A script that has
        // to be destroyed without a name to log it under is still destroyed.
        std::string name;
        try {
            name = Narrow(FileNameOf(entry.second.path));
        } catch (...) {
        }
        DestroyScript(std::move(entry.second.script), name);
    }
}

// What this host itself holds, measured off wotbmod.permissions rather than
// written down here, and cached as the ceiling every script is cut down to.
//
// Deriving it is the whole point. The spec's honest admission about this fence
// - "even a total bypass of the inner fence cannot get past events, UI and its
// own storage" - is a claim about the host's real grants, and a hardcoded list
// asserting it would drift from the manifest the moment either changed.
// Measuring makes the runtime enforce the claim instead.
//
// The one thing this can get wrong is being asked before the runtime has
// decided: a client that has not granted anything yet answers with nothing, and
// nothing denies everything. That is the safe direction, and it is a *measured*
// nothing - the client answered.
//
// A client that cannot be asked at all is the other case, and it now stops the
// host rather than being covered for. There used to be a fallback here: the
// four names the spec fixes for this host's slice, granted when the interface
// was missing or unreadable, logged as a guess. It was deliberate, it was
// tested, and the native outer fence capped what it could cost - and it was
// still the single place in this design where doubt granted instead of denying.
//
// The spec's own principle decides it: honest refusal beats coverage. A host
// that cannot read the ceiling it exists to enforce does not know what it is
// guarding, and a host that silently runs every script at the maximum because
// it could not read a tier is the failure nobody would ever notice - everything
// works, and the fence is decoration. So: no ceiling, no scripts, and the
// reason in the log at error level.
//
// False means the caller must not load anything.
//
// Which stage is asking matters, and only for what gets said. A failure at load
// is *expected* on a conforming runtime: the client learns what this mod wants
// from the WotbModV3Info the entry point fills in, so at the moment the entry
// returns there is nothing to grant yet and wotbmod.permissions may reasonably
// refuse the query. Logging "this client cannot be read, no script will be
// loaded" there would be a false alarm on every single load of a perfectly good
// client - an error-level line about a failure that has not happened, printed
// before the only stage that can decide has run.
//
// So on_enable is the authoritative measurement and the only one that refuses.
// Load-time measurement is a best effort whose only job is to give a real
// ceiling to anything created outside the enable lifecycle; when it fails it
// says so at debug level, in the words of what it actually is.
enum class CeilingStage { kLoad, kEnable };

bool MeasureCeiling(CeilingStage stage) noexcept {
    if (wotbmod::lua::MeasureHostCeiling(g_bootstrap, g_mod)) return true;
    if (stage == CeilingStage::kLoad) {
        HostLog(WOTBMOD_V3_LOG_DEBUG,
                "wotbmod.permissions is not answering yet at load time, which "
                "is normal - the runtime has not decided this mod's grants "
                "until on_enable. The ceiling stays empty, which denies "
                "everything, until on_enable measures it for real");
        return false;
    }
    HostLog(WOTBMOD_V3_LOG_ERROR,
            "wotbmod.permissions could not be read, so this host cannot know "
            "the ceiling it is meant to enforce; no script will be loaded. "
            "This is deliberate: running scripts at an assumed maximum because "
            "the real one could not be read would make the fence decoration. "
            "Update the client, or grant this mod its permissions, and enable "
            "again");
    return false;
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap, WotbModV3Handle mod) noexcept {
    // Re-enabling without a disable in between must not start a second watcher
    // or double the script set. ShutDown is idempotent and cheap when there is
    // nothing to shut down.
    ShutDown();
    if (bootstrap) g_bootstrap = bootstrap;
    g_mod = mod;

    // Before LoadFolder, and that ordering is the requirement rather than a
    // preference: the ceiling has to be the one every script loaded by this
    // enable is cut down to, and LoadFolder runs scripts.
    //
    // A failure returns before g_enabled is set, so no watcher starts, no
    // folder is scanned, and on_frame does nothing when the client keeps
    // calling it - which it will. The host stays loaded and inert, which is the
    // state a mod author can see and report; it does not run their scripts at a
    // privilege level nobody measured.
    if (!MeasureCeiling(CeilingStage::kEnable)) return;

    std::wstring folder;
    std::wstring installed_folder;
    uint32_t debounce_ms = 0u;
    try {
        folder = DevFolder();
        installed_folder = LuaModsFolder();
    } catch (...) {
        HostLog(WOTBMOD_V3_LOG_ERROR,
                "the Lua mod folder paths are unavailable");
        return;
    }
    {
        std::lock_guard<std::mutex> guard(HostLock());
        g_enabled = true;
        debounce_ms = g_debounce_override_ms;
    }

    // Installed mods are loaded once per enable from their manifests. They do
    // not depend on the optional dev folder or its watcher existing.
    if (installed_folder.empty()) {
        HostLog(WOTBMOD_V3_LOG_WARNING,
                "no installed Lua mod folder could be resolved");
    } else {
        LoadInstalledFolder(installed_folder);
    }

    if (folder.empty()) {
        HostLog(WOTBMOD_V3_LOG_WARNING,
                "no dev folder could be resolved; hot reload is off");
        return;
    }

    Watcher* watcher = nullptr;
    try {
        watcher = new Watcher();
        const std::chrono::milliseconds debounce =
            debounce_ms == 0u ? wotbmod::lua::kDefaultDebounce
                              : std::chrono::milliseconds(debounce_ms);
        if (!watcher->Start(folder, debounce)) {
            // SafeToDestroy first, exactly as ShutDown consults Stop() before
            // freeing g_watcher, and for the same reason: Start stops the
            // watcher internally, and a join that failed in there left a
            // detached worker still writing into this object. Deleting it then
            // would be freeing memory under a running thread - the one damage
            // Watcher::Stop refuses to do, undone by its caller. This site used
            // to delete unconditionally.
            if (watcher->SafeToDestroy()) {
                delete watcher;
            } else {
                HostLog(WOTBMOD_V3_LOG_ERROR,
                        "the dev-folder watcher could not be stopped while "
                        "starting; it has been left running rather than freed "
                        "under its own worker");
            }
            watcher = nullptr;
            // Not fatal, and deliberately so: a missing dev folder is the
            // ordinary state of a player's install. The host stays loaded and
            // simply never reloads anything.
            HostLog(WOTBMOD_V3_LOG_WARNING,
                    "the dev folder could not be watched (" +
                        Narrow(folder) + "); hot reload is off");
        }
    } catch (...) {
        // Same question, and it has to be asked here too: a throw out of Start
        // says nothing about whether its internal Stop joined. A watcher that
        // was never constructed is null and SafeToDestroy is not consulted.
        if (watcher && !watcher->SafeToDestroy()) {
            HostLog(WOTBMOD_V3_LOG_ERROR,
                    "the dev-folder watcher could not be stopped while "
                    "starting; it has been left running rather than freed "
                    "under its own worker");
        } else {
            delete watcher;
        }
        watcher = nullptr;
        HostLog(WOTBMOD_V3_LOG_ERROR, "the watcher could not be started");
    }
    if (watcher) {
        std::lock_guard<std::mutex> guard(HostLock());
        g_watcher = watcher;
    }

    // After the watcher, not before: a script written between the scan and the
    // watch starting would otherwise be missed entirely. The other order costs
    // at most one redundant reload of a file that was already loaded, which is
    // a frame; this order costs an edit.
    LoadFolder(folder);
}

// The reload pump, and the whole reason reloads are safe.
//
// Everything the watcher noticed is acted on here, on the thread the client
// calls on_frame on, between frames. A reload from the watcher's own thread
// could call lua_close on a state the render thread is currently executing
// inside - which is memory corruption, not a race for a value - and no lock
// this host holds would help, because the render thread would be holding the
// script lock legitimately while the watcher waited to destroy the very object
// that owns it.
void WOTBMOD_V3_CALL OnFrame(
    const WotbModV3Bootstrap*, WotbModV3Handle, uint64_t frame_index,
    double delta_seconds) noexcept {
    // try/catch, not decoration. `changed` is a vector of strings built from a
    // move out of the queue, and the loop below allocates on every iteration -
    // std::bad_alloc crossing a client callback out of a noexcept function is
    // std::terminate, which is the game gone. Every boundary in this file has
    // one, and ReloadOne has its own so that one bad file does not stop the
    // rest of a drain.
    try {
        {
            std::lock_guard<std::mutex> guard(HostLock());
            if (!g_enabled) return;
            ++g_frames_in_flight;
        }
        struct FrameLease {
            ~FrameLease() {
                std::lock_guard<std::mutex> guard(HostLock());
                --g_frames_in_flight;
                if (g_frames_in_flight == 0u) FramesIdle().notify_all();
            }
        } frame_lease;

        ReapInstructionLimitedScripts();

        // shared_ptr is the lifetime barrier. We take only shared references
        // under HostLock, release that lock, then enter Lua. Reload/disable may
        // detach concurrently, but Deactivate serializes on the script lock
        // and the object cannot disappear until this frame drops its copy.
        std::vector<std::pair<std::wstring, std::shared_ptr<LuaScript>>> frames;
        {
            std::lock_guard<std::mutex> guard(HostLock());
            frames.reserve(Scripts().size());
            for (const auto& entry : Scripts()) {
                frames.emplace_back(entry.first, entry.second.script);
            }
        }
        for (const auto& frame : frames) {
            if (!frame.second) continue;
            std::string error;
            if (frame.second->CallFrame(frame_index, delta_seconds, &error)) {
                continue;
            }
            std::shared_ptr<LuaScript> faulted =
                DetachIfSame(frame.first, frame.second);
            if (!faulted) continue;
            std::string name;
            try {
                name = Narrow(FileNameOf(frame.first));
                HostLog(WOTBMOD_V3_LOG_ERROR,
                        name + ": on_frame failed: " + error +
                            "; disabled");
            } catch (...) {
            }
            DestroyScript(std::move(faulted), name);
        }
        ReapInstructionLimitedScripts();

        std::vector<std::wstring> changed;
        {
            std::lock_guard<std::mutex> guard(HostLock());
            if (!g_enabled || !g_watcher) return;
            // Under the lock, so that a disable running on another thread
            // cannot delete the watcher between the test above and the call.
            // TakeChanged touches only the queue's own leaf lock, so this is
            // short and closes no cycle.
            changed = g_watcher->TakeChanged();
        }
        for (const std::wstring& path : changed) ReloadOne(path);
    } catch (...) {
        HostLog(WOTBMOD_V3_LOG_ERROR, "a frame could not be pumped");
    }
}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap*, WotbModV3Handle) noexcept {
    // The walk Task 8 could not write, because the set did not exist. What it
    // does *not* have to do is anything about subscriptions, controls or
    // transactions: deleting a LuaScript revokes everything it made - see
    // ~LuaScript and lua_ownership.h - so a disable that deletes each script is
    // a complete disable, and one that forgot a resource kind is not a bug this
    // file can have.
    //
    // ShutDown is itself noexcept and catches internally, so this cannot throw
    // today; the guard is here because "the boundary catches" must be true of
    // the boundary rather than of what it happens to call.
    try {
        ShutDown();
    } catch (...) {
    }
}

void WOTBMOD_V3_CALL OnUnload(
    const WotbModV3Bootstrap*, WotbModV3Handle) noexcept {
    // Idempotent after a disable, and load bearing without one: a runtime that
    // unloads a mod it never disabled must not leave a worker thread running
    // inside a DLL that is about to be unmapped.
    try {
        ShutDown();
    } catch (...) {
    }
    g_bootstrap = nullptr;
    g_mod = WOTBMOD_V3_INVALID_HANDLE;
}

}  // namespace

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;

    g_bootstrap = bootstrap;
    g_mod = mod;

    // The first moment this host holds a bootstrap, and therefore the first
    // moment it can ask what it was granted. on_enable asks again, and that one
    // is the authoritative answer - a runtime learns what this mod wants from
    // the out_info being filled in below, so it cannot have decided the grants
    // yet when this call returns. Asking twice costs one query and means a
    // script created outside the enable lifecycle - the test entry points at
    // the bottom of this file are the only ones today - runs against a real
    // ceiling rather than an empty one.
    //
    // Discarded here, unlike in on_enable: this is the entry point, and a mod
    // that refused to load at all would be indistinguishable to the runtime
    // from one that is broken. The refusal belongs at enable, where there is a
    // log channel and a lifecycle to refuse within; here the only effect of a
    // failure is that the ceiling stays empty, which denies everything.
    //
    // kLoad, so a failure says what it is - "not answering yet, on_enable will
    // ask again" at debug level - rather than crying wolf at error level on
    // every load of a conforming client that simply has not decided this mod's
    // grants yet. on_enable is the authoritative measurement.
    (void)MeasureCeiling(CeilingStage::kLoad);

    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    strncpy_s(out_info->id, "wotbmod.lua_host", _TRUNCATE);
    strncpy_s(out_info->name, "WotbMod Lua Host", _TRUNCATE);
    strncpy_s(out_info->version, "0.1.0-preview.1", _TRUNCATE);
    strncpy_s(out_info->author, "BlitzForge SDK", _TRUNCATE);
    strncpy_s(out_info->description,
              "Runs Lua scripts against the V3 ABI", _TRUNCATE);
    // REVIEWED is the ceiling for every script this host will ever run.
    // Raising it raises it for all of them at once, so it does not move
    // without a matching change to the spec's security posture.
    out_info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    // The reload pump. Every reload happens inside this callback, on the
    // thread the client calls it on, between frames - see OnFrame.
    out_info->on_frame = &OnFrame;
    return WOTBMOD_V3_OK;
}

// ===========================================================================
// Test-only exports - compiled out of a release DLL
// ===========================================================================
//
// Everything below this line exists so the suite can drive this host directly,
// and none of it is part of the mod ABI. It is now behind a build flag, and the
// reason is narrower and more serious than tidiness.
//
// Three of these mint scripts: WotbLuaHost_RunScriptForTests,
// WotbLuaHost_CreateScriptForTests and
// WotbLuaHost_CreateScriptWithPermissionsForTests. The first two hand out a
// script at DevCeiling() - the host's full measured ceiling - and the third
// hands out whatever set the caller names. Reaching them needs native code
// already inside the process, so there is no escalation available to a *script*
// and nothing in Lua can find them. But "an attacker who is already native"
// is not the standard a security boundary is held to: an exported entry point
// whose whole job is to mint a maximum-privilege script does not belong in a
// shipped DLL, whoever can reach it. Removing them costs the release build
// nothing and removes the question.
//
// The test build defines WOTBMOD_LUA_HOST_TESTS. The release build does not,
// and tests\build_lua_host_tests.cmd builds the DLL a second time without it
// and greps the export table to prove the symbols are gone - because a gate
// nobody checks is a gate that quietly stops being one.
#ifdef WOTBMOD_LUA_HOST_TESTS

extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_InputCursorOwnersForTests() {
    return static_cast<uint32_t>(wotbmod::lua::InputCursorOwnerCount());
}

// Exported for host tests only: compile and run one script in a fresh
// sandboxed state and report which stage failed. Not part of the mod ABI.
//
// LuaScript::Create reports the failing stage explicitly via out_stage,
// rather than this shim trying to recover it from the error text: a runtime
// error from a chunk named "@probe" (e.g. error('boom') -> "probe:1: boom")
// contains the same "probe:" substring a compile error does, so the message
// alone cannot distinguish the two.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_RunScriptForTests(
    const char* source, char* out_error, uint32_t error_size) {
    std::string error;
    wotbmod::lua::LuaScriptStage stage = wotbmod::lua::LuaScriptStage::kOk;
    wotbmod::lua::LuaScript* script = wotbmod::lua::LuaScript::Create(
        "probe", source, g_bootstrap, g_mod,
        wotbmod::lua::ScriptPermissions::DevCeiling(), &error, &stage);
    if (!script) {
        if (out_error && error_size) {
            strncpy_s(out_error, error_size, error.c_str(), _TRUNCATE);
        }
        return stage == wotbmod::lua::LuaScriptStage::kRuntimeFailed ? 2u : 1u;
    }
    delete script;
    return 0u;
}

// The permissions a test asks for, in the one spelling the four exports below
// share: a null manifest means "the dev folder's trust level", which is the
// ceiling, and anything else is a manifest to be read and intersected.
//
// Null is a shim convention rather than something ScriptPermissions believes -
// FromManifest(nullptr) is a refusal, because a script that came with no
// manifest at all asked for nothing and gets nothing.
namespace {
bool PermissionsForTests(const char* manifest_json,
                         wotbmod::lua::ScriptPermissions* out_permissions,
                         char* out_error, uint32_t error_size) {
    if (!manifest_json) {
        *out_permissions = wotbmod::lua::ScriptPermissions::DevCeiling();
        return true;
    }
    const char* manifest_error = nullptr;
    *out_permissions = wotbmod::lua::ScriptPermissions::FromManifest(
        manifest_json, &manifest_error);
    if (!manifest_error) return true;
    if (out_error && error_size) {
        strncpy_s(out_error, error_size, manifest_error, _TRUNCATE);
    }
    return false;
}
}  // namespace

// Exported for host tests only: WotbLuaHost_RunScriptForTests, with the script
// cut down to what a manifest asks for.
//
// 0 = ran, 1 = compile error, 2 = raised, 3 = the manifest was refused and
// nothing ran at all. 3 is its own status rather than "ran with nothing
// granted" because a manifest this host cannot read is not a script this host
// should be running: the fence would hold either way, but refusing to start is
// the answer that cannot be mistaken for a script that simply asked for little.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_RunScriptWithPermissionsForTests(
    const char* manifest_json, const char* source, char* out_error,
    uint32_t error_size) {
    wotbmod::lua::ScriptPermissions permissions;
    if (!PermissionsForTests(manifest_json, &permissions, out_error,
                             error_size)) {
        return 3u;
    }
    std::string error;
    wotbmod::lua::LuaScriptStage stage = wotbmod::lua::LuaScriptStage::kOk;
    wotbmod::lua::LuaScript* script = wotbmod::lua::LuaScript::Create(
        "probe", source, g_bootstrap, g_mod, permissions, &error, &stage);
    if (!script) {
        if (out_error && error_size) {
            strncpy_s(out_error, error_size, error.c_str(), _TRUNCATE);
        }
        return stage == wotbmod::lua::LuaScriptStage::kRuntimeFailed ? 2u : 1u;
    }
    delete script;
    return 0u;
}

// Exported for host tests only: WotbLuaHost_CreateScriptForTests, with the
// script cut down to what a manifest asks for.
//
// Its own export rather than a parameter on the existing one, so that every
// call site in the suite that does not care about permissions keeps saying so
// by not mentioning them. Null on a refused manifest, exactly as on a script
// that would not compile.
extern "C" WOTBMOD_V3_EXPORT void* WOTBMOD_V3_CALL
WotbLuaHost_CreateScriptWithPermissionsForTests(
    const char* manifest_json, const char* id, const char* source,
    char* out_error, uint32_t error_size) {
    wotbmod::lua::ScriptPermissions permissions;
    if (!PermissionsForTests(manifest_json, &permissions, out_error,
                             error_size)) {
        return nullptr;
    }
    std::string error;
    wotbmod::lua::LuaScript* script = wotbmod::lua::LuaScript::Create(
        id, source, g_bootstrap, g_mod, permissions, &error, nullptr);
    if (!script && out_error && error_size) {
        strncpy_s(out_error, error_size, error.c_str(), _TRUNCATE);
    }
    return script;
}

// Exported for host tests only: does the permission set a manifest yields
// allow one named permission?
//
// 0 = it does not, 1 = it does, 2 = the manifest was refused and out_error says
// why. A null manifest asks the same question of the ceiling itself, which is
// how the suite reads what MeasureHostCeiling measured without this host having
// to publish a bitmask whose meaning would then live in two places.
//
// This is what exercises `bool Allows(const char*) const` directly. The fence
// itself never calls it - a guard resolves its family's name to a bit once, at
// registration - so without this the mandated by-name API would be reachable
// only through the bit it shares.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_ManifestAllowsForTests(
    const char* manifest_json, const char* permission, char* out_error,
    uint32_t error_size) {
    wotbmod::lua::ScriptPermissions permissions;
    if (!PermissionsForTests(manifest_json, &permissions, out_error,
                             error_size)) {
        return 2u;
    }
    return permissions.Allows(permission) ? 1u : 0u;
}

// Exported for host tests only: a script that outlives the call that made it.
//
// WotbLuaHost_RunScriptForTests above destroys its script before it returns,
// which is the right shape for every binding a script calls *out* through -
// the ABI has already been called by the time the chunk ends. Events is the
// one family where the client calls *in*, so a test has to hold a script
// still alive when the event is fired, run more source in that same state
// afterwards to see what the callback did, and then destroy it. Three calls,
// because those are three separate moments; one "run, fire, check" helper
// would hide exactly the lifetime this interface has to get right.
//
// The handle is the LuaScript* itself, opaque to the test.
extern "C" WOTBMOD_V3_EXPORT void* WOTBMOD_V3_CALL
WotbLuaHost_CreateScriptForTests(
    const char* id, const char* source, char* out_error, uint32_t error_size) {
    std::string error;
    wotbmod::lua::LuaScript* script = wotbmod::lua::LuaScript::Create(
        id, source, g_bootstrap, g_mod,
        wotbmod::lua::ScriptPermissions::DevCeiling(), &error, nullptr);
    if (!script && out_error && error_size) {
        strncpy_s(out_error, error_size, error.c_str(), _TRUNCATE);
    }
    return script;
}

// Runs more source inside an existing script's state, under that script's
// own lock - the same lock an event delivery takes, which is what makes
// "fire from another thread, then read what the handler saw" a meaningful
// sequence rather than two unrelated states.
//
// 0 = ran, 1 = did not compile, 2 = raised, 3 = refused because a previous
// entry exhausted this script's instruction budget.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_EvalInScriptForTests(
    void* handle, const char* source, char* out_error, uint32_t error_size) {
    auto* script = static_cast<wotbmod::lua::LuaScript*>(handle);
    if (!script || !source) return 1u;
    wotbmod::lua::LuaScript::Entry entry(*script);
    lua_State* state = entry.state();
    const int top = lua_gettop(state);
    uint32_t status = 0u;
    bool instruction_limit = script->InstructionLimitExceeded();
    if (instruction_limit) {
        status = 3u;
    } else if (luaL_loadbufferx(state, source, std::strlen(source), "@probe", "t") !=
        LUA_OK) {
        status = 1u;
    } else {
        const wotbmod::lua::LuaProtectedCallResult result =
            script->ProtectedCall(state, 0, 0);
        instruction_limit =
            result == wotbmod::lua::LuaProtectedCallResult::kInstructionLimit;
        if (instruction_limit) {
            status = 3u;
        } else if (result != wotbmod::lua::LuaProtectedCallResult::kOk) {
            status = 2u;
        }
    }
    if (status != 0u && out_error && error_size) {
        const char* message = instruction_limit
                                  ? wotbmod::lua::InstructionBudgetError()
                                  : lua_tostring(state, -1);
        strncpy_s(out_error, error_size,
                  message ? message : "<non-string error>", _TRUNCATE);
    }
    lua_settop(state, top);
    return status;
}

extern "C" WOTBMOD_V3_EXPORT void WOTBMOD_V3_CALL
WotbLuaHost_DestroyScriptForTests(void* handle) {
    delete static_cast<wotbmod::lua::LuaScript*>(handle);
}

// Exported for host tests only: the courtesy half of DestroyScript - the
// wotb.mod.on_disable handlers and the author's global on_disable - without
// the delete, so a test can read what they did before the state closes.
// 1 when every handler returned, 0 with out_error when one raised.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_DeactivateScriptForTests(void* handle, char* out_error,
                                     uint32_t error_size) {
    auto* script = static_cast<wotbmod::lua::LuaScript*>(handle);
    if (!script) return 0u;
    std::string error;
    const bool ok = script->Deactivate(true, &error);
    if (!ok && out_error && error_size) {
        strncpy_s(out_error, error_size, error.c_str(), _TRUNCATE);
    }
    return ok ? 1u : 0u;
}

// Exported for host tests only: how many resources this script's ownership
// registry is still holding - subscriptions, controls and open transactions
// together.
//
// It exists so that "nothing survived the script" can be checked from both
// sides rather than one. The mock's own LiveSubscriptions()/LiveControls()/
// LiveTransactions() say what the client is still holding; this says what the
// host still believes it owes. Two counters that must both reach zero catch
// the case one alone cannot: a ledger that quietly stopped recording would
// leave the client's counters at zero for the wrong reason, and every leak
// test in the suite would pass over it.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_OwnedCountForTests(void* handle) {
    auto* script = static_cast<wotbmod::lua::LuaScript*>(handle);
    if (!script) return 0u;
    return static_cast<uint32_t>(script->Ownership().Count());
}

// Exported for host tests only: how many subscription records this host is
// still holding, across every script that has run in this process. Takes no
// handle - it is deliberately a question about the host, asked after every
// script is gone. See EventSubscriptionRecordCount in lua_bindings.h for why
// the client's own view cannot answer it.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_SubscriptionRecordsForTests() {
    return static_cast<uint32_t>(wotbmod::lua::EventSubscriptionRecordCount());
}

// Exported for host tests only: the high-water mark of the registry's array
// part, which is exactly the number of luaL_ref slots this state has ever
// needed at once.
//
// It exists because "unsubscribing releases the registry reference" is
// otherwise untestable from a script. luaL_unref returns a slot to the
// registry's free list and luaL_ref reuses it, so a subscribe/unsubscribe
// cycle repeated a hundred times keeps this constant when the unref happens
// and grows it by a hundred when it does not. Nothing in the sandbox can see
// that - collectgarbage and debug are both removed, deliberately - so the
// only place to read it is here.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_RegistrySlotsForTests(void* handle) {
    auto* script = static_cast<wotbmod::lua::LuaScript*>(handle);
    if (!script) return 0u;
    wotbmod::lua::LuaScript::Entry entry(*script);
    return static_cast<uint32_t>(
        lua_rawlen(entry.state(), LUA_REGISTRYINDEX));
}

// Exported for host tests only: point the dev folder at a temp directory and,
// optionally, shorten the debounce.
//
// A null folder clears the override, so the next enable resolves the real one.
// debounce_ms of 0 leaves kDefaultDebounce alone; anything else replaces it,
// which exists for one reason: fifty reloads at the real 200 ms interval is ten
// seconds of a test suite doing nothing but sleeping. The interval itself is
// proved separately against fabricated time points (ChangeQueue), and that the
// host ships with 200 ms is proved by
// WotbLuaHost_DevWatcherDebounceMsForTests below - so shortening it here costs
// no coverage, which is the only reason it is acceptable to offer.
//
// Takes effect at the next on_enable, not immediately: the folder a running
// watcher is watching is not a thing that can be changed underneath it.
extern "C" WOTBMOD_V3_EXPORT void WOTBMOD_V3_CALL
WotbLuaHost_SetDevFolderForTests(const wchar_t* folder, uint32_t debounce_ms) {
    std::lock_guard<std::mutex> guard(HostLock());
    if (folder) {
        DevFolderOverride() = folder;
    } else {
        DevFolderOverride().clear();
    }
    g_debounce_override_ms = debounce_ms;
}

// Points the installed-mod scan at a temp root for the next enable. Kept
// separate from the dev override because the two trust paths must be exercised
// together: installed scripts use their manifests; dev scripts use the full
// measured ceiling.
extern "C" WOTBMOD_V3_EXPORT void WOTBMOD_V3_CALL
WotbLuaHost_SetLuaModsFolderForTests(const wchar_t* folder) {
    std::lock_guard<std::mutex> guard(HostLock());
    if (folder) {
        LuaModsFolderOverride() = folder;
    } else {
        LuaModsFolderOverride().clear();
    }
}

// Exported for host tests only: the folder DevFolder() would resolve *right
// now*, through exactly the code OnEnable uses.
//
// It exists because of a gap the reviewer named and the suite could not have
// found: branches 2 and 3 of DevFolder - %WOTBMOD_LUA_DEV_DIR%, and this DLL's
// own path truncated two levels and joined to `lua-dev` - are the only ones a
// real mod author ever takes, and every other test in this file reaches the
// reload path through the branch-1 override. If the truncation were off by one
// level the feature would ship silently dead: no folder, no watcher, one
// warning in a log nobody reads, and every one of this suite's checks still
// green because all of them override it.
//
// Returns the length in characters, not counting the terminator, and writes
// only if the whole string plus a terminator fits. Zero means DevFolder could
// not resolve one at all. The out-of-band length rather than a truncating copy,
// because a *silently shortened path* is precisely the class of bug this
// accessor was added to catch, and it would be perverse to reintroduce it here.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_ResolveDevFolderForTests(wchar_t* buffer, uint32_t buffer_chars) {
    std::wstring folder;
    try {
        folder = DevFolder();
    } catch (...) {
        return 0u;
    }
    if (folder.empty()) return 0u;
    const uint32_t length = static_cast<uint32_t>(folder.size());
    if (buffer && buffer_chars > length) {
        std::memcpy(buffer, folder.c_str(), (length + 1u) * sizeof(wchar_t));
    }
    return length;
}

extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_ResolveLuaModsFolderForTests(wchar_t* buffer,
                                        uint32_t buffer_chars) {
    std::wstring folder;
    try {
        folder = LuaModsFolder();
    } catch (...) {
        return 0u;
    }
    if (folder.empty()) return 0u;
    const uint32_t length = static_cast<uint32_t>(folder.size());
    if (buffer && buffer_chars > length) {
        std::memcpy(buffer, folder.c_str(), (length + 1u) * sizeof(wchar_t));
    }
    return length;
}

// Exported for host tests only: how many scripts this host is holding.
//
// This is the counter Task 8's experiment 4 says a leak test needs and the
// mock cannot supply. Every client-side counter can read a blameless zero
// while this one climbs by one per reload - a map entry naming a LuaScript
// that was destroyed, or a script that was created and never taken back - and
// that is precisely the shape hot reload introduces. Asked *during* the fifty
// reloads rather than only after them: a host that leaked fifty and swept them
// up at disable would pass an end-state assertion.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_LoadedScriptCountForTests() {
    std::lock_guard<std::mutex> guard(HostLock());
    return static_cast<uint32_t>(Scripts().size());
}

// Exported for host tests only: the sum of every loaded script's ownership
// ledger - subscriptions, controls and open transactions together.
//
// The second host-side view, and not a duplicate of the one above. A host
// holding one script whose ledger has grown to 150 entries has leaked exactly
// as badly as one holding fifty scripts of three, and only this counter can
// tell the difference. It also catches the opposite failure, a ledger that
// quietly stopped recording, which would leave every client-side counter at
// zero for entirely the wrong reason.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_LoadedOwnedCountForTests() {
    std::lock_guard<std::mutex> guard(HostLock());
    size_t total = 0u;
    for (const std::pair<const std::wstring, Loaded>& entry : Scripts()) {
        if (entry.second.script) total += entry.second.script->Ownership().Count();
    }
    return static_cast<uint32_t>(total);
}

// Exported for host tests only: the debounce the running watcher is using, in
// milliseconds, or 0 when no watcher is running.
//
// Two questions in one accessor, and both are asked: that the host ships with
// 200 ms (rather than whichever interval the reload tests asked for), and that
// a disable really stopped the watcher - a worker thread that outlived the
// scripts it feeds would be a thread queueing reloads into a host that has
// shut down.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_DevWatcherDebounceMsForTests() {
    std::lock_guard<std::mutex> guard(HostLock());
    if (!g_watcher || !g_watcher->Running()) return 0u;
    return static_cast<uint32_t>(g_watcher->Debounce().count());
}

// Exported for host tests only: how many watcher worker threads are running
// inside this DLL.
//
// The accessor above answers "is this host holding a watcher", which is a
// question about a pointer. This one answers "is a thread still running",
// which is the question a disable actually has to get right - and the two came
// apart under experiment: a ShutDown that nulled the pointer and leaked the
// Watcher passed every assertion in the suite, while a worker went on holding
// a directory handle inside a DLL the runtime was about to unmap.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_WatcherThreadsForTests() {
    return static_cast<uint32_t>(wotbmod::lua::RunningWatcherThreads());
}

// Exported for host tests only: how many LuaScript objects exist right now.
//
// The counter that watches the script rather than what the script made. Every
// other counter in this file - the client's three, the loaded-script map, the
// summed ledgers, the subscription records - would read exactly right for a
// reload that revoked everything correctly and then forgot to delete the
// object, while a lua_State leaked on every save. See LiveLuaScriptCount.
//
// Takes no HostLock: the count is a module-local atomic and includes scripts
// this host does not own (the ones the test entry points above create by hand),
// which is what makes "zero once every script the suite ran has gone" a
// meaningful thing to ask at the end. It is process-wide only in the current
// build because lua_script.cpp is linked into this DLL exactly once.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_LiveScriptObjectsForTests() {
    return static_cast<uint32_t>(wotbmod::lua::LiveLuaScriptCount());
}

// Exported for host tests only: compile a script, then invoke
// LuaScript::CallGlobal on the given name, and report which stage failed.
// Not part of the mod ABI. Exists to cover CallGlobal directly - including
// the case a script can no longer turn into a process-wide abort() by
// setting a metatable on _G before this call runs (CallGlobal uses a raw
// global lookup rather than lua_getglobal for exactly that reason).
//
// 0 = compiled and the call succeeded (a call target that is not a
// function, or absent entirely, is also success - see CallGlobal's
// contract), 1 = compile error, 2 = the call itself raised.
extern "C" WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbLuaHost_RunScriptCallGlobalForTests(
    const char* source, const char* global_name,
    char* out_error, uint32_t error_size) {
    std::string error;
    wotbmod::lua::LuaScriptStage stage = wotbmod::lua::LuaScriptStage::kOk;
    wotbmod::lua::LuaScript* script = wotbmod::lua::LuaScript::Create(
        "probe", source, g_bootstrap, g_mod,
        wotbmod::lua::ScriptPermissions::DevCeiling(), &error, &stage);
    if (!script) {
        if (out_error && error_size) {
            strncpy_s(out_error, error_size, error.c_str(), _TRUNCATE);
        }
        return stage == wotbmod::lua::LuaScriptStage::kRuntimeFailed ? 2u : 1u;
    }
    std::string call_error;
    const bool ok = script->CallGlobal(global_name, &call_error);
    delete script;
    if (!ok) {
        if (out_error && error_size) {
            strncpy_s(out_error, error_size, call_error.c_str(), _TRUNCATE);
        }
        return 2u;
    }
    return 0u;
}

#endif  // WOTBMOD_LUA_HOST_TESTS
