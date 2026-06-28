#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "builtin.h"
#include "config.h"
#include "gettext.h"
#include "list-objects-filter-options.h"
#include "odb.h"
#include "parse-options.h"
#include "repack.h"

static const char *const git_repack_usage[] = {
	N_("git repack [-a] [-A] [-d] [-f] [-F] [-l] [-n] [-q] [-b] [-m]\n"
	   "[--window=<n>] [--depth=<n>] [--threads=<n>] [--keep-pack=<pack-name>]\n"
	   "[--write-midx[=<mode>]] [--name-hash-version=<n>] [--path-walk]"),
	NULL
};

static int option_parse_write_midx(const struct option *opt, const char *arg,
				   int unset)
{
	enum repack_write_midx_mode *cfg = opt->value;

	if (unset) {
		*cfg = REPACK_WRITE_MIDX_NONE;
		return 0;
	}

	if (!arg || !*arg)
		*cfg = REPACK_WRITE_MIDX_DEFAULT;
	else if (!strcmp(arg, "incremental"))
		*cfg = REPACK_WRITE_MIDX_INCREMENTAL;
	else
		return error(_("unknown value for %s: %s"), opt->long_name, arg);

	return 0;
}

int cmd_repack(int argc,
	       const char **argv,
	       const char *prefix,
	       struct repository *repo)
{
	struct repack_opts opts = REPACK_OPTS_INIT;
	struct odb_optimize_opts oopts = { 0 };
	const char *opt_window = NULL;
	const char *opt_window_memory = NULL;
	const char *opt_depth = NULL;
	const char *opt_threads = NULL;
	int ret;

	struct option builtin_repack_options[] = {
		OPT_BIT('a', NULL, &opts.pack_everything,
				N_("pack everything in a single pack"), ALL_INTO_ONE),
		OPT_BIT('A', NULL, &opts.pack_everything,
				N_("same as -a, and turn unreachable objects loose"),
				   LOOSEN_UNREACHABLE | ALL_INTO_ONE),
		OPT_BIT(0, "cruft", &opts.pack_everything,
				N_("same as -a, pack unreachable cruft objects separately"),
				   PACK_CRUFT),
		OPT_STRING(0, "cruft-expiration", &opts.cruft_expiration, N_("approxidate"),
				N_("with --cruft, expire objects older than this")),
		OPT_UNSIGNED(0, "combine-cruft-below-size",
			     &opts.combine_cruft_below_size,
			     N_("with --cruft, only repack cruft packs smaller than this")),
		OPT_UNSIGNED(0, "max-cruft-size", &opts.cruft_po_args.max_pack_size,
			     N_("with --cruft, limit the size of new cruft packs")),
		OPT_BOOL('d', NULL, &opts.delete_redundant,
				N_("remove redundant packs, and run git-prune-packed")),
		OPT_BOOL('f', NULL, &opts.po_args.no_reuse_delta,
				N_("pass --no-reuse-delta to git-pack-objects")),
		OPT_BOOL('F', NULL, &opts.po_args.no_reuse_object,
				N_("pass --no-reuse-object to git-pack-objects")),
		OPT_INTEGER(0, "name-hash-version", &opts.po_args.name_hash_version,
				N_("specify the name hash version to use for grouping similar objects by path")),
		OPT_BOOL(0, "path-walk", &opts.po_args.path_walk,
				N_("pass --path-walk to git-pack-objects")),
		OPT_NEGBIT('n', NULL, &opts.run_update_server_info,
				N_("do not run git-update-server-info"), 1),
		OPT__QUIET(&opts.po_args.quiet, N_("be quiet")),
		OPT_BOOL('l', "local", &opts.po_args.local,
				N_("pass --local to git-pack-objects")),
		OPT_BOOL('b', "write-bitmap-index", &opts.write_bitmaps,
				N_("write bitmap index")),
		OPT_BOOL('i', "delta-islands", &opts.use_delta_islands,
				N_("pass --delta-islands to git-pack-objects")),
		OPT_STRING(0, "unpack-unreachable", &opts.unpack_unreachable, N_("approxidate"),
				N_("with -A, do not loosen objects older than this")),
		OPT_BOOL('k', "keep-unreachable", &opts.keep_unreachable,
				N_("with -a, repack unreachable objects")),
		OPT_STRING(0, "window", &opt_window, N_("n"),
				N_("size of the window used for delta compression")),
		OPT_STRING(0, "window-memory", &opt_window_memory, N_("bytes"),
				N_("same as the above, but limit memory size instead of entries count")),
		OPT_STRING(0, "depth", &opt_depth, N_("n"),
				N_("limits the maximum delta depth")),
		OPT_STRING(0, "threads", &opt_threads, N_("n"),
				N_("limits the maximum number of threads")),
		OPT_UNSIGNED(0, "max-pack-size", &opts.po_args.max_pack_size,
			     N_("maximum size of each packfile")),
		OPT_PARSE_LIST_OBJECTS_FILTER(&opts.po_args.filter_options),
		OPT_BOOL(0, "pack-kept-objects", &opts.po_args.pack_kept_objects,
				N_("repack objects in packs marked with .keep")),
		OPT_STRING_LIST(0, "keep-pack", &opts.keep_pack_list, N_("name"),
				N_("do not repack this pack")),
		OPT_INTEGER('g', "geometric", &opts.split_factor,
			    N_("find a geometric progression with factor <N>")),
		OPT_CALLBACK_F(0, "write-midx", &opts.write_midx,
			   N_("mode"),
			   N_("write a multi-pack index of the resulting packs"),
			   PARSE_OPT_OPTARG, option_parse_write_midx),
		OPT_SET_INT_F('m', NULL, &opts.write_midx,
			   N_("write a multi-pack index of the resulting packs"),
			   REPACK_WRITE_MIDX_DEFAULT,
			   PARSE_OPT_HIDDEN),
		OPT_STRING(0, "expire-to", &opts.expire_to, N_("dir"),
			   N_("pack prefix to store a pack containing pruned objects")),
		OPT_STRING(0, "filter-to", &opts.filter_to, N_("dir"),
			   N_("pack prefix to store a pack containing filtered out objects")),
		OPT_END()
	};

	list_objects_filter_init(&opts.po_args.filter_options);

	repo_config(repo, repack_config, &opts);

	argc = parse_options(argc, argv, prefix, builtin_repack_options,
				git_repack_usage, 0);

	opts.po_args.window = xstrdup_or_null(opt_window);
	opts.po_args.window_memory = xstrdup_or_null(opt_window_memory);
	opts.po_args.depth = xstrdup_or_null(opt_depth);
	opts.po_args.threads = xstrdup_or_null(opt_threads);

	/*
	 * "git repack" is the object-store analog of "git pack-refs": route it
	 * through the maintenance vtable so it dispatches to the primary
	 * source's backend. The files source repacks using the fully-parsed
	 * options below; other backends (e.g. a helper) optimize their own
	 * storage off the generic flags and ignore the files-only opts.
	 */
	oopts.repack = &opts;
	if (opts.delete_redundant)
		oopts.flags |= ODB_OPTIMIZE_PRUNE;
	if (opts.po_args.quiet)
		oopts.flags |= ODB_OPTIMIZE_QUIET;
	if (opts.pack_everything & PACK_CRUFT)
		oopts.flags |= ODB_OPTIMIZE_CRUFT;
	ret = odb_optimize(repo->objects, &oopts);

	repack_opts_release(&opts);

	return ret;
}
