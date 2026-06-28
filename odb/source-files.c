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
	files->base.find_abbrev_len = odb_source_files_find_abbrev_len;
	files->base.freshen_object = odb_source_files_freshen_object;
	files->base.write_object = odb_source_files_write_object;
	files->base.write_object_stream = odb_source_files_write_object_stream;
	files->base.begin_transaction = odb_source_files_begin_transaction;
	files->base.begin_pack_ingest = odb_source_files_begin_pack_ingest;
	files->base.install_loose_object = odb_source_files_install_loose_object;
	files->base.note_received_pack = odb_source_files_note_received_pack;
	files->base.note_indexed_pack = odb_source_files_note_indexed_pack;
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

