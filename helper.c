/*
 * Shared helper process for local storage backends.
 *
 * A single "git-local-<name>" process handles both object and ref
 * operations, matching how "git-remote-<name>" handles both refs
 * and objects for remote repositories.
 *
 * The helper_process lives on struct repository and is shared by
 * the ODB and ref backends, just as remote helpers share one
 * transport for refs and objects.
 */
#include "git-compat-util.h"
#include "gettext.h"
#include "helper.h"
#include "run-command.h"
#include "sigchain.h"
#include "strvec.h"
#include "wrapper.h"

void helper_process_init(struct helper_process *hp, const char *name,
			 const char *gitdir)
{
	memset(hp, 0, sizeof(*hp));

	if (strchr(name, '/'))
		die(_("helper name '%s' must not contain '/'"), name);

	hp->name = xstrdup(name);
	hp->gitdir = xstrdup(gitdir);
}

struct helper_process *helper_process_ensure(struct helper_process *hp)
{
	struct child_process *child;
	struct strbuf line = STRBUF_INIT;
	int duped;

	if (hp->child)
		return hp;

	child = xmalloc(sizeof(*child));
	child_process_init(child);
	child->in = -1;
	child->out = -1;
	child->err = 0;
	child->silent_exec_failure = 1;
	/*
	 * The helper is long-lived (it backs the object store for the duration
	 * of the process). Closing inherited descriptors keeps it from pinning
	 * unrelated descriptors open, most importantly the upload-pack/fetch
	 * connection pipe: were it inherited, the remote would never see EOF on
	 * its stdin and the fetch would deadlock.
	 */
	child->close_fd_above_stderr = 1;
	strvec_pushf(&child->args, "git-local-%s", hp->name);
	strvec_push(&child->args, hp->gitdir);

	if (start_command(child) < 0)
		die(_("unable to start helper '%s'"), hp->name);

	hp->child = child;

	duped = dup(child->out);
	if (duped < 0)
		die_errno(_("cannot dup helper output fd"));
	hp->out = fdopen(duped, "r");
	if (!hp->out)
		die_errno(_("cannot fdopen helper output"));

	/* Negotiate capabilities */
	sigchain_push(SIGPIPE, SIG_IGN);
	if (write_in_full(child->in, "capabilities\n", 13) < 0)
		die_errno(_("unable to write to local helper '%s'"), hp->name);
	sigchain_pop(SIGPIPE);

	while (strbuf_getline_lf(&line, hp->out) != EOF) {
		if (!line.len)
			break;
		if (!strcmp(line.buf, "get"))
			hp->cap_get = 1;
		else if (!strcmp(line.buf, "put"))
			hp->cap_put = 1;
		else if (!strcmp(line.buf, "put-raw"))
			hp->cap_put_raw = 1;
		else if (!strcmp(line.buf, "get-delta"))
			hp->cap_get_delta = 1;
		else if (!strcmp(line.buf, "replace"))
			hp->cap_replace = 1;
		else if (!strcmp(line.buf, "have"))
			hp->cap_have = 1;
		else if (!strcmp(line.buf, "freshen"))
			hp->cap_freshen = 1;
		else if (!strcmp(line.buf, "info"))
			hp->cap_info = 1;
		else if (!strcmp(line.buf, "list-objects"))
			hp->cap_list_objects = 1;
		else if (!strcmp(line.buf, "odb-transaction"))
			hp->cap_odb_transaction = 1;
		else if (!strcmp(line.buf, "read"))
			hp->cap_read = 1;
		else if (!strcmp(line.buf, "list"))
			hp->cap_list = 1;
		else if (!strcmp(line.buf, "transaction"))
			hp->cap_transaction = 1;
		else if (!strcmp(line.buf, "create"))
			hp->cap_create = 1;
		else if (!strcmp(line.buf, "remove"))
			hp->cap_remove = 1;
		else if (!strcmp(line.buf, "reflog"))
			hp->cap_reflog = 1;
		else if (!strcmp(line.buf, "reflog-read-reverse"))
			hp->cap_reflog_read_reverse = 1;
		else if (!strcmp(line.buf, "put-stream"))
			hp->cap_put_stream = 1;
		else if (!strcmp(line.buf, "refresh"))
			hp->cap_refresh = 1;
		else if (!strcmp(line.buf, "kept"))
			hp->cap_kept = 1;
		else if (!strcmp(line.buf, "promisor"))
			hp->cap_promisor = 1;
		else if (!strcmp(line.buf, "optimize"))
			hp->cap_optimize = 1;
		else if (!strcmp(line.buf, "optimize-required"))
			hp->cap_optimize_required = 1;
		else if (!strcmp(line.buf, "verify"))
			hp->cap_verify = 1;
		else if (!strcmp(line.buf, "prune"))
			hp->cap_prune = 1;
		else if (!strcmp(line.buf, "keep-pack"))
			hp->cap_keep_pack = 1;
		else if (!strcmp(line.buf, "bitmap"))
			hp->cap_bitmap = 1;
		else if (!strcmp(line.buf, "commit-bitmap"))
			hp->cap_commit_bitmap = 1;
		else if (!strcmp(line.buf, "graph"))
			hp->cap_graph = 1;
		else if (!strcmp(line.buf, "reuse-pack"))
			hp->cap_reuse_pack = 1;
	}
	strbuf_release(&line);

	return hp;
}

void helper_process_send(struct helper_process *hp, const char *fmt, ...)
{
	struct strbuf buf = STRBUF_INIT;
	va_list ap;
	int ret;

	helper_process_ensure(hp);

	va_start(ap, fmt);
	strbuf_vaddf(&buf, fmt, ap);
	va_end(ap);

	sigchain_push(SIGPIPE, SIG_IGN);
	ret = write_in_full(hp->child->in, buf.buf, buf.len);
	sigchain_pop(SIGPIPE);

	strbuf_release(&buf);

	/*
	 * A failed write means the helper has died or closed its input. Fail
	 * loudly, like transport-helper.c does for remote helpers, rather than
	 * letting the next read return EOF and be mistaken for "object/ref not
	 * found".
	 */
	if (ret < 0)
		die_errno(_("unable to write to local helper '%s'"), hp->name);
}

void helper_process_write(struct helper_process *hp, const void *buf,
			  size_t len)
{
	int ret;

	helper_process_ensure(hp);

	sigchain_push(SIGPIPE, SIG_IGN);
	ret = write_in_full(hp->child->in, buf, len);
	sigchain_pop(SIGPIPE);

	if (ret < 0)
		die_errno(_("unable to write to local helper '%s'"), hp->name);
}

int helper_process_readline(struct helper_process *hp, struct strbuf *line)
{
	helper_process_ensure(hp);
	strbuf_reset(line);
	return strbuf_getline_lf(line, hp->out);
}

void helper_process_refresh(struct helper_process *hp)
{
	/*
	 * Refresh only resets a running helper's read state. If the helper was
	 * never spawned there is nothing to refresh, and spawning it here would
	 * defeat the lazy model: odb_reprepare() fires on many paths (after
	 * every pack write, connectivity checks, gc, ...) that need not have
	 * touched helper-backed storage at all.
	 */
	if (!hp->child)
		return;
	if (hp->cap_refresh)
		helper_process_send(hp, "refresh\n");
}

void helper_process_disconnect(struct helper_process *hp)
{
	if (hp->child) {
		/*
		 * Shut the helper down the same way transport-helper.c's
		 * disconnect_helper() shuts a remote helper down: close its
		 * stdin so it sees EOF and exits, close our read ends, then
		 * reap it. Closing stdin first is what lets finish_command()
		 * return instead of blocking on a process waiting for more
		 * input. The name/gitdir are left intact so helper_process_ensure()
		 * can re-spawn the helper after a disconnect.
		 */
		close(hp->child->in);
		close(hp->child->out);
		fclose(hp->out);
		finish_command(hp->child);
		free(hp->child);
		hp->child = NULL;
		hp->out = NULL;
	}
}

void helper_process_release(struct helper_process *hp)
{
	helper_process_disconnect(hp);
	free(hp->name);
	hp->name = NULL;
	free(hp->gitdir);
	hp->gitdir = NULL;
}
