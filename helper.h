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
 * ODB commands:
 *   info <oid>             -> <type> <size> | missing
 *   get <oid>              -> <type> <size> + data | missing
 *   put <oid> <type> <size> + data -> <oid>
 *   have <oid>             -> true | false
 *   list-objects           -> <oid> <type> <size> per line, blank end
 *   put-stream <type> <size> + data -> <oid> (helper computes OID)
 *   odb-transaction-begin  -> ok | error <msg>
 *   odb-transaction-commit -> ok | error <msg>
 *
 * Ref commands:
 *   read <refname>         -> <oid> | symref <target> | missing
 *   list [<prefix>]        -> <refname> <oid|symref target>, blank end
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
 *   reflog-list            -> refnames with reflogs, one per line,
 *                             blank end
 *   reflog-read-reverse <refname> -> entries newest-first, blank end
 */
struct helper_process {
	struct child_process *child;
	FILE *out;
	char *name;
	char *gitdir;

	unsigned cap_get:1,
		 cap_put:1,
		 cap_have:1,
		 cap_info:1,
		 cap_list_objects:1,
		 cap_odb_transaction:1,
		 cap_read:1,
		 cap_list:1,
		 cap_transaction:1,
		 cap_create:1,
		 cap_remove:1,
		 cap_reflog_read:1,
		 cap_reflog_append:1,
		 cap_reflog_exists:1,
		 cap_reflog_delete:1,
		 cap_reflog_list:1,
		 cap_reflog_read_reverse:1,
		 cap_put_stream:1,
		 cap_refresh:1,
		 cap_kept:1,
		 cap_promisor:1,
		 cap_connectivity_check:1;
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
 * Drain remaining lines until a blank line or EOF. Used to keep the
 * pipe synchronized after early abort from iteration callbacks.
 */
void helper_process_drain(struct helper_process *hp);

/*
 * Release the helper process and all associated resources.
 */
void helper_process_release(struct helper_process *hp);

/*
 * Send "refresh" to the helper if it supports the capability.
 * This causes the helper to checkpoint its database and reset
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
