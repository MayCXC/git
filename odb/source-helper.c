/*
 * ODB helper: delegate object storage to an external process.
 *
 * Uses the shared helper process (helper.c) which also handles ref
 * operations in the same binary, matching how remote helpers handle
 * both refs and objects.
 */
#include "git-compat-util.h"
#include "config.h"
#include "gettext.h"
#include "hex.h"
#include "object.h"
#include "object-file.h"
#include "odb/source-helper.h"
#include "odb/streaming.h"
#include "helper.h"
#include "lockfile.h"
#include "promisor-remote.h"
#include "repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "strvec.h"
#include "wrapper.h"
#include "write-or-die.h"

/* ---- odb_source callbacks ---- */

static void helper_free(struct odb_source *source)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);

	/* Don't release the helper process; it's shared via repo->local_helper */
	odb_source_release(&src->base);
	free(src);
}

static void helper_close(struct odb_source *source)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;

	if (!hp->child)
		return;

	helper_process_send(hp, "close\n");
	fclose(hp->out);
	hp->out = NULL;
	finish_command(hp->child);
	free(hp->child);
	hp->child = NULL;
}

static void helper_reprepare(struct odb_source *source)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	helper_process_refresh(src->hp);
}

static int helper_read_object_info(struct odb_source *source,
				   const struct object_id *oid,
				   struct object_info *oi,
				   enum object_info_flags flags)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	char type_str[32];
	unsigned long size;
	enum object_type type;
	int want_content;

	int ret = -1;

	/* Ensure the helper process is started and capabilities negotiated */
	helper_process_ensure(hp);

	oid_to_hex_r(hex, oid);

	/*
	 * Kept-only queries use the "have-kept" command when the
	 * helper advertises the "kept" capability. Backends that
	 * do not track kept status return -1 (not found).
	 */
	if (flags & OBJECT_INFO_KEPT_ONLY) {
		if (!hp->cap_kept)
			return -1;
		helper_process_send(hp, "have-kept %s\n", hex);
		if (helper_process_readline(hp, &line) != EOF)
			ret = !strcmp(line.buf, "true") ? 0 : -1;
		strbuf_release(&line);
		return ret;
	}

	if (!oi) {
		/* Existence check only (called from odb_has_object) */
		if (hp->cap_have) {
			helper_process_send(hp, "have %s\n", hex);
			if (helper_process_readline(hp, &line) != EOF)
				ret = !strcmp(line.buf, "true") ? 0 : -1;
		} else if (hp->cap_info) {
			helper_process_send(hp, "info %s\n", hex);
			if (helper_process_readline(hp, &line) != EOF)
				ret = strcmp(line.buf, "missing") ? 0 : -1;
		}
		strbuf_release(&line);
		return ret;
	}

	want_content = oi->contentp != NULL;

	/*
	 * Use "info" for metadata-only requests and "get" when the
	 * caller also wants the object content. This avoids sending
	 * data over the pipe when only type and size are needed.
	 */
	if (want_content && hp->cap_get)
		helper_process_send(hp, "get %s\n", hex);
	else if (hp->cap_info)
		helper_process_send(hp, "info %s\n", hex);
	else if (hp->cap_get)
		helper_process_send(hp, "get %s\n", hex);
	else
		goto out;

	if (helper_process_readline(hp, &line) == EOF)
		goto out;

	if (!strcmp(line.buf, "missing"))
		goto out;

	/* Parse "<type> <size>" */
	if (sscanf(line.buf, "%31s %lu", type_str, &size) != 2)
		goto out;

	type = type_from_string(type_str);
	if (oi->typep)
		*(oi->typep) = type;
	if (oi->sizep)
		*(oi->sizep) = size;

	if (want_content && hp->cap_get) {
		size_t got;

		*oi->contentp = xmallocz(size);
		/*
		 * Must use fread rather than read_in_full on
		 * fileno(hp->out) because strbuf_getline may have
		 * buffered data in the FILE*.
		 */
		got = fread(*oi->contentp, 1, size, hp->out);
		if (got != size) {
			free(*oi->contentp);
			*oi->contentp = NULL;
			goto out;
		}
	}

	ret = 0;
out:
	strbuf_release(&line);
	return ret;
}

/* ---- Streaming read ---- */

struct helper_read_stream {
	struct odb_read_stream base;
	struct helper_process *hp;
	size_t remaining;
};

static ssize_t helper_stream_read(struct odb_read_stream *_st, char *buf, size_t sz)
{
	struct helper_read_stream *st =
		container_of(_st, struct helper_read_stream, base);
	size_t got;

	if (!st->remaining)
		return 0;

	if (sz > st->remaining)
		sz = st->remaining;

	got = fread(buf, 1, sz, st->hp->out);
	st->remaining -= got;
	return got ? (ssize_t)got : -1;
}

static int helper_stream_close(struct odb_read_stream *_st)
{
	struct helper_read_stream *st =
		container_of(_st, struct helper_read_stream, base);

	/* Drain unread bytes so the pipe stays synchronized */
	while (st->remaining > 0) {
		char drain[4096];
		size_t n = st->remaining < sizeof(drain) ? st->remaining : sizeof(drain);
		size_t got = fread(drain, 1, n, st->hp->out);
		if (!got)
			break;
		st->remaining -= got;
	}
	return 0;
}

static int helper_read_object_stream(struct odb_read_stream **out,
				     struct odb_source *source,
				     const struct object_id *oid)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	char type_str[32];
	unsigned long size;
	struct helper_read_stream *st;

	helper_process_ensure(hp);

	if (!hp->cap_get)
		return -1;

	oid_to_hex_r(hex, oid);
	helper_process_send(hp, "get %s\n", hex);

	if (helper_process_readline(hp, &line) == EOF ||
	    !strcmp(line.buf, "missing")) {
		strbuf_release(&line);
		return -1;
	}

	if (sscanf(line.buf, "%31s %lu", type_str, &size) != 2) {
		strbuf_release(&line);
		return -1;
	}
	strbuf_release(&line);

	CALLOC_ARRAY(st, 1);
	st->base.type = type_from_string(type_str);
	st->base.size = size;
	st->base.read = helper_stream_read;
	st->base.close = helper_stream_close;
	st->hp = hp;
	st->remaining = size;

	*out = &st->base;
	return 0;
}

static int helper_for_each_object(struct odb_source *source,
				  const struct object_info *request,
				  odb_for_each_object_cb cb,
				  void *cb_data,
				  unsigned flags)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	struct strbuf cmd = STRBUF_INIT;
	int ret = 0;

	helper_process_ensure(hp);

	if (!hp->cap_list_objects)
		return 0;

	strbuf_addstr(&cmd, "list-objects");
	if ((flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY) && hp->cap_promisor)
		strbuf_addstr(&cmd, " --promisor-only");
	if ((flags & (ODB_FOR_EACH_OBJECT_SKIP_IN_CORE_KEPT_PACKS |
		      ODB_FOR_EACH_OBJECT_SKIP_ON_DISK_KEPT_PACKS)) &&
	    hp->cap_kept)
		strbuf_addstr(&cmd, " --skip-kept");
	strbuf_addch(&cmd, '\n');
	helper_process_send(hp, "%s", cmd.buf);
	strbuf_release(&cmd);

	/*
	 * Buffer all OIDs before invoking the callback. The callback
	 * may send commands (get, info) to the same helper pipe, which
	 * would corrupt the in-progress list-objects stream.
	 */
	{
		struct object_id *oids = NULL;
		enum object_type *types = NULL;
		unsigned long *sizes = NULL;
		size_t nr = 0, alloc = 0;

		while (helper_process_readline(hp, &line) != EOF) {
			char oid_hex[GIT_MAX_HEXSZ + 1];
			char type_str[32];
			unsigned long sz;
			struct object_id oid;

			if (!line.len)
				break;
			if (sscanf(line.buf, "%64s %31s %lu",
				   oid_hex, type_str, &sz) != 3)
				continue;
			if (get_oid_hex_any(oid_hex, &oid) == GIT_HASH_UNKNOWN)
				continue;

			ALLOC_GROW(oids, nr + 1, alloc);
			REALLOC_ARRAY(types, nr + 1);
			REALLOC_ARRAY(sizes, nr + 1);
			oidcpy(&oids[nr], &oid);
			types[nr] = type_from_string(type_str);
			sizes[nr] = sz;
			nr++;
		}
		strbuf_release(&line);

		for (size_t i = 0; i < nr; i++) {
			struct object_info oi = OBJECT_INFO_INIT;
			if (request && request->typep)
				oi.typep = &types[i];
			if (request && request->sizep)
				oi.sizep = &sizes[i];
			ret = cb(&oids[i], &oi, cb_data);
			if (ret)
				break;
		}

		free(oids);
		free(types);
		free(sizes);
	}
	return ret;
}

static int helper_freshen_object(struct odb_source *source,
				 const struct object_id *oid)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	int exists;

	helper_process_ensure(hp);

	if (!hp->cap_have)
		return 0;

	oid_to_hex_r(hex, oid);
	helper_process_send(hp, "have %s\n", hex);

	if (helper_process_readline(hp, &line) == EOF) {
		strbuf_release(&line);
		return 0;
	}
	exists = !strcmp(line.buf, "true");
	strbuf_release(&line);
	return exists;
}

static int helper_write_object(struct odb_source *source,
			       const void *buf, unsigned long len,
			       enum object_type type,
			       struct object_id *oid,
			       struct object_id *compat_oid,
			       unsigned flags UNUSED)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];

	/* Ensure the helper process is started and capabilities negotiated */
	helper_process_ensure(hp);

	if (!hp->cap_put)
		return -1;

	/* Compute OID locally so the helper can store it directly */
	hash_object_file(src->base.odb->repo->hash_algo, buf, len, type, oid);
	oid_to_hex_r(hex, oid);

	helper_process_send(hp, "put %s %s %lu\n", hex, type_name(type), len);

	helper_process_write(hp, buf, len);

	if (helper_process_readline(hp, &line) == EOF ||
	    strcmp(line.buf, hex)) {
		strbuf_release(&line);
		return -1;
	}
	strbuf_release(&line);

	if (compat_oid)
		oidcpy(compat_oid, oid);
	return 0;
}

static int helper_write_object_stream(struct odb_source *source,
				      struct odb_write_stream *in_stream,
				      size_t len,
				      struct object_id *oid)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;

	helper_process_ensure(hp);

	if (!hp->cap_put_stream) {
		/* Fallback: buffer everything, use regular put */
		char *buf = xmalloc(len ? len : 1);
		size_t total = 0;
		int ret;
		while (!in_stream->is_finished && total < len) {
			unsigned long chunk_len;
			const void *chunk = in_stream->read(in_stream, &chunk_len);
			if (chunk_len > 0) {
				memcpy(buf + total, chunk, chunk_len);
				total += chunk_len;
			}
		}
		ret = helper_write_object(source, buf, total, OBJ_BLOB,
					  oid, NULL, 0);
		free(buf);
		return ret;
	}

	/* Streaming path: hash incrementally while sending to helper */
	{
		struct git_hash_ctx ctx;
		char hdr[32];
		int hdrlen;
		char hex[GIT_MAX_HEXSZ + 1];

		helper_process_send(hp, "put-stream %s %"PRIuMAX"\n",
				    type_name(OBJ_BLOB), (uintmax_t)len);

		hdrlen = xsnprintf(hdr, sizeof(hdr), "blob %"PRIuMAX,
				   (uintmax_t)len);
		hdrlen++; /* include trailing NUL in hash */

		git_hash_init(&ctx, algo);
		git_hash_update(&ctx, hdr, hdrlen);

		while (!in_stream->is_finished) {
			unsigned long chunk_len;
			const void *chunk = in_stream->read(in_stream, &chunk_len);
			if (chunk_len > 0) {
				helper_process_write(hp, chunk, chunk_len);
				git_hash_update(&ctx, chunk, chunk_len);
			}
		}

		git_hash_final_oid(oid, &ctx);

		/* Verify helper computed the same OID */
		oid_to_hex_r(hex, oid);

		if (helper_process_readline(hp, &line) == EOF ||
		    strcmp(line.buf, hex)) {
			strbuf_release(&line);
			return -1;
		}
		strbuf_release(&line);
		return 0;
	}
}

static void helper_transaction_commit(struct odb_transaction *transaction)
{
	struct odb_source_helper *src =
		container_of(transaction->source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;

	helper_process_send(hp, "odb-transaction-commit\n");
	helper_process_readline(hp, &line);
	strbuf_release(&line);
	free(transaction);
}

static int helper_begin_transaction(struct odb_source *source,
				    struct odb_transaction **out)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	struct odb_transaction *txn;

	helper_process_ensure(hp);

	if (!hp->cap_odb_transaction)
		return -1;

	helper_process_send(hp, "odb-transaction-begin\n");
	if (helper_process_readline(hp, &line) == EOF ||
	    strcmp(line.buf, "ok")) {
		strbuf_release(&line);
		return -1;
	}
	strbuf_release(&line);

	txn = xcalloc(1, sizeof(*txn));
	txn->source = source;
	txn->commit = helper_transaction_commit;
	*out = txn;
	return 0;
}

static int helper_read_alternates(struct odb_source *source,
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

static int helper_write_alternate(struct odb_source *source,
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
		goto done;
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
		goto done;
	}

	if (found) {
		rollback_lock_file(&lock);
	} else {
		fprintf_or_die(out, "%s\n", alternate);
		if (commit_lock_file(&lock)) {
			ret = error_errno(_("unable to move new alternates file into place"));
			goto done;
		}
	}

	ret = 0;

done:
	free(path);
	return ret;
}

/*
 * Ingest a pack by spawning unpack-objects, which writes each object
 * individually through odb_write_object (dispatched to write_object
 * on this source via the ODB).
 */
static int helper_write_packfile(struct odb_source *source,
				 int pack_fd,
				 struct odb_write_packfile_options *opts)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct repository *repo = source->odb->repo;
	struct child_process cmd = CHILD_PROCESS_INIT;
	int fsck_objects = 0;
	int ret;

	cmd.in = pack_fd;
	cmd.git_cmd = 1;
	cmd.stdout_to_stderr = 1;
	strvec_push(&cmd.args, "unpack-objects");
	strvec_push(&cmd.args, "-q");

	repo_config_get_bool(repo, "transfer.fsckobjects", &fsck_objects);
	repo_config_get_bool(repo, "fetch.fsckobjects", &fsck_objects);
	if (fsck_objects)
		strvec_push(&cmd.args, "--strict");

	/*
	 * Start kept/promisor batch BEFORE unpack-objects runs.
	 * Objects written via put during this batch are auto-marked.
	 * The helper's "refresh" command clears kept marks (matching
	 * the files backend where reprepare releases .keep files).
	 */
	helper_process_ensure(hp);
	if (hp->cap_kept)
		helper_process_send(hp, "mark-kept-recent\n");
	if (hp->cap_promisor && repo_has_promisor_remote(repo))
		helper_process_send(hp, "mark-promisor-recent\n");

	ret = run_command(&cmd) ? -1 : 0;
	if (ret)
		return ret;

	/*
	 * If the helper supports connectivity checking, verify that
	 * all referenced objects exist. Report via the options output
	 * field so the transport layer can skip its own check.
	 */
	if (opts && opts->check_self_contained && hp->cap_connectivity_check) {
		struct strbuf line = STRBUF_INIT;
		helper_process_send(hp, "connectivity-check\n");
		if (helper_process_readline(hp, &line) != EOF &&
		    !strcmp(line.buf, "ok"))
			opts->self_contained_out = 1;
		strbuf_release(&line);
	}

	return 0;
}

/*
 * Iterate objects whose OID matches a prefix for disambiguation.
 * Uses "list-objects" and filters client-side by prefix.
 */
static int helper_for_each_unique_abbrev(struct odb_source *source,
					 const struct object_id *oid_prefix,
					 unsigned int prefix_len,
					 odb_for_each_object_cb cb,
					 void *cb_data)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char prefix_hex[GIT_MAX_HEXSZ + 1];
	int ret = 0;

	helper_process_ensure(hp);

	if (!hp->cap_list_objects)
		return 0;

	oid_to_hex_r(prefix_hex, oid_prefix);

	helper_process_send(hp, "list-objects\n");

	while (helper_process_readline(hp, &line) != EOF) {
		char oid_hex[GIT_MAX_HEXSZ + 1];
		char type_str[32];
		unsigned long size;
		struct object_id oid;

		if (!line.len)
			break;

		if (sscanf(line.buf, "%64s %31s %lu",
			   oid_hex, type_str, &size) != 3)
			continue;

		if (get_oid_hex_any(oid_hex, &oid) == GIT_HASH_UNKNOWN)
			continue;

		/* Filter by prefix */
		if (strncmp(oid_hex, prefix_hex, prefix_len))
			continue;

		ret = cb(&oid, NULL, cb_data);
		if (ret) {
			helper_process_drain(hp);
			break;
		}
	}
	strbuf_release(&line);
	return ret;
}

/*
 * Return an approximate count of objects by counting "list-objects" lines.
 */
static int helper_count_objects(struct odb_source *source,
				enum odb_count_objects_flags flags UNUSED,
				unsigned long *out)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	unsigned long count = 0;

	helper_process_ensure(hp);

	if (!hp->cap_list_objects) {
		*out = 0;
		return 0;
	}

	helper_process_send(hp, "list-objects\n");

	while (helper_process_readline(hp, &line) != EOF) {
		if (!line.len)
			break;
		count++;
	}
	strbuf_release(&line);
	*out = count;
	return 0;
}

static int helper_convert_object_id(struct odb_source *source,
				    const struct object_id *src,
				    const struct git_hash_algo *to,
				    struct object_id *dest)
{
	struct odb_source_helper *src_h =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src_h->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	int ret = -1;

	helper_process_ensure(hp);

	oid_to_hex_r(hex, src);
	helper_process_send(hp, "convert-oid %s %s\n", hex, to->name);

	if (helper_process_readline(hp, &line) != EOF &&
	    strcmp(line.buf, "missing")) {
		if (get_oid_hex_any(line.buf, dest) != GIT_HASH_UNKNOWN)
			ret = 0;
	}
	strbuf_release(&line);
	return ret;
}

/* ---- Constructor ---- */

struct odb_source_helper *odb_source_helper_new(struct object_database *odb,
						const char *helper_name,
						const char *path,
						bool local)
{
	struct odb_source_helper *src;

	CALLOC_ARRAY(src, 1);
	odb_source_init(&src->base, odb, ODB_SOURCE_HELPER, path, local);

	/* Share the repo-level helper process, creating it if needed */
	if (!odb->repo->local_helper) {
		odb->repo->local_helper = xcalloc(1, sizeof(*odb->repo->local_helper));
		helper_process_init(odb->repo->local_helper, helper_name, path);
	} else {
		if (strcmp(odb->repo->local_helper->name, helper_name))
			die(_("ODB helper name '%s' does not match existing helper '%s'"),
			    helper_name, odb->repo->local_helper->name);
		if (!odb->repo->local_helper->gitdir)
			odb->repo->local_helper->gitdir = xstrdup(path);
	}
	src->hp = odb->repo->local_helper;
	src->base.free = helper_free;
	src->base.close = helper_close;
	src->base.reprepare = helper_reprepare;
	src->base.read_object_info = helper_read_object_info;
	src->base.read_object_stream = helper_read_object_stream;
	src->base.for_each_object = helper_for_each_object;
	src->base.freshen_object = helper_freshen_object;
	src->base.write_object = helper_write_object;
	src->base.write_object_stream = helper_write_object_stream;
	src->base.begin_transaction = helper_begin_transaction;
	src->base.read_alternates = helper_read_alternates;
	src->base.write_alternate = helper_write_alternate;
	src->base.write_packfile = helper_write_packfile;
	src->base.for_each_unique_abbrev = helper_for_each_unique_abbrev;
	src->base.count_objects = helper_count_objects;
	src->base.convert_object_id = helper_convert_object_id;
	return src;
}

struct odb_source *odb_source_helper_new_base(struct object_database *odb,
					      const char *path,
					      bool local)
{
	char *name_alloc = NULL;
	const char *name;

	/*
	 * The helper name comes from repo->local_helper (set during config
	 * reading from extensions.localHelper) or from the repo config as
	 * a fallback for code paths where local_helper was not set early.
	 */
	if (odb->repo->local_helper) {
		name = odb->repo->local_helper->name;
	} else {
		repo_config_get_string(odb->repo, "extensions.localhelper",
				       &name_alloc);
		name = name_alloc;
	}

	if (!name || !*name)
		die(_("helper backend requires extensions.localHelper"));

	{
		struct odb_source *ret;
		ret = &odb_source_helper_new(odb, name, path, local)->base;
		free(name_alloc);
		return ret;
	}
}
