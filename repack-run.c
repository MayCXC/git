#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "git-compat-util.h"
#include "blob.h"
#include "config.h"
#include "environment.h"
#include "gettext.h"
#include "list-objects-filter-options.h"
#include "midx.h"
#include "object.h"
#include "odb.h"
#include "pack-objects.h"
#include "packfile.h"
#include "parse-options.h"
#include "path.h"
#include "prune-packed.h"
#include "promisor-remote.h"
#include "repack.h"
#include "repository.h"
#include "run-command.h"
#include "server-info.h"
#include "shallow.h"
#include "string-list.h"
#include "strbuf.h"
#include "tempfile.h"
#include "tree.h"

static const char incremental_bitmap_conflict_error[] = N_(
"Incremental repacks are incompatible with bitmap indexes.  Use\n"
"--no-write-bitmap-index or disable the pack.writeBitmaps configuration."
);

int repack_config(const char *var, const char *value,
		  const struct config_context *ctx, void *cb)
{
	struct repack_opts *opts = cb;
	struct pack_objects_args *po_args = &opts->po_args;
	struct pack_objects_args *cruft_po_args = &opts->cruft_po_args;

	if (!strcmp(var, "repack.usedeltabaseoffset")) {
		po_args->delta_base_offset = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.packkeptobjects")) {
		po_args->pack_kept_objects = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.writebitmaps") ||
	    !strcmp(var, "pack.writebitmaps")) {
		opts->write_bitmaps = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.usedeltaislands")) {
		opts->use_delta_islands = git_config_bool(var, value);
		return 0;
	}
	if (strcmp(var, "repack.updateserverinfo") == 0) {
		opts->run_update_server_info = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.cruftwindow")) {
		free(cruft_po_args->window);
		return git_config_string(&cruft_po_args->window, var, value);
	}
	if (!strcmp(var, "repack.cruftwindowmemory")) {
		free(cruft_po_args->window_memory);
		return git_config_string(&cruft_po_args->window_memory, var, value);
	}
	if (!strcmp(var, "repack.cruftdepth")) {
		free(cruft_po_args->depth);
		return git_config_string(&cruft_po_args->depth, var, value);
	}
	if (!strcmp(var, "repack.cruftthreads")) {
		free(cruft_po_args->threads);
		return git_config_string(&cruft_po_args->threads, var, value);
	}
	if (!strcmp(var, "repack.midxmustcontaincruft")) {
		opts->midx_must_contain_cruft = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.midxsplitfactor")) {
		opts->midx_split_factor = git_config_int(var, value, ctx->kvi);
		return 0;
	}
	if (!strcmp(var, "repack.midxnewlayerthreshold")) {
		opts->midx_new_layer_threshold = git_config_int(var, value,
								ctx->kvi);
		return 0;
	}
	return git_default_config(var, value, ctx, cb);
}

void repack_opts_release(struct repack_opts *opts)
{
	string_list_clear(&opts->keep_pack_list, 0);
	pack_objects_args_release(&opts->po_args);
	pack_objects_args_release(&opts->cruft_po_args);
}

int repack_run(struct repository *repo, struct repack_opts *opts)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	struct string_list names = STRING_LIST_INIT_DUP;
	struct existing_packs existing = EXISTING_PACKS_INIT;
	struct pack_geometry geometry = { 0 };
	struct tempfile *refs_snapshot = NULL;
	char *packdir = NULL, *packtmp_name = NULL, *packtmp = NULL;
	int i, ret;
	int show_progress;

	if (opts->delete_redundant && repo->repository_format_precious_objects)
		die(_("cannot delete packs in a precious-objects repo"));

	die_for_incompatible_opt3(opts->unpack_unreachable || (opts->pack_everything & LOOSEN_UNREACHABLE), "-A",
				  opts->keep_unreachable, "-k/--keep-unreachable",
				  opts->pack_everything & PACK_CRUFT, "--cruft");

	if (opts->pack_everything & PACK_CRUFT)
		opts->pack_everything |= ALL_INTO_ONE;

	if (opts->write_bitmaps < 0) {
		if (opts->write_midx == REPACK_WRITE_MIDX_NONE &&
		    (!(opts->pack_everything & ALL_INTO_ONE) || !is_bare_repository()))
			opts->write_bitmaps = 0;
	}
	if (opts->po_args.pack_kept_objects < 0)
		opts->po_args.pack_kept_objects = opts->write_bitmaps > 0 &&
			opts->write_midx == REPACK_WRITE_MIDX_NONE;

	if (opts->write_bitmaps && !(opts->pack_everything & ALL_INTO_ONE) &&
	    opts->write_midx == REPACK_WRITE_MIDX_NONE)
		die(_(incremental_bitmap_conflict_error));

	if (opts->write_bitmaps && opts->po_args.local &&
	    odb_has_alternates(repo->objects)) {
		/*
		 * When asked to do a local repack, but we have
		 * packfiles that are inherited from an alternate, then
		 * we cannot guarantee that the multi-pack-index would
		 * have full coverage of all objects. We thus disable
		 * writing bitmaps in that case.
		 */
		warning(_("disabling bitmap writing, as some objects are not being packed"));
		opts->write_bitmaps = 0;
	}

	if (opts->midx_split_factor < 2)
		die(_("invalid value for %s: %d"), "--midx-split-factor",
		    opts->midx_split_factor);
	if (opts->midx_new_layer_threshold < 1)
		die(_("invalid value for %s: %d"), "--midx-new-layer-threshold",
		    opts->midx_new_layer_threshold);

	if (opts->write_midx != REPACK_WRITE_MIDX_NONE && opts->write_bitmaps) {
		struct strbuf path = STRBUF_INIT;

		strbuf_addf(&path, "%s/%s_XXXXXX",
			    repo_get_object_directory(repo),
			    "bitmap-ref-tips");

		refs_snapshot = xmks_tempfile(path.buf);
		midx_snapshot_refs(repo, refs_snapshot);

		strbuf_release(&path);
	}

	packdir = mkpathdup("%s/pack", repo_get_object_directory(repo));
	packtmp_name = xstrfmt(".tmp-%d-pack", (int)getpid());
	packtmp = mkpathdup("%s/%s", packdir, packtmp_name);

	existing.repo = repo;
	existing_packs_collect(&existing, &opts->keep_pack_list);

	if (opts->split_factor) {
		geometry.split_factor = opts->split_factor;
		if (opts->pack_everything)
			die(_("options '%s' and '%s' cannot be used together"), "--geometric", "-A/-a");
		if (opts->write_midx == REPACK_WRITE_MIDX_INCREMENTAL) {
			geometry.midx_layer_threshold = opts->midx_new_layer_threshold;
			geometry.midx_layer_threshold_set = true;
		}
		pack_geometry_init(&geometry, &existing, &opts->po_args);
		pack_geometry_split(&geometry);
	}

	prepare_pack_objects(&cmd, &opts->po_args, packtmp);

	show_progress = !opts->po_args.quiet && isatty(2);

	strvec_push(&cmd.args, "--keep-true-parents");
	for (i = 0; i < opts->keep_pack_list.nr; i++)
		strvec_pushf(&cmd.args, "--keep-pack=%s",
			     opts->keep_pack_list.items[i].string);
	strvec_push(&cmd.args, "--non-empty");
	if (!geometry.split_factor) {
		/*
		 * We need to grab all reachable objects, including those that
		 * are reachable from reflogs and the index.
		 *
		 * When repacking into a geometric progression of packs,
		 * however, we ask 'git pack-objects --stdin-packs', and it is
		 * not about packing objects based on reachability but about
		 * repacking all the objects in specified packs and loose ones
		 * (indeed, --stdin-packs is incompatible with these options).
		 */
		strvec_push(&cmd.args, "--all");
		strvec_push(&cmd.args, "--reflog");
		strvec_push(&cmd.args, "--indexed-objects");
	}
	if (repo_has_promisor_remote(repo))
		strvec_push(&cmd.args, "--exclude-promisor-objects");
	if (opts->write_midx == REPACK_WRITE_MIDX_NONE) {
		if (opts->write_bitmaps > 0)
			strvec_push(&cmd.args, "--write-bitmap-index");
		else if (opts->write_bitmaps < 0)
			strvec_push(&cmd.args, "--write-bitmap-index-quiet");
	}
	if (opts->use_delta_islands)
		strvec_push(&cmd.args, "--delta-islands");

	if (opts->pack_everything & ALL_INTO_ONE) {
		repack_promisor_objects(repo, &opts->po_args, &names, packtmp);

		if (existing_packs_has_non_kept(&existing) &&
		    opts->delete_redundant &&
		    !(opts->pack_everything & PACK_CRUFT)) {
			for_each_string_list_item(item, &names) {
				strvec_pushf(&cmd.args, "--keep-pack=%s-%s.pack",
					     packtmp_name, item->string);
			}
			if (opts->unpack_unreachable) {
				strvec_pushf(&cmd.args,
					     "--unpack-unreachable=%s",
					     opts->unpack_unreachable);
			} else if (opts->pack_everything & LOOSEN_UNREACHABLE) {
				strvec_push(&cmd.args,
					    "--unpack-unreachable");
			} else if (opts->keep_unreachable) {
				strvec_push(&cmd.args, "--keep-unreachable");
			}
		}

		if (opts->keep_unreachable && opts->delete_redundant &&
		    !(opts->pack_everything & PACK_CRUFT))
			strvec_push(&cmd.args, "--pack-loose-unreachable");
	} else if (geometry.split_factor) {
		pack_geometry_repack_promisors(repo, &opts->po_args, &geometry,
					       &names, packtmp);

		if (opts->midx_must_contain_cruft)
			strvec_push(&cmd.args, "--stdin-packs");
		else
			strvec_push(&cmd.args, "--stdin-packs=follow");
		strvec_push(&cmd.args, "--unpacked");
	} else {
		strvec_push(&cmd.args, "--unpacked");
		strvec_push(&cmd.args, "--incremental");
	}

	if (opts->po_args.filter_options.choice)
		strvec_pushf(&cmd.args, "--filter=%s",
			     expand_list_objects_filter_spec(&opts->po_args.filter_options));
	else if (opts->filter_to)
		die(_("option '%s' can only be used along with '%s'"), "--filter-to", "--filter");

	if (geometry.split_factor)
		cmd.in = -1;
	else
		cmd.no_stdin = 1;

	ret = start_command(&cmd);
	if (ret)
		goto cleanup;

	if (geometry.split_factor) {
		FILE *in = xfdopen(cmd.in, "w");
		/*
		 * The resulting pack should contain all objects in packs that
		 * are going to be rolled up, but exclude objects in packs which
		 * are being left alone.
		 */
		for (i = 0; i < geometry.split; i++)
			fprintf(in, "%s\n", pack_basename(geometry.pack[i]));
		for (i = geometry.split; i < geometry.pack_nr; i++) {
			const char *basename = pack_basename(geometry.pack[i]);
			char marker = '^';

			if (!opts->midx_must_contain_cruft &&
			    !string_list_has_string(&existing.midx_packs,
						    basename)) {
				/*
				 * Assume non-MIDX'd packs are not
				 * necessarily closed under
				 * reachability.
				 */
				marker = '!';
			}

			fprintf(in, "%c%s\n", marker, basename);
		}
		fclose(in);
	}

	{
		struct write_pack_opts wopts = {
			.packdir = packdir,
			.destination = packdir,
			.packtmp = packtmp,
		};
		ret = finish_pack_objects_cmd(repo->hash_algo, &wopts, &cmd,
					      &names);
		if (ret)
			goto cleanup;
	}

	if (!names.nr) {
		if (!opts->po_args.quiet)
			printf_ln(_("Nothing new to pack."));
		/*
		 * If we didn't write any new packs, the non-cruft packs
		 * may refer to once-unreachable objects in the cruft
		 * pack(s).
		 *
		 * If there isn't already a MIDX, the one we write
		 * must include the cruft pack(s), in case the
		 * non-cruft pack(s) refer to once-cruft objects.
		 *
		 * If there is already a MIDX, we can punt here, since
		 * midx_has_unknown_packs() will make the decision for
		 * us.
		 */
		if (!get_multi_pack_index(existing.source))
			opts->midx_must_contain_cruft = 1;
	}

	if (opts->pack_everything & PACK_CRUFT) {
		struct write_pack_opts wopts = {
			.po_args = &opts->cruft_po_args,
			.destination = packtmp,
			.packtmp = packtmp,
			.packdir = packdir,
		};

		if (!opts->cruft_po_args.window)
			opts->cruft_po_args.window = xstrdup_or_null(opts->po_args.window);
		if (!opts->cruft_po_args.window_memory)
			opts->cruft_po_args.window_memory = xstrdup_or_null(opts->po_args.window_memory);
		if (!opts->cruft_po_args.depth)
			opts->cruft_po_args.depth = xstrdup_or_null(opts->po_args.depth);
		if (!opts->cruft_po_args.threads)
			opts->cruft_po_args.threads = xstrdup_or_null(opts->po_args.threads);
		if (!opts->cruft_po_args.max_pack_size)
			opts->cruft_po_args.max_pack_size = opts->po_args.max_pack_size;

		opts->cruft_po_args.local = opts->po_args.local;
		opts->cruft_po_args.quiet = opts->po_args.quiet;
		opts->cruft_po_args.delta_base_offset = opts->po_args.delta_base_offset;
		opts->cruft_po_args.pack_kept_objects = 0;

		ret = write_cruft_pack(&wopts, opts->cruft_expiration,
				       opts->combine_cruft_below_size, &names,
				       &existing);
		if (ret)
			goto cleanup;

		if (opts->delete_redundant && opts->expire_to) {
			/*
			 * If `--expire-to` is given with `-d`, it's possible
			 * that we're about to prune some objects. With cruft
			 * packs, pruning is implicit: any objects from existing
			 * packs that weren't picked up by new packs are removed
			 * when their packs are deleted.
			 *
			 * Generate an additional cruft pack, with one twist:
			 * `names` now includes the name of the cruft pack
			 * written in the previous step. So the contents of
			 * _this_ cruft pack exclude everything contained in the
			 * existing cruft pack (that is, all of the unreachable
			 * objects which are no older than
			 * `--cruft-expiration`).
			 *
			 * To make this work, cruft_expiration must become NULL
			 * so that this cruft pack doesn't actually prune any
			 * objects. If it were non-NULL, this call would always
			 * generate an empty pack (since every object not in the
			 * cruft pack generated above will have an mtime older
			 * than the expiration).
			 *
			 * Pretend we don't have a `--combine-cruft-below-size`
			 * argument, since we're not selectively combining
			 * anything based on size to generate the limbo cruft
			 * pack, but rather removing all cruft packs from the
			 * main repository regardless of size.
			 */
			wopts.destination = opts->expire_to;
			ret = write_cruft_pack(&wopts, NULL, 0ul, &names,
					       &existing);
			if (ret)
				goto cleanup;
		}
	}

	if (opts->po_args.filter_options.choice) {
		struct write_pack_opts wopts = {
			.po_args = &opts->po_args,
			.destination = opts->filter_to,
			.packdir = packdir,
			.packtmp = packtmp,
		};

		if (!wopts.destination)
			wopts.destination = packtmp;

		ret = write_filtered_pack(&wopts, &existing, &names);
		if (ret)
			goto cleanup;
	}

	string_list_sort(&names);

	odb_close(repo->objects);

	/*
	 * Ok we have prepared all new packfiles.
	 */
	for_each_string_list_item(item, &names)
		generated_pack_install(item->util, item->string, packdir,
				       packtmp);
	/* End of pack replacement. */

	if (opts->delete_redundant && opts->pack_everything & ALL_INTO_ONE) {
		if (opts->write_midx == REPACK_WRITE_MIDX_INCREMENTAL)
			existing_packs_retain_midx_packs(&existing);
		existing_packs_mark_for_deletion(&existing, &names);
	}

	if (opts->write_midx != REPACK_WRITE_MIDX_NONE) {
		struct repack_write_midx_opts midx_opts = {
			.existing = &existing,
			.geometry = &geometry,
			.names = &names,
			.refs_snapshot = refs_snapshot ? get_tempfile_path(refs_snapshot) : NULL,
			.packdir = packdir,
			.show_progress = show_progress,
			.write_bitmaps = opts->write_bitmaps > 0,
			.midx_must_contain_cruft = opts->midx_must_contain_cruft,
			.midx_split_factor = opts->midx_split_factor,
			.midx_new_layer_threshold = opts->midx_new_layer_threshold,
			.mode = opts->write_midx,
		};

		ret = repack_write_midx(&midx_opts);
		if (ret)
			goto cleanup;
	}

	odb_reprepare(repo->objects);

	if (opts->delete_redundant) {
		int prune_opts = 0;
		bool wrote_incremental_midx = opts->write_midx == REPACK_WRITE_MIDX_INCREMENTAL;

		existing_packs_remove_redundant(&existing, packdir,
						wrote_incremental_midx);

		if (geometry.split_factor)
			pack_geometry_remove_redundant(&geometry, &names,
						       &existing, packdir,
						       wrote_incremental_midx);
		if (show_progress)
			prune_opts |= PRUNE_PACKED_VERBOSE;
		prune_packed_objects(prune_opts);

		if (!opts->keep_unreachable &&
		    (!(opts->pack_everything & LOOSEN_UNREACHABLE) ||
		     opts->unpack_unreachable) &&
		    is_repository_shallow(repo))
			prune_shallow(PRUNE_QUICK);
	}

	if (opts->run_update_server_info)
		update_server_info(repo, 0);

	if (git_env_bool(GIT_TEST_MULTI_PACK_INDEX, 0)) {
		unsigned flags = 0;
		if (git_env_bool(GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL, 0))
			flags |= MIDX_WRITE_INCREMENTAL;
		odb_source_multi_pack_index_write(existing.source, NULL, NULL, NULL,
						  NULL, flags);
	}

cleanup:
	string_list_clear(&names, 1);
	existing_packs_release(&existing);
	pack_geometry_release(&geometry);
	free(packdir);
	free(packtmp_name);
	free(packtmp);

	return ret;
}

uint64_t repack_total_ram(void)
{
#if defined(HAVE_SYSINFO)
	struct sysinfo si;

	if (!sysinfo(&si)) {
		uint64_t total = si.totalram;

		if (si.mem_unit > 1)
			total *= (uint64_t)si.mem_unit;
		return total;
	}
#elif defined(HAVE_BSD_SYSCTL) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM) || defined(HW_PHYSMEM64))
	uint64_t physical_memory;
	int mib[2];
	size_t length;

	mib[0] = CTL_HW;
# if defined(HW_MEMSIZE)
	mib[1] = HW_MEMSIZE;
# elif defined(HW_PHYSMEM64)
	mib[1] = HW_PHYSMEM64;
# else
	mib[1] = HW_PHYSMEM;
# endif
	length = sizeof(physical_memory);
	if (!sysctl(mib, 2, &physical_memory, &length, NULL, 0)) {
		if (length == 4) {
			uint32_t mem;

			if (!sysctl(mib, 2, &mem, &length, NULL, 0))
				physical_memory = mem;
		}
		return physical_memory;
	}
#elif defined(GIT_WINDOWS_NATIVE)
	MEMORYSTATUSEX memInfo;

	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	if (GlobalMemoryStatusEx(&memInfo))
		return memInfo.ullTotalPhys;
#endif
	return 0;
}

uint64_t repack_estimate_memory(struct repository *repo,
				struct packed_git *pack,
				size_t delta_base_cache_limit,
				size_t max_delta_cache_size)
{
	unsigned long nr_objects;
	size_t os_cache, heap;

	if (odb_count_objects(repo->objects,
			      ODB_COUNT_OBJECTS_APPROXIMATE, &nr_objects) < 0)
		return 0;

	if (!pack || !nr_objects)
		return 0;

	/*
	 * First we have to scan through at least one pack.
	 * Assume enough room in OS file cache to keep the entire pack
	 * or we may accidentally evict data of other processes from
	 * the cache.
	 */
	os_cache = pack->pack_size + pack->index_size;
	/* then pack-objects needs lots more for book keeping */
	heap = sizeof(struct object_entry) * nr_objects;
	/*
	 * internal rev-list --all --objects takes up some memory too,
	 * let's say half of it is for blobs
	 */
	heap += sizeof(struct blob) * nr_objects / 2;
	/*
	 * and the other half is for trees (commits and tags are
	 * usually insignificant)
	 */
	heap += sizeof(struct tree) * nr_objects / 2;
	/* and then obj_hash[], underestimated in fact */
	heap += sizeof(struct object *) * nr_objects;
	/* revindex is used also */
	heap += (sizeof(off_t) + sizeof(uint32_t)) * nr_objects;
	/*
	 * read_sha1_file() (either at delta calculation phase, or
	 * writing phase) also fills up the delta base cache
	 */
	heap += delta_base_cache_limit;
	/* and of course pack-objects has its own delta cache */
	heap += max_delta_cache_size;

	return os_cache + heap;
}
