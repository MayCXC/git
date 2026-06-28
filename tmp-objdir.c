#include "git-compat-util.h"
#include "tmp-objdir.h"
#include "abspath.h"
#include "chdir-notify.h"
#include "dir.h"
#include "environment.h"
#include "strbuf.h"
#include "strvec.h"
#include "quote.h"
#include "odb.h"
#include "odb/source.h"
#include "repository.h"

struct tmp_objdir {
	struct repository *repo;
	struct strbuf path;
	struct strvec env;
	struct odb_source *prev_source;
	int will_destroy;
};

/*
 * Allow only one tmp_objdir at a time in a running process, which simplifies
 * our atexit cleanup routines.  It's doubtful callers will ever need
 * more than one, and we can expand later if so.  You can have many such
 * tmp_objdirs simultaneously in many processes, of course.
 */
static struct tmp_objdir *the_tmp_objdir;

static void tmp_objdir_free(struct tmp_objdir *t)
{
	strbuf_release(&t->path);
	strvec_clear(&t->env);
	free(t);
}

static void tmp_objdir_reparent(const char *name UNUSED,
				const char *old_cwd,
				const char *new_cwd,
				void *cb_data)
{
	struct tmp_objdir *t = cb_data;
	char *path;

	path = reparent_relative_path(old_cwd, new_cwd,
				      t->path.buf);
	strbuf_reset(&t->path);
	strbuf_addstr(&t->path, path);
	free(path);
}

int tmp_objdir_destroy(struct tmp_objdir *t)
{
	int err;

	if (!t)
		return 0;

	if (t == the_tmp_objdir)
		the_tmp_objdir = NULL;

	if (t->prev_source)
		odb_restore_primary_source(t->repo->objects, t->prev_source, t->path.buf);

	err = remove_dir_recursively(&t->path, 0);

	chdir_notify_unregister(NULL, tmp_objdir_reparent, t);
	tmp_objdir_free(t);

	return err;
}

static void remove_tmp_objdir(void)
{
	tmp_objdir_destroy(the_tmp_objdir);
}

void tmp_objdir_discard_objects(struct tmp_objdir *t)
{
	remove_dir_recursively(&t->path, REMOVE_DIR_KEEP_TOPLEVEL);
}

/*
 * These env_* functions are for setting up the child environment; the
 * "replace" variant overrides the value of any existing variable with that
 * "key". The "append" variant puts our new value at the end of a list,
 * separated by PATH_SEP (which is what separate values in
 * GIT_ALTERNATE_OBJECT_DIRECTORIES).
 */
static void env_append(struct strvec *env, const char *key, const char *val)
{
	struct strbuf quoted = STRBUF_INIT;
	const char *old;

	/*
	 * Avoid quoting if it's not necessary, for maximum compatibility
	 * with older parsers which don't understand the quoting.
	 */
	if (*val == '"' || strchr(val, PATH_SEP)) {
		strbuf_addch(&quoted, '"');
		quote_c_style(val, &quoted, NULL, 1);
		strbuf_addch(&quoted, '"');
		val = quoted.buf;
	}

	old = getenv(key);
	if (!old)
		strvec_pushf(env, "%s=%s", key, val);
	else
		strvec_pushf(env, "%s=%s%c%s", key, old, PATH_SEP, val);

	strbuf_release(&quoted);
}

static void env_replace(struct strvec *env, const char *key, const char *val)
{
	strvec_pushf(env, "%s=%s", key, val);
}

static int setup_tmp_objdir(const char *root)
{
	char *path;
	int ret = 0;

	path = xstrfmt("%s/pack", root);
	ret = mkdir(path, 0777);
	free(path);

	return ret;
}

struct tmp_objdir *tmp_objdir_create(struct repository *r,
				     const char *prefix)
{
	static int installed_handlers;
	struct tmp_objdir *t;

	if (the_tmp_objdir)
		BUG("only one tmp_objdir can be used at a time");

	t = xcalloc(1, sizeof(*t));
	t->repo = r;
	strbuf_init(&t->path, 0);
	strvec_init(&t->env);

	/*
	 * Use a string starting with tmp_ so that the builtin/prune.c code
	 * can recognize any stale objdirs left behind by a crash and delete
	 * them.
	 */
	strbuf_addf(&t->path, "%s/tmp_objdir-%s-XXXXXX",
		    repo_get_object_directory(r), prefix);

	if (!is_absolute_path(t->path.buf))
		chdir_notify_register(NULL, tmp_objdir_reparent, t);

	if (!mkdtemp(t->path.buf)) {
		/* free, not destroy, as we never touched the filesystem */
		tmp_objdir_free(t);
		return NULL;
	}

	the_tmp_objdir = t;
	if (!installed_handlers) {
		atexit(remove_tmp_objdir);
		installed_handlers++;
	}

	if (setup_tmp_objdir(t->path.buf)) {
		tmp_objdir_destroy(t);
		return NULL;
	}

	env_append(&t->env, ALTERNATE_DB_ENVIRONMENT,
		   absolute_path(repo_get_object_directory(r)));
	env_replace(&t->env, DB_ENVIRONMENT, absolute_path(t->path.buf));
	env_replace(&t->env, GIT_QUARANTINE_ENVIRONMENT,
		    absolute_path(t->path.buf));

	return t;
}

int tmp_objdir_migrate(struct tmp_objdir *t)
{
	struct odb_source *primary;
	int ret;

	if (!t)
		return 0;

	if (t->prev_source) {
		if (odb_primary_source(t->repo->objects)->will_destroy)
			BUG("migrating an ODB that was marked for destruction");
		odb_restore_primary_source(t->repo->objects, t->prev_source, t->path.buf);
		t->prev_source = NULL;
	}

	/*
	 * Hand the quarantine's accepted objects to the primary source to
	 * incorporate. The files source renames the loose objects and packs into
	 * its object directory in place; a source that stores objects in its own
	 * medium (a helper) copies them in instead. Each reads the quarantine, a
	 * files tmp-objdir, through its own backend.
	 */
	primary = odb_primary_source(t->repo->objects);
	ret = primary->migrate_quarantine(primary, t->path.buf);

	tmp_objdir_destroy(t);
	return ret;
}

const char **tmp_objdir_env(const struct tmp_objdir *t)
{
	if (!t)
		return NULL;
	return t->env.v;
}

void tmp_objdir_add_as_alternate(const struct tmp_objdir *t)
{
	odb_add_to_alternates_memory(t->repo->objects, t->path.buf);
}

void tmp_objdir_replace_primary_odb(struct tmp_objdir *t, int will_destroy)
{
	if (t->prev_source)
		BUG("the primary object database is already replaced");
	t->prev_source = odb_set_temporary_primary_source(t->repo->objects,
							  t->path.buf, will_destroy);
	t->will_destroy = will_destroy;
}
