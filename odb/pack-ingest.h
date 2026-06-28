#ifndef ODB_PACK_INGEST_H
#define ODB_PACK_INGEST_H

#include "object.h"
#include "strbuf.h"

struct odb_source;
struct object_database;
struct object_id;
struct odb_write_stream;
struct pack_idx_entry;
struct pack_idx_option;
struct packed_git;
struct strbuf;

/*
 * A received packfile (e.g. the output of fetch/clone/receive-pack, resolved by
 * index-pack) is git's universal wire format and is backend-agnostic: every ODB
 * source can be handed one to install. How a source installs it differs, so
 * installation is a vtable operation. A "files" source keeps the received pack
 * as-is (it is already its storage format); other sources explode it into their
 * own object storage.
 *
 * To install a received pack without re-resolving its deltas, the receiver
 * drives a pack-ingest session: it begins one against the primary source, hands
 * each fully-resolved object to it as the object is reconstructed, then commits.
 * This mirrors the begin/write/commit shape of struct odb_transaction; indeed
 * the generic (non-files) session is implemented on top of an odb_transaction.
 */

/*
 * Describes the received pack to commit. The object index entries and the
 * temporary pack file let a "files" source finalize the pack in place; other
 * sources ignore these because they already received each object via
 * odb_pack_ingest_object().
 */
struct odb_received_pack {
	/* Temporary pack file written by the receiver (already fsync'd+closed). */
	const char *pack_tmp_name;
	/* The pack's trailing hash. */
	unsigned char *pack_hash;

	/* Resolved object entries, used to write the pack index. */
	struct pack_idx_entry **objects;
	uint32_t nr_objects;
	struct pack_idx_option *idx_opts;

	/* Optional final destinations / annotations from the command line. */
	const char *pack_name;
	const char *index_name;
	const char *rev_index_name;
	const char *keep_msg;
	const char *promisor_msg;
};

struct odb_pack_ingest {
	/* The ODB source the pack is being installed into. */
	struct odb_source *source;

	/*
	 * Set for the generic session: the underlying transaction the resolved
	 * objects are written into. NULL for sources that keep the pack as-is.
	 */
	struct odb_transaction *transaction;

	/*
	 * Repack mode: the pack being installed was produced by gc/repack running
	 * pack-objects over THIS source's own objects to re-deltify them, not a
	 * fetch from a remote. The receive callbacks then write with
	 * ODB_WRITE_OBJECT_REPLACE, so the source overwrites each object's stored
	 * representation in place (full<->delta) instead of keeping the existing
	 * copy. Set via odb_pack_ingest_set_repack(); 0 for an ordinary install.
	 */
	int repack;

	/*
	 * Loosen threshold: if non-zero and the received pack holds at most this
	 * many objects, a source that would otherwise keep the pack as-is (the
	 * files source) stores the objects individually instead, so tiny packs do
	 * not accumulate. Set by the receiver via
	 * odb_pack_ingest_set_loosen_if_at_most(); a source that always explodes
	 * the pack ignores it. The mirror image of `repack`: a backend-specific
	 * policy knob the receiver sets unconditionally and only the backend that
	 * cares consumes, so it lives on the session, not in the odb_received_pack
	 * description that every backend reads. Zero means keep the pack.
	 */
	uint32_t loosen_if_at_most;

	/*
	 * Capture buffer for one object's prepared (compressed) bytes during the
	 * single-threaded streaming pass, filled by odb_pack_ingest_capture and
	 * consumed by odb_pack_ingest_store. Unused by a session that does not
	 * absorb prepared bytes; the threaded resolve pass slices per call instead.
	 */
	struct strbuf prepared;

	/*
	 * Receive one fully-resolved object from the pack being installed. The
	 * object data is only borrowed for the duration of the call. Returns 0
	 * on success, a negative error code otherwise.
	 */
	int (*receive_object)(struct odb_pack_ingest *ingest,
			      const struct object_id *oid,
			      enum object_type type,
			      const void *data, unsigned long size);

	/*
	 * Receive one fully-resolved object whose content is delivered as a
	 * stream rather than a buffer. index-pack inflates large blobs (those
	 * over core.bigFileThreshold) without ever holding them in memory, so it
	 * hands them over this way; the stream yields the object content and is
	 * read to completion during the call. Sources that keep the pack as-is
	 * leave this NULL (the wrapper then no-ops). Returns 0 on success, a
	 * negative error code otherwise.
	 */
	int (*receive_object_stream)(struct odb_pack_ingest *ingest,
				     const struct object_id *oid,
				     enum object_type type,
				     struct odb_write_stream *stream,
				     unsigned long size);

	/*
	 * Receive git's prepared compressed entry: the compressed bytes
	 * (`compressed`/`clen`) exactly as a pack holds them, `usize` their
	 * uncompressed length, and `base_oid` the delta base (NULL for a whole
	 * object) -- so a fetch/migrate into a source that stores git's native form
	 * (a helper, via put-raw) neither inflates nor recompresses, and a delta
	 * stays a delta. `resolved`/`resolved_size` carry the reconstructed object
	 * (index-pack holds it; NULL for a streamed large blob, which never reaches
	 * here): used to compute the compat-hash id, and as the payload when a source
	 * lacks write_prepared and must fall back to a resolved write. Sources that
	 * keep the received pack as-is (files) leave this NULL and the wrapper no-ops.
	 * Returns 0 on success, a negative error code otherwise.
	 */
	int (*receive_object_prepared)(struct odb_pack_ingest *ingest,
				       const struct object_id *oid,
				       enum object_type type, unsigned long usize,
				       const struct object_id *base_oid,
				       const void *compressed, unsigned long clen,
				       const void *resolved, unsigned long resolved_size);

	/*
	 * Finish installing the received pack and write the receiver's report
	 * (the line index-pack emits on stdout) into the report buffer.
	 */
	struct packed_git *(*commit)(struct odb_pack_ingest *ingest,
				     const struct odb_received_pack *pack,
				     struct strbuf *report);
};

/*
 * Begin a pack-ingest session against the object database's primary source.
 * Returns NULL if a transaction is already pending on the database.
 */
struct odb_pack_ingest *odb_source_begin_pack_ingest(struct object_database *odb);

/*
 * Hand one fully-resolved object to the session. Returns 0 on success, a
 * negative error code otherwise.
 */
int odb_pack_ingest_object(struct odb_pack_ingest *ingest,
			   const struct object_id *oid,
			   enum object_type type,
			   const void *data, unsigned long size);

/*
 * Hand one object to the session as git's prepared compressed entry already in
 * hand: the compressed bytes + their uncompressed length, base_oid set for a
 * git-format delta (NULL for a whole object), with the reconstructed object in
 * resolved/resolved_size (for the compat id and the no-write_prepared fallback).
 * Used by a source that already holds the compressed form (the helper migrating
 * its quarantine); index-pack goes through odb_pack_ingest_store instead, which
 * produces the bytes on demand. No-ops for a session that keeps the pack.
 * Returns 0 on success, a negative error code otherwise.
 */
int odb_pack_ingest_object_prepared(struct odb_pack_ingest *ingest,
				    const struct object_id *oid,
				    enum object_type type, unsigned long usize,
				    const struct object_id *base_oid,
				    const void *compressed, unsigned long clen,
				    const void *resolved, unsigned long resolved_size);

/*
 * A receiver-supplied accessor that materializes one object's compressed
 * (git-format) entry on demand: it returns the bytes and sets *clen. The
 * returned buffer is heap-allocated and odb_pack_ingest_store frees it.
 * odb_pack_ingest_store calls this only for a session that absorbs prepared
 * bytes, so a files keep-the-pack session never pays for the slice.
 */
typedef const void *(*odb_pack_ingest_slice_fn)(void *ctx, unsigned long *clen);

/*
 * Hand one fully-resolved object to the session. The decision of HOW to store it
 * is made inside the dispatch: a session that absorbs git's native form
 * (generic/helper) stores the prepared (compressed) entry verbatim, taking the
 * compressed bytes from its capture buffer (slice == NULL, the streaming pass)
 * or by calling slice (the resolve pass, which slices the now-readable pack per
 * call); any other session stores the reconstructed object; a files
 * keep-the-pack session absorbs nothing here. base_oid is the delta base (NULL
 * for a whole object). Returns 0 on success, a negative error code otherwise.
 */
int odb_pack_ingest_store(struct odb_pack_ingest *ingest,
			  const struct object_id *oid,
			  enum object_type type, unsigned long usize,
			  const struct object_id *base_oid,
			  const void *resolved, unsigned long resolved_size,
			  odb_pack_ingest_slice_fn slice, void *slice_ctx);

/*
 * Append one run of an object's compressed (deflate) bytes to the session's
 * capture buffer, used by the streaming first pass which has no readable pack to
 * slice from yet. A no-op for a session that does not absorb prepared bytes (a
 * files keep-the-pack session, or no session), so the common path pays nothing.
 */
void odb_pack_ingest_capture(struct odb_pack_ingest *ingest,
			     const void *buf, unsigned long len);

/*
 * Hand one fully-resolved object to the session, with its content delivered as
 * a stream (used for large blobs that are never materialized in full). Returns
 * 0 on success, a negative error code otherwise.
 */
int odb_pack_ingest_object_stream(struct odb_pack_ingest *ingest,
				  const struct object_id *oid,
				  enum object_type type,
				  struct odb_write_stream *stream,
				  unsigned long size);

/*
 * Commit the session, installing the received pack, and write the report line
 * index-pack should emit into the report buffer. Frees the session.
 */
struct packed_git *odb_pack_ingest_commit(struct odb_pack_ingest *ingest,
					  const struct odb_received_pack *pack,
					  struct strbuf *report);

/*
 * The generic (non-files) session constructor: writes each received object into
 * an odb_transaction and commits it. Sources that store objects individually
 * use this; the files source overrides it to keep the received pack as-is.
 */
struct odb_pack_ingest *odb_pack_ingest_begin_generic(struct odb_source *source);

/*
 * Put the session into repack mode (see struct odb_pack_ingest's `repack`).
 * index-pack sets this when it is driven by a source's optimize/repack pass, so
 * the re-deltified objects overwrite their stored representations in place
 * rather than being kept-existing. A no-op concept for a session whose source
 * keeps the received pack as-is (files repacks via its own pack install).
 */
void odb_pack_ingest_set_repack(struct odb_pack_ingest *ingest);

/*
 * Set the session's loosen threshold (see struct odb_pack_ingest's
 * `loosen_if_at_most`): the receiver passes the unpack limit it computed from
 * config, and a files source loosens a received pack at or under that many
 * objects instead of keeping it as a packfile. A no-op concept for a source
 * that always explodes the pack. No-op if there is no session.
 */
void odb_pack_ingest_set_loosen_if_at_most(struct odb_pack_ingest *ingest,
					   uint32_t at_most);

#endif /* ODB_PACK_INGEST_H */
