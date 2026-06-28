#define DISABLE_SIGN_COMPARE_WARNINGS

#include "builtin.h"
#include "commit.h"
#include "diff.h"
#include "dir.h"
#include "environment.h"
#include "gettext.h"
#include "hex.h"
#include "revision.h"
#include "reachable.h"
#include "parse-options.h"
#include "path.h"
#include "progress.h"
#include "replace-object.h"
#include "object-file.h"
#include "object-name.h"
#include "odb.h"
#include "odb/source.h"
#include "oidset.h"
#include "shallow.h"

static const char * const prune_usage[] = {
	N_("git prune [-n] [-v] [--progress] [--expire <time>] [--] [<head>...]"),
	NULL
};
static int show_only;
static int verbose;
static timestamp_t expire;
static int show_progress = -1;

static void perform_reachability_traversal(struct rev_info *revs)
{
	static int initialized;
	struct progress *progress = NULL;

	if (initialized)
		return;

	if (show_progress)
		progress = start_delayed_progress(revs->repo,
						  _("Checking connectivity"), 0);
	mark_reachable_objects(revs, 1, expire, progress);
	stop_progress(&progress);
	initialized = 1;
}

static int is_object_reachable(const struct object_id *oid,
			       struct rev_info *revs)
{
	struct object *obj;

	perform_reachability_traversal(revs);

	obj = lookup_object(revs->repo, oid);
	return obj && (obj->flags & SEEN);
}

struct prune_collect {
	struct oidset *unreachable;
	struct rev_info *revs;
};

static int collect_unreachable(const struct object_id *oid,
			       struct object_info *oi UNUSED, void *data)
{
	struct prune_collect *c = data;

	if (!is_object_reachable(oid, c->revs))
		oidset_insert(c->unreachable, oid);
	return 0;
}

/*
 * The object-pruning half of "git prune", routed through the source vtable so
 * it works on any backend: git owns reachability (mark_reachable_objects, via
 * is_object_reachable), enumerates each local source's objects, and hands the
 * source the set it found unreachable; the source deletes those older than
 * `expire` using its own timestamps (the files source the loose mtime, a helper
 * its stored time). With --dry-run / -v we report exactly the objects the
 * source prunes (the loose-and-expired subset for files, the helper's own
 * expired subset) instead of (or as well as) deleting them, not the whole
 * unreachable candidate set.
 */
static void prune_unreachable_objects(struct repository *repo,
				      struct rev_info *revs)
{
	struct odb_source *source;
	struct odb_for_each_object_options opts = { 0 };

	/*
	 * Run the reachability traversal up front: it reads objects through the
	 * backend, which on a helper must not be interleaved with a for_each_object
	 * listing on the same connection (doing so desyncs the helper protocol).
	 * After this, is_object_reachable() is a pure in-memory SEEN lookup.
	 */
	perform_reachability_traversal(revs);

	for (source = odb_primary_source(repo->objects); source; source = source->next) {
		struct oidset unreachable = OIDSET_INIT;
		struct prune_collect c = { &unreachable, revs };

		if (!source->local)
			continue;

		odb_source_for_each_object(source, NULL, collect_unreachable,
					   &c, &opts);

		if (show_only || verbose) {
			struct oidset removed = OIDSET_INIT;
			struct oidset_iter iter;
			const struct object_id *oid;

			/*
			 * Report exactly what prune removes, not the whole candidate
			 * set: a dry-run pass records the members this source would
			 * prune (the loose-and-expired subset for files, the helper's
			 * own expired subset) into `removed` without deleting, so each
			 * object's type can still be read for the report.
			 */
			odb_source_remove_objects(source, &unreachable, expire, 1,
						  &removed);
			oidset_iter_init(&removed, &iter);
			while ((oid = oidset_iter_next(&iter))) {
				enum object_type type =
					odb_read_object_info(repo->objects,
							     oid, NULL);
				printf("%s %s\n", oid_to_hex(oid),
				       (type > 0) ? type_name(type) : "unknown");
			}
			oidset_clear(&removed);
		}

		if (!show_only)
			odb_source_remove_objects(source, &unreachable, expire,
						  0, NULL);

		oidset_clear(&unreachable);
	}
}

int cmd_prune(int argc,
	      const char **argv,
	      const char *prefix,
	      struct repository *repo)
{
	struct rev_info revs;
	int exclude_promisor_objects = 0;
	const struct option options[] = {
		OPT__DRY_RUN(&show_only, N_("do not remove, show only")),
		OPT__VERBOSE(&verbose, N_("report pruned objects")),
		OPT_BOOL(0, "progress", &show_progress, N_("show progress")),
		OPT_EXPIRY_DATE(0, "expire", &expire,
				N_("expire objects older than <time>")),
		OPT_BOOL(0, "exclude-promisor-objects", &exclude_promisor_objects,
			 N_("limit traversal to objects outside promisor packfiles")),
		OPT_END()
	};

	expire = TIME_MAX;
	save_commit_buffer = 0;
	disable_replace_refs();

	argc = parse_options(argc, argv, prefix, options, prune_usage, 0);

	repo_init_revisions(repo, &revs, prefix);
	if (repo->repository_format_precious_objects)
		die(_("cannot prune in a precious-objects repo"));

	while (argc--) {
		struct object_id oid;
		const char *name = *argv++;

		if (!repo_get_oid(repo, name, &oid)) {
			struct object *object = parse_object_or_die(repo, &oid, name);
			add_pending_object(&revs, object, "");
		}
		else
			die("unrecognized argument: %s", name);
	}

	if (show_progress == -1)
		show_progress = isatty(2);
	if (exclude_promisor_objects) {
		fetch_if_missing = 0;
		revs.exclude_promisor_objects = 1;
	}

	prune_unreachable_objects(repo, &revs);

	/*
	 * Tidy each local source's on-disk cruft (stale temp files, empty fanout
	 * dirs, redundant loose objects). Dispatched through the source vtable so a
	 * non-files backend (a helper, which has no loose tier or object directory)
	 * is never handed a filesystem sweep of storage it does not own.
	 */
	odb_prune_cruft(repo->objects, expire, show_only, verbose);

	if (is_repository_shallow(repo)) {
		perform_reachability_traversal(&revs);
		prune_shallow(show_only ? PRUNE_SHOW_ONLY : 0);
	}

	release_revisions(&revs);
	return 0;
}
