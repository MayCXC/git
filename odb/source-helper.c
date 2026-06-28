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
#include "commit.h"
#include "commit-graph.h"
#include "odb/transaction.h"
#include "object-file.h"
#include "tempfile.h"
#include "oidset.h"
#include "loose.h"
#include "odb/pack-ingest.h"
#include "odb/source-files.h"
#include "odb/source-helper.h"
#include "environment.h"
#include "odb/streaming.h"
#include "pack-bitmap.h"
#include "packfile.h"
#include "pack-revindex.h"
#include "dir.h"
#include "helper.h"
#include "lockfile.h"
#include "pack.h"
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

	oidset_clear(&src->promisor_objects);
	/* Don't release the helper process; it's shared via repo->odb_local_helper */
	odb_source_release(&src->base);
	free(src);
}

static void helper_close(struct odb_source *source UNUSED)
{
	/*
	 * The object helper process is shared via repo->odb_local_helper
	 * (every object source in the repo borrows it), so this ODB source
	 * is only a borrower and must not stop it, exactly as helper_free()
	 * above leaves it alone. Unlike a remote helper, which is owned by a
	 * single transport and torn down once in disconnect_helper(), our
	 * helper has one owner, repo->odb_local_helper, shut down once by
	 * helper_process_release() at repository teardown. The old code
	 * here sent a "close" command and waited for the process without
	 * closing its stdin; a helper that exits on EOF rather than on that
	 * command would never be told to exit and finish_command() hung.
	 */
}

static void helper_reprepare(struct odb_source *source)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	helper_process_refresh(src->hp);
	/* Objects may have changed; drop the cached promisor set. */
	oidset_clear(&src->promisor_objects);
	src->promisor_loaded = 0;
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

	/*
	 * Mirror packfile_store_read_object_info(): when the first read misses,
	 * the ODB layer (odb.c do_oid_object_info_extended) retries with
	 * OBJECT_INFO_SECOND_READ so a source can reload on-disk state. A helper
	 * may answer reads from a snapshot (e.g. a read transaction) that does
	 * not yet reflect objects another process has committed since; refreshing
	 * drops that snapshot before we query again, exactly as the packed store
	 * reprepares to discover newly arrived packs. The refresh is paid only on
	 * a miss, never on the hot path, and a helper lacking the capability
	 * treats it as a no-op.
	 */
	if (flags & OBJECT_INFO_SECOND_READ)
		helper_process_refresh(hp);

	oid_to_hex_r(hex, oid);

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
	if (want_content) {
		/*
		 * Only "get" returns the bytes; if the helper cannot supply
		 * them, fail rather than fall through to "info" and report
		 * success with the content pointer left unset.
		 */
		if (!hp->cap_get)
			goto out;
		helper_process_send(hp, "get %s\n", hex);
	} else if (hp->cap_info) {
		helper_process_send(hp, "info %s\n", hex);
	} else {
		/*
		 * Metadata-only request but the helper has no "info" capability.
		 * Do not fall back to "get": its reply carries the object payload
		 * after the header, which a metadata-only caller leaves unread and
		 * which would desync the pipe for the next command. A helper must
		 * advertise "info" to answer metadata-only queries.
		 */
		goto out;
	}

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
	/*
	 * The helper exposes only type and size; default the remaining
	 * response fields the way the in-memory source does, and set whence so
	 * callers (e.g. has_object_pack via OI_PACKED) never mistake a helper
	 * object for a packed one.
	 */
	if (oi->disk_sizep)
		*(oi->disk_sizep) = 0;
	/*
	 * The reply is "<type> <size>" for a resolved object and
	 * "<type> <size> <base-oid> <delta-len>" when the helper stores the
	 * object as a git-format delta. Report the delta base and raw delta
	 * length when present, so the send path can reuse the stored delta; clear
	 * them otherwise, as the in-memory source does.
	 */
	{
		char base_hex[GIT_MAX_HEXSZ + 1];
		unsigned long dlen;
		struct object_id dbase;

		if (sscanf(line.buf, "%*s %*u %64s %lu", base_hex, &dlen) == 2 &&
		    !get_oid_hex_algop(base_hex, &dbase, source->odb->repo->hash_algo)) {
			if (oi->delta_base_oid)
				oidcpy(oi->delta_base_oid, &dbase);
			if (oi->delta_size)
				*(oi->delta_size) = dlen;
		} else {
			if (oi->delta_base_oid)
				oidclr(oi->delta_base_oid, source->odb->repo->hash_algo);
			if (oi->delta_size)
				*(oi->delta_size) = 0;
		}
	}
	if (oi->mtimep)
		*(oi->mtimep) = 0;
	oi->whence = OI_CACHED;

	if (want_content) {
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
	struct tempfile *tmp;
	off_t remaining;
};

static ssize_t helper_stream_read(struct odb_read_stream *_st, char *buf, size_t sz)
{
	struct helper_read_stream *st =
		container_of(_st, struct helper_read_stream, base);
	ssize_t got;

	if (!st->remaining)
		return 0;

	if (sz > (size_t)st->remaining)
		sz = (size_t)st->remaining;

	got = read_in_full(st->tmp->fd, buf, sz);
	if (got < 0)
		return -1;
	st->remaining -= got;
	return got;
}

static int helper_stream_close(struct odb_read_stream *_st)
{
	struct helper_read_stream *st =
		container_of(_st, struct helper_read_stream, base);

	delete_tempfile(&st->tmp);
	return 0;
}

/*
 * Read a (possibly large) object by spooling it off the shared helper pipe into
 * a tempfile, then handing back a stream over that file. The caller may touch
 * the helper for other objects while consuming the stream (a clean/smudge filter
 * reading another blob, say); draining the whole object here first, in a tight
 * loop that issues no other helper command, keeps the pipe in sync so that
 * cannot desync it (the re-entrancy class fixed for the reflog iterator). The
 * returned stream then reads the tempfile, never the pipe. git owns the boundary
 * file both directions: ingestion crosses a pack file in, this crosses an object
 * file out, via git's tempfile API (object directory, auto-removed on close or
 * exit). Written then immediately read and deleted, it stays in the page cache,
 * so it is RAM-speed despite being a named file.
 */
static int helper_read_object_stream(struct odb_read_stream **out,
				     struct odb_source *source,
				     const struct object_id *oid)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT, tmpl = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	char type_str[32];
	unsigned long size;
	struct helper_read_stream *st;
	struct tempfile *tmp;
	off_t spooled = 0;

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

	strbuf_addf(&tmpl, "%s/tmp_helper_obj_XXXXXX",
		    repo_get_object_directory(source->odb->repo));
	tmp = mks_tempfile(tmpl.buf);
	strbuf_release(&tmpl);
	if (!tmp)
		return -1;

	while (spooled < (off_t)size) {
		char buf[65536];
		size_t want = sizeof(buf);
		size_t got;

		if ((off_t)want > (off_t)size - spooled)
			want = (size_t)((off_t)size - spooled);
		got = fread(buf, 1, want, hp->out);
		if (!got || write_in_full(tmp->fd, buf, got) < 0) {
			delete_tempfile(&tmp);
			return -1;
		}
		spooled += (off_t)got;
	}

	if (lseek(tmp->fd, 0, SEEK_SET) == (off_t)-1) {
		delete_tempfile(&tmp);
		return -1;
	}

	CALLOC_ARRAY(st, 1);
	st->base.type = type_from_string(type_str);
	st->base.size = size;
	st->base.read = helper_stream_read;
	st->base.close = helper_stream_close;
	st->tmp = tmp;
	st->remaining = (off_t)size;

	*out = &st->base;
	return 0;
}

/*
 * Match the first `hex_len` hex digits of `oid` against `prefix`. The prefix's
 * own algorithm is irrelevant: we compare raw hash bytes, which lets a compat
 * object id be matched against a storage-algorithm prefix and vice versa.
 */
static int oid_has_hex_prefix(const struct object_id *oid,
			      const struct object_id *prefix, size_t hex_len)
{
	size_t nbytes = hex_len / 2;
	if (memcmp(oid->hash, prefix->hash, nbytes))
		return 0;
	if ((hex_len & 1) &&
	    ((oid->hash[nbytes] ^ prefix->hash[nbytes]) & 0xf0))
		return 0;
	return 1;
}

static int helper_for_each_object(struct odb_source *source,
				  const struct object_info *request,
				  odb_for_each_object_cb cb,
				  void *cb_data,
				  const struct odb_for_each_object_options *opts)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct repository *repo = source->odb->repo;
	const struct git_hash_algo *compat = repo->compat_hash_algo;
	const struct object_id *prefix = opts ? opts->prefix : NULL;
	size_t prefix_len = opts ? opts->prefix_hex_len : 0;
	struct strbuf line = STRBUF_INIT;
	struct strbuf cmd = STRBUF_INIT;
	unsigned flags = opts ? opts->flags : 0;
	int ret = 0;
	/*
	 * Scope the enumeration to the requested prefix server-side, except under
	 * compat interop: a prefix may name an object by its compat id
	 * (extensions.compatObjectFormat), which the helper, knowing only storage
	 * ids, cannot match, so in interop mode we stream every id and filter
	 * git-side below (trying the compat id too).
	 */
	int scoped = prefix && !compat;

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
	if (scoped) {
		char prefix_hex[GIT_MAX_HEXSZ + 1];
		oid_to_hex_r(prefix_hex, prefix);
		prefix_hex[prefix_len] = '\0';
		strbuf_addf(&cmd, " %s", prefix_hex);
	}
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
			size_t prev_alloc;

			if (!line.len)
				break;
			if (sscanf(line.buf, "%64s %31s %lu",
				   oid_hex, type_str, &sz) != 3)
				continue;
			if (get_oid_hex_any(oid_hex, &oid) == GIT_HASH_UNKNOWN)
				continue;

			/* Grow the three parallel arrays in lockstep under one
			 * geometric capacity; sizing types/sizes per element (the
			 * old REALLOC_ARRAY(.., nr+1)) reallocated every iteration. */
			prev_alloc = alloc;
			ALLOC_GROW(oids, nr + 1, alloc);
			if (alloc != prev_alloc) {
				REALLOC_ARRAY(types, alloc);
				REALLOC_ARRAY(sizes, alloc);
			}
			oidcpy(&oids[nr], &oid);
			types[nr] = type_from_string(type_str);
			sizes[nr] = sz;
			nr++;
		}
		strbuf_release(&line);

		for (size_t i = 0; i < nr; i++) {
			struct object_info oi = OBJECT_INFO_INIT;
			time_t unknown_mtime = 0;
			if (request && request->typep)
				oi.typep = &types[i];
			if (request && request->sizep)
				oi.sizep = &sizes[i];
			/*
			 * The list-objects protocol carries no per-object time, so
			 * report 0 ("not recent") when an mtime is requested (e.g. by
			 * "git prune"'s recent-object extension). Recency for a helper
			 * is enforced by its own stored time in remove_objects, not by
			 * this enumeration.
			 */
			if (request && request->mtimep)
				oi.mtimep = &unknown_mtime;

			/*
			 * When the prefix was not scoped server-side (compat
			 * interop, see above), filter git-side as the files backend
			 * filters via its loose cache. A prefix may name an object by
			 * its compat id (extensions.compatObjectFormat), so when the
			 * storage id does not match, try the object's compat id and
			 * yield that instead - callers expect the matching id, and the
			 * read path converts a compat id.
			 */
			if (!scoped && prefix &&
			    !oid_has_hex_prefix(&oids[i], prefix, prefix_len)) {
				struct object_id compat_oid;
				if (compat &&
				    !repo_loose_object_map_oid(repo, &oids[i], compat, &compat_oid) &&
				    oid_has_hex_prefix(&compat_oid, prefix, prefix_len)) {
					ret = cb(&compat_oid, &oi, cb_data);
					if (ret)
						break;
				}
				continue;
			}

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

/*
 * Freshen an object so prune keeps it within the grace window even when
 * unreachable (the odb freshen_object vtable; files utime's the loose object or
 * its pack). The "freshen" verb bumps the helper's stored last_used and reports
 * existence; a helper that does not advertise it degrades to "have" (report
 * existence without advancing, the pre-freshen behavior), and one with neither
 * reports nothing. Returns 1 if the object exists (was freshened), 0 otherwise.
 */
static int helper_freshen_object(struct odb_source *source,
				 const struct object_id *oid)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	const char *verb;
	int exists;

	helper_process_ensure(hp);

	verb = hp->cap_freshen ? "freshen" : (hp->cap_have ? "have" : NULL);
	if (!verb)
		return 0;

	oid_to_hex_r(hex, oid);
	helper_process_send(hp, "%s %s\n", verb, hex);

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
			       enum odb_write_object_flags flags)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	int replace;

	/* Ensure the helper process is started and capabilities negotiated */
	helper_process_ensure(hp);

	if (!hp->cap_put)
		return -1;

	/*
	 * Repack re-representation: a repack-mode pack-ingest session resolves an
	 * object that was stored as a delta back to a full object (pack-objects
	 * re-based it off a now-unreachable base). The trailing replace argument on
	 * "put" overwrites the stored representation in place; a normal write keeps
	 * the existing copy. Graceful absence: a helper that does not advertise the
	 * "replace" capability gets replace=0 and simply will not re-represent, like
	 * any optional capability.
	 */
	replace = (flags & ODB_WRITE_OBJECT_REPLACE) && hp->cap_replace;

	/* Compute OID locally so the helper can store it directly */
	hash_object_file(src->base.odb->repo->hash_algo, buf, len, type, oid);
	oid_to_hex_r(hex, oid);

	helper_process_send(hp, "put %s %s %lu %d\n",
			    hex, type_name(type), len, replace);

	helper_process_write(hp, buf, len);

	if (helper_process_readline(hp, &line) == EOF ||
	    strcmp(line.buf, hex)) {
		strbuf_release(&line);
		return -1;
	}
	strbuf_release(&line);

	/*
	 * The helper is a dumb store: it persists the object by its primary id
	 * and knows nothing about the compat algorithm. When the repository
	 * tracks one (extensions.compatObjectFormat), git (not the helper)
	 * records the precomputed storage<->compat mapping in the object
	 * database's git-side map - kept in a loose-object-idx at the object
	 * directory - so a compat object id still resolves. There is no loose
	 * source backing a helper primary, hence the NULL.
	 */
	if (compat_oid)
		return repo_add_loose_object_map(source->odb, NULL, oid, compat_oid);
	return 0;
}

/*
 * Store git's prepared compressed object bytes verbatim via the put-raw verb --
 * the recompress-free receive/migrate path, the per-object analog of the files
 * backend keeping a received pack's compressed entries. git already compressed
 * the bytes (index-pack's incoming pack entry, pack-objects' on repack, or a
 * quarantine pack read on push migrate), so the helper neither compresses nor
 * resolves. base_oid NULL stores a whole object (compressed content); a base oid
 * stores a compressed git-format delta against it. usize is the uncompressed
 * length of the stored bytes (the pack entry-header size). compat_oid (when the
 * repo tracks a compat algorithm) is recorded in the git-side loose-object map,
 * since the helper stores opaque bytes and cannot hash them, exactly as
 * helper_write_object does for a resolved write. Returns -1 (caller falls back to
 * a resolved write) if the helper does not advertise put-raw.
 */
static int helper_write_prepared(struct odb_source *source,
				 const struct object_id *oid,
				 const struct object_id *compat_oid,
				 enum object_type type, unsigned long usize,
				 const struct object_id *base_oid,
				 const void *compressed, unsigned long clen,
				 enum odb_write_object_flags flags)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1], base_hex[GIT_MAX_HEXSZ + 1];
	int replace;

	helper_process_ensure(hp);

	if (!hp->cap_put_raw)
		return -1;

	/* Repack overwrite (see helper_write_object); graceful absence -> keep-existing. */
	replace = (flags & ODB_WRITE_OBJECT_REPLACE) && hp->cap_replace;

	oid_to_hex_r(hex, oid);
	if (base_oid)
		oid_to_hex_r(base_hex, base_oid);

	/* "put-raw <oid> <type> <usize> <base|0> <clen> [<replace>]" + clen bytes. */
	helper_process_send(hp, "put-raw %s %s %lu %s %lu %d\n",
			    hex, type_name(type), usize,
			    base_oid ? base_hex : "0", clen, replace);
	helper_process_write(hp, compressed, clen);

	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, hex)) {
		strbuf_release(&line);
		return -1;
	}
	strbuf_release(&line);

	/* Record the storage<->compat map git-side (see helper_write_object). */
	if (compat_oid)
		return repo_add_loose_object_map(source->odb, NULL, oid, compat_oid);
	return 0;
}

/*
 * Read a stored git-format delta via the "get-delta" verb: the reply is
 * "delta <base-oid> <raw-len> <clen>" followed by the clen COMPRESSED delta
 * bytes, or "plain" when the object is not stored as a delta. The read companion to
 * helper_write_prepared; it lets the send path reuse the stored compressed delta
 * verbatim instead of recomputing it. A helper without the get-delta capability
 * falls back to the generic default ("not a stored delta"), so the caller resolves
 * and recomputes as usual.
 */
static int helper_read_object_delta(struct odb_source *source,
				    const struct object_id *oid,
				    struct object_id *base_oid,
				    void **delta, unsigned long *delta_len,
				    unsigned long *raw_len)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1], base_hex[GIT_MAX_HEXSZ + 1];
	unsigned long clen, rlen;
	unsigned char *buf;
	int ret = 0;

	helper_process_ensure(hp);

	if (!hp->cap_get_delta)
		return odb_source_read_object_delta_generic(source, oid, base_oid,
							    delta, delta_len,
							    raw_len);

	oid_to_hex_r(hex, oid);
	helper_process_send(hp, "get-delta %s\n", hex);

	if (helper_process_readline(hp, &line) == EOF)
		goto out;
	/*
	 * "plain" (or any non-delta-header reply): not stored as a delta. The
	 * header is "delta <base> <raw-len> <clen>": the uncompressed delta length
	 * (for the reuse size-match) and the compressed length (the bytes that
	 * follow). The payload is the COMPRESSED git-format delta, reused verbatim.
	 */
	if (sscanf(line.buf, "delta %64s %lu %lu", base_hex, &rlen, &clen) != 3 ||
	    get_oid_hex_algop(base_hex, base_oid, source->odb->repo->hash_algo))
		goto out;

	/*
	 * Read the compressed delta payload that follows the header. Use fread on
	 * the FILE* (not fileno) because helper_process_readline may have buffered
	 * past the newline, exactly as the content read in helper_read_object_info.
	 */
	buf = xmallocz(clen);
	if (fread(buf, 1, clen, hp->out) != clen) {
		free(buf);
		ret = error(_("helper returned short delta for %s"), hex);
		goto out;
	}
	*delta = buf;
	*delta_len = clen;
	*raw_len = rlen;
	ret = 1;
out:
	strbuf_release(&line);
	return ret;
}

/*
 * Stream the reusable pack_pos prefix [0, end_pos) via the "reuse-pack" verb for
 * source-backed pack reuse (the bulk analog of helper_read_object_delta). The
 * helper replies with one record per object, "<type> <size> <base-oid|-> <clen>"
 * followed by clen verbatim COMPRESSED bytes, in pack_pos order, ended by a blank
 * line; emit() is invoked once per object so the caller frames it into the output
 * pack with no inflate or recompress. A helper without the capability returns -1,
 * and the caller falls back to emitting the objects per-object.
 */
static int helper_stream_reuse(struct odb_source *source, uint32_t end_pos,
			       odb_reuse_emit_fn emit, void *data)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	helper_process_ensure(hp);
	if (!hp->cap_reuse_pack)
		return -1;

	helper_process_send(hp, "reuse-pack %"PRIu32"\n", end_pos);

	while (1) {
		char typebuf[32], basebuf[GIT_MAX_HEXSZ + 1];
		unsigned long size, clen;
		enum object_type type;
		struct object_id base, *basep = NULL;
		unsigned char *buf;

		if (helper_process_readline(hp, &line) == EOF) { ret = -1; break; }
		if (!line.len)
			break;	/* blank line terminates the stream */

		if (sscanf(line.buf, "%31s %lu %64s %lu",
			   typebuf, &size, basebuf, &clen) != 4) {
			ret = error(_("helper sent a malformed reuse record"));
			break;
		}
		type = type_from_string(typebuf);
		if (strcmp(basebuf, "-")) {
			if (get_oid_hex_algop(basebuf, &base,
					      source->odb->repo->hash_algo)) {
				ret = error(_("helper sent a bad reuse base oid"));
				break;
			}
			basep = &base;
		}
		/*
		 * Read the verbatim compressed payload following the header on the
		 * FILE* (helper_process_readline may have buffered past the newline),
		 * exactly as helper_read_object_delta reads its delta bytes.
		 */
		buf = xmallocz(clen);
		if (fread(buf, 1, clen, hp->out) != clen) {
			free(buf);
			ret = error(_("helper returned short reuse content"));
			break;
		}
		if (emit(data, type, size, basep, buf, clen)) {
			free(buf);
			ret = -1;
			break;
		}
		free(buf);
	}

	strbuf_release(&line);
	return ret;
}

/*
 * Grant the reuse window: a helper bulk-reuses the whole candidate prefix when it
 * advertises reuse-pack, and declines (0) otherwise. cap_bitmap and cap_reuse_pack
 * are independent advertisements, so a helper that served the bitmap may still
 * decline here, keeping its objects on the per-object path rather than dying at
 * write time in stream_reuse.
 */
static uint32_t helper_reuse_window(struct odb_source *source, uint32_t candidate)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;

	helper_process_ensure(hp);
	return hp->cap_reuse_pack ? candidate : 0;
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
		while (total < len) {
			ssize_t n = in_stream->read(in_stream,
						    (unsigned char *)buf + total,
						    len - total);
			if (n < 0) {
				free(buf);
				return -1;
			}
			if (!n)
				break;
			total += n;
		}
		ret = helper_write_object(source, buf, total, OBJ_BLOB,
					  oid, NULL, 0);
		free(buf);
		return ret;
	}

	/* Streaming path: hash incrementally while sending to helper */
	{
		struct git_hash_ctx ctx;
		struct git_hash_ctx compat_ctx;
		const struct git_hash_algo *compat = source->odb->repo->compat_hash_algo;
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
		/*
		 * Dual-hash with the compat algorithm so the git-side compat
		 * (sha1<->sha256) map is recorded for streamed blobs too;
		 * odb_write_object_stream does not compute it, while the buffered
		 * helper_write_object does. Only blobs are streamed and their bytes
		 * are identical across hash algorithms, so the same header+content
		 * yields the compat id.
		 */
		if (compat) {
			git_hash_init(&compat_ctx, compat);
			git_hash_update(&compat_ctx, hdr, hdrlen);
		}

		while (1) {
			unsigned char chunk[8192];
			ssize_t n = in_stream->read(in_stream, chunk, sizeof(chunk));
			if (n < 0) {
				/*
				 * The source errored mid-stream after we told the
				 * helper to expect `len` bytes via put-stream. We can no
				 * longer satisfy that, and the helper would block reading
				 * the remainder, desyncing the connection; tear it down so
				 * the next use re-spawns a clean one.
				 */
				helper_process_disconnect(hp);
				return -1;
			}
			if (!n)
				break;
			helper_process_write(hp, chunk, n);
			git_hash_update(&ctx, chunk, n);
			if (compat)
				git_hash_update(&compat_ctx, chunk, n);
		}

		git_hash_final_oid(oid, &ctx);
		oid_to_hex_r(hex, oid);

		/*
		 * Send git's oid as a trailer. The helper does not hash (it is
		 * hash-agnostic, taking the algo from this hex's length); it keys
		 * the streamed bytes on the oid git owns, then echoes it as the ack.
		 */
		helper_process_send(hp, "%s\n", hex);
		if (helper_process_readline(hp, &line) == EOF ||
		    strcmp(line.buf, hex)) {
			strbuf_release(&line);
			return -1;
		}
		strbuf_release(&line);

		if (compat) {
			struct object_id compat_oid;
			git_hash_final_oid(&compat_oid, &compat_ctx);
			return repo_add_loose_object_map(source->odb, NULL, oid,
							 &compat_oid);
		}
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
	/* The transaction object itself is freed by odb_transaction_commit(). */
}

static int helper_transaction_write_object_stream(struct odb_transaction *transaction,
						  struct odb_write_stream *stream,
						  size_t len, struct object_id *oid)
{
	return helper_write_object_stream(transaction->source, stream, len, oid);
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
	txn->write_object_stream = helper_transaction_write_object_stream;
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
 * Record the given objects as promisor objects (the mark_objects_promisor
 * vtable method). The objects were already written to the helper; this only
 * adds the promisor annotation so that a later "list-objects --promisor-only"
 * (which is_promisor_object() consumes) enumerates them. It is called both for
 * a received promisor pack's own objects and for the local objects that pack
 * references. The OIDs are sent as a "promisor" command followed by one per
 * line and a blank-line terminator, matching the framing of the list-style
 * protocol commands.
 */
static int helper_mark_objects_promisor(struct odb_source *source,
					struct oidset *oids)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	struct oidset_iter iter;
	struct object_id *oid;
	char hex[GIT_MAX_HEXSZ + 1];
	int ret = 0;

	helper_process_ensure(hp);

	/*
	 * A helper that does not track promisor status keeps the objects but
	 * not the annotation; there is nothing to send.
	 */
	if (!hp->cap_promisor)
		return 0;

	helper_process_send(hp, "promisor\n");
	oidset_iter_init(oids, &iter);
	while ((oid = oidset_iter_next(&iter))) {
		oid_to_hex_r(hex, oid);
		helper_process_send(hp, "%s\n", hex);
	}
	helper_process_send(hp, "\n");

	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok"))
		ret = error(_("helper failed to mark promisor objects"));

	strbuf_release(&line);
	/*
	 * The promisor set just changed; drop the cached membership so a later
	 * is_promisor_object() re-reads it. The files backend re-scans its
	 * promisor packs on every call and so never goes stale; invalidating
	 * here keeps the cached helper query equivalent.
	 */
	src->promisor_loaded = 0;
	oidset_clear(&src->promisor_objects);
	return ret;
}

/*
 * Re-deltify the helper's objects, the files-faithful way. files' repack runs
 * pack-objects (git's sole delta-search site) to build a new, delta-compressed
 * pack, then installs it and deletes the now-redundant loose objects. The helper
 * has no packfiles, so it does the relational equivalent: pack-objects packs the
 * source's own objects (reading them back through the odb), and index-pack
 * --re-deltify drives the resulting deltas into the helper through the repack-
 * mode pack-ingest session (put / put-raw with their replace argument set),
 * overwriting each object's stored representation in place. The pack streams
 * between the two
 * subprocesses over a pipe, so nothing is written to a temporary file, and both
 * subprocesses reach this same helper (same repo config) over their own
 * connections; WAL lets pack-objects read while index-pack writes.
 */
/*
 * Re-deltify the helper's objects and capture a reachability bitmap in a single
 * pack-objects pass. pack-objects writes a temp pack WITH a bitmap (hash-cache
 * and lookup-table off, the source backing loads entries eagerly and carries
 * no pack-order name hashes) and prints its hash; index-pack ingests the
 * re-deltified objects from that temp pack; then we lift the .bitmap blob and
 * the two orderings the pack already computed, its .idx (oids in index order)
 * and .rev (bit -> index), off it and hand all three to the helper through
 * "store-bitmap", so nothing is re-sorted when the bitmap is later opened. The
 * temp pack is scratch (like a received pack during fetch ingestion) and is
 * removed at the end. If no bitmap was produced (empty repo, or no commits to
 * bitmap), any stale bitmap is cleared instead.
 */
/*
 * for_each_bitmap_commit_entry() callback (data = the helper_process): ship one
 * commit's stored bitmap to the helper as a relational row, base-by-oid, the
 * bitmap analog of a stored object delta (put-delta). xor_base is the chain-base
 * commit (NULL = a full entry).
 */
static int helper_store_commit_bitmap(const struct object_id *commit_oid,
				      const struct object_id *xor_base,
				      int flags, const void *ewah,
				      size_t ewah_len, void *data)
{
	struct helper_process *hp = data;
	char base_hex[GIT_MAX_HEXSZ + 1];
	const char *base_str = "-";

	if (xor_base) {
		oid_to_hex_r(base_hex, xor_base);
		base_str = base_hex;
	}
	/*
	 * One streamed record under the open store-commit-bitmaps verb: a header
	 * line then the verbatim EWAH bytes, no per-row ack. The single ack is read
	 * after the blank terminator, so the whole set is one round-trip and one
	 * helper transaction.
	 */
	helper_process_send(hp, "%s %s %u %"PRIuMAX"\n",
			    oid_to_hex(commit_oid), base_str, (unsigned)flags,
			    (uintmax_t)ewah_len);
	helper_process_write(hp, ewah, ewah_len);
	return 0;
}

static int helper_repack(struct odb_source_helper *src)
{
	struct repository *repo = src->base.odb->repo;
	struct helper_process *hp = src->hp;
	struct child_process po = CHILD_PROCESS_INIT;
	struct child_process ip = CHILD_PROCESS_INIT;
	struct strbuf dir = STRBUF_INIT, base = STRBUF_INIT, hash = STRBUF_INIT;
	struct strbuf path = STRBUF_INIT, blob = STRBUF_INIT, reply = STRBUF_INIT;
	struct packed_git *p = NULL;
	int packfd = -1, ret = 0;

	strbuf_addf(&dir, "%s/bmtmp-XXXXXX", repo_get_object_directory(repo));
	if (!mkdtemp(dir.buf)) {
		strbuf_release(&dir);
		return error_errno(_("repack: cannot create helper bitmap tempdir"));
	}
	strbuf_addf(&base, "%s/pack", dir.buf);

	po.git_cmd = 1;
	strvec_pushl(&po.args, "-c", "pack.writeBitmapHashCache=false",
		     "-c", "pack.writeBitmapLookupTable=false",
		     "pack-objects", "--all", "--write-bitmap-index",
		     "--delta-base-offset", "-q", base.buf, NULL);
	po.no_stdin = 1;
	po.out = -1;
	if (start_command(&po)) {
		ret = error(_("repack: could not start pack-objects for the helper"));
		goto out;
	}
	if (strbuf_read(&hash, po.out, GIT_MAX_HEXSZ + 1) < 0)
		ret = error_errno(_("repack: cannot read pack-objects output"));
	close(po.out);
	if (finish_command(&po))
		ret = error(_("repack: pack-objects failed for the helper"));
	if (ret)
		goto out;
	strbuf_trim(&hash);

	/* No objects packed (empty repo): clear any stale bitmap and stop. */
	if (!hash.len) {
		helper_process_send(hp, "clear-bitmap\n");
		if (helper_process_readline(hp, &reply) == EOF || strcmp(reply.buf, "ok"))
			ret = error(_("repack: helper failed to clear bitmap"));
		goto out;
	}

	/* Ingest the re-deltified objects from the temp pack (the --stdin path). */
	strbuf_addf(&path, "%s-%s.pack", base.buf, hash.buf);
	packfd = open(path.buf, O_RDONLY);
	if (packfd < 0) {
		ret = error_errno(_("repack: cannot open helper temp pack"));
		goto out;
	}
	ip.git_cmd = 1;
	strvec_pushl(&ip.args, "index-pack", "--stdin", "--re-deltify", NULL);
	ip.in = packfd;		/* start_command takes ownership of the fd */
	packfd = -1;
	ip.no_stdout = 1;
	if (start_command(&ip)) {
		ret = error(_("repack: could not start index-pack for the helper"));
		goto out;
	}
	if (finish_command(&ip)) {
		ret = error(_("repack: index-pack failed for the helper"));
		goto out;
	}

	/* Capture the bitmap: read the .bitmap blob, or clear if none was made. */
	strbuf_reset(&path);
	strbuf_addf(&path, "%s-%s.bitmap", base.buf, hash.buf);
	if (strbuf_read_file(&blob, path.buf, 0) < 0) {
		helper_process_send(hp, "clear-bitmap\n");
		if (helper_process_readline(hp, &reply) == EOF || strcmp(reply.buf, "ok"))
			ret = error(_("repack: helper failed to clear bitmap"));
		goto out;
	}

	/* The object order in pack/bit order, via the temp pack's revindex. */
	strbuf_reset(&path);
	strbuf_addf(&path, "%s-%s.idx", base.buf, hash.buf);
	p = add_packed_git(repo, path.buf, path.len, 1);
	if (!p || open_pack_index(p) || load_pack_revindex(repo, p)) {
		ret = error(_("repack: cannot read helper temp pack index"));
		goto out;
	}

	/*
	 * Store git's 4 type bitmaps (commits/trees/blobs/tags) verbatim, and only
	 * those: the commit entries are the per-commit rows, and the .bitmap framing
	 * (header + trailing checksum) is git's own concern, synthesized at open in
	 * helper_open_bitmap(). So the helper holds git's bitmap data, never the
	 * format. read_bitmap() (git's own reader) locates the end of the type
	 * bitmaps, we never hand-parse the EWAH.
	 */
	{
		size_t hsz = sizeof(struct bitmap_disk_header) - GIT_MAX_RAWSZ +
			     repo->hash_algo->rawsz;
		size_t tpos = hsz;
		uint32_t i;
		int ok = (blob.len >= hsz);

		for (i = 0; ok && i < 4; i++) {
			struct ewah_bitmap *e = read_bitmap((const unsigned char *)blob.buf,
							    blob.len, &tpos);
			if (e) ewah_pool_free(e); else ok = 0;
		}
		if (!ok) {
			ret = error(_("repack: cannot parse helper bitmap type bitmaps"));
			goto out;
		}
		helper_process_send(hp, "store-bitmap %"PRIuMAX"\n",
				    (uintmax_t)(tpos - hsz));
		helper_process_write(hp, blob.buf + hsz, tpos - hsz);
	}
	{
		uint32_t i, n = p->num_objects;
		struct object_id oid;

		/* The .idx: oids in index order (nth_packed_object_id order). */
		for (i = 0; i < n; i++) {
			nth_packed_object_id(&oid, p, i);
			helper_process_send(hp, "%s\n", oid_to_hex(&oid));
		}
		helper_process_send(hp, "\n");
		/* The .rev: each bit (pack position) -> its index position. */
		for (i = 0; i < n; i++)
			helper_process_send(hp, "%"PRIu32"\n",
					    pack_pos_to_index(p, i));
		helper_process_send(hp, "\n");
	}
	if (helper_process_readline(hp, &reply) == EOF || strcmp(reply.buf, "ok"))
		ret = error(_("repack: helper failed to store bitmap"));

	/*
	 * Decompose the same bitmap into per-commit rows the helper stores
	 * relationally (commit_oid, xor_base, flags, ewah), the bitmap analog
	 * of object deltas, base-by-oid, reusing git's own bitmap reader
	 * (for_each_bitmap_commit_entry). The whole-blob store above still serves
	 * until the selective-open seam (the per-commit serve path) lands.
	 */
	if (!ret && hp->cap_commit_bitmap) {
		/*
		 * Stream every per-commit row under one store-commit-bitmaps verb; the
		 * helper clears and stores the set in a single transaction (atomic, one
		 * write), then acks once after the blank terminator. The callback drives
		 * git's own bitmap reader and writes each record without a round-trip.
		 */
		helper_process_send(hp, "store-commit-bitmaps\n");
		for_each_bitmap_commit_entry(repo, p, helper_store_commit_bitmap, hp);
		helper_process_send(hp, "\n");	/* blank line terminates the stream */
		if (helper_process_readline(hp, &reply) == EOF || strcmp(reply.buf, "ok"))
			ret = error(_("repack: helper failed to store commit bitmaps"));
	}

out:
	if (packfd >= 0)
		close(packfd);
	if (p) {
		close_pack(p);
		free(p);
	}
	if (dir.len)
		remove_dir_recursively(&dir, 0);
	strbuf_release(&dir);
	strbuf_release(&base);
	strbuf_release(&hash);
	strbuf_release(&path);
	strbuf_release(&blob);
	strbuf_release(&reply);
	return ret;
}

/*
 * Optimize the helper's object storage (the odb_source optimize vtable), the
 * helper analog of files' "git repack". The odb-level odb_optimize() reaches
 * here for "git gc"/"git repack" on a helper-backed repository. Like the remote-
 * helper capabilities, optimization is optional: a helper that does not advertise
 * it has nothing to optimize, so this is a no-op. A helper that supports it does
 * two things, mirroring files' repack-then-prune: first re-deltify the stored
 * objects (helper_repack), then compact its backing store to reclaim the space
 * the re-representation frees. A helper that cannot re-represent (no "replace"
 * capability) skips straight to the compaction, exactly as it did before deltifying
 * repack existed (graceful absence).
 */
static int helper_optimize(struct odb_source *source,
			   struct odb_optimize_opts *opts UNUSED)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	helper_process_ensure(hp);

	if (!hp->cap_optimize)
		return 0;

	if (hp->cap_replace) {
		ret = helper_repack(src);
		if (ret)
			return ret;
	}

	helper_process_send(hp, "optimize\n");

	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok"))
		ret = error(_("helper failed to optimize the object database"));

	strbuf_release(&line);
	return ret;
}

/*
 * Tell git whether the helper's store would benefit from "git gc", the gc-auto
 * gate (the odb_source optimize_required vtable; this overrides the no-op default
 * so gc --auto can fire for a helper at all). files counts loose objects / packs
 * against gc.auto / gc.autoPackLimit; the helper has no loose or pack tiers, so it
 * asks the store how many objects were added since the last optimize (the
 * relational analog of loose objects accumulating) and compares that to gc.auto.
 * The geometric-repack and multi-pack-index maintenance modes are files-pack
 * concepts the helper has no analog for, so it reports "not required" for them.
 * Graceful absence: a helper without the optimize-required capability cannot
 * self-assess, so gc --auto stays off for it (the prior, no-op-default behavior).
 */
static int helper_optimize_required(struct odb_source *source,
				    struct odb_optimize_opts *opts,
				    bool *required)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	int gc_auto = 6700;

	*required = false;

	/* No pack geometry or multi-pack-index maintenance for a helper. */
	if (opts->flags & (ODB_OPTIMIZE_GEOMETRIC | ODB_OPTIMIZE_MIDX))
		return 0;

	helper_process_ensure(hp);
	if (!hp->cap_optimize_required)
		return 0;

	repo_config_get_int(source->odb->repo, "gc.auto", &gc_auto);
	/* gc.auto <= 0 disables automatic maintenance entirely (files semantics). */
	if (gc_auto <= 0)
		return 0;

	helper_process_send(hp, "optimize-required\n");
	if (helper_process_readline(hp, &line) != EOF) {
		char *end;
		long churn = strtol(line.buf, &end, 10);
		if (end != line.buf && churn >= gc_auto)
			*required = true;
	}
	strbuf_release(&line);
	return 0;
}

/*
 * Push migrate (the migrate_quarantine vtable): copy a push quarantine's
 * accepted objects into the helper. A push always stages in a files quarantine
 * (a tmp-objdir) so the connectivity check and pre-receive hook, separate
 * processes that cannot see an uncommitted helper transaction, can read the
 * objects before acceptance; on accept they move here. files renames them into
 * its object directory; the helper, having none, copies them in, preserving each
 * object's representation: a packed delta stays a delta, its base and COMPRESSED
 * bytes, read straight from the quarantine pack via read_object_delta and stored
 * verbatim through the prepared receive (put-raw), no resolve and no recompress --
 * while a whole object is read resolved and re-stored. The relational analog of
 * files renaming the pack in: move the storage units, deltas intact. index-pack
 * already resolved the quarantine at receive, so nothing resolves twice.
 */
struct helper_quarantine_migrate {
	struct odb_source *quarantine;
	struct odb_pack_ingest *ingest;
	struct object_database *odb;
	/*
	 * The destination helper advertises put-raw, so it stores git's native form
	 * and a quarantine delta can be copied in verbatim (compressed). A helper
	 * that stores objects whole (no put-raw, e.g. testgit) gets each object
	 * resolved instead.
	 */
	int keeps_deltas;
};

static int helper_migrate_one_object(const struct object_id *oid,
				     struct object_info *oi UNUSED,
				     void *cb_data)
{
	struct helper_quarantine_migrate *m = cb_data;
	struct object_id base;
	void *delta;
	unsigned long clen, raw_len;
	enum object_type type;
	unsigned long size;
	int ret;

	ret = odb_source_read_object_delta(m->quarantine, oid, &base, &delta,
					   &clen, &raw_len);
	if (ret < 0)
		return -1;
	if (ret > 0 && m->keeps_deltas) {
		/*
		 * A delta in the quarantine pack, into a helper that keeps git's native
		 * form: copy it verbatim (still compressed) through the prepared receive.
		 * usize is the uncompressed delta length; type is the resolved type. The
		 * resolved object is read only to record a compat-hash id (a compat repo);
		 * otherwise just the type is needed, no resolve of the delta chain.
		 */
		void *resolved = NULL;
		unsigned long resolved_size = 0;

		if (m->odb->repo->compat_hash_algo) {
			resolved = odb_read_object(m->odb, oid, &type, &resolved_size);
			if (!resolved) {
				free(delta);
				return error(_("cannot read quarantined %s"),
					     oid_to_hex(oid));
			}
		} else {
			struct object_info info = OBJECT_INFO_INIT;

			info.typep = &type;
			if (odb_read_object_info_extended(m->odb, oid, &info, 0) < 0) {
				free(delta);
				return error(_("cannot read info for quarantined %s"),
					     oid_to_hex(oid));
			}
		}
		ret = odb_pack_ingest_object_prepared(m->ingest, oid, type, raw_len,
						      &base, delta, clen,
						      resolved, resolved_size);
		free(delta);
		free(resolved);
		return ret;
	}
	if (ret > 0)
		free(delta);	/* whole-only helper: store the object resolved instead */

	/* Stored whole (loose, a pack base, or a delta resolved for a whole-only
	 * helper): copy the resolved object. */
	{
		void *buf = odb_read_object(m->odb, oid, &type, &size);
		if (!buf)
			return error(_("cannot read quarantined %s"), oid_to_hex(oid));
		ret = odb_pack_ingest_object(m->ingest, oid, type, buf, size);
		free(buf);
	}
	return ret;
}

static int helper_migrate_quarantine(struct odb_source *source,
				     const char *quarantine_path)
{
	struct object_database *odb = source->odb;
	struct odb_source_helper *self =
		container_of(source, struct odb_source_helper, base);
	struct helper_quarantine_migrate m;
	struct odb_for_each_object_options opts = { 0 };
	struct odb_received_pack received = { 0 };
	int ret;

	/*
	 * receive-pack adds the quarantine as an alternate before migrating, so
	 * reuse that already-prepared source; fall back to adding it for any
	 * caller that migrates a quarantine it did not register first.
	 */
	m.quarantine = odb_find_source_by_path(odb, quarantine_path);
	if (!m.quarantine)
		m.quarantine = odb_add_to_alternates_memory(odb, quarantine_path);
	if (!m.quarantine)
		return error(_("cannot open push quarantine %s"), quarantine_path);
	m.odb = odb;
	/*
	 * Spawn + negotiate so cap_put_raw is known before the first object: it
	 * decides whether a quarantine delta is copied in verbatim (compressed) or
	 * resolved (a whole-only helper). See helper_migrate_one_object.
	 */
	helper_process_ensure(self->hp);
	m.keeps_deltas = self->hp->cap_put_raw;
	m.ingest = odb_source_begin_pack_ingest(odb);
	if (!m.ingest)
		return error(_("cannot begin ingest for the push quarantine"));

	/*
	 * Iterate just the quarantine source's objects and hand each to the
	 * session. On error, leave the session uncommitted: its transaction rolls
	 * back, so a failed migrate stores nothing (all-or-nothing on accept, like
	 * files' rename).
	 */
	ret = odb_source_for_each_object(m.quarantine, NULL,
					 helper_migrate_one_object, &m, &opts);
	if (ret)
		return error(_("failed to migrate the push quarantine into the helper"));

	odb_pack_ingest_commit(m.ingest, &received, NULL);
	return 0;
}

/*
 * Record that a just-ingested received pack's objects belong to it, so the
 * helper's prune can spare them while the pack's .keep marker is live (the
 * relational form of files keeping a .keep'd pack's objects). git calls this
 * just before the ingest transaction commits, so the membership lands in the
 * same transaction as the objects; the helper keys it by the pack's trailing
 * hash, matching the pack-<hash>.keep file git's transport lifecycle manages. A
 * helper without the capability keeps the objects but not the membership, so its
 * keep degrades to best-effort (the .keep file still gates transport).
 */
static int helper_keep_pack(struct odb_source *source,
			    const struct odb_received_pack *pack)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;
	struct strbuf line = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];
	uint32_t i;
	int ret = 0;

	helper_process_ensure(hp);
	if (!hp->cap_keep_pack)
		return 0;

	helper_process_send(hp, "keep-pack %s\n", hash_to_hex_algop(pack->pack_hash, algo));
	for (i = 0; i < pack->nr_objects; i++) {
		oid_to_hex_r(hex, &pack->objects[i]->oid);
		helper_process_send(hp, "%s\n", hex);
	}
	helper_process_send(hp, "\n");

	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok"))
		ret = error(_("helper failed to record pack membership"));
	strbuf_release(&line);
	return ret;
}

/*
 * Send the set of packs whose ".keep" marker is currently live, one
 * "keep <pack_hash>" line each then a blank line, so the helper's prune can
 * spare their objects. git owns the keep lifecycle (generic_commit writes a
 * pack-<hash>.keep when a fetch requests a keep, transport_unlock_pack removes
 * it after the refs land), so the live marker files are the authority for which
 * packs are kept; the hash is the marker filename between "pack-" and ".keep".
 */
static void helper_send_kept_packs(struct helper_process *hp,
				   struct repository *repo)
{
	struct strbuf packdir = STRBUF_INIT;
	DIR *dir;
	struct dirent *de;

	strbuf_addf(&packdir, "%s/pack", repo_get_object_directory(repo));
	dir = opendir(packdir.buf);
	if (dir) {
		while ((de = readdir(dir))) {
			const char *hash;
			size_t len;

			if (!skip_prefix(de->d_name, "pack-", &hash))
				continue;
			len = strlen(hash);
			if (len <= 5 || strcmp(hash + len - 5, ".keep"))
				continue;
			helper_process_send(hp, "keep %.*s\n", (int)(len - 5), hash);
		}
		closedir(dir);
	}
	strbuf_release(&packdir);
	helper_process_send(hp, "\n");	/* end of keep section */
}

static int helper_remove_objects(struct odb_source *source,
				 struct oidset *prune, timestamp_t expire,
				 int dry_run, struct oidset *removed)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	struct oidset_iter iter;
	const struct object_id *oid;
	int ret = 0;

	helper_process_ensure(hp);

	/*
	 * Like the remote-helper capabilities, pruning is optional: a helper that
	 * does not advertise it keeps its unreachable objects (a safe default), so
	 * this removes (and reports) nothing. git has already filtered `prune` to
	 * the objects it found unreachable; hand the helper that set plus the
	 * resolved expire (an absolute time) and the dry-run flag. git owns
	 * reachability; the helper applies its own stored timestamp, deletes the
	 * members stored at or before expire (unless dry_run), and reports back the
	 * oids it pruned (or would prune) so we can record exactly those in
	 * `removed` for the caller's report instead of the whole candidate set.
	 */
	if (!hp->cap_prune)
		return 0;

	/*
	 * Honor in-flight fetch keeps. The prune exchange has two blank-terminated
	 * sections: first the packs whose .keep marker is still live, then the
	 * candidate oids. git owns the keep lifecycle (generic_commit writes the
	 * marker, transport_unlock_pack removes it once the fetch's refs land), so
	 * the live marker files name exactly the kept packs; the helper spares any
	 * object in one of them via its recorded pack membership, the relational
	 * form of files keeping a kept pack's objects, even under --prune=now.
	 */
	strbuf_addf(&line, "prune %d %"PRItime"\n", dry_run ? 1 : 0, expire);
	helper_process_send(hp, line.buf);
	helper_send_kept_packs(hp, source->odb->repo);

	oidset_iter_init(prune, &iter);
	while ((oid = oidset_iter_next(&iter))) {
		strbuf_reset(&line);
		strbuf_addf(&line, "%s\n", oid_to_hex(oid));
		helper_process_send(hp, line.buf);
	}
	helper_process_send(hp, "\n");

	/*
	 * The helper replies with the oids it pruned, one per line, terminated by
	 * "ok". Record them in `removed` when the caller wants a report; read to
	 * the terminator either way so the protocol stream stays in sync.
	 */
	for (;;) {
		strbuf_reset(&line);
		if (helper_process_readline(hp, &line) == EOF) {
			ret = error(_("helper failed to prune unreachable objects"));
			break;
		}
		if (!strcmp(line.buf, "ok"))
			break;
		if (removed) {
			struct object_id pruned;

			if (get_oid_hex_any(line.buf, &pruned) != GIT_HASH_UNKNOWN)
				oidset_insert(removed, &pruned);
		}
	}

	strbuf_release(&line);
	return ret;
}

struct helper_verify_ctx {
	struct odb_source *source;
	odb_verify_cb cb;
	void *cb_data;
	int ret;
};

/*
 * Verify one object the helper enumerated: read its bytes back from this very
 * source (not odb-wide, so an alternate cannot mask helper corruption), confirm
 * they hash to the OID we asked for (the helper's analogue of the files
 * backend's hash-path check), then hand them to `cb` for content fsck. Reading
 * uses the non-dying read_object_info path so one bad object does not abort the
 * whole check.
 */
static int helper_verify_object(const struct object_id *oid,
				struct object_info *oi UNUSED, void *cb_data)
{
	struct helper_verify_ctx *ctx = cb_data;
	struct repository *repo = ctx->source->odb->repo;
	struct object_info read = OBJECT_INFO_INIT;
	enum object_type type = OBJ_NONE;
	unsigned long size = 0;
	void *buf = NULL;
	struct object_id computed;
	int eaten = 0;

	read.typep = &type;
	read.sizep = &size;
	read.contentp = &buf;
	if (odb_source_read_object_info(ctx->source, oid, &read, 0) < 0) {
		error(_("%s: unable to read object from helper"), oid_to_hex(oid));
		ctx->ret = -1;
		return 0; /* keep checking other objects */
	}

	hash_object_file(repo->hash_algo, buf, size, type, &computed);
	if (!oideq(&computed, oid)) {
		error(_("%s: hash mismatch, helper served %s"),
		      oid_to_hex(oid), oid_to_hex(&computed));
		ctx->ret = -1;
		free(buf);
		return 0;
	}

	if (ctx->cb && ctx->cb(oid, type, size, buf, &eaten, ctx->cb_data))
		ctx->ret = -1;

	if (!eaten)
		free(buf);
	return 0; /* keep checking other objects */
}

/*
 * Verify the helper's stored objects (the storage side of "git fsck"). A helper
 * has no loose files or packs to scan, so it enumerates its objects via
 * "list-objects" and we content-check each through `cb` (which also marks them
 * HAS_OBJ, so builtin/fsck's backend-agnostic connectivity walk then works). If
 * the helper advertises the "verify" capability we additionally ask it to check
 * the integrity of its own backing store (e.g. a database integrity check). `o`
 * is unused: storage-format problems are reported with error() as elsewhere.
 */
static int helper_read_compat_map(struct odb_source *source)
{
	/*
	 * A helper's objects live in its backing store, but git keeps their
	 * SHA-1<->SHA-256 map in a loose-object-idx at the helper's object
	 * directory, with no loose source to cache it; load it at the odb level.
	 */
	return odb_read_object_dir_compat_map(source->odb);
}

static int helper_verify(struct odb_source *source, struct fsck_options *o UNUSED,
			 odb_verify_cb cb, void *cb_data)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct helper_verify_ctx ctx = {
		.source = source,
		.cb = cb,
		.cb_data = cb_data,
		.ret = 0,
	};

	helper_process_ensure(hp);

	/*
	 * Content-check every object the helper can enumerate. Skipped for a
	 * format/storage-only verify (no cb, e.g. fsck --connectivity-only): the
	 * helper has no rev-index/bitmap/midx, so only its own store check below
	 * applies.
	 */
	if (cb)
		helper_for_each_object(source, NULL, helper_verify_object, &ctx, NULL);

	/* Ask the helper to check its own backing store, if it can. */
	if (hp->cap_verify) {
		struct strbuf line = STRBUF_INIT;

		helper_process_send(hp, "verify\n");
		if (helper_process_readline(hp, &line) == EOF ||
		    strcmp(line.buf, "ok")) {
			error(_("helper reported its object store is corrupt: %s"),
			      line.len ? line.buf : "verify failed");
			ctx.ret = -1;
		}
		strbuf_release(&line);
	}

	return ctx.ret;
}

/*
 * Return the minimal unambiguous abbreviation length (>= min_length) for an
 * object among this source's objects, scanning "list-objects" for the
 * closest-sharing other object. Mirrors source-inmemory's find_abbrev_len.
 */
static int helper_find_abbrev_len(struct odb_source *source,
				  const struct object_id *oid,
				  unsigned min_length,
				  unsigned *out)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	char target_hex[GIT_MAX_HEXSZ + 1];
	unsigned hexsz = source->odb->repo->hash_algo->hexsz;
	unsigned len = min_length;

	helper_process_ensure(hp);

	oid_to_hex_r(target_hex, oid);

	if (hp->cap_list_objects) {
		/*
		 * Scope to objects sharing the target's first min_length nibbles:
		 * only those can force a longer abbreviation, so the helper need not
		 * stream its whole object table for every disambiguation.
		 */
		if (min_length)
			helper_process_send(hp, "list-objects %.*s\n",
					     (int)min_length, target_hex);
		else
			helper_process_send(hp, "list-objects\n");
		while (helper_process_readline(hp, &line) != EOF) {
			char oid_hex[GIT_MAX_HEXSZ + 1];
			char type_str[32];
			unsigned long size;
			unsigned i;

			if (!line.len)
				break;
			if (sscanf(line.buf, "%64s %31s %lu",
				   oid_hex, type_str, &size) != 3)
				continue;
			if (!strcmp(oid_hex, target_hex))
				continue;
			for (i = 0; i < hexsz && oid_hex[i] == target_hex[i]; i++)
				;
			if (i >= len)
				len = i + 1;
		}
		strbuf_release(&line);
	}

	if (len > hexsz)
		len = hexsz;
	*out = len;
	return 0;
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

static int helper_collect_promisor_cb(const struct object_id *oid,
				      struct object_info *oi UNUSED,
				      void *cb_data)
{
	oidset_insert(cb_data, oid);
	return 0;
}

/*
 * Report whether the helper stores `oid` as a promisor object. The protocol
 * exposes this only as a set ("list-objects --promisor-only"), so cache it on
 * first use; without this the default vtable noop reports every helper-stored
 * promisor object as non-promisor, defeating the partial-clone connectivity
 * fast path (connected.c via odb_is_promisor_object). The cache is invalidated
 * on reprepare, after which newly received promisor objects are picked up.
 */
static int helper_is_promisor_object(struct odb_source *source,
				     const struct object_id *oid)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);

	if (!src->promisor_loaded) {
		struct odb_for_each_object_options opts = { 0 };
		opts.flags = ODB_FOR_EACH_OBJECT_PROMISOR_ONLY;
		helper_for_each_object(source, NULL, helper_collect_promisor_cb,
				       &src->promisor_objects, &opts);
		src->promisor_loaded = 1;
	}
	return oidset_contains(&src->promisor_objects, oid);
}

/*
 * Open the helper's stored reachability bitmap as this source's bitmap. The
 * reply to "get-bitmap" is "bitmap <len>" + <len> EWAH bytes + the .idx (oids in
 * index order, one hex per line, blank-terminated) + the .rev (one integer per
 * line, bit -> index position, blank-terminated), or "missing". We read all
 * three and hand them to pack-bitmap's source backing, which interprets the bits
 * over those orders exactly as it would over a pack's .idx/.rev. A helper
 * without the capability (or with no stored bitmap) reports none, so the caller
 * falls back to a full walk. Returns 0 if a bitmap was opened, -1 otherwise.
 */
static int helper_open_bitmap(struct odb_source *source,
			      struct bitmap_index *bitmap_git)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;
	struct strbuf line = STRBUF_INIT;
	unsigned char *map = NULL;
	size_t map_size = 0;
	unsigned long map_len = 0, n_objects = 0;
	int ret = -1;

	helper_process_ensure(hp);
	if (!hp->cap_bitmap)
		return -1;

	helper_process_send(hp, "get-bitmap\n");
	if (helper_process_readline(hp, &line) == EOF)
		goto out;
	if (sscanf(line.buf, "bitmap %lu %lu", &map_len, &n_objects) != 2 || !map_len)
		goto out;	/* "missing" or empty -> no bitmap here */
	map_size = map_len;

	/*
	 * The reply is "bitmap <type_bitmaps_len> <n_objects>" + the type-bitmap
	 * bytes. The bit <-> oid orderings are NOT sent; git faults them in on
	 * demand (pos-of-oid / orderings-range) over objects.pack_pos, O(touched),
	 * the source analogue of mmap'ing a pack's .idx. fread on hp->out (not the
	 * fd) because helper_process_readline may have buffered past the newline.
	 */
	map = xmalloc(map_size);
	if (fread(map, 1, map_size, hp->out) != map_size) {
		ret = error(_("helper returned short bitmap"));
		goto out;
	}

	/*
	 * The helper stores only the type bitmaps; synthesize git's .bitmap framing
	 * around them, header (version 1, FULL_DAG, zero commit entries: they fault
	 * in lazily via get-commit-bitmap) plus the trailing checksum slot, so git's
	 * load_bitmap reads it as a normal bitmap with an empty commit index.
	 */
	{
		struct bitmap_disk_header h;
		size_t hsz = sizeof(h) - GIT_MAX_RAWSZ + algo->rawsz;
		size_t total = hsz + map_size + algo->rawsz;
		unsigned char *full = xmalloc(total);

		memset(&h, 0, sizeof(h));
		memcpy(h.magic, BITMAP_IDX_SIGNATURE, sizeof(h.magic));
		h.version = htons(1);
		h.options = htons(BITMAP_OPT_FULL_DAG);
		h.entry_count = htonl(0);
		memcpy(full, &h, hsz);			/* synthesized header */
		memcpy(full + hsz, map, map_size);	/* the stored type bitmaps */
		memset(full + hsz + map_size, 0, algo->rawsz);	/* trailing checksum slot */
		free(map);
		map = full;
		map_size = total;
	}

	ret = bitmap_git_open_source(bitmap_git, source->odb->repo,
				     map, map_size, (uint32_t)n_objects, source);
	map = NULL;	/* ownership transferred to bitmap_git on success */
out:
	free(map);
	strbuf_release(&line);
	return ret;
}

/*
 * odb_source get_commit_bitmap: fetch one commit's stored bitmap entry from the
 * helper (a commit_bitmap row) for the selective serve path. Reply is
 * "commit-bitmap <xor_base|-> <flags> <len>" + <len> EWAH bytes, or "missing".
 */
static int helper_get_commit_bitmap(struct odb_source *source,
				    const struct object_id *commit_oid,
				    struct object_id *xor_base, int *flags,
				    void **ewah, size_t *ewah_len)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;
	struct strbuf line = STRBUF_INIT;
	char xb[GIT_MAX_HEXSZ + 1];
	unsigned long len = 0;
	int f = 0, ret = -1;
	void *buf;

	helper_process_ensure(hp);
	if (!hp->cap_commit_bitmap)
		return -1;

	helper_process_send(hp, "get-commit-bitmap %s\n", oid_to_hex(commit_oid));
	if (helper_process_readline(hp, &line) == EOF)
		goto out;
	if (sscanf(line.buf, "commit-bitmap %64s %d %lu", xb, &f, &len) != 3)
		goto out;	/* "missing" */

	buf = xmalloc(len ? len : 1);
	if (len && fread(buf, 1, len, hp->out) != len) {
		free(buf);
		goto out;
	}
	if (!strcmp(xb, "-"))
		oidclr(xor_base, algo);		/* full entry: no xor base */
	else if (get_oid_hex_algop(xb, xor_base, algo)) {
		free(buf);
		goto out;
	}
	*flags = f;
	*ewah = buf;
	*ewah_len = len;
	ret = 0;
out:
	strbuf_release(&line);
	return ret;
}

/*
 * odb_source pos_of_oid: the object's bit position in the source bitmap, or -1
 * (the want/have lookup). Reply "pos <bit>" or "none".
 */
static int helper_pos_of_oid(struct odb_source *source,
			     const struct object_id *oid)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	long long bit = -1;
	int ret = -1;

	helper_process_ensure(hp);
	if (!hp->cap_bitmap)
		return -1;
	helper_process_send(hp, "pos-of-oid %s\n", oid_to_hex(oid));
	if (helper_process_readline(hp, &line) != EOF &&
	    sscanf(line.buf, "pos %lld", &bit) == 1)
		ret = (int)bit;
	strbuf_release(&line);
	return ret;
}

/*
 * odb_source resolve_bits: hand the helper a set of bits and emit(oid) for each,
 * in ascending bit (pack_pos) order, the batched bit->oid for a bitmap
 * operation. Sends "result-oids" + the bits (one per line, blank-terminated);
 * the reply is the oids (one hex per line, blank-terminated). The helper drains
 * the whole bit set before replying, so the two directions never overlap.
 */
static int helper_resolve_bits(struct odb_source *source,
			       const uint32_t *bits, size_t n,
			       void (*emit)(const struct object_id *oid, void *data),
			       void *data)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	const struct git_hash_algo *algo = source->odb->repo->hash_algo;
	struct strbuf req = STRBUF_INIT, line = STRBUF_INIT;
	size_t i;
	int ret = 0;

	helper_process_ensure(hp);
	if (!hp->cap_bitmap)
		return -1;
	strbuf_addstr(&req, "result-oids\n");
	for (i = 0; i < n; i++)
		strbuf_addf(&req, "%"PRIu32"\n", bits[i]);
	strbuf_addch(&req, '\n');		/* end of the bit set */
	helper_process_send(hp, "%s", req.buf);
	strbuf_release(&req);
	/*
	 * Drain the entire oid reply into memory BEFORE emitting any of it.
	 * emit() is the bitmap show callback, which can call back into this
	 * same helper (e.g. oid_object_info -> "info <oid>") while we would
	 * otherwise still be mid-reply; on the single shared pipe that
	 * re-entrant command interleaves with the bytes of this reply and
	 * desyncs both conversations. Buffering keeps each request/reply
	 * atomic, the way the in-pack source (random-access .idx) is
	 * re-entrant for free.
	 */
	{
		struct object_id *oids = NULL;
		size_t oids_nr = 0, oids_alloc = 0;

		for (;;) {
			struct object_id oid;
			if (helper_process_readline(hp, &line) == EOF) { ret = -1; break; }
			if (!line.len)
				break;			/* blank terminator */
			if (get_oid_hex_algop(line.buf, &oid, algo)) { ret = -1; break; }
			ALLOC_GROW(oids, oids_nr + 1, oids_alloc);
			oidcpy(&oids[oids_nr++], &oid);
		}
		if (!ret)
			for (i = 0; i < oids_nr; i++)
				emit(&oids[i], data);
		free(oids);
	}
	strbuf_release(&line);
	return ret;
}

/*
 * Store the source's commit-graph generation numbers (the store_commit_graph
 * vtable). Ship "store-commit-graph" + one "<oid> <generation>" line per commit
 * + a blank terminator; the helper persists them and writes no
 * commit-graph file. Generation is commit_graph_data_at()->generation, set by
 * the time git reaches write_commit_graph_file; tree/parents/date are read back
 * from the commit objects on access, so they are not shipped (no duplication of
 * the objects). A helper without the graph capability returns 1 ("not handled")
 * so git writes its commit-graph file as usual, exactly as the vtable's no-op
 * default does; only a graph-capable helper suppresses the file and stores the
 * generations. Returns 0 on success (suppressed), 1 when not handled.
 */
static int helper_store_commit_graph(struct odb_source *source,
				     struct write_commit_graph_context *ctx)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf req = STRBUF_INIT, reply = STRBUF_INIT;
	struct commit **commits;
	size_t i, nr;
	int ret = 0;

	helper_process_ensure(hp);
	if (!hp->cap_graph)
		return write_commit_graph_to_file(ctx);	/* no DB generations: write the file as the files source does */

	commits = commit_graph_ctx_commits(ctx, &nr);
	strbuf_addstr(&req, "store-commit-graph\n");
	for (i = 0; i < nr; i++) {
		struct commit *c = commits[i];
		strbuf_addf(&req, "%s %"PRItime"\n",
			    oid_to_hex(&c->object.oid),
			    commit_graph_generation(c));
	}
	strbuf_addch(&req, '\n');	/* end of the generation list */
	helper_process_send(hp, "%s", req.buf);
	strbuf_release(&req);

	if (helper_process_readline(hp, &reply) == EOF || strcmp(reply.buf, "ok"))
		ret = error(_("helper failed to store commit-graph generations"));
	strbuf_release(&reply);
	return ret;
}

/* Does the helper supply commit generations? (It does once it advertised the
 * "graph" capability; empty store degrades gracefully to no-pruning.) */
static int helper_provides_commit_generations(struct odb_source *source)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	helper_process_ensure(src->hp);
	return src->hp->cap_graph;
}

/*
 * Install the helper as a source, adding receive-time quarantine staging. The
 * helper's object writes go through its own process, not into
 * GIT_OBJECT_DIRECTORY, so a push quarantine (which redirects that env var to a
 * tmp-objdir) would not capture them. When a quarantine is active, put a files
 * quarantine in front as the write primary and keep the helper as a read-through
 * secondary, rooted at the common object directory so pre-existing objects still
 * resolve; the connectivity check and pre-receive hook (separate processes) read
 * the staged files, and migrate_quarantine ingests the accepted objects on
 * accept. Otherwise the helper is the sole primary.
 */
static void helper_prepare_source_list(struct odb_source *source,
				       struct object_database *odb)
{
	if (getenv(GIT_QUARANTINE_ENVIRONMENT)) {
		struct odb_source *quarantine =
			&odb_source_files_new(odb, odb->object_dir, true)->base;
		source->local = false;	/* now a read-through secondary */
		quarantine->next = source;
		odb->sources = quarantine;
		odb->sources_tail = &source->next;
	} else {
		odb->sources = source;
		odb->sources_tail = &source->next;
	}
}

/*
 * Fault one commit's generation from the helper (the lazy commit-graph read):
 * "commit-generation <oid>" -> "<generation>" or "none". O(1) per touched
 * commit, the generation analogue of pos-of-oid. Returns 0 + *gen, or -1.
 */
static int helper_get_commit_generation(struct odb_source *source,
					const struct object_id *oid,
					timestamp_t *gen)
{
	struct odb_source_helper *src =
		container_of(source, struct odb_source_helper, base);
	struct helper_process *hp = src->hp;
	struct strbuf line = STRBUF_INIT;
	int ret = -1;

	helper_process_ensure(hp);
	if (!hp->cap_graph)
		return -1;
	helper_process_send(hp, "commit-generation %s\n", oid_to_hex(oid));
	if (helper_process_readline(hp, &line) != EOF && strcmp(line.buf, "none")) {
		*gen = (timestamp_t)strtoull(line.buf, NULL, 10);
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
	const char *helper_dir;

	CALLOC_ARRAY(src, 1);
	odb_source_init(&src->base, odb, path, local);

	/*
	 * Root the object helper at the common directory, not the object
	 * directory. The ref backend (refs/helper-backend.c) roots its own helper
	 * at the common directory too, so the two independent helper processes
	 * (objects and refs) open the same shared sqlite database under one
	 * deterministic gitdir; otherwise they would disagree and refs would
	 * scatter between <commondir> and <objectdir>. The source path stays the
	 * object directory (used for info/alternates).
	 */
	helper_dir = odb->repo->commondir ? odb->repo->commondir : path;

	/* Share the repo-level object helper process, creating it if needed */
	if (!odb->repo->odb_local_helper) {
		odb->repo->odb_local_helper = xcalloc(1, sizeof(*odb->repo->odb_local_helper));
		helper_process_init(odb->repo->odb_local_helper, helper_name, helper_dir);
	} else {
		if (strcmp(odb->repo->odb_local_helper->name, helper_name))
			die(_("ODB helper name '%s' does not match existing helper '%s'"),
			    helper_name, odb->repo->odb_local_helper->name);
		if (!odb->repo->odb_local_helper->gitdir)
			odb->repo->odb_local_helper->gitdir = xstrdup(helper_dir);
	}
	src->hp = odb->repo->odb_local_helper;
	src->base.free = helper_free;
	src->base.close = helper_close;
	src->base.reprepare = helper_reprepare;
	src->base.read_object_info = helper_read_object_info;
	src->base.read_object_stream = helper_read_object_stream;
	src->base.for_each_object = helper_for_each_object;
	src->base.freshen_object = helper_freshen_object;
	src->base.write_object = helper_write_object;
	src->base.write_prepared = helper_write_prepared;
	src->base.read_object_delta = helper_read_object_delta;
	src->base.stream_reuse = helper_stream_reuse;
	src->base.reuse_window = helper_reuse_window;
	src->base.write_object_stream = helper_write_object_stream;
	src->base.begin_transaction = helper_begin_transaction;
	src->base.read_alternates = helper_read_alternates;
	src->base.write_alternate = helper_write_alternate;
	src->base.mark_objects_promisor = helper_mark_objects_promisor;
	src->base.keep_pack = helper_keep_pack;
	src->base.open_bitmap = helper_open_bitmap;
	src->base.get_commit_bitmap = helper_get_commit_bitmap;
	src->base.pos_of_oid = helper_pos_of_oid;
	src->base.resolve_bits = helper_resolve_bits;
	src->base.store_commit_graph = helper_store_commit_graph;
	src->base.provides_commit_generations = helper_provides_commit_generations;
	src->base.prepare_source_list = helper_prepare_source_list;
	src->base.get_commit_generation = helper_get_commit_generation;
	src->base.is_promisor_object = helper_is_promisor_object;
	src->base.optimize = helper_optimize;
	src->base.optimize_required = helper_optimize_required;
	src->base.migrate_quarantine = helper_migrate_quarantine;
	src->base.verify = helper_verify;
	src->base.remove_objects = helper_remove_objects;
	src->base.read_compat_map = helper_read_compat_map;
	src->base.count_objects = helper_count_objects;
	src->base.find_abbrev_len = helper_find_abbrev_len;
	return src;
}

struct odb_source *odb_source_helper_new_base(struct object_database *odb,
					      const char *name,
					      const char *path,
					      bool local)
{
	if (!name || !*name)
		die(_("helper object backend requires a name"));
	return &odb_source_helper_new(odb, name, path, local)->base;
}
