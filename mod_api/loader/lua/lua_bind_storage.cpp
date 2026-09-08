#include "lua_bindings.h"

#include "lua_convert.h"
#include "lua_permissions.h"
#include "lua_script.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

#include <cstdint>

namespace wotbmod {
namespace lua {
namespace {

// The one thing every one of the 14 closures needs: the interface pointer
// this script was bound against, and the mod handle to pass with every call.
// Pushed once as a full userdata (lua_newuserdatauv, no metatable - it never
// reaches a script, only ever read back through lua_upvalueindex) and shared
// across all 14 closures by luaL_setfuncs, which duplicates the *reference*
// per closure, not the bytes: one StorageContext object no matter how many
// slots read it.
struct StorageContext {
    const WotbModV3StorageApiV1* api;
    WotbModV3Handle mod;
    // Only begin_transaction, commit and rollback touch this: it is how an
    // open transaction gets onto the script's ownership ledger and off it
    // again. The other 11 slots are stateless request-replies against the
    // client and hold nothing.
    LuaScript* script;
};

// GuardedUpvalueIndex, not lua_upvalueindex: every slot in this file is
// installed behind the shared permission guard, which owns upvalue 1. See
// lua_permissions.h.
StorageContext* Context(lua_State* state) noexcept {
    return static_cast<StorageContext*>(
        lua_touserdata(state, GuardedUpvalueIndex(1)));
}

// The transaction argument every commit/rollback/transaction_* slot takes:
// rule 6's CheckArgHandle plus the one expected-type phrase all five call
// sites need, so none of them types it by hand.
//
// This was CheckHandle-then-PushArgumentError written out here, one of five
// identical copies across three interfaces, on the reasoning that a handle
// argument had no accessor of its own. It has one now; the only thing left
// for this wrapper to do is the WotbModV3Token cast, which is a fact about
// storage rather than about handles.
bool CheckArgTransaction(lua_State* state, int index,
                         WotbModV3Token* out) noexcept {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgHandle(state, index, kHandleStorageTransaction,
                        "a wotb.storage_transaction handle", &handle)) {
        return false;
    }
    if (out) *out = static_cast<WotbModV3Token>(handle);
    return true;
}

// The four legal WotbModV3StoragePathKind values, and nothing else -
// checked by value, not merely by type. LUA_TNUMBER accepts a float and
// lua_isinteger alone would still accept an in-range-looking integer like 0
// or 99, neither of which the ABI defines: get_path(2.5) or get_path(0)
// must be refused the same way a wrong-type argument is, not translated
// into whichever path_kind happens to fall out of the cast.
//
// This was a hand-rolled switch and a second, separate refusal next to a
// CheckArg* call that had already checked the type - the gap that gave rule
// 6 its CheckArgEnum. Now it is the value set and the phrase, and the
// accessor does the rest.
constexpr lua_Integer kPathKindValues[] = {
    WOTBMOD_V3_STORAGE_PATH_DATA, WOTBMOD_V3_STORAGE_PATH_CONFIG,
    WOTBMOD_V3_STORAGE_PATH_CACHE, WOTBMOD_V3_STORAGE_PATH_TEMP};

const EnumValues kPathKind =
    Enum(kPathKindValues,
         "PATH_DATA, PATH_CONFIG, PATH_CACHE or PATH_TEMP");

// ---------------------------------------------------------------------------
// get_json, get_path: the sized-string dance, via FetchSizedString/PushSizedString.
// ---------------------------------------------------------------------------

int StorageGetJson(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    const char* key = nullptr;
    if (!CheckArgString(state, 1, &key)) return 2;
    const WotbModV3StorageApiV1* api = ctx->api;
    const WotbModV3Handle mod = ctx->mod;
    // key is a pointer into the Lua string at stack index 1; it stays valid
    // for the life of this call (nothing pops the stack or triggers GC
    // between here and PushSizedString's own use of it), so the lambda
    // captures the pointer rather than copying the text - three words,
    // comfortably inside std::function's 32-bit small-object buffer.
    return PushSizedString(
        state,
        [api, mod, key](char* buffer, uint32_t* inout_size) {
            return api->get_json(mod, key, buffer, inout_size);
        },
        "storage.get_json");
}

int StorageGetPath(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    lua_Integer raw = 0;
    if (!CheckArgEnum(state, 1, kPathKind, &raw)) return 2;
    const uint32_t kind = static_cast<uint32_t>(raw);
    const WotbModV3StorageApiV1* api = ctx->api;
    const WotbModV3Handle mod = ctx->mod;
    return PushSizedString(
        state,
        [api, mod, kind](char* buffer, uint32_t* inout_size) {
            return api->get_path(mod, kind, buffer, inout_size);
        },
        "storage.get_path");
}

// ---------------------------------------------------------------------------
// set_json, erase, flush: plain commands, PushResult.
// ---------------------------------------------------------------------------

int StorageSetJson(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    const char* key = nullptr;
    if (!CheckArgString(state, 1, &key)) return 2;
    const char* json = nullptr;
    if (!CheckArgString(state, 2, &json)) return 2;
    return PushResult(state, ctx->api->set_json(ctx->mod, key, json),
                      "storage.set_json");
}

int StorageErase(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    const char* key = nullptr;
    if (!CheckArgString(state, 1, &key)) return 2;
    return PushResult(state, ctx->api->erase(ctx->mod, key), "storage.erase");
}

int StorageFlush(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    return PushResult(state, ctx->api->flush(ctx->mod), "storage.flush");
}

// ---------------------------------------------------------------------------
// contains: booleans and genuine absence, not a general "falsy
// out-parameter" family.
//
// `0` and `""` are truthy in Lua - only `false` and `nil` are falsy - so a
// slot returning a number or a string out-parameter bare is fine exactly as
// rule 2 already states, and does not need this treatment. `contains`
// answers `false`, a value that IS one of Lua's two falsy values, and
// `false` ("the key is not there") is a legitimate, successful outcome, not
// a failure. So this is narrowly a booleans-and-genuine-absence rule: rule
// 2's optional-handle shape (`true` then the value) is the one that already
// covers "a legitimate answer that would otherwise misread as failure", and
// contains needs exactly that same shape for the same reason
// PushOptionalHandle exists, not a broader "any out-parameter" rule that
// would also, wrongly, wrap a number or string that happens to be zero or
// empty.
// ---------------------------------------------------------------------------

int StorageContains(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    const char* key = nullptr;
    if (!CheckArgString(state, 1, &key)) return 2;
    const StackMark mark = MarkStack(state);
    uint32_t out_contains = 0u;
    const WotbModV3Result result =
        ctx->api->contains(ctx->mod, key, &out_contains);
    lua_pushboolean(state, 1);
    lua_pushboolean(state, out_contains != 0u);
    return PushResultWith(state, result, mark, "storage.contains");
}

// ---------------------------------------------------------------------------
// get_bytes, set_bytes, transaction_set_bytes: raw byte buffers, through
// FetchByteBuffer/PushByteBuffer in lua_convert.h - not FetchSizedString,
// whose BoundedLength truncation at an embedded '\0' would be wrong for
// binary data. See lua_convert.h's rule 4 and FetchByteBuffer's own comment.
// ---------------------------------------------------------------------------

int StorageGetBytes(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    const char* key = nullptr;
    if (!CheckArgString(state, 1, &key)) return 2;
    const WotbModV3StorageApiV1* api = ctx->api;
    const WotbModV3Handle mod = ctx->mod;
    return PushByteBuffer(
        state,
        [api, mod, key](char* buffer, uint32_t* inout_size) {
            WotbModV3Buffer buf = {};
            WOTBMOD_V3_INIT_STRUCT(buf, WOTBMOD_V3_ABI_VERSION);
            buf.data = buffer;
            buf.capacity = *inout_size;
            buf.size = 0u;
            const WotbModV3Result result = api->get_bytes(mod, key, &buf);
            *inout_size = buf.size;
            return result;
        },
        "storage.get_bytes");
}

int StorageSetBytes(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    const char* key = nullptr;
    if (!CheckArgString(state, 1, &key)) return 2;
    const char* data = nullptr;
    size_t length = 0u;
    if (!CheckArgBytes(state, 2, &data, &length)) return 2;
    WotbModV3ConstBuffer value = {};
    WOTBMOD_V3_INIT_STRUCT(value, WOTBMOD_V3_ABI_VERSION);
    value.data = data;
    value.size = static_cast<uint32_t>(length);
    return PushResult(state, ctx->api->set_bytes(ctx->mod, key, &value),
                      "storage.set_bytes");
}

// ---------------------------------------------------------------------------
// begin_transaction: a token, boxed with PushToken (not PushHandle -
// WotbModV3Token has no WOTBMOD_V3_INVALID_HANDLE-style sentinel; see
// PushToken's header comment) under kHandleStorageTransaction. commit,
// rollback, transaction_set_json, transaction_set_bytes, transaction_erase
// all take that same boxed token back via CheckArgTransaction.
// ---------------------------------------------------------------------------

int StorageBeginTransaction(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    WotbModV3Token token = 0u;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->begin_transaction(ctx->mod, &token);
    // Onto the ledger before the token is ever handed to the script, so that
    // an open transaction is revocable from the moment it exists. A refused
    // record means this script is being destroyed (see lua_ownership.h): the
    // transaction is rolled back here and now rather than handed to a script
    // whose teardown pass has already gone by, because a transaction nothing
    // will ever close holds whatever the client holds for one - a file lock, a
    // partial write - for the life of the session.
    if (result == WOTBMOD_V3_OK &&
        !ctx->script->Ownership().RecordTransaction(token)) {
        ctx->api->rollback(ctx->mod, token);
        lua_pushnil(state);
        lua_pushliteral(state,
                        "storage.begin_transaction: this script is being "
                        "unloaded");
        return 2;
    }
    // Required, not optional: a successful begin_transaction always hands
    // back a live transaction, so the token itself is the truthy first
    // value (rule 2's "required" shape), unlike contains' boolean above.
    PushToken(state, token, kHandleStorageTransaction);
    return PushResultWith(state, result, mark, "storage.begin_transaction");
}

int StorageTransactionSetJson(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    WotbModV3Token token = 0u;
    if (!CheckArgTransaction(state, 1, &token)) return 2;
    const char* key = nullptr;
    if (!CheckArgString(state, 2, &key)) return 2;
    const char* json = nullptr;
    if (!CheckArgString(state, 3, &json)) return 2;
    return PushResult(
        state, ctx->api->transaction_set_json(ctx->mod, token, key, json),
        "storage.transaction_set_json");
}

int StorageTransactionSetBytes(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    WotbModV3Token token = 0u;
    if (!CheckArgTransaction(state, 1, &token)) return 2;
    const char* key = nullptr;
    if (!CheckArgString(state, 2, &key)) return 2;
    const char* data = nullptr;
    size_t length = 0u;
    if (!CheckArgBytes(state, 3, &data, &length)) return 2;
    WotbModV3ConstBuffer value = {};
    WOTBMOD_V3_INIT_STRUCT(value, WOTBMOD_V3_ABI_VERSION);
    value.data = data;
    value.size = static_cast<uint32_t>(length);
    return PushResult(
        state, ctx->api->transaction_set_bytes(ctx->mod, token, key, &value),
        "storage.transaction_set_bytes");
}

int StorageTransactionErase(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    WotbModV3Token token = 0u;
    if (!CheckArgTransaction(state, 1, &token)) return 2;
    const char* key = nullptr;
    if (!CheckArgString(state, 2, &key)) return 2;
    return PushResult(state, ctx->api->transaction_erase(ctx->mod, token, key),
                      "storage.transaction_erase");
}

// commit and rollback are the two terminal calls of a transaction, and both
// forget it - but only when the client says it really is finished. A refused
// commit leaves the transaction open at the client, and a host that had
// already forgotten it would leave it open forever; keeping it means the
// script can try again, and that RevokeAll rolls it back if the script never
// does. Forgetting on failure is the mistake to avoid here, not forgetting
// twice: a second Forget* of a token that is already gone does nothing.
int StorageCommit(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    WotbModV3Token token = 0u;
    if (!CheckArgTransaction(state, 1, &token)) return 2;
    const WotbModV3Result result = ctx->api->commit(ctx->mod, token);
    if (result == WOTBMOD_V3_OK) {
        ctx->script->Ownership().ForgetTransaction(token);
    }
    return PushResult(state, result, "storage.commit");
}

int StorageRollback(lua_State* state) noexcept {
    StorageContext* ctx = Context(state);
    WotbModV3Token token = 0u;
    if (!CheckArgTransaction(state, 1, &token)) return 2;
    const WotbModV3Result result = ctx->api->rollback(ctx->mod, token);
    if (result == WOTBMOD_V3_OK) {
        ctx->script->Ownership().ForgetTransaction(token);
    }
    return PushResult(state, result, "storage.rollback");
}

const luaL_Reg kStorageFuncs[] = {
    {"get_json", &StorageGetJson},
    {"set_json", &StorageSetJson},
    {"get_bytes", &StorageGetBytes},
    {"set_bytes", &StorageSetBytes},
    {"erase", &StorageErase},
    {"contains", &StorageContains},
    {"flush", &StorageFlush},
    {"begin_transaction", &StorageBeginTransaction},
    {"transaction_set_json", &StorageTransactionSetJson},
    {"transaction_set_bytes", &StorageTransactionSetBytes},
    {"transaction_erase", &StorageTransactionErase},
    {"commit", &StorageCommit},
    {"rollback", &StorageRollback},
    {"get_path", &StorageGetPath},
    {nullptr, nullptr},
};

}  // namespace

void RegisterStorage(lua_State* state, const WotbModV3StorageApiV1* api,
                     WotbModV3Handle mod, LuaScript* script) {
    if (!state || !api || !script) return;

    PushWotbTable(state);                              // [wotb]
    lua_newtable(state);                               // [wotb, storage]

    StorageContext* ctx = static_cast<StorageContext*>(
        lua_newuserdatauv(state, sizeof(StorageContext), 0));
    ctx->api = api;
    ctx->mod = mod;
    ctx->script = script;                               // [wotb, storage, ctx]

    // SetFuncsGuarded registers each entry of kStorageFuncs into the table
    // just below its upvalues (storage, here), giving each closure its own
    // copy of the reference to ctx, then pops the upvalue it consumed - all
    // exactly as luaL_setfuncs, which this replaced, did. What goes into the
    // table is the shared permission guard rather than the binding itself, so
    // every slot here refuses with `nil, "permission denied: storage"` unless
    // this script holds it, before a single argument is read.
    SetFuncsGuarded(state, kStorageFuncs, 1, script,
                    kPermissionNameStorage);            // [wotb, storage]

    lua_pushinteger(state, WOTBMOD_V3_STORAGE_PATH_DATA);
    lua_setfield(state, -2, "PATH_DATA");
    lua_pushinteger(state, WOTBMOD_V3_STORAGE_PATH_CONFIG);
    lua_setfield(state, -2, "PATH_CONFIG");
    lua_pushinteger(state, WOTBMOD_V3_STORAGE_PATH_CACHE);
    lua_setfield(state, -2, "PATH_CACHE");
    lua_pushinteger(state, WOTBMOD_V3_STORAGE_PATH_TEMP);
    lua_setfield(state, -2, "PATH_TEMP");

    lua_setfield(state, -2, "storage");                 // [wotb]
    lua_pop(state, 1);                                  // []
}

}  // namespace lua
}  // namespace wotbmod
