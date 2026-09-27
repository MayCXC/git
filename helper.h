#ifndef HELPER_H
#define HELPER_H

#include "strbuf.h"

struct child_process;

/*
 * A local helper is a "git-local-<name>" program that stores a repository's
 * references or objects for Git, the way a "git-remote-<name>" program
 * reaches a remote repository for it. Git runs the helper as a separate
 * process and speaks a line-based protocol to it on its standard input and
 * output.
 *
 * The process is started lazily, when a command is first sent to it, and
 * negotiates the capabilities of the helper then.
 */

/*
 * A capability a helper may advertise, and the bit recording it in
 * `helper_process.capabilities` when the helper does. A required one is a
 * capability the caller cannot do without, so that starting a helper that
 * does not advertise it dies. Tables of them end with an entry whose name is
 * NULL.
 */
struct helper_capability {
	const char *name;
	unsigned int flag;
	unsigned int required : 1;
};

struct helper_process {
	struct child_process *child;
	FILE *out;
	char *name;
	char *gitdir;
	const struct helper_capability *known_capabilities;
	/*
	 * The flags of the known capabilities the running helper advertised;
	 * read them through helper_process_capable().
	 */
	unsigned int capabilities;
};

/*
 * Set up `hp` to run "git-local-<name>" for `gitdir`, recognizing the
 * capabilities in `known_capabilities`. Nothing is started yet.
 */
void helper_process_init(struct helper_process *hp, const char *name,
			 const char *gitdir,
			 const struct helper_capability *known_capabilities);

/*
 * Start the helper, unless it is running already, and return `hp`. Dies if
 * the helper cannot be started.
 */
struct helper_process *helper_process_ensure(struct helper_process *hp);

/*
 * Whether the helper advertised the capability recorded as `flag`, starting
 * the helper if need be.
 */
bool helper_process_capable(struct helper_process *hp, unsigned int flag);

/* Send a command to the helper, starting it if need be. */
void helper_process_send(struct helper_process *hp, const char *fmt, ...);

/* Send raw bytes to the helper, such as the contents following a command. */
void helper_process_write(struct helper_process *hp, const void *buf,
			  size_t len);

/*
 * Read a line of the reply of the helper into `line`, without its line
 * ending. Returns EOF when the helper has hung up.
 */
int helper_process_readline(struct helper_process *hp, struct strbuf *line);

/*
 * Stop the running helper, if any, but keep `hp` set up so that the next
 * command starts it anew. This recovers from a reply that could not be read
 * in full, which leaves the rest of it in the way of the next reply.
 */
void helper_process_disconnect(struct helper_process *hp);

/* Stop the running helper, if any, and release `hp`. */
void helper_process_release(struct helper_process *hp);

#endif
