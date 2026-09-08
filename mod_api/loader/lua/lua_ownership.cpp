#include "lua_ownership.h"

#include "lua_bindings.h"

namespace wotbmod {
namespace lua {
namespace {

// The one shape all three Record* slots share. Templated over the token type
// rather than written three times, because the only thing that differs between
// them is which vector the value lands in - and three copies of a
// closed-check-then-push_back is three places for the closed check to be
// forgotten when a fourth resource kind arrives.
//
// `closed` is taken by reference, not by value: read by value it would be read
// before the guard below is taken, which is exactly the unlocked read of the
// flag that RevokeAll sets from another thread.
//
// The try/catch is the point of the bool return: push_back is the only thing
// in here that can fail, it fails by throwing, and the callers are all
// lua_CFunctions that a throw would terminate the client from. Rule 5.
template <typename T>
bool RecordInto(std::mutex& lock, const bool& closed, std::vector<T>& into,
                T value) noexcept {
    std::lock_guard<std::mutex> guard(lock);
    if (closed) return false;
    try {
        into.push_back(value);
    } catch (...) {
        return false;
    }
    return true;
}

template <typename T>
void ForgetFrom(std::mutex& lock, std::vector<T>& from, T value) noexcept {
    std::lock_guard<std::mutex> guard(lock);
    for (auto it = from.begin(); it != from.end(); ++it) {
        if (*it == value) {
            from.erase(it);
            return;
        }
    }
}

}  // namespace

void OwnershipRegistry::Bind(const WotbModV3EventsApiV1* events,
                             const WotbModV3UiApiV2* ui,
                             const WotbModV3StorageApiV1* storage,
                             const WotbModV3HandlesApiV1* handles,
                             LuaDavaNativeReleaseFn dava_native_release,
                             LuaDavaNativeReleaseAsyncFn
                                 dava_native_release_async,
                             WotbModV3Handle mod) noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    events_ = events;
    ui_ = ui;
    storage_ = storage;
    handles_ = handles;
    dava_native_release_ = dava_native_release;
    dava_native_release_async_ = dava_native_release_async;
    mod_ = mod;
}

bool OwnershipRegistry::RecordSubscription(WotbModV3EventToken token) noexcept {
    return RecordInto(lock_, closed_, subscriptions_, token);
}

bool OwnershipRegistry::RecordControl(WotbModV3Handle control) noexcept {
    return RecordInto(lock_, closed_, controls_, control);
}

bool OwnershipRegistry::RecordTransaction(WotbModV3Token transaction) noexcept {
    return RecordInto(lock_, closed_, transactions_, transaction);
}

bool OwnershipRegistry::RecordHandle(WotbModV3Handle handle) noexcept {
    return RecordInto(lock_, closed_, handles_owned_, handle);
}

bool OwnershipRegistry::RecordDavaNative(
    WotbModDavaNativeToken token) noexcept {
    return RecordInto(lock_, closed_, dava_native_owned_, token);
}

void OwnershipRegistry::ForgetSubscription(WotbModV3EventToken token) noexcept {
    ForgetFrom(lock_, subscriptions_, token);
}

void OwnershipRegistry::ForgetControl(WotbModV3Handle control) noexcept {
    ForgetFrom(lock_, controls_, control);
}

void OwnershipRegistry::ForgetTransaction(WotbModV3Token transaction) noexcept {
    ForgetFrom(lock_, transactions_, transaction);
}

void OwnershipRegistry::ForgetHandle(WotbModV3Handle handle) noexcept {
    ForgetFrom(lock_, handles_owned_, handle);
}

void OwnershipRegistry::ForgetDavaNative(
    WotbModDavaNativeToken token) noexcept {
    ForgetFrom(lock_, dava_native_owned_, token);
}

size_t OwnershipRegistry::Count() const noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    return subscriptions_.size() + controls_.size() + transactions_.size() +
           handles_owned_.size() + dava_native_owned_.size();
}

void OwnershipRegistry::RevokeAll() noexcept {
    // Step one: shut the door. Everything after this point is a script being
    // destroyed, and from here on nothing it does can add to the list of
    // things that have to be destroyed with it. Done before the subscription
    // step specifically, because that step is the one that waits - it is the
    // window in which a handler on another thread is still running Lua code.
    {
        std::lock_guard<std::mutex> guard(lock_);
        closed_ = true;
    }

    // Step two: subscriptions, and nothing else until they are gone. See the
    // header for why this delegates rather than unsubscribing from the list
    // below. Interface-agnostic on purpose: this file knows that a script's
    // bindings have to be released before anything else is destroyed, not
    // which bindings exist.
    ReleaseScriptBindings(script_);

    // Step three: take the lists. Swapped rather than copied - a copy
    // allocates, and this runs from a destructor with no way to report a
    // failure. The lock is released before a single ABI call is made.
    std::vector<WotbModV3EventToken> subscriptions;
    std::vector<WotbModV3Handle> controls;
    std::vector<WotbModV3Token> transactions;
    std::vector<WotbModV3Handle> handles_owned;
    std::vector<WotbModDavaNativeToken> dava_native_owned;
    {
        std::lock_guard<std::mutex> guard(lock_);
        subscriptions.swap(subscriptions_);
        controls.swap(controls_);
        transactions.swap(transactions_);
        handles_owned.swap(handles_owned_);
        dava_native_owned.swap(dava_native_owned_);
    }

    // Anything the events release did not account for. Usually empty - it
    // forgets each token as it unsubscribes it - but not dead code, and it
    // matters that it is not: a subscription the script itself tried to
    // unsubscribe and the *client refused* is gone from the events table (this
    // host retired its record) while the client still holds it, so the release
    // above cannot see it and this is the only thing left that will retry.
    // That path is exercised by the suite, so a wrong interface pointer or mod
    // handle here fails a test rather than sitting unrun.
    if (events_ && events_->unsubscribe) {
        for (const WotbModV3EventToken token : subscriptions) {
            events_->unsubscribe(mod_, token);
        }
    }

    // Controls, newest first. A tree is built parent-then-child, so unwinding
    // it in reverse destroys children before their parents; a client that
    // destroys a subtree along with its root then finds every remaining handle
    // already gone and refuses it, which is a refusal this loop ignores by
    // design rather than a destroy that silently did nothing.
    if (ui_ && ui_->control_destroy) {
        for (size_t i = controls.size(); i > 0u; --i) {
            ui_->control_destroy(mod_,
                                 static_cast<WotbModV3UiHandle>(controls[i - 1u]));
        }
    }

    // Transactions roll back, they do not commit. A script that was destroyed
    // mid-transaction never said its writes were finished, and half of an
    // intended change written to a player's storage is worse than none of it.
    if (storage_ && storage_->rollback) {
        for (size_t i = transactions.size(); i > 0u; --i) {
            storage_->rollback(mod_, transactions[i - 1u]);
        }
    }

    // Generated interfaces return ordinary ref-counted V3 handles. Every
    // successful output or retain contributes one ledger entry, so duplicate
    // values here are intentional and each reference is released newest-first.
    if (handles_ && handles_->release) {
        for (size_t i = handles_owned.size(); i > 0u; --i) {
            handles_->release(mod_, handles_owned[i - 1u]);
        }
    }

    // Private DAVA objects are host tokens, never engine pointers. Releasing
    // newest-first preserves dependency order while the registry verifies the
    // owner and dispatches the exact provider destructor.
    if (dava_native_release_async_ || dava_native_release_) {
        for (size_t i = dava_native_owned.size(); i > 0u; --i) {
            if (dava_native_release_async_) {
                dava_native_release_async_(
                    mod_, dava_native_owned[i - 1u]);
            } else {
                dava_native_release_(mod_, dava_native_owned[i - 1u]);
            }
        }
    }
}

}  // namespace lua
}  // namespace wotbmod
