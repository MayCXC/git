#ifndef HELPER_H
#define HELPER_H

#include "strbuf.h"

struct child_process;

/*
 * Shared helper process for delegating both object and ref storage
 * to an external process. The helper binary is discovered as
 * "git-local-<name>" in PATH and communicates via a line-based
 * protocol on stdin/stdout.
 *
 * A single helper process handles both ODB and ref operations,
 * matching how remote helpers handle both refs (list) and objects
 * (fetch/push) in one process. Commands use a flat namespace with
 * distinct names per operation.
 *
 * Semantically a local helper sits between the in-tree filesystem backends
 * (files/reftable/loose) and remote helpers, and closer to the latter. It keeps
 * the fine granularity of the filesystem backends (random-access reads by oid or
 * refname, per-ref transactions) because it is local and cheap to call, but it
 * shares the defining property of a remote helper: an IPC boundary the git side
 * cannot make race-free assertions across. So we look to the filesystem backends
 * for WHAT operations exist, and to remote helpers for HOW responsibility splits
 * across the boundary (see "Helper obligations" below).
 *
 * ODB commands:
 *   info <oid>             -> <type> <size> [<base-oid> <delta-len>] | missing
 *                             the resolved type and size; when the object is
 *                             stored as a git-format delta, also its base oid
 *                             and raw delta length, so git can reuse the stored
 *                             delta on a send pack (bytes fetched via "get-delta").
 *                             A caller wanting only type/size reads the first two.
 *   get <oid>              -> <type> <size> + data | missing
 *   put <oid> <type> <size> + data -> <oid>
 *   have <oid>             -> true | false
 *   list-objects [--promisor-only] [--skip-kept] [<hexprefix>]
 *                          -> <oid> <type> <size> per line, blank end;
 *                             a hex prefix scopes the listing server-side
 *   put-stream <type> <size> + data + <oid> -> <oid> (git owns the hash; helper stores)
 *   put-raw <oid> <type> <usize> <base|0> <clen> [<replace>] + data -> <oid>
 *                             store an object in git's native pack form: <data>
 *                             is the compressed entry bytes (<clen> long),
 *                             persisted verbatim; <usize> is the uncompressed
 *                             length. base "0" stores a whole object (compressed
 *                             content); a base oid stores a compressed git-format
 *                             delta against it. git already prepared the bytes
 *                             (index-pack's incoming entry, pack-objects' on
 *                             repack, a quarantine pack on migrate), so there is
 *                             no inflate or recompress. Gated on the "put-raw"
 *                             capability; absent -> git sends a resolved "put".
 *                             A trailing <replace> (repack) overwrites in place.
 *   get-delta <oid>        -> delta <base-oid> <raw-len> <clen> + data | plain
 *                             return a stored COMPRESSED git-format delta with
 *                             its base, the uncompressed delta length and the
 *                             compressed byte length, so git reuses the delta
 *                             verbatim on a send pack (no inflate, no recompress);
 *                             "plain" means not deltified (use "get"). Gated on
 *                             the "get-delta" capability.
 *   odb-transaction-begin  -> ok | error <msg>
 *   odb-transaction-commit -> ok | error <msg>
 *   refresh                -> (no response) reload any cached read view so
 *                             subsequent reads observe every write committed by
 *                             any process (see "Helper obligations"). Sent
 *                             fire-and-forget: the backend does not read a reply.
 *   optimize               -> ok | error <msg>  optimize the helper's object
 *                             storage (e.g. compacting its backing store). Reached for an
 *                             explicit "git gc"/"git repack"; optional, gated on
 *                             the "optimize" capability.
 *   verify                 -> ok | error <msg>  check the integrity of the
 *                             helper's own backing store (e.g. an
 *                             integrity check). Reached from "git fsck";
 *                             optional, gated on the "verify" capability.
 *   prune <dry_run> <expire> -> <oid>... ok | error <msg>  receive the
 *                             unreachable object ids git found (one per line,
 *                             blank-terminated) and delete those stored at or
 *                             before <expire> (an absolute unix time): the
 *                             helper's half of "git prune". Reply with the oids
 *                             actually pruned (one per line, terminated by
 *                             "ok") so git can report exactly what was removed;
 *                             with <dry_run> set (1), report what would be
 *                             pruned without deleting (git prune --dry-run).
 *                             git owns reachability; the helper applies its own
 *                             stored time. Gated on the "prune" capability.
 *
 * Reachability-bitmap, pack-reuse and commit-graph commands. All optional, each
 * gated on its own capability; they back git's bitmap, pack-reuse, per-commit
 * bitmap and commit-graph seams so the helper serves them from its store with no
 * on-disk .bitmap/.rev/.midx or commit-graph file. "bitmap" and "reuse-pack" are
 * advertised independently, so a bitmap-only helper omits "reuse-pack" and git
 * falls back to emitting those objects per-object.
 *   store-bitmap <bitmap_len> + <bitmap bytes>, then the .idx (index->oid, one oid
 *                 per line) blank-terminated, then the .rev (bit->index, one index
 *                 per line) blank-terminated -> ok | error <msg>
 *                             store git's EWAH type-.bitmap blob plus the two
 *                             orderings; the helper derives each object's bit
 *                             position and clusters its reachable content (the
 *                             relational form of a .bitmap + .idx + .rev). Gated
 *                             on "bitmap".
 *   get-bitmap             -> bitmap <bitmap_len> <n_objects> + data | missing
 *                             the stored EWAH blob; <n_objects> sizes git's source
 *                             bitmap. The orderings are not sent (git resolves them
 *                             on demand via pos-of-oid / result-oids).
 *   clear-bitmap           -> ok | error <msg>  drop the stored bitmap + orderings.
 *   pos-of-oid <oid>       -> pos <bit> | none  one want/have's bit position in
 *                             the stored bitmap.
 *   result-oids + <bit>... (blank end) -> <oid>... (blank end)  resolve a staged
 *                             bit set to oids in bit (pack_pos) order: the batched
 *                             bit->oid for a bitmap operation.
 *   reuse-pack <end-pos>   -> <type> <size> <base-oid|-> <clen> + data per object,
 *                             blank end   stream the reusable pack_pos prefix
 *                             [0, end-pos) as pack-entry records (verbatim
 *                             compressed bytes, no inflate or recompress): git's
 *                             source-backed pack reuse, the bulk analog of copying
 *                             a contiguous .pack region. Gated on "reuse-pack".
 *   store-commit-bitmaps + a record per commit "<commit-oid> <xor-base|-> <flags>
 *                 <ewah_len>" + data, blank end -> ok | error <msg>   store the
 *                             per-commit reachability bitmaps (the bitmap analog of
 *                             object deltas, base-by-oid), clearing the prior set
 *                             in one transaction. Gated on "commit-bitmap".
 *   get-commit-bitmap <commit-oid> -> commit-bitmap <xor-base|-> <flags> <len>
 *                             + data | missing   one commit's stored bitmap. Gated
 *                             on "commit-bitmap".
 *   store-commit-graph + "<commit-oid> <generation>" per line, blank end
 *                          -> ok | error <msg>   store the generation numbers git
 *                             computed writing its commit-graph (clearing the prior
 *                             set), so no commit-graph file is written; generation
 *                             is the only datum not re-derivable from the commit
 *                             objects. Gated on "graph".
 *   commit-generation <commit-oid> -> <generation> | none   serve a commit's
 *                             stored generation on demand (the lazy commit-graph
 *                             read), so generation cutoffs work with no file. Gated
 *                             on "graph".
 *
 * Ref commands:
 *   read <refname>         -> <oid> | symref <target> | missing
 *   list [<prefix>]        -> <refname> <oid|symref target>, blank end;
 *                             refs MUST be in refname (byte/strcmp) order, and
 *                             if <prefix> is given, ONLY refs under it. The
 *                             backend relies on both (it does not re-sort or
 *                             re-filter), the way the reftable/files backends
 *                             trust their sorted storage.
 *   transaction-begin      -> ok
 *   transaction-update/create/delete/create-symref -> ok
 *   transaction-prepare    -> ok | error <msg>
 *   transaction-finish     -> ok | error <msg>
 *   transaction-abort      -> ok
 *   create                 -> ok | error <msg>
 *   remove                 -> ok | error <msg>
 *
 * Reflog commands (native reflog line format):
 *   reflog-read <refname>  -> lines in git reflog format, blank end
 *     Each line: <old-oid> SP <new-oid> SP <name> SP LT <email> GT SP
 *                <ts> SP <tz> TAB <msg>
 *   reflog-append <refname> <old-oid> <new-oid> <name> <email> <ts>
 *                 <tz> <msg> -> ok | error <msg>
 *   reflog-exists <refname> -> true | false
 *   reflog-delete <refname> -> ok | error <msg>
 *   reflog-list            -> refnames with reflogs, one per line, blank end;
 *                             refnames MUST be in refname (byte/strcmp) order,
 *                             which the iterator relies on to dedup consecutive
 *                             names (it does not re-sort), as for "list" above
 *   reflog-read-reverse <refname> -> entries newest-first, blank end
 *   reflog-copy <old> <new> <delete_old> -> ok | error <msg>
 *     Carry <old>'s reflog onto <new> inside the open ref transaction:
 *     delete_old=1 moves it (rename), 0 duplicates it (copy).
 *
 * Helper obligations. Like a remote helper (transport-helper.c), this helper
 * owns one side of a process boundary, and the backend relies on it to enforce
 * the storage contracts that cannot be checked race-free from the git side. We
 * keep the contracts the git side already enforces for any helper (e.g. refname
 * syntax) and push the rest across the boundary rather than have the backend
 * re-do what the in-process files/reftable backends do (re-sort, re-filter,
 * re-verify): any assertion the backend makes by reading then acting is a TOCTOU
 * once a separate process owns the data, which is as true for a local helper as
 * for a remote one.
 *   - Atomicity: everything between transaction-prepare and transaction-finish
 *     applies all-or-nothing, as a remote helper applies an atomic push.
 *   - Old-value preconditions, enforced under the helper's own lock: a
 *     "transaction-update <ref> <old> <new>" must fail unless the ref's current
 *     value is <old>; "transaction-create" sets the ref (force/upsert) and is
 *     used when the caller imposed no old value; updating or deleting a missing
 *     ref must fail. The backend forwards these but does NOT re-verify them,
 *     because the only race-free place to check is inside the helper's
 *     transaction (a backend-side read+check would be a TOCTOU), the same way
 *     the remote helper, not the pushing client, is what actually enforces a
 *     compare-and-swap push.
 *   - Directory/file conflicts: the helper must reject a transaction that would
 *     let a ref and a ref-as-directory coexist (e.g. refs/heads/foo and
 *     refs/heads/foo/bar); the backend does not run
 *     refs_verify_refnames_available() on its behalf.
 *   - Read visibility: after a "refresh" the helper must answer subsequent
 *     reads against every write committed by any process. The backend issues
 *     "refresh" on the ODB second read (mirroring how the packed store
 *     reprepares) and otherwise does not assume a read snapshot is current; it
 *     never infers "absent" from a single miss, which across the boundary may
 *     just be a stale snapshot.
 *   - "list" output is sorted and prefix-scoped as described above.
 */
struct helper_process {
	struct child_process *child;
	FILE *out;
	char *name;
	char *gitdir;

	unsigned cap_get:1,
		 cap_put:1,
		 cap_put_raw:1,
		 cap_get_delta:1,
		 /* "replace": put / put-raw honor a trailing replace argument that
		  * overwrites an object's stored representation in place (repack) rather
		  * than keep-existing. One capability covers both verbs. */
		 cap_replace:1,
		 cap_have:1,
		 cap_freshen:1,
		 cap_info:1,
		 cap_list_objects:1,
		 cap_odb_transaction:1,
		 cap_read:1,
		 cap_list:1,
		 cap_transaction:1,
		 cap_create:1,
		 cap_remove:1,
		 /* "reflog": a reflog store inherently does read/append/exists/delete/
		  * list/copy, so they negotiate as one cap (no helper supports a proper
		  * subset; the missing-cap paths are defensive, not a real mode). */
		 cap_reflog:1,
		 /* "reflog-read-reverse" stays separate: git-core has a transparent
		  * fallback (forward read, buffer, replay reversed), so a helper may
		  * losslessly omit it. */
		 cap_reflog_read_reverse:1,
		 cap_put_stream:1,
		 cap_refresh:1,
		 cap_kept:1,
		 cap_promisor:1,
		 cap_optimize:1,
		 cap_optimize_required:1,
		 cap_verify:1,
		 cap_prune:1,
		 cap_keep_pack:1,
		 cap_bitmap:1,
		 cap_commit_bitmap:1,
		 cap_graph:1,
		 cap_reuse_pack:1;
};

/*
 * Get or lazily start the helper process. The helper is spawned as
 * "git-local-<name>" with <gitdir> as its sole argument. Capabilities
 * are negotiated on first call. Returns the helper process.
 */
struct helper_process *helper_process_ensure(struct helper_process *hp);

/*
 * Send a formatted command to the helper.
 */
void helper_process_send(struct helper_process *hp, const char *fmt, ...);

/*
 * Send raw bytes to the helper (for binary data after a text header).
 */
void helper_process_write(struct helper_process *hp, const void *buf,
			  size_t len);

/*
 * Read one line from the helper. Returns EOF on end of stream.
 */
int helper_process_readline(struct helper_process *hp, struct strbuf *line);

/*
 * Disconnect the running helper process (close its pipes and reap it) but keep
 * the helper_process configured, so the next helper_process_ensure() re-spawns
 * it. Used to recover from a protocol desync that leaves the connection
 * unusable (e.g. a streamed object whose source errored mid-transfer).
 */
void helper_process_disconnect(struct helper_process *hp);

/*
 * Release the helper process and all associated resources.
 */
void helper_process_release(struct helper_process *hp);

/*
 * Send "refresh" to the helper if it supports the capability.
 * This causes the helper to checkpoint its backing store and reset
 * its read state so subsequent reads see writes from other
 * processes (e.g., subprocess helpers).
 */
void helper_process_refresh(struct helper_process *hp);

/*
 * Initialize a helper_process struct with the given name and gitdir.
 * Does not start the process; that happens lazily on first use.
 */
void helper_process_init(struct helper_process *hp, const char *name,
			 const char *gitdir);

#endif
