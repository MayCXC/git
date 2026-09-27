#include "git-compat-util.h"
#include "gettext.h"
#include "helper.h"
#include "run-command.h"
#include "sigchain.h"
#include "strvec.h"
#include "wrapper.h"

void helper_process_init(struct helper_process *hp, const char *name,
			 const char *gitdir,
			 const struct helper_capability *known_capabilities)
{
	memset(hp, 0, sizeof(*hp));

	if (strchr(name, '/'))
		die(_("helper name '%s' must not contain '/'"), name);

	hp->name = xstrdup(name);
	hp->gitdir = xstrdup(gitdir);
	hp->known_capabilities = known_capabilities;
}

static void negotiate_capabilities(struct helper_process *hp)
{
	const struct helper_capability *cap;
	struct strbuf line = STRBUF_INIT;
	int ret;

	helper_process_send(hp, "capabilities\n");
	while ((ret = helper_process_readline(hp, &line)) != EOF && line.len) {
		/* Capabilities the caller does not know go unused. */
		for (cap = hp->known_capabilities; cap && cap->name; cap++)
			if (!strcmp(line.buf, cap->name))
				hp->capabilities |= cap->flag;
	}
	strbuf_release(&line);
	if (ret == EOF)
		die(_("helper '%s' hung up while listing its capabilities"),
		    hp->name);

	for (cap = hp->known_capabilities; cap && cap->name; cap++)
		if (cap->required && !(hp->capabilities & cap->flag))
			die(_("helper '%s' does not support the '%s' capability"),
			    hp->name, cap->name);
}

struct helper_process *helper_process_ensure(struct helper_process *hp)
{
	struct child_process *child;
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
	 * The helper lives as long as the storage it backs is in use. Closing
	 * inherited descriptors keeps it from pinning unrelated descriptors
	 * open, most importantly the upload-pack/fetch connection pipe: were it
	 * inherited, the remote would never see EOF on its stdin and the fetch
	 * would deadlock.
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

	hp->capabilities = 0;
	negotiate_capabilities(hp);
	return hp;
}

bool helper_process_capable(struct helper_process *hp, unsigned int flag)
{
	return helper_process_ensure(hp)->capabilities & flag;
}

void helper_process_send(struct helper_process *hp, const char *fmt, ...)
{
	struct strbuf buf = STRBUF_INIT;
	va_list ap;

	va_start(ap, fmt);
	strbuf_vaddf(&buf, fmt, ap);
	va_end(ap);

	helper_process_write(hp, buf.buf, buf.len);
	strbuf_release(&buf);
}

void helper_process_write(struct helper_process *hp, const void *buf,
			  size_t len)
{
	int ret;

	helper_process_ensure(hp);

	sigchain_push(SIGPIPE, SIG_IGN);
	ret = write_in_full(hp->child->in, buf, len);
	sigchain_pop(SIGPIPE);

	/*
	 * A failed write means the helper has died or closed its input. Fail
	 * loudly, like transport-helper.c does for remote helpers, rather than
	 * letting the next read return EOF and be mistaken for "object/ref not
	 * found".
	 */
	if (ret < 0)
		die_errno(_("unable to write to local helper '%s'"), hp->name);
}

int helper_process_readline(struct helper_process *hp, struct strbuf *line)
{
	helper_process_ensure(hp);
	strbuf_reset(line);
	return strbuf_getline_lf(line, hp->out);
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
		 * input.
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
	FREE_AND_NULL(hp->name);
	FREE_AND_NULL(hp->gitdir);
}
