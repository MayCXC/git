#include "git-compat-util.h"
#include "config.h"
#include "dir.h"
#include "gettext.h"
#include "hash-lookup.h"
#include "hex.h"
#include "helper.h"
#include "loose.h"
#include "midx.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-files.h"
#include "odb/source-helper.h"
#include "odb/source-loose.h"
#include "odb/streaming.h"
#include "odb/transaction.h"
#include "oid-array.h"
#include "oidset.h"
#include "packfile.h"
#include "promisor-remote.h"
#include "repack.h"
#include "repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "strvec.h"
#include "tempfile.h"
#include "wrapper.h"

enum object_helper_capability {
	OBJECT_HELPER_GET = (1 << 0),
	OBJECT_HELPER_PUT = (1 << 1),
	OBJECT_HELPER_LIST_OBJECTS = (1 << 2),
	OBJECT_HELPER_INFO = (1 << 3),
	OBJECT_HELPER_HAVE = (1 << 4),
	OBJECT_HELPER_PUT_STREAM = (1 << 5),
	OBJECT_HELPER_ODB_TRANSACTION = (1 << 6),
	OBJECT_HELPER_REFRESH = (1 << 7),
	OBJECT_HELPER_PROMISOR = (1 << 8),
	OBJECT_HELPER_OPTIMIZE = (1 << 9),
	OBJECT_HELPER_OPTIMIZE_REQUIRED = (1 << 10),
	OBJECT_HELPER_VERIFY = (1 << 11),
	OBJECT_HELPER_PUT_RAW = (1 << 12),
	OBJECT_HELPER_REPLACE = (1 << 13),
	OBJECT_HELPER_FRESHEN = (1 << 14),
	OBJECT_HELPER_PRUNE = (1 << 15),
	OBJECT_HELPER_GET_RAW = (1 << 16),
};

static const struct helper_capability object_helper_capabilities[] = {
	{ "get", OBJECT_HELPER_GET, 1 },
	{ "put", OBJECT_HELPER_PUT, 1 },
	{ "list-objects", OBJECT_HELPER_LIST_OBJECTS, 1 },
	{ "info", OBJECT_HELPER_INFO },
	{ "have", OBJECT_HELPER_HAVE },
	{ "put-stream", OBJECT_HELPER_PUT_STREAM },
	{ "odb-transaction", OBJECT_HELPER_ODB_TRANSACTION },
	{ "refresh", OBJECT_HELPER_REFRESH },
	{ "promisor", OBJECT_HELPER_PROMISOR },
	{ "optimize", OBJECT_HELPER_OPTIMIZE },
	{ "optimize-required", OBJECT_HELPER_OPTIMIZE_REQUIRED },
	{ "verify", OBJECT_HELPER_VERIFY },
	{ "put-raw", OBJECT_HELPER_PUT_RAW },
	{ "replace", OBJECT_HELPER_REPLACE },
	{ "freshen", OBJECT_HELPER_FRESHEN },
	{ "prune", OBJECT_HELPER_PRUNE },
	{ "get-raw", OBJECT_HELPER_GET_RAW },
	{ NULL },
};

static struct helper_process *started(struct odb_source_helper *helper)
{
	return helper_process_ensure(&helper->hp);
}

static bool capable(struct odb_source_helper *helper, unsigned int flag)
{
	return helper_process_capable(&helper->hp, flag);
}

/*
 * Have a running helper reload the read view of its objects, if it keeps
 * one. A helper not started yet has no view to reload, and starting it for
 * that would start one whenever the object database is prepared again.
 */
static void refresh(struct odb_source_helper *helper)
{
	if (helper->hp.child && capable(helper, OBJECT_HELPER_REFRESH))
		helper_process_send(&helper->hp, "refresh\n");
}

/*
 * Tear the connection down after a reply could not be read in full, so that
 * the next command starts on a fresh helper rather than on the rest of the
 * broken reply.
 */
static void drop_connection(struct odb_source_helper *helper)
{
	helper_process_disconnect(&helper->hp);
}

/*
 * Parse "<type> <size>" at the start of `p` and point `end` past it. Returns
 * 0 on success and -1 otherwise.
 */
static int parse_type_and_size(const char *p, enum object_type *type,
			       size_t *size, const char **end)
{
	const char *space = strchr(p, ' ');
	uintmax_t parsed;
	char *size_end;

	if (!space)
		return -1;
	*type = type_from_string_gently(p, space - p, 1);
	if (*type < 0)
		return -1;

	errno = 0;
	parsed = strtoumax(space + 1, &size_end, 10);
	if (errno || size_end == space + 1 || parsed > SIZE_MAX)
		return -1;
	*size = parsed;
	*end = size_end;
	return 0;
}

/*
 * Parse the header of an "info" or "get" reply, "<type> <size>", followed by
 * " <delta-base> <delta-size>" for an object the helper stores as a delta
 * against another. Returns 1 for an object, 0 for "missing" and -1 for
 * anything else.
 */
static int parse_object_header(struct odb_source_helper *helper,
			       const char *line, enum object_type *type,
			       size_t *size, struct object_id *delta_base,
			       size_t *delta_size)
{
	const struct git_hash_algo *algo = helper->base.odb->repo->hash_algo;
	struct object_id base;
	uintmax_t parsed;
	const char *p;
	char *end;

	if (!strcmp(line, "missing"))
		return 0;
	if (parse_type_and_size(line, type, size, &p))
		return -1;

	if (!*p) {
		if (delta_base)
			oidclr(delta_base, algo);
		if (delta_size)
			*delta_size = 0;
		return 1;
	}
	if (*p != ' ' || parse_oid_hex_algop(p + 1, &base, &p, algo) ||
	    *p != ' ')
		return -1;
	errno = 0;
	parsed = strtoumax(p + 1, &end, 10);
	if (errno || end == p + 1 || *end || !parsed || parsed > SIZE_MAX)
		return -1;
	if (delta_base)
		oidcpy(delta_base, &base);
	if (delta_size)
		*delta_size = parsed;
	return 1;
}

/* Read `size` payload bytes following a "get" reply into a new buffer. */
static void *read_payload(struct odb_source_helper *helper, size_t size)
{
	char *buf = xmallocz(size);

	/*
	 * The header was read through the buffered FILE, so the payload must
	 * be read through it as well.
	 */
	if (fread(buf, 1, size, helper->hp.out) != size) {
		free(buf);
		drop_connection(helper);
		return NULL;
	}
	return buf;
}

static enum odb_read_status helper_read_object(struct odb_source_helper *helper,
					       const struct object_id *oid,
					       struct object_info *oi,
					       enum object_info_flags flags,
					       struct strbuf *errmsg)
{
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	enum object_type type;
	size_t size;
	void *content = NULL;
	int want_content = oi && oi->contentp;
	int use_get;
	enum odb_read_status ret;
	int found;

	/*
	 * A miss makes the object database look again with
	 * OBJECT_INFO_SECOND_READ, so that a source can notice objects written
	 * since it last looked. The helper may answer from a read snapshot, so
	 * have it refresh the snapshot first.
	 */
	if (flags & OBJECT_INFO_SECOND_READ)
		refresh(helper);

	oid_to_hex_r(hex, oid);

	if (!oi && capable(helper, OBJECT_HELPER_HAVE)) {
		helper_process_send(hp, "have %s\n", hex);
		if (helper_process_readline(hp, &line) == EOF) {
			drop_connection(helper);
			ret = ODB_READ_ERROR;
			if (errmsg)
				strbuf_addf(errmsg, _("helper '%s' hung up"), hp->name);
		} else {
			ret = strcmp(line.buf, "true") ? ODB_READ_NOT_FOUND : ODB_READ_OK;
		}
		goto out;
	}

	use_get = want_content || !capable(helper, OBJECT_HELPER_INFO);
	helper_process_send(hp, "%s %s\n", use_get ? "get" : "info", hex);

	if (helper_process_readline(hp, &line) == EOF) {
		drop_connection(helper);
		ret = ODB_READ_ERROR;
		if (errmsg)
			strbuf_addf(errmsg, _("helper '%s' hung up"), hp->name);
		goto out;
	}

	found = parse_object_header(helper, line.buf, &type, &size,
				    oi ? oi->delta_base_oid : NULL,
				    oi ? oi->delta_sizep : NULL);
	if (found <= 0) {
		ret = found ? ODB_READ_ERROR : ODB_READ_NOT_FOUND;
		if (found && errmsg)
			strbuf_addf(errmsg, _("helper '%s' sent a malformed reply: %s"),
				    hp->name, line.buf);
		if (found)
			drop_connection(helper);
		goto out;
	}

	/* A "get" reply carries the object; read it even when not wanted. */
	if (use_get) {
		content = read_payload(helper, size);
		if (!content) {
			ret = ODB_READ_ERROR;
			if (errmsg)
				strbuf_addf(errmsg, _("helper '%s' sent a short object"),
					    hp->name);
			goto out;
		}
	}

	if (oi) {
		if (oi->typep)
			*oi->typep = type;
		if (oi->sizep)
			*oi->sizep = size;
		if (oi->disk_sizep)
			*oi->disk_sizep = 0;
		if (oi->mtimep)
			*oi->mtimep = 0;
		if (oi->source_infop)
			oi->source_infop->source = &helper->base;
		if (want_content) {
			*oi->contentp = content;
			content = NULL;
		}
	}
	ret = ODB_READ_OK;

out:
	free(content);
	strbuf_release(&line);
	return ret;
}

static enum odb_read_status odb_source_helper_read_object_info(struct odb_source *source,
								const struct object_id *oid,
								struct object_info *oi,
								enum object_info_flags flags,
								struct strbuf *errmsg)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	enum odb_read_status ret;

	ret = helper_read_object(helper, oid, oi, flags, errmsg);
	if (ret != ODB_READ_NOT_FOUND)
		return ret;
	return odb_source_read_object_info(&helper->files->base, oid, oi,
					   flags, errmsg);
}

struct helper_read_stream {
	struct odb_stream base;
	struct tempfile *spool;
};

static ssize_t helper_read_stream_read(struct odb_stream *stream,
				       char *buf, size_t len)
{
	struct helper_read_stream *st =
		container_of(stream, struct helper_read_stream, base);
	return xread(get_tempfile_fd(st->spool), buf, len);
}

static int helper_read_stream_close(struct odb_stream *stream)
{
	struct helper_read_stream *st =
		container_of(stream, struct helper_read_stream, base);
	return delete_tempfile(&st->spool);
}

/*
 * A stream over a helper object spools the whole object off the pipe into a
 * temporary file first. The reader of the stream may use the object database
 * before it is done with the stream, and a stream reading from the pipe
 * itself would then find the pipe carrying the reply to another command.
 */
static int helper_read_object_stream(struct odb_source_helper *helper,
				     struct odb_stream **out,
				     const struct object_id *oid)
{
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT, template = STRBUF_INIT;
	struct helper_read_stream *st;
	struct tempfile *spool;
	enum object_type type;
	size_t size, left;
	int found;

	helper_process_send(hp, "get %s\n", oid_to_hex(oid));
	if (helper_process_readline(hp, &line) == EOF) {
		drop_connection(helper);
		strbuf_release(&line);
		return -1;
	}
	found = parse_object_header(helper, line.buf, &type, &size, NULL, NULL);
	strbuf_release(&line);
	if (found <= 0) {
		if (found)
			drop_connection(helper);
		return -1;
	}

	strbuf_addf(&template, "%s/helper_object_XXXXXX", helper->base.path);
	spool = mks_tempfile(template.buf);
	strbuf_release(&template);
	if (!spool) {
		/* The reply still has to be consumed before the next command. */
		free(read_payload(helper, size));
		return -1;
	}

	for (left = size; left; ) {
		char buf[65536];
		size_t chunk = left < sizeof(buf) ? left : sizeof(buf);

		if (fread(buf, 1, chunk, hp->out) != chunk) {
			drop_connection(helper);
			delete_tempfile(&spool);
			return -1;
		}
		if (write_in_full(get_tempfile_fd(spool), buf, chunk) < 0) {
			/* Keep reading to leave the pipe at a reply boundary. */
			free(read_payload(helper, left - chunk));
			delete_tempfile(&spool);
			return -1;
		}
		left -= chunk;
	}

	if (lseek(get_tempfile_fd(spool), 0, SEEK_SET) < 0) {
		delete_tempfile(&spool);
		return -1;
	}

	CALLOC_ARRAY(st, 1);
	st->base.read = helper_read_stream_read;
	st->base.close = helper_read_stream_close;
	st->base.type = type;
	st->base.size = size;
	st->spool = spool;
	*out = &st->base;
	return 0;
}

static int odb_source_helper_read_object_stream(struct odb_stream **out,
						struct odb_source *source,
						const struct object_id *oid)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);

	if (!helper_read_object_stream(helper, out, oid))
		return 0;
	return odb_source_read_object_stream(out, &helper->files->base, oid);
}

/*
 * Read the object as the helper stores it, which "get-raw" answers as
 * "put-raw" gives it: "<type> <size> <delta-base> <compressed-size>", with a
 * <delta-base> of "0" for an object stored whole, followed by the
 * compressed bytes.
 */
static int odb_source_helper_read_object_raw(struct odb_source *source,
					     const struct object_id *oid,
					     struct packed_raw_entry *entry)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;
	struct helper_process *hp;
	struct strbuf line = STRBUF_INIT;
	uintmax_t data_len;
	const char *p;
	char *end;
	int ret = -1;

	memset(entry, 0, sizeof(*entry));
	if (!capable(helper, OBJECT_HELPER_GET_RAW))
		return 1;

	hp = started(helper);
	helper_process_send(hp, "get-raw %s\n", oid_to_hex(oid));
	if (helper_process_readline(hp, &line) == EOF) {
		drop_connection(helper);
		goto out;
	}
	if (!strcmp(line.buf, "missing")) {
		ret = 1;
		goto out;
	}

	if (parse_type_and_size(line.buf, &entry->type, &entry->size, &p) ||
	    *p++ != ' ')
		goto malformed;
	if (skip_prefix(p, "0 ", &p))
		oidclr(&entry->delta_base, algo);
	else if (parse_oid_hex_algop(p, &entry->delta_base, &p, algo) ||
		 *p++ != ' ')
		goto malformed;
	errno = 0;
	data_len = strtoumax(p, &end, 10);
	if (errno || end == p || *end || data_len > SIZE_MAX)
		goto malformed;

	entry->data_len = data_len;
	entry->data = read_payload(helper, entry->data_len);
	if (entry->data)
		ret = 0;
	goto out;

malformed:
	error(_("helper '%s' sent a malformed reply: %s"), hp->name, line.buf);
	drop_connection(helper);
out:
	strbuf_release(&line);
	return ret;
}

struct listed_object {
	struct object_id oid;
	enum object_type type;
	size_t size;
};

/*
 * Match the first `hex_len` hex digits of `oid` against `prefix`, comparing
 * raw hash bytes so that the algorithm of either does not matter.
 */
static int has_hex_prefix(const struct object_id *oid,
			  const struct object_id *prefix, size_t hex_len)
{
	size_t bytes = hex_len / 2;

	if (memcmp(oid->hash, prefix->hash, bytes))
		return 0;
	if ((hex_len & 1) && ((oid->hash[bytes] ^ prefix->hash[bytes]) & 0xf0))
		return 0;
	return 1;
}

/*
 * List the objects of the helper with "list-objects", into an array, as the
 * callers use the helper for other commands while going through the list.
 * Returns 0 on success and -1 in case the reply could not be read.
 */
static int list_objects(struct odb_source_helper *helper, const char *args,
			struct listed_object **out, size_t *nr)
{
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	size_t alloc = 0;

	*out = NULL;
	*nr = 0;

	helper_process_send(hp, "list-objects%s\n", args);
	while (1) {
		struct listed_object *object;
		const char *p;

		if (helper_process_readline(hp, &line) == EOF)
			goto broken;
		if (!line.len)
			break;

		/* Each line is "<oid> <type> <size>". */
		ALLOC_GROW(*out, *nr + 1, alloc);
		object = &(*out)[*nr];
		if (parse_oid_hex_any(line.buf, &object->oid, &p) == GIT_HASH_UNKNOWN ||
		    *p != ' ' ||
		    parse_type_and_size(p + 1, &object->type, &object->size, &p) ||
		    *p)
			goto broken;
		(*nr)++;
	}

	strbuf_release(&line);
	return 0;

broken:
	drop_connection(helper);
	FREE_AND_NULL(*out);
	*nr = 0;
	strbuf_release(&line);
	return -1;
}

static int for_each_helper_object(struct odb_source_helper *helper,
				  const struct object_info *request,
				  odb_for_each_object_cb cb, void *cb_data,
				  const struct odb_for_each_object_options *opts)
{
	struct repository *repo = helper->base.odb->repo;
	struct helper_process *hp = started(helper);
	const struct object_id *prefix = opts->prefix;
	size_t prefix_len = opts->prefix_hex_len;
	struct strbuf args = STRBUF_INIT;
	struct listed_object *objects;
	size_t nr;
	/*
	 * The helper knows objects by their storage ID only, so a prefix of a
	 * compatibility ID is matched here rather than by the helper.
	 */
	int scoped = prefix && !repo->compat_hash_algo;
	int ret = 0;

	if (opts->flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY) {
		/*
		 * Objects of a helper that cannot tell promisor objects apart
		 * are no promisor objects.
		 */
		if (!capable(helper, OBJECT_HELPER_PROMISOR))
			return 0;
		strbuf_addstr(&args, " --promisor-only");
	}
	if (scoped) {
		char hex[GIT_MAX_HEXSZ + 1];

		oid_to_hex_r(hex, prefix);
		strbuf_addf(&args, " %.*s", (int)prefix_len, hex);
	}

	ret = list_objects(helper, args.buf, &objects, &nr);
	strbuf_release(&args);
	if (ret < 0)
		return error(_("unable to list the objects of helper '%s'"),
			     hp->name);

	for (size_t i = 0; i < nr; i++) {
		struct object_id *oid = &objects[i].oid;
		struct object_id compat_oid;
		struct object_info oi;

		if (prefix && !scoped && !has_hex_prefix(oid, prefix, prefix_len)) {
			/* The prefix may name the object by its compatibility ID. */
			if (repo_loose_object_map_oid(repo, oid, repo->compat_hash_algo,
						      &compat_oid) ||
			    !has_hex_prefix(&compat_oid, prefix, prefix_len))
				continue;
			oid = &compat_oid;
		}

		if (!request) {
			ret = cb(oid, NULL, cb_data);
		} else if (request->contentp || request->delta_base_oid ||
			   request->delta_sizep) {
			/* The listing tells neither contents nor deltas. */
			oi = *request;
			if (helper_read_object(helper, &objects[i].oid, &oi, 0, NULL) < 0) {
				ret = error(_("unable to read %s from helper '%s'"),
					    oid_to_hex(&objects[i].oid), hp->name);
				break;
			}
			ret = cb(oid, &oi, cb_data);
		} else {
			oi = *request;
			if (oi.typep)
				*oi.typep = objects[i].type;
			if (oi.sizep)
				*oi.sizep = objects[i].size;
			if (oi.disk_sizep)
				*oi.disk_sizep = 0;
			if (oi.mtimep)
				*oi.mtimep = 0;
			if (oi.source_infop)
				oi.source_infop->source = &helper->base;
			ret = cb(oid, &oi, cb_data);
		}
		if (ret)
			break;
	}

	free(objects);
	return ret;
}

static int odb_source_helper_for_each_object(struct odb_source *source,
					     const struct object_info *request,
					     odb_for_each_object_cb cb,
					     void *cb_data,
					     const struct odb_for_each_object_options *opts)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	int ret;

	if ((opts->flags & ODB_FOR_EACH_OBJECT_LOCAL_ONLY) && !source->local)
		return 0;

	ret = for_each_helper_object(helper, request, cb, cb_data, opts);
	if (ret)
		return ret;
	return odb_source_for_each_object(&helper->files->base, request,
					  cb, cb_data, opts);
}

static int odb_source_helper_count_objects(struct odb_source *source,
					   enum odb_count_objects_flags flags,
					   unsigned long *out)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct listed_object *objects;
	size_t nr;
	int ret;

	if (list_objects(helper, "", &objects, &nr) < 0)
		return error(_("unable to list the objects of helper '%s'"),
			     helper->hp.name);
	free(objects);

	ret = odb_source_count_objects(&helper->files->base, flags, out);
	if (ret < 0)
		return ret;
	*out += nr;
	return 0;
}

static int odb_source_helper_find_abbrev_len(struct odb_source *source,
					     const struct object_id *oid,
					     unsigned min_len, unsigned *out)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	unsigned hexsz = source->odb->repo->hash_algo->hexsz;
	struct listed_object *objects;
	char hex[GIT_MAX_HEXSZ + 1];
	struct strbuf args = STRBUF_INIT;
	unsigned len = min_len;
	size_t nr;
	int ret;

	/*
	 * Only the objects sharing the first `min_len` hex digits with the
	 * object can call for a longer abbreviation.
	 */
	oid_to_hex_r(hex, oid);
	if (min_len)
		strbuf_addf(&args, " %.*s", (int)min_len, hex);
	ret = list_objects(helper, args.buf, &objects, &nr);
	strbuf_release(&args);
	if (ret < 0)
		return error(_("unable to list the objects of helper '%s'"),
			     helper->hp.name);

	for (size_t i = 0; i < nr; i++) {
		unsigned common = oid_common_prefix_hexlen(&objects[i].oid, oid);
		if (common != hexsz && common >= len)
			len = common + 1;
	}
	free(objects);

	ret = odb_source_find_abbrev_len(&helper->files->base, oid, len, out);
	if (ret < 0)
		return ret;
	if (*out < len)
		*out = len;
	return 0;
}

static int odb_source_helper_freshen_object(struct odb_source *source,
					    const struct object_id *oid,
					    const time_t *mtime)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	int exists = 0;

	/*
	 * "freshen" moves the time the helper keeps for when an object was
	 * last written, which its pruning goes by. A helper keeping no such
	 * time only tells whether it has the object.
	 */
	if (capable(helper, OBJECT_HELPER_FRESHEN)) {
		helper_process_send(hp, "freshen %s\n", oid_to_hex(oid));
		if (helper_process_readline(hp, &line) == EOF)
			drop_connection(helper);
		else
			exists = !strcmp(line.buf, "true");
		strbuf_release(&line);
	} else {
		exists = helper_read_object(helper, oid, NULL, 0, NULL) == ODB_READ_OK;
	}
	if (exists)
		return 1;
	return odb_source_freshen_object(&helper->files->base, oid, mtime);
}

static int odb_source_helper_write_object(struct odb_source *source,
					  const void *buf, size_t len,
					  enum object_type type,
					  const struct object_id *oid,
					  const struct object_id *compat_oid,
					  const time_t *mtime UNUSED,
					  enum odb_write_object_flags flags)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	int ret = 0;

	oid_to_hex_r(hex, oid);
	helper_process_send(hp, "put %s %s %"PRIuMAX"\n", hex, type_name(type),
			    (uintmax_t)len);
	helper_process_write(hp, buf, len);
	if (helper_process_readline(hp, &line) == EOF)
		drop_connection(helper);
	if (strcmp(line.buf, hex)) {
		if (!(flags & ODB_WRITE_OBJECT_SILENT))
			error(_("helper '%s' failed to write %s: %s"), hp->name,
			      hex, line.len ? line.buf : _("no reply"));
		ret = -1;
	}
	strbuf_release(&line);
	if (ret)
		return ret;

	/*
	 * The helper knows objects by their storage ID only, so the mapping to
	 * the compatibility ID goes where the files store keeps it for its own
	 * objects.
	 */
	if (compat_oid)
		return repo_add_loose_object_map(helper->files->loose, oid, compat_oid);
	return 0;
}

static int odb_source_helper_write_object_stream(struct odb_source *source,
						 struct odb_stream *stream,
						 struct object_id *oid)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct helper_process *hp = started(helper);
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;
	const struct git_hash_algo *compat = source->odb->repo->compat_hash_algo;
	struct git_hash_ctx ctx, compat_ctx;
	struct object_id compat_oid;
	struct strbuf line = STRBUF_INIT;
	char hdr[MAX_HEADER_LEN];
	char hex[GIT_MAX_HEXSZ + 1];
	size_t written = 0;
	int hdrlen, ret = 0;

	/*
	 * Streams carry blobs, whose compatibility ID hashes the same bytes,
	 * as the loose store computes it.
	 */
	if (!capable(helper, OBJECT_HELPER_PUT_STREAM)) {
		char *buf = xmallocz(stream->size);

		while (written < stream->size) {
			ssize_t got = odb_stream_read(stream, buf + written,
						      stream->size - written);
			if (got <= 0) {
				free(buf);
				return error(_("object stream yielded less bytes than expected"));
			}
			written += got;
		}
		hash_object_file(algo, buf, stream->size, stream->type, oid);
		if (compat)
			hash_object_file(compat, buf, stream->size, stream->type,
					 &compat_oid);
		ret = odb_source_helper_write_object(source, buf, stream->size,
						     stream->type, oid,
						     compat ? &compat_oid : NULL,
						     NULL, 0);
		free(buf);
		return ret;
	}

	hdrlen = format_object_header(hdr, sizeof(hdr), stream->type, stream->size);
	git_hash_init(&ctx, algo);
	git_hash_update(&ctx, hdr, hdrlen);
	if (compat) {
		git_hash_init(&compat_ctx, compat);
		git_hash_update(&compat_ctx, hdr, hdrlen);
	}

	helper_process_send(hp, "put-stream %s %"PRIuMAX"\n",
			    type_name(stream->type), (uintmax_t)stream->size);
	while (written < stream->size) {
		char buf[65536];
		ssize_t got = odb_stream_read(stream, buf, sizeof(buf));

		if (got <= 0 || written + got > stream->size) {
			/*
			 * The helper still waits for the rest of the object,
			 * which is not coming.
			 */
			drop_connection(helper);
			return error(_("object stream yielded %s bytes than expected"),
				     got > 0 ? "more" : "less");
		}
		helper_process_write(hp, buf, got);
		git_hash_update(&ctx, buf, got);
		if (compat)
			git_hash_update(&compat_ctx, buf, got);
		written += got;
	}

	/* The helper keys the object by the ID git computed and echoes it. */
	git_hash_final_oid(oid, &ctx);
	oid_to_hex_r(hex, oid);
	helper_process_send(hp, "%s\n", hex);
	if (helper_process_readline(hp, &line) == EOF)
		drop_connection(helper);
	if (strcmp(line.buf, hex))
		ret = error(_("helper '%s' failed to write %s: %s"), hp->name,
			    hex, line.len ? line.buf : _("no reply"));
	strbuf_release(&line);

	if (!ret && compat) {
		git_hash_final_oid(&compat_oid, &compat_ctx);
		ret = repo_add_loose_object_map(helper->files->loose, oid, &compat_oid);
	}
	return ret;
}

/*
 * The transaction of a helper batches the objects written to it, when the
 * helper can batch them, into one commit of its own.
 */
static int helper_transaction_commit(struct odb_transaction *transaction)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(transaction->source);
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	if (!capable(helper, OBJECT_HELPER_ODB_TRANSACTION))
		return 0;

	helper_process_send(hp, "odb-transaction-commit\n");
	if (helper_process_readline(hp, &line) == EOF)
		drop_connection(helper);
	if (strcmp(line.buf, "ok"))
		ret = error(_("helper '%s' failed to commit its transaction: %s"),
			    hp->name, line.buf);
	strbuf_release(&line);
	return ret;
}

static int helper_transaction_write_object_stream(struct odb_transaction *transaction,
						  struct odb_stream *stream,
						  struct object_id *oid)
{
	return odb_source_helper_write_object_stream(transaction->source, stream, oid);
}

static int helper_transaction_write_pack(struct odb_transaction *transaction,
					 int pack_fd UNUSED,
					 struct strbuf *err_msg,
					 const struct odb_transaction_write_pack_opts *opts UNUSED)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(transaction->source);

	strbuf_addf(err_msg, _("helper '%s' takes packfiles only through a "
			       "transaction for receiving them"), helper->hp.name);
	return -1;
}

/*
 * A child process opens a connection to the helper of its own, which writes
 * into the same store, so there is nothing to hand it.
 */
static int helper_transaction_env(struct odb_transaction *transaction UNUSED,
				  struct strvec *env UNUSED)
{
	return 0;
}

static int odb_source_helper_begin_transaction(struct odb_source *source,
					       struct odb_transaction **out,
					       enum odb_transaction_flags flags)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct helper_process *hp;
	struct odb_transaction *transaction;

	/*
	 * Objects received by git-receive-pack(1) must stay invisible until
	 * its hooks accept them, which the transaction of the files store
	 * does with a quarantine directory that child processes write into.
	 * Accepted objects land in the files store, which the helper takes in
	 * when optimized.
	 */
	if (flags & ODB_TRANSACTION_RECEIVE)
		return odb_source_begin_transaction(&helper->files->base, out, flags);

	hp = started(helper);
	if (capable(helper, OBJECT_HELPER_ODB_TRANSACTION)) {
		struct strbuf line = STRBUF_INIT;
		int ret = 0;

		helper_process_send(hp, "odb-transaction-begin\n");
		if (helper_process_readline(hp, &line) == EOF)
			drop_connection(helper);
		if (strcmp(line.buf, "ok"))
			ret = error(_("helper '%s' failed to begin a transaction: %s"),
				    hp->name, line.buf);
		strbuf_release(&line);
		if (ret)
			return ret;
	}

	CALLOC_ARRAY(transaction, 1);
	transaction->source = source;
	transaction->commit = helper_transaction_commit;
	transaction->write_object_stream = helper_transaction_write_object_stream;
	transaction->write_pack = helper_transaction_write_pack;
	transaction->env = helper_transaction_env;
	*out = transaction;
	return 0;
}

static int odb_source_helper_read_alternates(struct odb_source *source,
					     struct strvec *out)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	return odb_source_read_alternates(&helper->files->base, out);
}

struct drain {
	struct odb_source_helper *helper;
	/*
	 * Whether the objects replace how the helper stores those it has,
	 * rather than skipping them.
	 */
	unsigned replace : 1;
	/* Whether the loose objects of the files store move, too. */
	unsigned loose : 1;
	/* The packfiles whose objects move into the helper. */
	struct packed_git **packs;
	size_t packs_nr;
	/* The objects this drain stored in the helper. */
	struct oidset stored;
	/* Packed deltas waiting for their base to be stored first. */
	struct drain_waiting {
		struct packed_git *pack;
		uint32_t pos;
	} *waiting;
	size_t waiting_nr, waiting_alloc;
	struct oid_array promisor;
};

/* Whether the helper stores `oid`, from before or from this drain. */
static int drain_stored(struct drain *drain, const struct object_id *oid)
{
	return oidset_contains(&drain->stored, oid) ||
	       helper_read_object(drain->helper, oid, NULL, 0, NULL) == ODB_READ_OK;
}

/* Whether `oid` is among the objects this drain moves into the helper. */
static int drain_moves(struct drain *drain, const struct object_id *oid)
{
	for (size_t i = 0; i < drain->packs_nr; i++)
		if (find_pack_entry_one(oid, drain->packs[i]))
			return 1;
	return drain->loose &&
	       odb_source_read_object_info(&drain->helper->files->loose->base,
					   oid, NULL, 0, NULL) == ODB_READ_OK;
}

/* Copy one object of the files store into the helper whole. */
static int drain_whole(struct drain *drain, const struct object_id *oid)
{
	struct odb_source_helper *helper = drain->helper;
	struct object_info oi = OBJECT_INFO_INIT;
	enum object_type type;
	size_t size;
	void *buf = NULL;
	int ret;

	oi.typep = &type;
	oi.sizep = &size;
	oi.contentp = &buf;
	if (odb_source_read_object_info(&helper->files->base, oid, &oi, 0, NULL))
		return error(_("unable to read %s to move it into helper '%s'"),
			     oid_to_hex(oid), helper->hp.name);
	ret = odb_source_helper_write_object(&helper->base, buf, size, type,
					     oid, NULL, NULL, 0);
	if (!ret)
		oidset_insert(&drain->stored, oid);
	free(buf);
	return ret;
}

/* Store an object with "put-raw" as its packfile keeps it. */
static int put_raw(struct drain *drain, const struct object_id *oid,
		   const struct packed_raw_entry *entry)
{
	struct odb_source_helper *helper = drain->helper;
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1], base[GIT_MAX_HEXSZ + 1];
	int ret = 0;

	oid_to_hex_r(hex, oid);
	if (is_null_oid(&entry->delta_base))
		xsnprintf(base, sizeof(base), "0");
	else
		oid_to_hex_r(base, &entry->delta_base);

	helper_process_send(hp, "put-raw %s %s %"PRIuMAX" %s %"PRIuMAX"%s\n",
			    hex, type_name(entry->type), (uintmax_t)entry->size,
			    base, (uintmax_t)entry->data_len,
			    drain->replace ? " 1" : "");
	helper_process_write(hp, entry->data, entry->data_len);
	if (helper_process_readline(hp, &line) == EOF)
		drop_connection(helper);
	if (strcmp(line.buf, hex))
		ret = error(_("helper '%s' failed to write %s: %s"), hp->name,
			    hex, line.len ? line.buf : _("no reply"));
	else
		oidset_insert(&drain->stored, oid);
	strbuf_release(&line);
	return ret;
}

/*
 * Move the object at index position `pos` of `pack` into the helper as the
 * packfile keeps it, as a delta if it is one, provided the helper takes
 * objects so. The base of a delta is stored first: a delta whose base the
 * helper lacks waits for it if `may_wait` and the base moves along, like
 * the bases index-pack appends to a thin pack, and is stored whole
 * otherwise. Returns 0 once the object is stored or waiting, and -1 on
 * error.
 */
static int drain_packed(struct drain *drain, const struct object_id *oid,
			struct packed_git *pack, uint32_t pos, int may_wait)
{
	struct packed_raw_entry entry;
	int ret;

	if (!capable(drain->helper, OBJECT_HELPER_PUT_RAW) ||
	    packed_object_raw_entry(pack, nth_packed_object_offset(pack, pos),
				    &entry))
		return drain_whole(drain, oid);

	if (!is_null_oid(&entry.delta_base) &&
	    !drain_stored(drain, &entry.delta_base)) {
		free(entry.data);
		if (may_wait && drain_moves(drain, &entry.delta_base)) {
			ALLOC_GROW(drain->waiting, drain->waiting_nr + 1,
				   drain->waiting_alloc);
			drain->waiting[drain->waiting_nr].pack = pack;
			drain->waiting[drain->waiting_nr].pos = pos;
			drain->waiting_nr++;
			return 0;
		}
		return drain_whole(drain, oid);
	}

	ret = put_raw(drain, oid, &entry);
	free(entry.data);
	return ret;
}

/*
 * Store the deltas left waiting for their base, in rounds, as a base may
 * itself have been waiting. A round that stores none of them leaves no base
 * to wait for, so the rest are stored whole.
 */
static int drain_waiting(struct drain *drain)
{
	int may_wait = 1;

	while (drain->waiting_nr) {
		struct drain_waiting *waiting = drain->waiting;
		size_t nr = drain->waiting_nr;
		int ret = 0;

		drain->waiting = NULL;
		drain->waiting_nr = drain->waiting_alloc = 0;
		for (size_t i = 0; !ret && i < nr; i++) {
			struct object_id oid;

			if (nth_packed_object_id(&oid, waiting[i].pack,
						 waiting[i].pos) < 0)
				ret = -1;
			else if (!drain_stored(drain, &oid))
				ret = drain_packed(drain, &oid, waiting[i].pack,
						   waiting[i].pos, may_wait);
		}
		free(waiting);
		if (ret)
			return ret;
		if (drain->waiting_nr == nr)
			may_wait = 0;
	}
	return 0;
}

static int drain_packed_object(const struct object_id *oid,
			       struct packed_git *pack,
			       uint32_t pos, void *data)
{
	struct drain *drain = data;

	if (pack->pack_promisor)
		oid_array_append(&drain->promisor, oid);
	if (!drain->replace && drain_stored(drain, oid))
		return 0;
	return drain_packed(drain, oid, pack, pos, 1);
}

static int drain_loose_object(const struct object_id *oid,
			      struct object_info *oi UNUSED, void *data)
{
	struct drain *drain = data;

	if (drain_stored(drain, oid))
		return 0;
	return drain_whole(drain, oid);
}

/*
 * Remove a loose object the helper stores, as prune-packed removes one a
 * packfile holds: one that arrived after the drain looked for them stays.
 */
static int remove_loose_object(const struct object_id *oid, const char *path,
			       void *data)
{
	if (drain_stored(data, oid) && unlink(path) && errno != ENOENT)
		warning_errno(_("unable to remove %s"), path);
	return 0;
}

/* Remove a directory of loose objects left empty, as prune-packed does. */
static int remove_loose_subdir(unsigned int nr UNUSED, const char *path,
			       void *data UNUSED)
{
	rmdir(path);
	return 0;
}

/* Mark the objects of the promisor packs drained as promisor objects. */
static int mark_promisor_objects(struct drain *drain)
{
	struct odb_source_helper *helper = drain->helper;
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	if (!drain->promisor.nr)
		return 0;
	if (!capable(helper, OBJECT_HELPER_PROMISOR))
		return error(_("helper '%s' cannot keep promisor objects"), hp->name);

	helper_process_send(hp, "promisor\n");
	for (size_t i = 0; i < drain->promisor.nr; i++)
		helper_process_send(hp, "%s\n", oid_to_hex(&drain->promisor.oid[i]));
	helper_process_send(hp, "\n");
	if (helper_process_readline(hp, &line) == EOF)
		drop_connection(helper);
	if (strcmp(line.buf, "ok"))
		ret = error(_("helper '%s' failed to mark promisor objects: %s"),
			    hp->name, line.buf);
	strbuf_release(&line);
	return ret;
}

/*
 * Move the objects of `packs`, and with `loose` the loose objects of the
 * files store, into the helper: copy them, then remove the packfiles and
 * loose objects they came from. With `replace`, the objects replace how the
 * helper stores those it has already.
 */
static int drain(struct odb_source_helper *helper, struct packed_git **packs,
		 size_t packs_nr, int loose, int replace)
{
	struct odb_source_files *files = helper->files;
	struct odb_for_each_object_options opts = { 0 };
	struct drain drain = {
		.helper = helper,
		.replace = !!replace,
		.loose = !!loose,
		.packs = packs,
		.packs_nr = packs_nr,
		.stored = OIDSET_INIT,
		.promisor = OID_ARRAY_INIT,
	};
	struct odb_transaction *transaction = NULL;
	int ret = 0;

	for (size_t i = 0; i < packs_nr; i++)
		/* The objects of a pack are counted once its index is open. */
		if (open_pack_index(packs[i]))
			return error(_("unable to open the index of %s"),
				     packs[i]->pack_name);

	if (!helper->base.odb->transaction &&
	    odb_source_helper_begin_transaction(&helper->base, &transaction, 0)) {
		ret = -1;
		goto out;
	}

	/* In pack order, the base of an offset delta comes before it. */
	for (size_t i = 0; i < drain.packs_nr; i++) {
		struct packed_git *p = drain.packs[i];

		if (for_each_object_in_pack(p, drain_packed_object, &drain,
					    ODB_FOR_EACH_OBJECT_PACK_ORDER)) {
			ret = error(_("unable to move %s into helper '%s'"),
				    p->pack_name, helper->hp.name);
			goto out;
		}
	}
	if ((loose && odb_source_for_each_object(&files->loose->base, NULL,
						 drain_loose_object, &drain,
						 &opts)) ||
	    drain_waiting(&drain)) {
		ret = -1;
		goto out;
	}

	ret = mark_promisor_objects(&drain);
	if (ret)
		goto out;

	/* Only remove what the helper keeps durably. */
	if (transaction) {
		ret = helper_transaction_commit(transaction);
		FREE_AND_NULL(transaction);
		if (ret)
			goto out;
	}

	if (loose)
		for_each_loose_file_in_source(&files->loose->base,
					      remove_loose_object, NULL,
					      remove_loose_subdir, &drain);
	if (packs_nr && get_multi_pack_index(files->packed))
		clear_midx_file(helper->base.odb->repo);
	for (size_t i = 0; i < packs_nr; i++) {
		close_pack(packs[i]);
		unlink_pack_path(packs[i]->pack_name, 0);
	}
	odb_source_prepare(&files->base, ODB_PREPARE_FLUSH_CACHES);

out:
	/* Leaving the batch of the helper uncommitted discards it. */
	if (transaction) {
		drop_connection(helper);
		free(transaction);
	}
	free(drain.waiting);
	oidset_clear(&drain.stored);
	oid_array_clear(&drain.promisor);
	return ret;
}

/*
 * Move the objects of the files store into the helper. A packfile with a
 * .keep file is left for later, as the .keep of a fetch or push still
 * receiving it looks the same as one keeping it for good, and so is one
 * that another process removed since the store was last prepared, which
 * stays listed.
 */
static int drain_files_store(struct odb_source_helper *helper)
{
	struct odb_source_files *files = helper->files;
	struct packfile_list_entry *e;
	struct packed_git **packs = NULL;
	size_t nr = 0, alloc = 0;
	int ret;

	odb_source_prepare(&files->base, ODB_PREPARE_FLUSH_CACHES);
	for (e = packfile_store_get_packs(files->packed); e; e = e->next) {
		if (e->pack->pack_keep || !file_exists(e->pack->pack_name))
			continue;
		ALLOC_GROW(packs, nr + 1, alloc);
		packs[nr++] = e->pack;
	}

	ret = drain(helper, packs, nr, 1, 0);
	free(packs);
	return ret;
}

/*
 * Have the helper store its objects as deltas where git finds good ones, as
 * a repack does for packfiles: pack-objects packs every reachable object
 * into the files store, reading those of the helper through the object
 * database to search for deltas among them, and the packfile then moves
 * into the helper, replacing how it stores each object. The objects of kept
 * packfiles stay out of it, as they stay out of the helper.
 */
static int deltify(struct odb_source_helper *helper,
		   const struct odb_optimize_options *opts)
{
	struct repository *repo = helper->base.odb->repo;
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct pack_objects_args args = PACK_OBJECTS_ARGS_INIT;
	struct strbuf base = STRBUF_INIT, line = STRBUF_INIT;
	struct packed_git **packs = NULL;
	size_t nr = 0, alloc = 0;
	int ret = 0;
	FILE *out;

	args.local = 1;
	args.pack_kept_objects = 0;
	args.delta_base_offset = 1;
	args.quiet = !(opts->flags & ODB_OPTIMIZE_VERBOSE);
	args.no_reuse_delta = !!(opts->flags & ODB_OPTIMIZE_NO_REUSE_DELTAS);
	if (opts->window > 0)
		args.window = xstrfmt("%d", opts->window);
	if (opts->depth > 0)
		args.depth = xstrfmt("%d", opts->depth);

	strbuf_addf(&base, "%s/pack/pack", helper->files->base.path);
	prepare_pack_objects(&cmd, &args, base.buf);
	strvec_pushl(&cmd.args, "--keep-true-parents", "--non-empty",
		     "--all", "--reflog", "--indexed-objects", NULL);
	if (repo_has_promisor_remote(repo))
		strvec_push(&cmd.args, "--exclude-promisor-objects");
	cmd.no_stdin = 1;

	if (start_command(&cmd)) {
		ret = error(_("unable to start pack-objects for helper '%s'"),
			    helper->hp.name);
		goto out;
	}
	/* pack-objects names each packfile it writes by its hash. */
	out = xfdopen(cmd.out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		struct strbuf idx = STRBUF_INIT;
		struct packed_git *p;

		strbuf_addf(&idx, "%s-%s.idx", base.buf, line.buf);
		p = packfile_store_load_pack(helper->files->packed, idx.buf, 1);
		if (!p)
			ret = error(_("unable to open the packfile %s"), idx.buf);
		else {
			ALLOC_GROW(packs, nr + 1, alloc);
			packs[nr++] = p;
		}
		strbuf_release(&idx);
	}
	fclose(out);
	if (finish_command(&cmd))
		ret = error(_("pack-objects failed for helper '%s'"),
			    helper->hp.name);

	if (!ret && nr)
		ret = drain(helper, packs, nr, 0, 1);

out:
	pack_objects_args_release(&args);
	strbuf_release(&base);
	strbuf_release(&line);
	free(packs);
	return ret;
}

/*
 * Remove the unreachable objects of the helper past the expiry, as the
 * repack optimizing the files backend expires the unreachable objects of
 * its packfiles. git-prune(1) finds out which objects are reachable.
 */
static int prune_unreachable(struct odb_source_helper *helper,
			     const char *expire)
{
	struct child_process cmd = CHILD_PROCESS_INIT;

	cmd.git_cmd = 1;
	strvec_pushl(&cmd.args, "prune", "--expire", expire, NULL);
	if (repo_has_promisor_remote(helper->base.odb->repo))
		strvec_push(&cmd.args, "--exclude-promisor-objects");
	if (run_command(&cmd))
		return error(_("unable to prune the objects of helper '%s'"),
			     helper->hp.name);
	return 0;
}

static int odb_source_helper_optimize(struct odb_source *source,
				      const struct odb_optimize_options *opts)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct helper_process *hp;
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	/*
	 * Objects of a repository that others borrow through their alternates
	 * stay in its files store, where those see them, and are never
	 * pruned.
	 */
	if (!source->odb->repo->repository_format_precious_objects) {
		if (drain_files_store(helper))
			return -1;
		if (capable(helper, OBJECT_HELPER_PUT_RAW) &&
		    capable(helper, OBJECT_HELPER_REPLACE) &&
		    deltify(helper, opts))
			return -1;
		/* Deltas off an unreachable base were moved off it just now. */
		if (opts->prune_expire && capable(helper, OBJECT_HELPER_PRUNE) &&
		    prune_unreachable(helper, opts->prune_expire))
			return -1;
	}

	hp = started(helper);
	if (!capable(helper, OBJECT_HELPER_OPTIMIZE))
		return 0;

	helper_process_send(hp, "optimize\n");
	if (helper_process_readline(hp, &line) == EOF)
		drop_connection(helper);
	if (strcmp(line.buf, "ok"))
		ret = error(_("helper '%s' failed to optimize its objects: %s"),
			    hp->name, line.buf);
	strbuf_release(&line);
	return ret;
}

static int found_object(const struct object_id *oid UNUSED,
			struct object_info *oi UNUSED, void *data UNUSED)
{
	return 1;
}

static bool odb_source_helper_optimize_required(struct odb_source *source,
						const struct odb_optimize_options *opts UNUSED)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct odb_source_files *files = helper->files;
	struct odb_for_each_object_options each_opts = { 0 };
	struct helper_process *hp;
	struct strbuf line = STRBUF_INIT;
	int gc_auto = 6700;
	bool required = false;

	repo_config_get_int(source->odb->repo, "gc.auto", &gc_auto);
	if (gc_auto <= 0)
		return false;

	/* Objects waiting in the files store are due to move into the helper. */
	if (!source->odb->repo->repository_format_precious_objects) {
		odb_source_prepare(&files->base, ODB_PREPARE_FLUSH_CACHES);
		for (struct packfile_list_entry *e = packfile_store_get_packs(files->packed);
		     e; e = e->next)
			if (!e->pack->pack_keep)
				return true;
		if (odb_source_for_each_object(&files->loose->base, NULL,
					       found_object, NULL, &each_opts) > 0)
			return true;
	}

	/*
	 * The helper counts the objects written since it was last optimized,
	 * which stands in for the loose objects the files backend counts
	 * against gc.auto.
	 */
	hp = started(helper);
	if (!capable(helper, OBJECT_HELPER_OPTIMIZE_REQUIRED))
		return false;
	helper_process_send(hp, "optimize-required\n");
	if (helper_process_readline(hp, &line) == EOF) {
		drop_connection(helper);
	} else {
		char *end;
		long churn = strtol(line.buf, &end, 10);
		required = end != line.buf && churn >= gc_auto;
	}
	strbuf_release(&line);
	return required;
}

static int odb_source_helper_generate_pack(struct odb_source *source,
					   struct odb_pack_generator **out,
					   const struct odb_generate_pack_options *opts)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);

	/*
	 * The files backend generates packfiles with git-pack-objects(1),
	 * which reads objects through the object database, so it serves the
	 * objects of the helper as well.
	 */
	return odb_source_generate_pack(&helper->files->base, out, opts);
}

struct fsck_helper_data {
	struct odb_fsck_options *opts;
	const struct git_hash_algo *algo;
	int errors;
};

static int fsck_helper_object(const struct object_id *oid,
			      struct object_info *oi, void *data)
{
	struct fsck_helper_data *fsck = data;
	struct object_id computed;
	int eaten = 0;

	hash_object_file(fsck->algo, *oi->contentp, *oi->sizep, *oi->typep,
			 &computed);
	if (!oideq(&computed, oid)) {
		error(_("%s: hash mismatch, found %s"), oid_to_hex(oid),
		      oid_to_hex(&computed));
		fsck->errors++;
	} else if (fsck->opts->object_cb(oid, *oi->typep, *oi->sizep,
					 *oi->contentp, &eaten,
					 fsck->opts->object_payload)) {
		fsck->errors++;
	}

	if (!eaten)
		free(*oi->contentp);
	*oi->contentp = NULL;
	/* Keep checking the other objects. */
	return 0;
}

static int odb_source_helper_fsck(struct odb_source *source,
				  struct odb_fsck_options *opts)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct odb_for_each_object_options each_opts = { 0 };
	struct helper_process *hp = started(helper);
	struct fsck_helper_data fsck = {
		.opts = opts,
		.algo = source->odb->repo->hash_algo,
	};
	enum object_type type;
	size_t size;
	void *content;
	struct object_info request = {
		.typep = &type,
		.sizep = &size,
		.contentp = &content,
	};
	int ret = 0;

	if (opts->object_cb &&
	    (for_each_helper_object(helper, &request, fsck_helper_object,
				    &fsck, &each_opts) || fsck.errors))
		ret = -1;

	if (capable(helper, OBJECT_HELPER_VERIFY)) {
		struct strbuf line = STRBUF_INIT;

		helper_process_send(hp, "verify\n");
		if (helper_process_readline(hp, &line) == EOF)
			drop_connection(helper);
		if (strcmp(line.buf, "ok"))
			ret = error(_("helper '%s' found its objects damaged: %s"),
				    hp->name, line.len ? line.buf : _("no reply"));
		strbuf_release(&line);
	}

	if (odb_source_fsck(&helper->files->base, opts))
		ret = -1;
	return ret;
}

static int listed_object_cmp(const void *a, const void *b)
{
	const struct listed_object *x = a, *y = b;
	return oidcmp(&x->oid, &y->oid);
}

static const struct object_id *listed_object_oid(size_t i, const void *table)
{
	const struct listed_object *objects = table;
	return &objects[i].oid;
}

/*
 * Git knows which objects are reachable and the helper when it wrote each,
 * so git sends the unreachable ones for the helper to remove those it wrote
 * before the expiry. The objects of the files store are loose objects and
 * packfiles, which git-prune(1) and git-repack(1) remove themselves.
 */
static int odb_source_helper_prune(struct odb_source *source,
				   const struct odb_prune_options *opts)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;
	struct helper_process *hp = started(helper);
	struct strbuf line = STRBUF_INIT;
	struct listed_object *objects;
	size_t nr, unreachable = 0;
	int ret = 0;

	if (!capable(helper, OBJECT_HELPER_PRUNE))
		return 0;
	if (list_objects(helper, "", &objects, &nr) < 0)
		return error(_("unable to list the objects of helper '%s'"),
			     hp->name);

	for (size_t i = 0; i < nr; i++)
		if (!opts->is_reachable(&objects[i].oid, opts->data))
			objects[unreachable++] = objects[i];
	if (!unreachable)
		goto out;
	QSORT(objects, unreachable, listed_object_cmp);

	/* An object written at any time may go, unless an expiry says. */
	helper_process_send(hp, "prune%s", (opts->flags & ODB_PRUNE_DRY_RUN) ?
			    " --dry-run" : "");
	if (opts->expire != TIME_MAX)
		helper_process_send(hp, " %"PRItime, opts->expire);
	helper_process_send(hp, "\n");
	for (size_t i = 0; i < unreachable; i++)
		helper_process_send(hp, "%s\n", oid_to_hex(&objects[i].oid));
	helper_process_send(hp, "\n");

	/* The helper names each object it prunes, then says "ok". */
	while (1) {
		struct object_id oid;
		const char *end;
		int pos;

		if (helper_process_readline(hp, &line) == EOF) {
			drop_connection(helper);
			ret = error(_("helper '%s' hung up while pruning"), hp->name);
			break;
		}
		if (!strcmp(line.buf, "ok"))
			break;
		if (parse_oid_hex_algop(line.buf, &oid, &end, algo) || *end ||
		    (pos = oid_pos(&oid, objects, unreachable,
				   listed_object_oid)) < 0) {
			const char *reason = line.buf;

			skip_prefix(reason, "error ", &reason);
			ret = error(_("helper '%s' failed to prune: %s"),
				    hp->name, reason);
			/* The reply may go on; start over on the next command. */
			drop_connection(helper);
			break;
		}
		opts->pruned(&oid, objects[pos].type, opts->data);
	}

out:
	strbuf_release(&line);
	free(objects);
	return ret;
}

int odb_source_helper_take_files_store(struct odb_source_helper *helper)
{
	return drain_files_store(helper);
}

/*
 * Pack `oids`, objects of the helper, into the files store with
 * pack-objects, which reads them through the object database, and mark the
 * packfile as a promisor packfile with `promisor`.
 */
static int pack_into_files_store(struct odb_source_helper *helper,
				 const struct oid_array *oids, int promisor)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct pack_objects_args args = PACK_OBJECTS_ARGS_INIT;
	struct strbuf base = STRBUF_INIT, line = STRBUF_INIT;
	FILE *in, *out;
	int ret = 0;

	if (!oids->nr)
		return 0;

	args.local = 1;
	args.quiet = 1;
	args.delta_base_offset = 1;
	strbuf_addf(&base, "%s/pack/pack", helper->files->base.path);
	prepare_pack_objects(&cmd, &args, base.buf);
	cmd.in = -1;
	if (start_command(&cmd)) {
		ret = error(_("unable to start pack-objects for helper '%s'"),
			    helper->hp.name);
		goto out;
	}

	/* pack-objects reads its input in full before it writes a line. */
	in = xfdopen(cmd.in, "w");
	for (size_t i = 0; i < oids->nr; i++)
		fprintf(in, "%s\n", oid_to_hex(&oids->oid[i]));
	fclose(in);

	out = xfdopen(cmd.out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		if (promisor) {
			struct strbuf path = STRBUF_INIT;

			strbuf_addf(&path, "%s-%s.promisor", base.buf, line.buf);
			write_file_buf(path.buf, "", 0);
			strbuf_release(&path);
		}
	}
	fclose(out);
	if (finish_command(&cmd))
		ret = error(_("pack-objects failed for helper '%s'"),
			    helper->hp.name);

out:
	pack_objects_args_release(&args);
	strbuf_release(&base);
	strbuf_release(&line);
	return ret;
}

int odb_source_helper_copy_to_files_store(struct odb_source_helper *helper,
					  struct oid_array *copied)
{
	struct oid_array regular = OID_ARRAY_INIT, promisor = OID_ARRAY_INIT;
	struct listed_object *objects = NULL, *promisors = NULL;
	size_t nr, promisors_nr = 0;
	int ret = 0;

	if (list_objects(helper, "", &objects, &nr) < 0 ||
	    (capable(helper, OBJECT_HELPER_PROMISOR) &&
	     list_objects(helper, " --promisor-only", &promisors,
			  &promisors_nr) < 0)) {
		ret = error(_("unable to list the objects of helper '%s'"),
			    helper->hp.name);
		goto out;
	}

	QSORT(promisors, promisors_nr, listed_object_cmp);
	for (size_t i = 0; i < nr; i++) {
		if (oid_pos(&objects[i].oid, promisors, promisors_nr,
			    listed_object_oid) >= 0)
			oid_array_append(&promisor, &objects[i].oid);
		else
			oid_array_append(&regular, &objects[i].oid);
	}

	if (pack_into_files_store(helper, &regular, 0) ||
	    pack_into_files_store(helper, &promisor, 1)) {
		ret = -1;
		goto out;
	}
	for (size_t i = 0; i < nr; i++)
		oid_array_append(copied, &objects[i].oid);

out:
	oid_array_clear(&regular);
	oid_array_clear(&promisor);
	free(objects);
	free(promisors);
	return ret;
}

/* Of the objects of the helper, those in the oid_array `data` go. */
static bool not_removed(const struct object_id *oid, void *data)
{
	return oid_array_lookup(data, oid) < 0;
}

static void removed(const struct object_id *oid UNUSED,
		    enum object_type type UNUSED, void *data UNUSED)
{
}

int odb_source_helper_remove_objects(struct odb_source_helper *helper,
				     struct oid_array *oids)
{
	struct odb_prune_options opts = {
		.expire = TIME_MAX,
		.is_reachable = not_removed,
		.pruned = removed,
		.data = oids,
	};

	if (!capable(helper, OBJECT_HELPER_PRUNE)) {
		warning(_("helper '%s' cannot remove the objects it stores"),
			helper->hp.name);
		return 0;
	}
	return odb_source_helper_prune(&helper->base, &opts);
}

static int odb_source_helper_create_on_disk(struct odb_source *source,
					    const struct odb_create_on_disk_options *opts)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	return odb_source_create_on_disk(&helper->files->base, opts);
}

static void odb_source_helper_prepare(struct odb_source *source,
				      enum odb_prepare_flags flags)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);

	odb_source_prepare(&helper->files->base, flags);
	if (flags & ODB_PREPARE_FLUSH_CACHES)
		refresh(helper);
}

static void odb_source_helper_close(struct odb_source *source)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);
	struct odb_transaction *transaction = source->odb->transaction;

	odb_source_close(&helper->files->base);

	/* A transaction still batching into the helper lives in the process. */
	if (transaction && transaction->source == source)
		return;
	helper_process_disconnect(&helper->hp);
}

static void odb_source_helper_free(struct odb_source *source)
{
	struct odb_source_helper *helper = odb_source_helper_downcast(source);

	helper_process_release(&helper->hp);
	odb_source_free(&helper->files->base);
	free(helper->base.path);
	free(helper);
}

struct odb_source_helper *odb_source_helper_new(struct object_database *odb,
						const char *name,
						const char *path,
						bool local)
{
	struct odb_source_helper *helper;

	CALLOC_ARRAY(helper, 1);
	odb_source_init(&helper->base, odb, ODB_SOURCE_HELPER, path, local);
	helper_process_init(&helper->hp, name, odb->repo->commondir,
			    object_helper_capabilities);
	helper->files = odb_source_files_new(odb, path, local);

	helper->base.free = odb_source_helper_free;
	helper->base.close = odb_source_helper_close;
	helper->base.create_on_disk = odb_source_helper_create_on_disk;
	helper->base.prepare = odb_source_helper_prepare;
	helper->base.fsck = odb_source_helper_fsck;
	helper->base.prune = odb_source_helper_prune;
	helper->base.read_object_info = odb_source_helper_read_object_info;
	helper->base.read_object_stream = odb_source_helper_read_object_stream;
	helper->base.read_object_raw = odb_source_helper_read_object_raw;
	helper->base.for_each_object = odb_source_helper_for_each_object;
	helper->base.count_objects = odb_source_helper_count_objects;
	helper->base.find_abbrev_len = odb_source_helper_find_abbrev_len;
	helper->base.freshen_object = odb_source_helper_freshen_object;
	helper->base.write_object = odb_source_helper_write_object;
	helper->base.write_object_stream = odb_source_helper_write_object_stream;
	helper->base.begin_transaction = odb_source_helper_begin_transaction;
	helper->base.read_alternates = odb_source_helper_read_alternates;
	helper->base.optimize = odb_source_helper_optimize;
	helper->base.optimize_required = odb_source_helper_optimize_required;
	helper->base.generate_pack = odb_source_helper_generate_pack;

	return helper;
}
