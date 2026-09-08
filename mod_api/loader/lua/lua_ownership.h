#ifndef WOTBMOD_LUA_OWNERSHIP_H_
#define WOTBMOD_LUA_OWNERSHIP_H_

#include "../../include/wotbmod/base.h"
#include "../../include/wotbmod/events_v1.h"
#include "../../include/wotbmod/handles_v1.h"
#include "../../include/wotbmod/storage_v1.h"
#include "../../include/wotbmod/ui_v2.h"
#include "../../include/wotb_mod_dava_native.h"

#include <cstddef>
#include <mutex>
#include <vector>

namespace wotbmod {
namespace lua {

class LuaScript;

typedef WotbModV3Result(WOTBMOD_V3_CALL* LuaDavaNativeReleaseFn)(
    WotbModV3Handle owner,
    WotbModDavaNativeToken object);
typedef WotbModV3Result(WOTBMOD_V3_CALL* LuaDavaNativeReleaseAsyncFn)(
    WotbModV3Handle owner,
    WotbModDavaNativeToken object);

// The ledger of everything one script has made the client hold on its behalf,
// and the machinery that takes all of it back.
//
// The premise, stated once because everything below follows from it: the host
// must be able to revoke a script's resources *without trusting the script to
// cooperate*. A script may define on_disable and tidy up its own affairs, and
// most will; but correctness must not depend on it doing so, or on its cleanup
// being correct, or on that cleanup not itself raising halfway through. Hot
// reload is what makes this acute rather than theoretical - after fifty edits
// in a battle, a leak of one control per reload is ten panels stacked on the
// screen and ten live subscriptions firing into dead states.
//
// So: every binding that creates something records it here, every binding that
// destroys something forgets it here, and RevokeAll takes back whatever is
// left. The script is never asked.
//
// ---------------------------------------------------------------------------
// Once teardown begins, nothing new
// ---------------------------------------------------------------------------
//
// Record* returns bool rather than the void the task brief sketched, and that
// return is the whole defence against a shape that was a live use-after-free
// before this class existed.
//
// Teardown is not instantaneous: ReleaseEventSubscriptions marks a script's
// subscriptions dead and then *waits* for deliveries already in flight on
// other threads to finish. A handler running in that window is ordinary Lua
// code - it can call wotb.events.subscribe, wotb.ui.control_create,
// wotb.storage.begin_transaction. Anything it creates then would be created
// for a script that is already being destroyed, and the teardown pass that
// would have collected it has already gone by. In the events case this was a
// real defect found in review: the client would be left holding a callback
// into a freed LuaScript, in a DLL injected into a player's game.
//
// Patching that one instance would leave the shape intact for the next
// interface. Instead RevokeAll closes the ledger before it revokes anything,
// and a closed ledger refuses every Record*. The binding's contract is then
// uniform and mechanical: create, then record; if the ledger refuses, take the
// thing straight back and answer the script `nil, message`. The record is
// taken *after* the client call rather than a "is it closed yet" check before
// it, because only the record itself is atomic against a close happening in
// between - a check before the call is advisory, and would still need the
// undo path anyway.
//
// A closed ledger never reopens. Reload creates a new LuaScript, which brings
// its own registry; there is deliberately no way to revive this one.
//
// ---------------------------------------------------------------------------
// Locking
// ---------------------------------------------------------------------------
//
// This lock is a leaf: nothing is called while it is held - not the ABI, not
// Lua, not another lock in this host. Record*/Forget* are called from bindings
// that already hold the script lock; RevokeAll is called from ~LuaScript
// before it takes the script lock, which is what lets it wait for a delivery
// that is blocked on that same lock. Because the ledger lock is never held
// across anything else, it can close no cycle with either.
class OwnershipRegistry {
  public:
    explicit OwnershipRegistry(LuaScript* script) noexcept : script_(script) {}

    OwnershipRegistry(const OwnershipRegistry&) = delete;
    OwnershipRegistry& operator=(const OwnershipRegistry&) = delete;

    // The interfaces RevokeAll will revoke through, and the mod handle to pass
    // with every call. Any of them may be null - the client did not offer that
    // interface - in which case nothing of that kind can have been recorded
    // either, since the Register* function for it never ran.
    //
    // Called once, from RegisterAll, before any binding is installed and so
    // before any script code can run. Not per-record: an interface pointer and
    // a mod handle are the same for every resource of a kind and outlive every
    // script, so storing them 50 times per reload would buy nothing.
    void Bind(const WotbModV3EventsApiV1* events, const WotbModV3UiApiV2* ui,
              const WotbModV3StorageApiV1* storage,
              const WotbModV3HandlesApiV1* handles,
              LuaDavaNativeReleaseFn dava_native_release,
              LuaDavaNativeReleaseAsyncFn dava_native_release_async,
              WotbModV3Handle mod) noexcept;

    // Each returns false when the ledger is closed (teardown has begun) or
    // when recording itself ran out of memory. Either way the caller must
    // undo what it just created: a resource this registry does not know about
    // is a resource nothing will ever take back.
    //
    // [[nodiscard]], and that is the difference between a rule and a
    // convention. `Ownership().RecordControl(h);` with the answer dropped is
    // the Task 6 defect returning by a new route - a resource created for a
    // script that is being destroyed, with nothing left that will collect it -
    // and it would otherwise compile clean. Under /W4 /WX it is now C4834 and
    // the build fails, so the create-record-undo shape is enforced by the
    // compiler rather than remembered by whoever writes the next binding.
    //
    // noexcept, and that is load bearing rather than decorative: every caller
    // is a lua_CFunction, and a std::bad_alloc crossing one of those is a
    // terminated client. The vector growth inside is caught here.
    [[nodiscard]] bool RecordSubscription(WotbModV3EventToken token) noexcept;
    [[nodiscard]] bool RecordControl(WotbModV3Handle control) noexcept;
    [[nodiscard]] bool RecordTransaction(WotbModV3Token transaction) noexcept;
    [[nodiscard]] bool RecordHandle(WotbModV3Handle handle) noexcept;
    [[nodiscard]] bool RecordDavaNative(
        WotbModDavaNativeToken token) noexcept;

    // The counterparts, called when a script hands a resource back itself -
    // events.unsubscribe, ui.control_destroy, storage.commit/rollback.
    //
    // One rule, the same for all three kinds: forget only when the client
    // answered WOTBMOD_V3_OK. Two things follow from it, and both are wanted.
    //
    // A release the client accepted is never asked for twice - a token the ABI
    // makes no promise about reusing must not be released again, or the second
    // release could land on whatever now holds that number.
    //
    // A release the client *refused* stays on the ledger, so RevokeAll retries
    // it. Without that, a mod that lost a permission mid-session would leave
    // the client holding a control, a transaction or a subscription that
    // nothing would ever take back - permanently, because the host's own
    // record of it is already gone by then. The residual risk is a client that
    // released the thing and reported a failure anyway; the ABI's convention
    // is that a call returning a failure did not do the thing, and one rule
    // across three kinds is worth more than three separately-argued ones.
    //
    // Forgetting something that was never recorded is not an error - see
    // below - so no caller needs a "did we create this" test of its own.
    //
    // Forgetting something that was never recorded is not an error. It is what
    // a script destroying a handle it got from somewhere other than a create
    // slot looks like (control_find_by_id hands back controls this script
    // never made and does not own).
    void ForgetSubscription(WotbModV3EventToken token) noexcept;
    void ForgetControl(WotbModV3Handle control) noexcept;
    void ForgetTransaction(WotbModV3Token transaction) noexcept;
    void ForgetHandle(WotbModV3Handle handle) noexcept;
    void ForgetDavaNative(WotbModDavaNativeToken token) noexcept;

    // Closes the ledger, then takes everything back: subscriptions, then
    // controls, then transactions.
    //
    // The order is not cosmetic. Subscriptions go first so that a callback
    // cannot fire against a control that is already gone - and "first" here
    // means fully first: the subscription step does not return until the
    // client has been told to stop delivering *and* every delivery already
    // inside this script's state has finished. Only then can a control be
    // destroyed with nobody left who could still reference it.
    //
    // That wait is *the* guarantee, and it is worth saying which mechanism the
    // guarantee rests on, because there are two and they are not peers.
    //
    // ReleaseEventSubscriptions waits explicitly on an in-flight count. It
    // does so unconditionally, whatever the loop after it happens to contain,
    // and it is the property this class's contract is written against.
    //
    // The loop after it also blocks, incidentally: it takes LuaScript::Entry
    // to release each registry reference, and a parked delivery holds that
    // same lock. Remove the explicit wait today and nothing in the suite
    // fails, which is worth knowing before anyone deletes either one - but
    // that second block is a *side effect* of a requirement, not a second
    // guarantee. It holds only while every subscription has a registry
    // reference to release and while that release stays under the script
    // lock, and neither is a stated contract; a refactor that unrefs outside
    // the lock, or a subscription kind that has nothing to unref, removes it
    // without touching anything the safety property is written down against.
    // So it must not be counted as defence in depth, and the wait must not be
    // deleted on the strength of it. Keep both; name only the wait.
    //
    // Subscriptions are the one kind this class does not revoke by itself. It
    // delegates to the events binding through ReleaseScriptBindings, because
    // revoking a subscription is more than releasing a token: there is a Lua
    // registry reference to release and an in-flight delivery to wait for,
    // both of which belong to the file that created them. What is left here
    // afterwards - normally nothing - is unsubscribed as a backstop, which is
    // safe precisely because that file forgets each token from this ledger at
    // the moment it unsubscribes it.
    //
    // Idempotent, and callable from a destructor: noexcept, allocates nothing
    // (the lists are swapped out, not copied), and a second call finds an
    // empty ledger.
    //
    // Must be called with no script lock held, and before lua_close.
    void RevokeAll() noexcept;

    // How many resources this script is still holding, across all three
    // kinds. Exists so that "the ledger agrees with the client" is a claim a
    // test can check from the inside as well as from the mock's side - two
    // counters that must both reach zero, rather than one.
    size_t Count() const noexcept;

  private:
    LuaScript* script_ = nullptr;

    mutable std::mutex lock_;
    bool closed_ = false;

    const WotbModV3EventsApiV1* events_ = nullptr;
    const WotbModV3UiApiV2* ui_ = nullptr;
    const WotbModV3StorageApiV1* storage_ = nullptr;
    const WotbModV3HandlesApiV1* handles_ = nullptr;
    LuaDavaNativeReleaseFn dava_native_release_ = nullptr;
    LuaDavaNativeReleaseAsyncFn dava_native_release_async_ = nullptr;
    WotbModV3Handle mod_ = WOTBMOD_V3_INVALID_HANDLE;

    std::vector<WotbModV3EventToken> subscriptions_;
    std::vector<WotbModV3Handle> controls_;
    std::vector<WotbModV3Token> transactions_;
    // One entry per owned reference, not per numeric handle. A successful
    // retain records a duplicate and teardown therefore releases it twice.
    std::vector<WotbModV3Handle> handles_owned_;
    std::vector<WotbModDavaNativeToken> dava_native_owned_;
};

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_OWNERSHIP_H_
