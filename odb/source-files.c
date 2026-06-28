#include "git-compat-util.h"
#include "abspath.h"
#include "chdir-notify.h"
#include "commit-graph.h"
#include "config.h"
#include "copy.h"
#include "dir.h"
#include "dir-iterator.h"
#include "gettext.h"
#include "iterator.h"
#include "setup.h"
#include "hex.h"
#include "loose.h"
#include "list-objects-filter-options.h"
#include "lockfile.h"
#include "object-file.h"
#include "odb.h"
#include "odb/pack-ingest.h"
#include "odb/source.h"
#include "odb/source-files.h"
#include "odb/source-loose.h"
#include "oidset.h"
#include "pack-bitmap.h"
#include "midx.h"
#include "pack.h"
#include "packfile.h"
#include "pack-mtimes.h"
#include "pack-objects.h"
#include "pack-revindex.h"
#include "path.h"
#include "pack-revindex.h"
#include "repack.h"
#include "repo-settings.h"
#include "run-command.h"
#include "fsck.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "wrapper.h"
#include "write-or-die.h"

static void odb_source_files_reparent(const char *name UNUSED,
				      const char *old_cwd,
				      const char *new_cwd,
				      void *cb_data)
{
	struct odb_source_files *files = cb_data;
	char *path = reparent_relative_path(old_cwd, new_cwd,
					    files->base.path);
	free(files->base.path);
	files->base.path = path;
}

static void odb_source_files_unregister(struct odb_source_files *files)
{
	struct object_database *odb = files->base.odb;
	struct odb_source_files **pp;

	for (pp = &odb->files_sources; *pp; pp = &(*pp)->next_files) {
		if (*pp != files)
			continue;
		*pp = files->next_files;
		if (odb->files_sources_tail == &files->next_files)
			odb->files_sources_tail = pp;
		files->next_files = NULL;
		return;
	}
}

static void odb_source_files_free(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	chdir_notify_unregister(NULL, odb_source_files_reparent, files);
	odb_source_files_unregister(files);
	odb_source_free(&files->loose->base);
	packfile_store_free(files->packed);
	odb_source_release(&files->base);
	free(files);
}

static void odb_source_files_close(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	odb_source_close(&files->loose->base);
	packfile_store_close(files->packed);
}

static void odb_source_files_reprepare(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	odb_source_reprepare(&files->loose->base);
	packfile_store_reprepare(files->packed);
}

static int odb_source_files_read_object_info(struct odb_source *source,
					     const struct object_id *oid,
					     struct object_info *oi,
					     enum object_info_flags flags)
{
	struct odb_source_files *files = odb_source_files_downcast(source);

	if (!packfile_store_read_object_info(files->packed, oid, oi, flags) ||
	    !odb_source_read_object_info(&files->loose->base, oid, oi, flags))
		return 0;

	return -1;
}

static int odb_source_files_read_object_stream(struct odb_read_stream **out,
					       struct odb_source *source,
					       const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	if (!packfile_store_read_object_stream(out, files->packed, oid) ||
	    !odb_source_read_object_stream(out, &files->loose->base, oid))
		return 0;
	return -1;
}

static int odb_source_files_for_each_object(struct odb_source *source,
					    const struct object_info *request,
					    odb_for_each_object_cb cb,
					    void *cb_data,
					    const struct odb_for_each_object_options *opts)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	int ret;

	if (!(opts->flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY)) {
		ret = odb_source_for_each_object(&files->loose->base, request, cb, cb_data, opts);
		if (ret)
			return ret;
	}

	ret = packfile_store_for_each_object(files->packed, request, cb, cb_data, opts);
	if (ret)
		return ret;

	return 0;
}

static int odb_source_files_count_objects(struct odb_source *source,
					  enum odb_count_objects_flags flags,
					  unsigned long *out)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	unsigned long count;
	int ret;

	ret = packfile_store_count_objects(files->packed, flags, &count);
	if (ret < 0)
		goto out;

	if (!(flags & ODB_COUNT_OBJECTS_APPROXIMATE)) {
		unsigned long loose_count;

		ret = odb_source_count_objects(&files->loose->base, flags, &loose_count);
		if (ret < 0)
			goto out;

		count += loose_count;
	}

	*out = count;
	ret = 0;

out:
	return ret;
}

/*
 * Accumulate this files source's local packfile layout into `report` (the
 * "git count-objects -v" pack columns). Only local packs are reported, matching
 * the dumb-transport view; packs borrowed from alternates are excluded.
 */
static void odb_source_files_count_packs(struct odb_source *source,
					 struct odb_pack_report *report)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct packfile_list_entry *entry;

	for (entry = packfile_store_get_packs(files->packed); entry;
	     entry = entry->next) {
		struct packed_git *p = entry->pack;

		if (!p->pack_local)
			continue;
		if (open_pack_index(p))
			continue;
		report->objects += p->num_objects;
		report->size += p->pack_size + p->index_size;
		report->packs++;
	}
}

/*
 * Count this files source's loose objects by delegating to its loose sub-source
 * (which honors ODB_COUNT_OBJECTS_APPROXIMATE by sampling a shard and scaling).
 */
static int odb_source_files_count_loose_objects(struct odb_source *source,
						enum odb_count_objects_flags flags,
						unsigned long *out)
{
	struct odb_source_files *files = odb_source_files_downcast(source);

	return odb_source_count_objects(&files->loose->base, flags, out);
}

static int odb_source_files_for_each_loose_object(struct odb_source *source,
						  each_loose_object_fn obj_cb,
						  each_loose_cruft_fn cruft_cb,
						  each_loose_subdir_fn subdir_cb,
						  void *data)
{
	return for_each_loose_file_in_source(source, obj_cb, cruft_cb,
					     subdir_cb, data);
}

static int odb_source_files_find_abbrev_len(struct odb_source *source,
					    const struct object_id *oid,
					    unsigned min_len,
					    unsigned *out)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	unsigned len = min_len;
	int ret;

	ret = packfile_store_find_abbrev_len(files->packed, oid, len, &len);
	if (ret < 0)
		goto out;

	ret = odb_source_find_abbrev_len(&files->loose->base, oid, len, &len);
	if (ret < 0)
		goto out;

	*out = len;
	ret = 0;

out:
	return ret;
}

static int odb_source_files_freshen_object(struct odb_source *source,
					   const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	if (packfile_store_freshen_object(files->packed, oid) ||
	    odb_source_freshen_object(&files->loose->base, oid))
		return 1;
	return 0;
}

static int odb_source_files_write_object(struct odb_source *source,
					 const void *buf, unsigned long len,
					 enum object_type type,
					 struct object_id *oid,
					 struct object_id *compat_oid,
					 enum odb_write_object_flags flags)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	return odb_source_write_object(&files->loose->base, buf, len, type,
				       oid, compat_oid, flags);
}

static int odb_source_files_write_object_stream(struct odb_source *source,
						struct odb_write_stream *stream,
						size_t len,
						struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	return odb_source_write_object_stream(&files->loose->base, stream, len, oid);
}

static int odb_source_files_install_loose_object(struct odb_source *source,
						 const char *temp_path,
						 const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct strbuf filename = STRBUF_INIT;
	int ret;

	/*
	 * The tempfile is already a complete loose object, so rename it into the
	 * loose layout (zero-copy). finalize_object_file() does not create the fanout
	 * directory, so create it first, as write_loose_object() does.
	 */
	odb_loose_path(files->loose, &filename, oid);
	if (safe_create_leading_directories_const(source->odb->repo, filename.buf)) {
		strbuf_release(&filename);
		return error(_("unable to create directory for %s"), oid_to_hex(oid));
	}
	ret = finalize_object_file(source->odb->repo, temp_path, filename.buf);
	strbuf_release(&filename);
	return ret;
}

static void odb_source_files_note_received_pack(struct odb_source *source,
						struct packed_git *p)
{
	packfile_store_add_pack(odb_source_files_downcast(source)->packed, p);
}

static void odb_source_files_note_indexed_pack(struct odb_source *source,
					       const char *idx_path)
{
	packfile_store_load_pack(odb_source_files_downcast(source)->packed,
				 idx_path, 0);
}

static int odb_source_files_begin_transaction(struct odb_source *source,
					      struct odb_transaction **out)
{
	struct odb_transaction *tx = odb_transaction_files_begin(source);
	if (!tx)
		return -1;
	*out = tx;
	return 0;
}

static int odb_source_files_read_alternates(struct odb_source *source,
					    struct strvec *out)
{
	struct strbuf buf = STRBUF_INIT;
	char *path;

	path = xstrfmt("%s/info/alternates", source->path);
	if (strbuf_read_file(&buf, path, 1024) < 0) {
		warn_on_fopen_errors(path);
		free(path);
		return 0;
	}
	parse_alternates(buf.buf, '\n', source->path, out);

	strbuf_release(&buf);
	free(path);
	return 0;
}

static int odb_source_files_write_alternate(struct odb_source *source,
					    const char *alternate)
{
	struct lock_file lock = LOCK_INIT;
	char *path = xstrfmt("%s/%s", source->path, "info/alternates");
	FILE *in, *out;
	int found = 0;
	int ret;

	hold_lock_file_for_update(&lock, path, LOCK_DIE_ON_ERROR);
	out = fdopen_lock_file(&lock, "w");
	if (!out) {
		ret = error_errno(_("unable to fdopen alternates lockfile"));
		goto out;
	}

	in = fopen(path, "r");
	if (in) {
		struct strbuf line = STRBUF_INIT;

		while (strbuf_getline(&line, in) != EOF) {
			if (!strcmp(alternate, line.buf)) {
				found = 1;
				break;
			}
			fprintf_or_die(out, "%s\n", line.buf);
		}

		strbuf_release(&line);
		fclose(in);
	} else if (errno != ENOENT) {
		ret = error_errno(_("unable to read alternates file"));
		goto out;
	}

	if (found) {
		rollback_lock_file(&lock);
	} else {
		fprintf_or_die(out, "%s\n", alternate);
		if (commit_lock_file(&lock)) {
			ret = error_errno(_("unable to move new alternates file into place"));
			goto out;
		}
	}

	ret = 0;

out:
	free(path);
	return ret;
}

static int files_ingest_receive_object(struct odb_pack_ingest *ingest UNUSED,
				       const struct object_id *oid UNUSED,
				       enum object_type type UNUSED,
				       const void *data UNUSED,
				       unsigned long size UNUSED)
{
	/*
	 * The received pack is kept as-is and becomes part of this source's
	 * storage, so there is nothing to do per object; the pack is installed
	 * wholesale at commit time.
	 */
	return 0;
}

/*
 * Explode a received pack into loose objects (used for a small received pack
 * not worth keeping as a packfile) and return whether unpack-objects succeeded.
 * unpack-objects writes through the object database, so a non-files primary
 * never needs this: it explodes the pack via the generic ingest session.
 */
static int files_loosen_received_pack(const char *pack_tmp_name)
{
	struct child_process unpack = CHILD_PROCESS_INIT;
	int fd = open(pack_tmp_name, O_RDONLY);

	if (fd < 0)
		return -1;
	unpack.in = fd;
	unpack.git_cmd = 1;
	strvec_pushl(&unpack.args, "unpack-objects", "-q", NULL);
	return run_command(&unpack);
}

static struct packed_git *files_ingest_commit(struct odb_pack_ingest *ingest,
					      const struct odb_received_pack *pack,
					      struct strbuf *report)
{
	struct odb_source_files *files = odb_source_files_downcast(ingest->source);
	struct repository *repo = ingest->source->odb->repo;
	const char *final_pack_name = pack->pack_name;
	const char *final_index_name = pack->index_name;
	const char *final_rev_index_name = pack->rev_index_name;
	const char *curr_index_name;
	const char *curr_rev_index_name = NULL;
	const char *report_token;
	struct packed_git *installed;

	/*
	 * A small received pack is stored as loose objects rather than kept as a
	 * packfile, so tiny packs do not accumulate (the caller opts in via
	 * loosen_if_at_most). The temporary pack is exploded and discarded; no
	 * packfile is installed. If exploding fails, fall through and keep it.
	 */
	if (ingest->loosen_if_at_most &&
	    pack->nr_objects <= ingest->loosen_if_at_most &&
	    !files_loosen_received_pack(pack->pack_tmp_name)) {
		unlink(pack->pack_tmp_name);
		if (report)
			strbuf_addf(report, "pack\t%0*d\n",
				    (int)repo->hash_algo->hexsz, 0);
		return NULL;
	}

	curr_index_name = write_idx_file(repo, final_index_name, pack->objects,
					 pack->nr_objects, pack->idx_opts,
					 pack->pack_hash);
	if (pack->idx_opts->flags & WRITE_REV)
		curr_rev_index_name = write_rev_file(repo, final_rev_index_name,
						     pack->objects,
						     pack->nr_objects,
						     pack->pack_hash,
						     pack->idx_opts->flags);

	report_token = install_packfile(repo, pack->pack_hash,
					&final_pack_name, pack->pack_tmp_name,
					&final_index_name, curr_index_name,
					&final_rev_index_name, curr_rev_index_name,
					pack->keep_msg, pack->promisor_msg, 1);

	/*
	 * install_packfile() only fills *final_index_name with a stable string
	 * when the caller supplied the name; when it had to derive one it points
	 * into a strbuf it then released, so it dangles here. Load from the
	 * caller-stable name, recomputing the canonical one from the hash (the
	 * same derivation finalize_pack_component() used) when none was given.
	 */
	if (pack->index_name) {
		installed = packfile_store_load_pack(files->packed, pack->index_name, 0);
	} else {
		struct strbuf idx = STRBUF_INIT;
		odb_pack_name(repo, &idx, pack->pack_hash, "idx");
		installed = packfile_store_load_pack(files->packed, idx.buf, 0);
		strbuf_release(&idx);
	}

	if (report)
		strbuf_addf(report, "%s\t%s\n", report_token,
			    hash_to_hex_algop(pack->pack_hash, repo->hash_algo));

	/* write_idx_file returns a freshly allocated name when none was given. */
	if (!pack->index_name)
		free((char *)curr_index_name);
	free((char *)curr_rev_index_name);

	return installed;
}

static struct odb_pack_ingest *odb_source_files_begin_pack_ingest(struct odb_source *source)
{
	struct odb_pack_ingest *ingest;

	CALLOC_ARRAY(ingest, 1);
	ingest->source = source;
	ingest->receive_object = files_ingest_receive_object;
	ingest->commit = files_ingest_commit;

	return ingest;
}

/*
 * A3 maintenance: the files object store optimizes itself by repacking, and
 * decides whether it needs to via the same loose-object / pack-count heuristics
 * "git gc --auto" applies. These run against this source's own loose store and
 * packs (never a downcast of whatever the primary happens to be), so they stay
 * correct when the primary is a non-files
 * backend.
 */

static int files_too_many_loose_objects(struct odb_source_files *files,
					 int auto_threshold)
{
	/*
	 * Round up to the next multiple of 256, retained from the historical
	 * "git gc --auto" behavior.
	 */
	int limit = DIV_ROUND_UP(auto_threshold, 256) * 256;
	unsigned long loose_count;

	if (odb_source_count_objects(&files->loose->base,
				     ODB_COUNT_OBJECTS_APPROXIMATE,
				     &loose_count) < 0)
		return 0;
	return loose_count > (unsigned long)limit;
}

static int files_too_many_packs(struct odb_source_files *files, int pack_limit)
{
	struct packfile_list_entry *entry;
	int cnt = 0;

	if (pack_limit <= 0)
		return 0;

	packfile_store_prepare(files->packed);
	for (entry = packfile_store_get_packs(files->packed); entry;
	     entry = entry->next) {
		struct packed_git *p = entry->pack;
		if (!p->pack_local || p->pack_keep)
			continue;
		cnt++;
	}
	return pack_limit < cnt;
}

static struct packed_git *files_find_base_packs(struct odb_source_files *files,
						struct string_list *packs,
						unsigned long limit)
{
	struct packfile_list_entry *entry;
	struct packed_git *base = NULL;

	packfile_store_prepare(files->packed);
	for (entry = packfile_store_get_packs(files->packed); entry;
	     entry = entry->next) {
		struct packed_git *p = entry->pack;
		if (!p->pack_local || p->is_cruft)
			continue;
		if (limit) {
			/* pack_size is off_t but never negative for a real pack. */
			if ((uintmax_t)p->pack_size >= limit)
				string_list_append(packs, p->pack_name);
		} else if (!base || base->pack_size < p->pack_size) {
			base = p;
		}
	}
	if (base)
		string_list_append(packs, base->pack_name);
	return base;
}

static int odb_source_files_optimize_required(struct odb_source *source,
					      struct odb_optimize_opts *opts,
					      bool *required)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct repository *repo = source->odb->repo;
	int auto_threshold = 6700;
	int auto_pack_limit = 50;

	*required = false;

	/*
	 * A geometric optimization is worthwhile exactly when at least one pack
	 * falls below the geometric split point, i.e. there are packs to roll
	 * up. (The caller separately weighs loose objects.) The geometry is a
	 * files-pack concept, computed here in the files source.
	 */
	if (opts->flags & ODB_OPTIMIZE_GEOMETRIC) {
		struct pack_geometry geometry = {
			.split_factor = opts->geometric_split_factor,
		};
		struct pack_objects_args po_args = { .local = 1 };
		struct existing_packs existing = EXISTING_PACKS_INIT;
		struct string_list geom_kept = STRING_LIST_INIT_DUP;

		existing.repo = repo;
		existing_packs_collect(&existing, &geom_kept);
		pack_geometry_init(&geometry, &existing, &po_args);
		pack_geometry_split(&geometry);
		*required = geometry.split > 0;
		pack_geometry_release(&geometry);
		existing_packs_release(&existing);
		string_list_clear(&geom_kept, 0);
		return 0;
	}

	/*
	 * Incremental-repack maintenance is worthwhile once enough packs sit
	 * outside the multi-pack-index, gated on core.multiPackIndex. The midx is
	 * a files-pack concept, so the count is taken here in the files source.
	 */
	if (opts->flags & ODB_OPTIMIZE_MIDX) {
		int incremental_repack_auto_limit = 10;
		int count = 0;
		struct packed_git *p;

		prepare_repo_settings(repo);
		if (!repo->settings.core_multi_pack_index)
			return 0;

		repo_config_get_int(repo, "maintenance.incremental-repack.auto",
				    &incremental_repack_auto_limit);
		if (!incremental_repack_auto_limit)
			return 0;
		if (incremental_repack_auto_limit < 0) {
			*required = true;
			return 0;
		}

		odb_for_each_files_pack(repo->objects, p) {
			if (count >= incremental_repack_auto_limit)
				break;
			if (!p->multi_pack_index)
				count++;
		}
		*required = count >= incremental_repack_auto_limit;
		return 0;
	}

	repo_config_get_int(repo, "gc.auto", &auto_threshold);
	repo_config_get_int(repo, "gc.autopacklimit", &auto_pack_limit);

	/* gc.auto <= 0 disables automatic maintenance entirely. */
	if (auto_threshold <= 0)
		return 0;

	if (files_too_many_packs(files, auto_pack_limit) ||
	    files_too_many_loose_objects(files, auto_threshold))
		*required = true;
	return 0;
}

/*
 * Multi-pack-index maintenance (ODB_OPTIMIZE_MIDX), the in-process equivalent
 * of the "incremental-repack" task's "git multi-pack-index write/expire/repack"
 * sequence: write the midx over our packs, expire the ones it makes redundant,
 * then repack small packs up to an automatically chosen batch size. The midx is
 * a files-pack concept, so this lives in the files source.
 */
static int odb_source_files_optimize_midx(struct odb_source *source,
					  struct odb_optimize_opts *opts)
{
	struct repository *repo = source->odb->repo;
	const char *progress = (opts->flags & ODB_OPTIMIZE_QUIET) ?
		"--no-progress" : "--progress";
	struct child_process write_cmd = CHILD_PROCESS_INIT;
	struct child_process expire_cmd = CHILD_PROCESS_INIT;
	struct child_process repack_cmd = CHILD_PROCESS_INIT;
	off_t max_size = 0, second_largest_size = 0, batch_size;
	struct packed_git *p;

	prepare_repo_settings(repo);
	if (!repo->settings.core_multi_pack_index) {
		warning(_("skipping incremental-repack task because core.multiPackIndex is disabled"));
		return 0;
	}

	/*
	 * The library midx write/expire/repack routines close packs the shared
	 * in-memory object store still references, so they are not safe to call
	 * back-to-back in one process. Run them as the git-multi-pack-index
	 * sub-commands the maintenance task always used (each a fresh process),
	 * the way repack_run() drives git-pack-objects for the other modes.
	 */
	write_cmd.git_cmd = 1;
	strvec_pushl(&write_cmd.args, "multi-pack-index", "write", progress, NULL);
	if (run_command(&write_cmd))
		return error(_("failed to write multi-pack-index"));

	expire_cmd.git_cmd = 1;
	expire_cmd.odb_to_close = repo->objects;
	strvec_pushl(&expire_cmd.args, "multi-pack-index", "expire", progress, NULL);
	if (run_command(&expire_cmd))
		return error(_("'git multi-pack-index expire' failed"));

	/*
	 * Choose a batch size one larger than the second-largest pack so the
	 * repack rolls up at least the two smallest of three-or-more packs (the
	 * old get_auto_pack_size() heuristic), capped at 2 GiB. Reprepare first,
	 * since expiry may have dropped packs.
	 */
	odb_reprepare(repo->objects);
	odb_for_each_files_pack(repo->objects, p) {
		if (p->pack_size > max_size) {
			second_largest_size = max_size;
			max_size = p->pack_size;
		} else if (p->pack_size > second_largest_size) {
			second_largest_size = p->pack_size;
		}
	}
	batch_size = second_largest_size + 1;
	if (batch_size > INT32_MAX)
		batch_size = INT32_MAX;

	repack_cmd.git_cmd = 1;
	repack_cmd.odb_to_close = repo->objects;
	strvec_pushl(&repack_cmd.args, "multi-pack-index", "repack", progress, NULL);
	strvec_pushf(&repack_cmd.args, "--batch-size=%"PRIuMAX,
		     (uintmax_t)batch_size);
	if (run_command(&repack_cmd))
		return error(_("'git multi-pack-index repack' failed"));

	return 0;
}

static int odb_source_files_optimize(struct odb_source *source,
				     struct odb_optimize_opts *opts)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct repository *repo = source->odb->repo;
	struct repack_opts ropts = REPACK_OPTS_INIT;
	struct string_list keep_pack = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	int aggressive_depth = 50, aggressive_window = 250;
	int auto_threshold = 6700, auto_pack_limit = 50;
	unsigned long big_pack_threshold = 0;
	unsigned long max_delta_cache_size = DEFAULT_DELTA_CACHE_SIZE;
	unsigned long delta_base_cache_limit = DEFAULT_DELTA_BASE_CACHE_LIMIT;
	char *repack_filter = NULL, *repack_filter_to = NULL;
	int all_into_one = 0, do_repack = 1;
	int ret;

	/*
	 * "git repack" hands us its fully-parsed options (command line plus
	 * config), the way "git pack-refs" hands refs_optimize() its parsed
	 * options; repack with exactly those. The "git gc" path leaves this
	 * NULL and derives maintenance defaults from config below.
	 */
	if (opts->repack)
		return repack_run(repo, opts->repack);

	if (opts->flags & ODB_OPTIMIZE_MIDX)
		return odb_source_files_optimize_midx(source, opts);

	/* Mirror cmd_repack: the filter options must be initialized before use. */
	list_objects_filter_init(&ropts.po_args.filter_options);

	/* Pick up repack.* / pack.writeBitmaps the way "git repack" would. */
	repo_config(repo, repack_config, &ropts);

	repo_config_get_int(repo, "gc.aggressivedepth", &aggressive_depth);
	repo_config_get_int(repo, "gc.aggressivewindow", &aggressive_window);
	repo_config_get_int(repo, "gc.auto", &auto_threshold);
	repo_config_get_int(repo, "gc.autopacklimit", &auto_pack_limit);
	repo_config_get_ulong(repo, "gc.bigpackthreshold", &big_pack_threshold);
	repo_config_get_ulong(repo, "pack.deltacachesize", &max_delta_cache_size);
	repo_config_get_ulong(repo, "core.deltabasecachelimit", &delta_base_cache_limit);
	repo_config_get_string(repo, "gc.repackfilter", &repack_filter);
	repo_config_get_string(repo, "gc.repackfilterto", &repack_filter_to);

	/* "git gc" always repacks the local store and removes redundant packs. */
	ropts.po_args.local = 1;
	ropts.delete_redundant = !!(opts->flags & ODB_OPTIMIZE_PRUNE);
	ropts.po_args.quiet = !!(opts->flags & ODB_OPTIMIZE_QUIET);
	if (opts->flags & ODB_OPTIMIZE_AGGRESSIVE) {
		ropts.po_args.no_reuse_delta = 1;
		if (aggressive_depth > 0)
			ropts.po_args.depth = xstrfmt("%d", aggressive_depth);
		if (aggressive_window > 0)
			ropts.po_args.window = xstrfmt("%d", aggressive_window);
	}

	/*
	 * Roll packs up into a geometric progression ("git repack
	 * --geometric"). The geometry is a files-pack concept, so it is computed
	 * here in the files source. When some packs sit above the split point we
	 * merge only the suffix below it; when every pack would be merged we
	 * instead fall through to a full all-into-one repack (the mapping below),
	 * which additionally lets unreachable and cruft objects be expired.
	 */
	if (opts->flags & ODB_OPTIMIZE_GEOMETRIC) {
		struct pack_geometry geometry = {
			.split_factor = opts->geometric_split_factor,
		};
		struct existing_packs existing = EXISTING_PACKS_INIT;
		struct string_list geom_kept = STRING_LIST_INIT_DUP;

		prepare_repo_settings(repo);
		if (repo->settings.core_multi_pack_index)
			ropts.write_midx = REPACK_WRITE_MIDX_DEFAULT;

		existing.repo = repo;
		existing_packs_collect(&existing, &geom_kept);
		pack_geometry_init(&geometry, &existing, &ropts.po_args);
		pack_geometry_split(&geometry);
		if (geometry.split < geometry.pack_nr)
			ropts.split_factor = opts->geometric_split_factor;
		else
			all_into_one = 1;
		pack_geometry_release(&geometry);
		existing_packs_release(&existing);
		string_list_clear(&geom_kept, 0);
	} else if (opts->flags & ODB_OPTIMIZE_AUTO) {
		if (files_too_many_packs(files, auto_pack_limit)) {
			all_into_one = 1;
			if (big_pack_threshold) {
				files_find_base_packs(files, &keep_pack, big_pack_threshold);
				if (keep_pack.nr >= (size_t)auto_pack_limit) {
					string_list_clear(&keep_pack, 0);
					files_find_base_packs(files, &keep_pack, 0);
				}
			} else {
				struct packed_git *base =
					files_find_base_packs(files, &keep_pack, 0);
				uint64_t mem_have = repack_total_ram();
				uint64_t mem_want = repack_estimate_memory(repo, base,
									   delta_base_cache_limit,
									   max_delta_cache_size);
				/*
				 * Only allow 1/2 of memory for pack-objects; if
				 * that is enough, repack everything rather than
				 * keeping the base pack.
				 */
				if (!mem_have || mem_want < mem_have / 2)
					string_list_clear(&keep_pack, 0);
			}
		} else if (files_too_many_loose_objects(files, auto_threshold)) {
			/* Incremental: leave existing packs, drop bitmaps. */
			ropts.write_bitmaps = 0;
		} else {
			do_repack = 0;
		}
	} else {
		all_into_one = 1;
		/*
		 * --no-keep-largest-pack keeps nothing, overriding any
		 * gc.bigPackThreshold the source would otherwise honor.
		 */
		if (opts->flags & ODB_OPTIMIZE_NO_KEEP_LARGEST_PACK)
			; /* keep no base pack */
		else if (opts->flags & ODB_OPTIMIZE_KEEP_LARGEST_PACK)
			files_find_base_packs(files, &keep_pack, 0);
		else if (big_pack_threshold)
			files_find_base_packs(files, &keep_pack, big_pack_threshold);
	}

	if (all_into_one) {
		const char *prune_expire = opts->prune_expire;

		/*
		 * Mirror "git gc"'s repack-mode choice: -a when pruning
		 * immediately, --cruft to keep unreachable objects in a cruft
		 * pack, otherwise -A to loosen them.
		 */
		if (prune_expire && !strcmp(prune_expire, "now") &&
		    !((opts->flags & ODB_OPTIMIZE_CRUFT) && opts->expire_to)) {
			ropts.pack_everything = ALL_INTO_ONE;
		} else if (opts->flags & ODB_OPTIMIZE_CRUFT) {
			ropts.pack_everything = PACK_CRUFT;
			ropts.cruft_expiration = prune_expire;
			ropts.cruft_po_args.max_pack_size = opts->max_cruft_size;
			ropts.expire_to = opts->expire_to;
		} else {
			ropts.pack_everything = ALL_INTO_ONE | LOOSEN_UNREACHABLE;
			ropts.unpack_unreachable = prune_expire;
		}

		if (repack_filter && *repack_filter)
			parse_list_objects_filter(&ropts.po_args.filter_options,
						  repack_filter);
		ropts.filter_to = repack_filter_to;

		/* repack matches keep-packs by basename, as "git gc" passed them. */
		for_each_string_list_item(item, &keep_pack)
			string_list_append(&ropts.keep_pack_list,
					   basename(item->string));
	}

	if (do_repack)
		ret = repack_run(repo, &ropts);
	else
		ret = 0;

	string_list_clear(&keep_pack, 0);
	free(repack_filter);
	free(repack_filter_to);
	repack_opts_release(&ropts);
	return ret;
}

/*
 * A3 verify: the storage side of "git fsck" for the files backend. We scan this
 * source's loose objects, reading each with its hash-path integrity check and
 * handing the raw contents to `cb` for content checking; cruft in the object
 * directory is reported too. Packs are not scanned here: "git fsck" verifies
 * them directly with verify_pack() over the repository's packs, which is
 * backend-agnostic. `o` is unused because every storage-format problem at this
 * layer is reported with error() exactly as the historical loose scan did.
 *
 * Lifted from builtin/fsck.c (fsck_source / fsck_loose / fsck_cruft); the
 * per-object content fsck stays in the caller's `cb` (fsck_obj_buffer).
 */
struct files_verify_cb {
	struct repository *repo;
	odb_verify_cb cb;
	void *cb_data;
	int ret;
};

static int files_verify_loose(const struct object_id *oid, const char *path,
			      void *data)
{
	struct files_verify_cb *vd = data;
	enum object_type type = OBJ_NONE;
	unsigned long size;
	void *contents = NULL;
	int eaten = 0;
	struct object_info oi = OBJECT_INFO_INIT;
	struct object_id real_oid = *null_oid(vd->repo->hash_algo);

	oi.sizep = &size;
	oi.typep = &type;

	if (read_loose_object(vd->repo, path, oid, &real_oid, &contents, &oi) < 0) {
		if (contents && !oideq(&real_oid, oid))
			error(_("%s: hash-path mismatch, found at: %s"),
			      oid_to_hex(&real_oid), path);
		else
			error(_("%s: object corrupt or missing: %s"),
			      oid_to_hex(oid), path);
		vd->ret = -1;
		free(contents);
		return 0; /* keep checking other objects */
	}

	if (!contents && type != OBJ_BLOB)
		BUG("read_loose_object streamed a non-blob");

	if (vd->cb && vd->cb(oid, type, size, contents, &eaten, vd->cb_data))
		vd->ret = -1;

	if (!eaten)
		free(contents);
	return 0; /* keep checking other objects, even if we saw an error */
}

static int files_verify_cruft(const char *basename, const char *path,
			      void *data UNUSED)
{
	if (!starts_with(basename, "tmp_obj_"))
		fprintf_ln(stderr, _("bad sha1 file: %s"), path);
	return 0;
}

static int odb_source_files_verify(struct odb_source *source,
				   struct fsck_options *o,
				   odb_verify_cb cb, void *cb_data)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct files_verify_cb vd = {
		.repo = source->odb->repo,
		.cb = cb,
		.cb_data = cb_data,
		.ret = 0,
	};
	struct packfile_list_entry *e;

	/*
	 * Loose objects: hash-path check + content via cb (the caller's
	 * per-object fsck handler, which also drives progress). Skipped for a
	 * format-only verify (no cb, e.g. fsck --connectivity-only), which checks
	 * only the storage formats (rev-index/bitmap/midx) below.
	 */
	if (cb)
		for_each_loose_file_in_source(source, files_verify_loose,
					      files_verify_cruft, NULL, &vd);

	/*
	 * This source's own packs. The agnostic odb_verify() fan visits every
	 * source, so this method validates only its own packs, with no
	 * cross-source walk here: validate each pack's reverse index always,
	 * and under --full also content-verify every packed object through the
	 * same cb. odb_verify_cb matches verify_fn, so verify_pack() feeds the
	 * cb directly, and the cb drives progress, so no progress bar is needed
	 * in this layer.
	 */
	for (e = packfile_store_get_packs(files->packed); e; e = e->next) {
		struct packed_git *p = e->pack;
		int load_error;

		if (open_pack_index(p))
			continue;

		load_error = load_pack_revindex_from_disk(p);
		if (load_error < 0) {
			error(_("unable to load rev-index for pack '%s'"),
			      p->pack_name);
			vd.ret = -1;
		} else if (!load_error && !load_pack_revindex(vd.repo, p) &&
			   verify_pack_revindex(p)) {
			error(_("invalid rev-index for pack '%s'"),
			      p->pack_name);
			vd.ret = -1;
		}

		if (cb && o && o->check_full &&
		    verify_pack(vd.repo, p, cb, cb_data, NULL, 0))
			vd.ret = -1;
	}

	/*
	 * This source's multi-pack-index, when the repository uses one. Folded
	 * in from fsck's old per-source "git multi-pack-index verify" spawn so
	 * fsck dispatches midx verification through the same odb_verify fan; a
	 * non-files source has none and keeps the no-op verify default.
	 */
	prepare_repo_settings(vd.repo);
	if (vd.repo->settings.core_multi_pack_index &&
	    verify_midx_file(source, (o && o->progress) ? MIDX_PROGRESS : 0))
		vd.ret = -1;

	return vd.ret;
}

/*
 * A3: report whether the object is kept (in a ".keep" pack, excluded from
 * repacking) in this files source. The cross-backend has_object_kept_pack()
 * dispatches here; non-files sources have no kept packs (the no-op default).
 */
static int odb_source_files_is_object_kept(struct odb_source *source,
					   const struct object_id *oid,
					   unsigned flags)
{
	struct odb_source_files *files = odb_source_files_downcast(source);

	return packfile_store_has_kept_object(files->packed, oid, flags);
}

static int odb_source_files_has_received_pack(struct odb_source *source,
					      const unsigned char *hash)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct packfile_list_entry *e;

	for (e = packfile_store_get_packs(files->packed); e; e = e->next)
		if (hasheq(e->pack->hash, hash, source->odb->repo->hash_algo))
			return 1;
	return 0;
}

static int odb_source_files_cruft_object_preserved(struct odb_source *source,
						   const struct object_id *oid,
						   unsigned flags,
						   uint32_t mtime)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct packed_git **cache = packfile_store_get_kept_pack_cache(files->packed, flags);

	for (; *cache; cache++) {
		struct packed_git *p = *cache;
		off_t ofs;
		uint32_t candidate_mtime;

		ofs = find_pack_entry_one(oid, p);
		if (!ofs)
			continue;

		/*
		 * A copy in a non-cruft pack preserves the object regardless of
		 * mtime, since it lives outside any cruft pack.
		 */
		if (!p->is_cruft)
			return 1;

		/*
		 * In a cruft pack, read the object's recorded mtime (falling
		 * back to the pack's mtime when the .mtimes data cannot be
		 * loaded); the copy survives when that mtime is at least the
		 * candidate.
		 */
		if (!load_pack_mtimes(p)) {
			uint32_t pos;
			if (offset_to_pack_pos(p, ofs, &pos) < 0)
				continue;
			candidate_mtime = nth_packed_mtime(p, pos);
		} else {
			candidate_mtime = p->mtime;
		}

		if (mtime <= candidate_mtime)
			return 1;
	}

	return 0;
}

/*
 * Report whether the object lives in a ".promisor" pack of this files source
 * (an object received from a promisor remote). The cross-backend
 * odb_is_promisor_object() dispatches here; non-files sources report via their
 * own backend (or the not-promisor no-op default).
 */
static int odb_source_files_is_promisor_object(struct odb_source *source,
					       const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct packfile_list_entry *entry;

	for (entry = packfile_store_get_packs(files->packed); entry;
	     entry = entry->next) {
		struct packed_git *p = entry->pack;

		if (!p->pack_promisor)
			continue;
		if (find_pack_entry_one(oid, p))
			return 1;
	}
	return 0;
}

/*
 * Record the given objects as promisor objects (the mark_objects_promisor
 * vtable method). The files source marks promisor objects by gathering them
 * into a ".promisor" packfile; objects that already live in a promisor pack
 * need no further marking and are skipped. This is reached for the local
 * objects a just-ingested promisor pack references (the received pack itself
 * already got its ".promisor" file at ingest time), retaining them against
 * pruning even though nothing local points at them yet.
 *
 * Unlike odb_source_files_optimize(), which repacks in-process through the
 * repack library, this spawns "git pack-objects" the way index-pack always
 * has, to reuse its --exclude-promisor-objects-best-effort reachability walk.
 */
static int odb_source_files_mark_objects_promisor(struct odb_source *source,
						   struct oidset *oids)
{
	struct repository *repo = source->odb->repo;
	struct child_process cmd = CHILD_PROCESS_INIT;
	FILE *out;
	struct strbuf line = STRBUF_INIT;
	struct oidset_iter iter;
	struct object_id *oid;
	char *base_name = NULL;

	oidset_iter_init(oids, &iter);
	while ((oid = oidset_iter_next(&iter))) {
		struct object_info info = OBJECT_INFO_INIT;
		if (odb_read_object_info_extended(source->odb, oid, &info, 0))
			/* Missing; assume it is a promisor object */
			continue;
		if (info.whence == OI_PACKED && info.u.packed.pack->pack_promisor)
			continue;

		if (!cmd.args.nr) {
			base_name = mkpathdup("%s/pack/pack",
					      repo_get_object_directory(repo));
			strvec_push(&cmd.args, "pack-objects");
			strvec_push(&cmd.args,
				    "--exclude-promisor-objects-best-effort");
			strvec_push(&cmd.args, base_name);
			cmd.git_cmd = 1;
			cmd.in = -1;
			cmd.out = -1;
			if (start_command(&cmd))
				die(_("could not start pack-objects to repack local links"));
		}

		if (write_in_full(cmd.in, hash_to_hex_algop(oid->hash, repo->hash_algo),
				  repo->hash_algo->hexsz) < 0 ||
		    write_in_full(cmd.in, "\n", 1) < 0)
			die(_("failed to feed local object to pack-objects"));
	}

	if (!cmd.args.nr)
		return 0;

	close(cmd.in);

	out = xfdopen(cmd.out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		unsigned char binary[GIT_MAX_RAWSZ];
		if (line.len != repo->hash_algo->hexsz ||
		    !hex_to_bytes(binary, line.buf, line.len))
			die(_("index-pack: Expecting full hex object ID lines only from pack-objects."));

		/*
		 * pack-objects creates the .pack and .idx files, but not the
		 * .promisor file. Create the .promisor file, which is empty.
		 */
		write_special_file(repo, "promisor", "", NULL, binary, NULL);
	}

	fclose(out);
	if (finish_command(&cmd))
		die(_("could not finish pack-objects to repack local links"));
	strbuf_release(&line);
	free(base_name);
	return 0;
}

struct files_prune_data {
	struct oidset *prune;
	timestamp_t expire;
	int dry_run;
	struct oidset *removed;
};

static int files_prune_loose_object(const struct object_id *oid,
				    const char *path, void *data)
{
	struct files_prune_data *d = data;
	struct stat st;

	if (!oidset_contains(d->prune, oid))
		return 0;
	if (lstat(path, &st))
		return 0; /* already gone */
	if ((timestamp_t)st.st_mtime > d->expire)
		return 0; /* written too recently to prune safely */
	if (d->removed)
		oidset_insert(d->removed, oid);
	if (!d->dry_run)
		unlink_or_warn(path);
	return 0;
}

static int odb_source_files_remove_objects(struct odb_source *source,
					   struct oidset *prune,
					   timestamp_t expire,
					   int dry_run, struct oidset *removed)
{
	struct files_prune_data d = { prune, expire, dry_run, removed };

	/*
	 * The loose half of "git prune": git has filtered `prune` to objects it
	 * found unreachable, so we unlink the loose ones written at or before
	 * `expire` (matching the loose mtime, so a just-landed object is kept),
	 * recording each in `removed` for the caller's report. Packed unreachable
	 * objects are dropped by repack, not here, so we only walk the loose tier
	 * (which is also why prune's report must come from this set, not the whole
	 * unreachable candidate set). With dry_run set we record without unlinking.
	 */
	return for_each_loose_file_in_source(source, files_prune_loose_object,
					     NULL, NULL, &d);
}

struct files_prune_cruft_data {
	struct odb_source *source;
	timestamp_t expire;
	int dry_run;
	int verbose;
};

static void files_prune_tmp_file(const char *fullpath,
				 struct files_prune_cruft_data *d)
{
	struct stat st;

	if (lstat(fullpath, &st))
		return;
	if ((timestamp_t)st.st_mtime > d->expire)
		return;
	if (S_ISDIR(st.st_mode)) {
		if (d->dry_run || d->verbose)
			printf("Removing stale temporary directory %s\n", fullpath);
		if (!d->dry_run) {
			struct strbuf remove_dir_buf = STRBUF_INIT;

			strbuf_addstr(&remove_dir_buf, fullpath);
			remove_dir_recursively(&remove_dir_buf, 0);
			strbuf_release(&remove_dir_buf);
		}
	} else {
		if (d->dry_run || d->verbose)
			printf("Removing stale temporary file %s\n", fullpath);
		if (!d->dry_run)
			unlink_or_warn(fullpath);
	}
}

static int files_prune_cruft_cb(const char *basename, const char *path,
				void *data)
{
	struct files_prune_cruft_data *d = data;

	if (starts_with(basename, "tmp_obj_"))
		files_prune_tmp_file(path, d);
	else
		fprintf(stderr, "bad sha1 file: %s\n", path);
	return 0;
}

static int files_prune_subdir_cb(unsigned int nr UNUSED, const char *path,
				 void *data)
{
	struct files_prune_cruft_data *d = data;

	if (!d->dry_run)
		rmdir(path);
	return 0;
}

static int files_prune_redundant_packed(const struct object_id *oid,
					const char *path, void *data)
{
	struct files_prune_cruft_data *d = data;

	if (!has_object_pack(d->source->odb->repo, oid))
		return 0;
	if (d->dry_run)
		printf("rm -f %s\n", path);
	else
		unlink_or_warn(path);
	return 0;
}

static void files_remove_temporary_files(const char *path,
					 struct files_prune_cruft_data *d)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir(path);
	if (!dir) {
		if (errno != ENOENT)
			fprintf(stderr, "Unable to open directory %s: %s\n",
				path, strerror(errno));
		return;
	}
	while ((de = readdir(dir)) != NULL)
		if (starts_with(de->d_name, "tmp_"))
			files_prune_tmp_file(mkpath("%s/%s", path, de->d_name), d);
	closedir(dir);
}

/*
 * The files source's multi-pack-index operations: it has the ".pack" layout a
 * midx indexes, so each runs the midx machinery directly. compact resolves its
 * endpoints from this source's own midx chain (the work the builtin used to do
 * before it dispatched).
 */
static int odb_source_files_multi_pack_index_write(struct odb_source *source,
						   struct string_list *packs_to_include,
						   const char *preferred_pack_name,
						   const char *refs_snapshot,
						   const char *incremental_base,
						   unsigned flags)
{
	if (packs_to_include)
		return write_midx_file_only(source, packs_to_include,
					    preferred_pack_name, refs_snapshot,
					    incremental_base, flags);
	return write_midx_file(source, preferred_pack_name, refs_snapshot, flags);
}

static int odb_source_files_multi_pack_index_compact(struct odb_source *source,
						     const char *from_checksum,
						     const char *to_checksum,
						     const char *incremental_base,
						     unsigned flags)
{
	struct multi_pack_index *m = get_multi_pack_index(source);
	struct multi_pack_index *cur, *from_midx = NULL, *to_midx = NULL;

	for (cur = m; cur && !(from_midx && to_midx); cur = cur->base_midx) {
		const char *midx_csum = midx_get_checksum_hex(cur);

		if (!from_midx && !strcmp(midx_csum, from_checksum))
			from_midx = cur;
		if (!to_midx && !strcmp(midx_csum, to_checksum))
			to_midx = cur;
	}

	if (!from_midx)
		die(_("could not find MIDX: %s"), from_checksum);
	if (!to_midx)
		die(_("could not find MIDX: %s"), to_checksum);
	if (from_midx == to_midx)
		die(_("MIDX compaction endpoints must be unique"));

	for (m = from_midx; m; m = m->base_midx) {
		if (m == to_midx)
			die(_("MIDX %s must be an ancestor of %s"),
			    from_checksum, to_checksum);
	}

	return write_midx_file_compact(source, from_midx, to_midx,
				       incremental_base, flags);
}

static int odb_source_files_multi_pack_index_verify(struct odb_source *source,
						    unsigned flags)
{
	return verify_midx_file(source, flags);
}

static int odb_source_files_multi_pack_index_expire(struct odb_source *source,
						    unsigned flags)
{
	return expire_midx_packs(source, flags);
}

static int odb_source_files_multi_pack_index_repack(struct odb_source *source,
						    size_t batch_size,
						    unsigned flags)
{
	return midx_repack(source, batch_size, flags);
}

static int odb_source_files_prune_cruft(struct odb_source *source,
					timestamp_t expire,
					int dry_run, int verbose)
{
	struct files_prune_cruft_data d = { source, expire, dry_run, verbose };
	struct strbuf pack_dir = STRBUF_INIT;

	/*
	 * Tidy the loose-object directory after "git prune" removed unreachable
	 * loose objects: drop stale tmp_obj_* temp files and now-empty fanout
	 * directories, then unlink loose objects made redundant by a pack. Cruft
	 * is a files-storage concept; odb_prune_cruft() dispatches this per local
	 * source (the no-op default covers a backend that stores objects in its
	 * own medium, which has no such cruft).
	 */
	for_each_loose_file_in_source(source, NULL, files_prune_cruft_cb,
				      files_prune_subdir_cb, &d);
	for_each_loose_file_in_source(source, files_prune_redundant_packed,
				      NULL, files_prune_subdir_cb, &d);

	/* Failed temp packs/indexes accumulate as tmp_* in objects/ and objects/pack/. */
	files_remove_temporary_files(source->path, &d);
	strbuf_addf(&pack_dir, "%s/pack", source->path);
	files_remove_temporary_files(pack_dir.buf, &d);
	strbuf_release(&pack_dir);

	return 0;
}

struct odb_source_files *odb_source_files_new(struct object_database *odb,
					      const char *path,
					      bool local)
{
	struct odb_source_files *files;

	CALLOC_ARRAY(files, 1);
	odb_source_init(&files->base, odb, path, local);
	files->loose = odb_source_loose_new(odb, path, local);
	files->packed = packfile_store_new(&files->base);

	files->base.free = odb_source_files_free;
	files->base.close = odb_source_files_close;
	files->base.reprepare = odb_source_files_reprepare;
	files->base.read_object_info = odb_source_files_read_object_info;
	files->base.read_object_stream = odb_source_files_read_object_stream;
	files->base.for_each_object = odb_source_files_for_each_object;
	files->base.count_objects = odb_source_files_count_objects;
	files->base.count_packs = odb_source_files_count_packs;
	files->base.count_loose_objects = odb_source_files_count_loose_objects;
	files->base.for_each_loose_object = odb_source_files_for_each_loose_object;
	files->base.find_abbrev_len = odb_source_files_find_abbrev_len;
	files->base.freshen_object = odb_source_files_freshen_object;
	files->base.write_object = odb_source_files_write_object;
	files->base.write_object_stream = odb_source_files_write_object_stream;
	files->base.begin_transaction = odb_source_files_begin_transaction;
	files->base.begin_pack_ingest = odb_source_files_begin_pack_ingest;
	files->base.install_loose_object = odb_source_files_install_loose_object;
	files->base.note_received_pack = odb_source_files_note_received_pack;
	files->base.note_indexed_pack = odb_source_files_note_indexed_pack;
	files->base.optimize = odb_source_files_optimize;
	files->base.optimize_required = odb_source_files_optimize_required;
	files->base.verify = odb_source_files_verify;
	files->base.remove_objects = odb_source_files_remove_objects;
	files->base.prune_cruft = odb_source_files_prune_cruft;
	files->base.multi_pack_index_write = odb_source_files_multi_pack_index_write;
	files->base.multi_pack_index_compact = odb_source_files_multi_pack_index_compact;
	files->base.multi_pack_index_verify = odb_source_files_multi_pack_index_verify;
	files->base.multi_pack_index_expire = odb_source_files_multi_pack_index_expire;
	files->base.multi_pack_index_repack = odb_source_files_multi_pack_index_repack;
	files->base.has_received_pack = odb_source_files_has_received_pack;
	files->base.is_object_kept = odb_source_files_is_object_kept;
	files->base.cruft_object_preserved = odb_source_files_cruft_object_preserved;
	files->base.is_promisor_object = odb_source_files_is_promisor_object;
	files->base.mark_objects_promisor = odb_source_files_mark_objects_promisor;
	files->base.read_alternates = odb_source_files_read_alternates;
	files->base.write_alternate = odb_source_files_write_alternate;

	/*
	 * Ideally, we would only ever store absolute paths in the source. This
	 * is not (yet) possible though because we access and assume relative
	 * paths in the primary ODB source in some user-facing functionality.
	 */
	if (!is_absolute_path(path))
		chdir_notify_register(NULL, odb_source_files_reparent, files);

	/*
	 * Register in the odb's list of files sources so packfile machinery can
	 * enumerate files sources without branching on the source type.
	 */
	*odb->files_sources_tail = files;
	odb->files_sources_tail = &files->next_files;

	return files;
}

/*
 * Walk to the first source at or after `source` that has at least one
 * packfile, recording it in `*out` and returning its first pack entry.
 * Returns NULL (with `*out` cleared) once no such source remains. Each source
 * reports its own packs through the `packs` vtable method (a non-files backend
 * reports none), so this merges packs across sources with no branch on the
 * backend type.
 */
static struct packfile_list_entry *first_files_pack(struct odb_source_files *files,
						    struct odb_source_files **out)
{
	for (; files; files = files->next_files) {
		struct packfile_list_entry *entry;

		entry = packfile_store_get_packs(files->packed);
		if (!entry)
			continue;

		*out = files;
		return entry;
	}

	*out = NULL;
	return NULL;
}

struct odb_files_pack_iter odb_files_pack_iter_begin(struct object_database *odb)
{
	struct odb_files_pack_iter iter = { 0 };

	odb_prepare_alternates(odb);
	iter.entry = first_files_pack(odb->files_sources, &iter.files);

	return iter;
}

void odb_files_pack_iter_advance(struct odb_files_pack_iter *iter)
{
	iter->entry = iter->entry->next;
	if (iter->entry)
		return;

	/* Exhausted this source's packs; move on to the next files source. */
	iter->entry = first_files_pack(iter->files->next_files, &iter->files);
}

struct packed_git *odb_files_pack_iter_pack(const struct odb_files_pack_iter *iter)
{
	return iter->entry ? iter->entry->pack : NULL;
}

