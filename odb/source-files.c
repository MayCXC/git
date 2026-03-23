#include "git-compat-util.h"
#include "abspath.h"
#include "cbtree.h"
#include "chdir-notify.h"
#include "config.h"
#include "gettext.h"
#include "lockfile.h"
#include "midx.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source.h"
#include "odb/source-files.h"
#include "oidtree.h"
#include "pack.h"
#include "packfile.h"
#include "run-command.h"
#include "strbuf.h"
#include "strvec.h"
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

static void odb_source_files_free(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	chdir_notify_unregister(NULL, odb_source_files_reparent, files);
	odb_source_loose_free(files->loose);
	packfile_store_free(files->packed);
	odb_source_release(&files->base);
	free(files);
}

static void odb_source_files_close(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	packfile_store_close(files->packed);
}

static void odb_source_files_reprepare(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	odb_source_loose_reprepare(&files->base);
	packfile_store_reprepare(files->packed);
}

static int odb_source_files_read_object_info(struct odb_source *source,
					     const struct object_id *oid,
					     struct object_info *oi,
					     enum object_info_flags flags)
{
	struct odb_source_files *files = odb_source_files_downcast(source);

	if (!packfile_store_read_object_info(files->packed, oid, oi, flags) ||
	    !odb_source_loose_read_object_info(source, oid, oi, flags))
		return 0;

	return -1;
}

static int odb_source_files_read_object_stream(struct odb_read_stream **out,
					       struct odb_source *source,
					       const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	if (!packfile_store_read_object_stream(out, files->packed, oid) ||
	    !odb_source_loose_read_object_stream(out, source, oid))
		return 0;
	return -1;
}

static int odb_source_files_for_each_object(struct odb_source *source,
					    const struct object_info *request,
					    odb_for_each_object_cb cb,
					    void *cb_data,
					    unsigned flags)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	int ret;

	if (!(flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY)) {
		ret = odb_source_loose_for_each_object(source, request, cb, cb_data, flags);
		if (ret)
			return ret;
	}

	ret = packfile_store_for_each_object(files->packed, request, cb, cb_data, flags);
	if (ret)
		return ret;

	return 0;
}

static int odb_source_files_freshen_object(struct odb_source *source,
					   const struct object_id *oid)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	if (packfile_store_freshen_object(files->packed, oid) ||
	    odb_source_loose_freshen_object(source, oid))
		return 1;
	return 0;
}

static int odb_source_files_write_object(struct odb_source *source,
					 const void *buf, unsigned long len,
					 enum object_type type,
					 struct object_id *oid,
					 struct object_id *compat_oid,
					 unsigned flags)
{
	return odb_source_loose_write_object(source, buf, len, type,
					     oid, compat_oid, flags);
}

static int odb_source_files_write_object_stream(struct odb_source *source,
						struct odb_write_stream *stream,
						size_t len,
						struct object_id *oid)
{
	return odb_source_loose_write_stream(source, stream, len, oid);
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

static int odb_source_files_write_packfile(struct odb_source *source,
					   int pack_fd, unsigned int nr_objects,
					   struct strvec *index_pack_args)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	int transfer_unpack_limit = -1;
	int fetch_unpack_limit = -1;
	int unpack_limit = 100;
	struct child_process cmd = CHILD_PROCESS_INIT;

	repo_config_get_int(source->odb->repo, "fetch.unpacklimit",
			    &fetch_unpack_limit);
	repo_config_get_int(source->odb->repo, "transfer.unpacklimit",
			    &transfer_unpack_limit);
	if (0 <= fetch_unpack_limit)
		unpack_limit = fetch_unpack_limit;
	else if (0 <= transfer_unpack_limit)
		unpack_limit = transfer_unpack_limit;

	if (nr_objects <= (unsigned int)unpack_limit || !index_pack_args) {
		cmd.in = pack_fd;
		cmd.git_cmd = 1;
		cmd.stdout_to_stderr = 1;
		strvec_push(&cmd.args, "unpack-objects");
		strvec_push(&cmd.args, "-q");
		return run_command(&cmd) ? -1 : 0;
	}

	cmd.in = pack_fd;
	cmd.out = -1;
	cmd.git_cmd = 1;
	strvec_push(&cmd.args, "index-pack");
	strvec_push(&cmd.args, "--stdin");
	strvec_pushv(&cmd.args, index_pack_args->v);

	if (start_command(&cmd))
		return error(_("unable to spawn index-pack"));

	{
		char *lockfile = index_pack_lockfile(source->odb->repo,
						     cmd.out, NULL);
		close(cmd.out);
		free(lockfile);
	}

	if (finish_command(&cmd))
		return error(_("index-pack failed"));

	packfile_store_reprepare(files->packed);

	return 0;
}

/*
 * Compare the first `len` hex characters (nibbles) of two raw hashes.
 * Returns 1 if they match, 0 otherwise.
 */
static int match_hash_prefix(unsigned len, const unsigned char *a,
			      const unsigned char *b)
{
	while (len > 1) {
		if (*a != *b)
			return 0;
		a++;
		b++;
		len -= 2;
	}
	if (len)
		if ((*a ^ *b) & 0xf0)
			return 0;
	return 1;
}

struct abbrev_cb_data {
	odb_for_each_object_cb cb;
	void *cb_data;
	int ret;
};

static enum cb_next abbrev_loose_cb(const struct object_id *oid, void *data)
{
	struct abbrev_cb_data *d = data;
	d->ret = d->cb(oid, NULL, d->cb_data);
	return d->ret ? CB_BREAK : CB_CONTINUE;
}

static int odb_source_files_for_each_unique_abbrev(struct odb_source *source,
						   const struct object_id *oid_prefix,
						   unsigned int prefix_len,
						   odb_for_each_object_cb cb,
						   void *cb_data)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct multi_pack_index *m;
	struct packfile_list_entry *entry;
	unsigned int hexsz = source->odb->repo->hash_algo->hexsz;
	unsigned int len = prefix_len > hexsz ? hexsz : prefix_len;

	/* Search loose objects via the loose object cache. */
	{
		struct oidtree *tree = odb_source_loose_cache(source, oid_prefix);
		struct abbrev_cb_data d = { cb, cb_data, 0 };
		oidtree_each(tree, oid_prefix, prefix_len, abbrev_loose_cb, &d);
		if (d.ret)
			return d.ret;
	}

	/* Search packed objects in multi-pack indices. */
	m = get_multi_pack_index(source);
	for (; m; m = m->base_midx) {
		uint32_t num, i, first = 0;

		if (!m->num_objects)
			continue;

		num = m->num_objects + m->num_objects_in_base;
		bsearch_one_midx(oid_prefix, m, &first);

		for (i = first; i < num; i++) {
			struct object_id oid;
			const struct object_id *current;
			int ret;

			current = nth_midxed_object_oid(&oid, m, i);
			if (!match_hash_prefix(len, oid_prefix->hash, current->hash))
				break;
			ret = cb(current, NULL, cb_data);
			if (ret)
				return ret;
		}
	}

	/* Search packed objects not covered by a MIDX. */
	for (entry = packfile_store_get_packs(files->packed); entry; entry = entry->next) {
		struct packed_git *p = entry->pack;
		uint32_t num, i, first = 0;

		if (p->multi_pack_index)
			continue;

		if (open_pack_index(p) || !p->num_objects)
			continue;

		num = p->num_objects;
		bsearch_pack(oid_prefix, p, &first);

		for (i = first; i < num; i++) {
			struct object_id oid;
			int ret;

			nth_packed_object_id(&oid, p, i);
			if (!match_hash_prefix(len, oid_prefix->hash, oid.hash))
				break;
			ret = cb(&oid, NULL, cb_data);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static unsigned long odb_source_files_approximate_object_count(
	struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct multi_pack_index *m;
	struct packfile_list_entry *entry;
	unsigned long count = 0;

	m = get_multi_pack_index(source);
	if (m)
		count += m->num_objects + m->num_objects_in_base;

	for (entry = packfile_store_get_packs(files->packed); entry; entry = entry->next) {
		struct packed_git *p = entry->pack;
		if (p->multi_pack_index || open_pack_index(p))
			continue;
		count += p->num_objects;
	}

	return count;
}

struct odb_source_files *odb_source_files_new(struct object_database *odb,
					      const char *path,
					      bool local)
{
	struct odb_source_files *files;

	CALLOC_ARRAY(files, 1);
	odb_source_init(&files->base, odb, ODB_SOURCE_FILES, path, local);
	files->loose = odb_source_loose_new(&files->base);
	files->packed = packfile_store_new(&files->base);

	files->base.free = odb_source_files_free;
	files->base.close = odb_source_files_close;
	files->base.reprepare = odb_source_files_reprepare;
	files->base.read_object_info = odb_source_files_read_object_info;
	files->base.read_object_stream = odb_source_files_read_object_stream;
	files->base.for_each_object = odb_source_files_for_each_object;
	files->base.freshen_object = odb_source_files_freshen_object;
	files->base.write_object = odb_source_files_write_object;
	files->base.write_object_stream = odb_source_files_write_object_stream;
	files->base.begin_transaction = odb_source_files_begin_transaction;
	files->base.read_alternates = odb_source_files_read_alternates;
	files->base.write_alternate = odb_source_files_write_alternate;
	files->base.write_packfile = odb_source_files_write_packfile;
	files->base.for_each_unique_abbrev = odb_source_files_for_each_unique_abbrev;
	files->base.approximate_object_count = odb_source_files_approximate_object_count;

	/*
	 * Ideally, we would only ever store absolute paths in the source. This
	 * is not (yet) possible though because we access and assume relative
	 * paths in the primary ODB source in some user-facing functionality.
	 */
	if (!is_absolute_path(path))
		chdir_notify_register(NULL, odb_source_files_reparent, files);

	return files;
}
