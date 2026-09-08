#pragma once

#include "vfs_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_VFS_VERSION_2 2u

/*
 * The largest single write this interface will accept, and the largest file it
 * will copy. 32 MB is deliberately above the archive limits a mod can already
 * reach (`max_total_unpacked_bytes` is 16 MB) so that copying a validated
 * extraction result is never the thing that fails, and deliberately far below
 * anything that would let a mod fill a disk one call at a time.
 */
#define WOTBMOD_V3_VFS_MAX_WRITE_BYTES (32u * 1024u * 1024u)

/*
 * VFS V2: the four routes that turn bytes a mod already holds into a file.
 *
 * WHY THIS EXISTS AT ALL. V1 can read every scheme and mount overlays over
 * `game://`, but it cannot create the file an overlay points AT: `mount_overlay`
 * takes a `mod://`/`data://`/`cache://` source that must already exist, and
 * `archive.extract_entry` can only write what was already inside an archive the
 * host itself opened from a physical path. A mod that downloads bytes therefore
 * had nowhere to put them, which made BOTH installing a downloaded pack and
 * composing an overlay source impossible on every client.
 *
 * SCOPE IS THE WHOLE SAFETY ARGUMENT. Every destination goes through
 * `ResolveWritableUri`, which admits `data://` and `cache://` and nothing else -
 * the mod's own two sandboxes, which `wotb.storage` already writes into. There
 * is no route here that can write to `game://`, and `copy_file` reading a
 * `game://` source grants nothing new because `vfs.open`/`vfs.read` already do.
 *
 * WHY `copy_file` IS NOT `read` + `write` IN LUA. The file this exists to copy
 * is the client's camouflage registry: 895,629 bytes. Moving it through Lua
 * costs far more than the host's 100,000-instruction budget per protected call,
 * so a mod that tried would be deleted rather than slowed. The copy happens
 * natively for the same reason the parse is sliced.
 *
 * WHY `append_file` IS A SLOT AND NOT A MODE FLAG. The composed overlay is
 * "the game's file, plus a small fragment". With append the fragment is the
 * only thing Lua ever holds; with a write-only surface the mod would have to
 * hold the whole 900 KB result to write it back, which is the same budget
 * failure one step later.
 */
typedef struct WotbModV3VfsApiV2 {
    WotbModV3VfsApiV1 v1;
    /*
     * Replaces the file at `uri` with `data`, atomically: the bytes land in a
     * sibling temporary and are renamed over the destination, so a reader never
     * observes a half-written file and a crash mid-write leaves the previous
     * contents intact. Creates parent directories. `data` may be empty, which
     * truncates.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* write_file)(
        WotbModV3Handle mod,
        const char* uri,
        const WotbModV3ConstBuffer* data);
    /*
     * Appends `data` to the file at `uri`, creating it when absent.
     *
     * NOT atomic, and it cannot be: an atomic append would mean rewriting the
     * whole file, which is the cost this slot exists to avoid. A caller that
     * needs the result to be all-or-nothing composes into a temporary URI and
     * renames it with `write_file`, or re-composes from scratch after a failure.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* append_file)(
        WotbModV3Handle mod,
        const char* uri,
        const WotbModV3ConstBuffer* data);
    /*
     * Copies the file `source_uri` resolves to onto `destination_uri`,
     * atomically, without the bytes passing through the caller.
     *
     * The SOURCE may be any readable scheme, including `game://` - this is a
     * read the caller could already perform - and resolves through the same
     * overlay chain `vfs.resolve` uses, so copying a file another mod has
     * overlaid copies the overlaid version. The DESTINATION is restricted like
     * every other write here.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* copy_file)(
        WotbModV3Handle mod,
        const char* source_uri,
        const char* destination_uri);
    /*
     * Deletes the file at `uri`. Answers `WOTBMOD_V3_E_NOT_FOUND` when there was
     * nothing there, so "make sure this is gone" is a call whose failure means
     * something, rather than one a caller has to ignore the result of.
     *
     * Refuses directories: removing a tree is not a primitive this hands out.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* remove_file)(
        WotbModV3Handle mod,
        const char* uri);
} WotbModV3VfsApiV2;

#ifdef __cplusplus
}
#endif
