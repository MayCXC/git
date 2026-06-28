#include "git-compat-util.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "odb.h"
#include "odb/source.h"
#include "odb/transaction.h"
#include "odb/pack-ingest.h"
#include "oidset.h"
#include "pack.h"
#include "repository.h"
#include "strbuf.h"

struct odb_pack_ingest *odb_source_begin_pack_ingest(struct object_database *odb)
{
	/*
	 * A pack-ingest session runs inside its own object transaction; refuse to
	 * start one while another transaction is already pending on the database
	 * (the contract documented in pack-ingest.h).
	 */
	if (odb->transaction)
		return NULL;
	odb_prepare_sources(odb);
	return odb->sources->begin_pack_ingest(odb->sources);
}

int odb_pack_ingest_object(struct odb_pack_ingest *ingest,
			   const struct object_id *oid,
			   enum object_type type,
			   const void *data, unsigned long size)
{
	/* No session means the caller is not installing into a source. */
	if (!ingest)
		return 0;
	return ingest->receive_object(ingest, oid, type, data, size);
}

int odb_pack_ingest_object_stream(struct odb_pack_ingest *ingest,
				  const struct object_id *oid,
				  enum object_type type,
				  struct odb_write_stream *stream,
				  unsigned long size)
{
	/*
	 * No session, or a source that keeps the received pack as-is (files):
	 * there is nothing to absorb per object, so the stream is left unread.
	 */
	if (!ingest || !ingest->receive_object_stream)
		return 0;
	return ingest->receive_object_stream(ingest, oid, type, stream, size);
}

int odb_pack_ingest_object_prepared(struct odb_pack_ingest *ingest,
				    const struct object_id *oid,
				    enum object_type type, unsigned long usize,
				    const struct object_id *base_oid,
				    const void *compressed, unsigned long clen,
				    const void *resolved, unsigned long resolved_size)
{
	/*
	 * No session, or a source that keeps the received pack as-is (files): the
	 * pack already holds git's compressed entry, so there is nothing to absorb.
	 */
	if (!ingest || !ingest->receive_object_prepared)
		return 0;
	return ingest->receive_object_prepared(ingest, oid, type, usize, base_oid,
					       compressed, clen, resolved,
					       resolved_size);
}

void odb_pack_ingest_capture(struct odb_pack_ingest *ingest,
			     const void *buf, unsigned long len)
{
	/*
	 * Only a session that absorbs git's prepared form accumulates the bytes; a
	 * files keep-the-pack session (or no session) leaves receive_object_prepared
	 * NULL, so the capture is a no-op and the streaming pass pays nothing.
	 */
	if (!ingest || !ingest->receive_object_prepared)
		return;
	strbuf_add(&ingest->prepared, buf, len);
}

int odb_pack_ingest_store(struct odb_pack_ingest *ingest,
			  const struct object_id *oid,
			  enum object_type type, unsigned long usize,
			  const struct object_id *base_oid,
			  const void *resolved, unsigned long resolved_size,
			  odb_pack_ingest_slice_fn slice, void *slice_ctx)
{
	/* No session: the caller is not installing into a source. */
	if (!ingest)
		return 0;

	/*
	 * A session that stores git's native form absorbs the prepared (compressed)
	 * entry verbatim. The bytes come from the session's capture buffer (the
	 * single-threaded streaming pass, slice == NULL) or are sliced from the
	 * now-readable pack per call (the threaded resolve pass). A files
	 * keep-the-pack session leaves receive_object_prepared NULL and never gets
	 * here, so it neither captures nor slices: the decision lives entirely in
	 * the dispatch, never in the agnostic receiver.
	 */
	if (ingest->receive_object_prepared) {
		const void *compressed;
		unsigned long clen;
		void *sliced = NULL;
		int ret;

		if (slice) {
			sliced = (void *)slice(slice_ctx, &clen);
			compressed = sliced;
		} else {
			compressed = ingest->prepared.buf;
			clen = ingest->prepared.len;
		}
		ret = odb_pack_ingest_object_prepared(ingest, oid, type, usize,
						      base_oid, compressed, clen,
						      resolved, resolved_size);
		free(sliced);
		/*
		 * The capture buffer holds one object; reset it for the next
		 * streaming object. The resolve pass slices per call and never fills
		 * it, so only reset on the single-threaded streaming path to avoid
		 * racing the resolve threads.
		 */
		if (!slice)
			strbuf_reset(&ingest->prepared);
		return ret;
	}

	/*
	 * Otherwise store the reconstructed object: a source without write_prepared,
	 * or a receiver that has only resolved data (unpack-objects). A files
	 * keep-the-pack session no-ops here too.
	 */
	if (ingest->receive_object)
		return ingest->receive_object(ingest, oid, type, resolved,
					      resolved_size);
	return 0;
}

struct packed_git *odb_pack_ingest_commit(struct odb_pack_ingest *ingest,
					  const struct odb_received_pack *pack,
					  struct strbuf *report)
{
	struct packed_git *installed;

	if (!ingest)
		return NULL;
	installed = ingest->commit(ingest, pack, report);
	strbuf_release(&ingest->prepared);
	free(ingest);
	return installed;
}

/*
 * Generic session: explode the received pack into the backend by writing each
 * resolved object through an ODB transaction. Used by every source that stores
 * objects individually; the files source overrides this to keep the pack.
 */

static int generic_receive_object(struct odb_pack_ingest *ingest,
				  const struct object_id *oid,
				  enum object_type type,
				  const void *data, unsigned long size)
{
	struct object_id written;

	/*
	 * Large blobs arrive without a buffer (data == NULL); index-pack routes
	 * those to generic_receive_object_stream() instead, so reaching here with
	 * no data is a routing bug in the receiver, not a user-facing condition.
	 */
	if (!data)
		BUG("large object %s must be ingested via the streaming path",
		    oid_to_hex(oid));

	return odb_write_object_ext(ingest->source->odb, data, size, type,
				    &written, NULL,
				    ODB_WRITE_OBJECT_PERSIST |
				    (ingest->repack ? ODB_WRITE_OBJECT_REPLACE : 0));
}

/*
 * Receive git's prepared compressed entry (whole object or git-format delta) and
 * store it verbatim via the source's write_prepared (a helper, via put-raw), no
 * compress, no resolve. odb_write_prepared_ext computes any compat-hash id from the
 * resolved object and gracefully falls back to a resolved write for a source
 * without write_prepared. The files source keeps the pack and never installs this
 * (the wrapper no-ops there).
 */
static int generic_receive_object_prepared(struct odb_pack_ingest *ingest,
					   const struct object_id *oid,
					   enum object_type type,
					   unsigned long usize,
					   const struct object_id *base_oid,
					   const void *compressed,
					   unsigned long clen,
					   const void *resolved,
					   unsigned long resolved_size)
{
	return odb_write_prepared_ext(ingest->source->odb, oid, type, usize,
				      base_oid, compressed, clen,
				      resolved, resolved_size,
				      ODB_WRITE_OBJECT_PERSIST |
				      (ingest->repack ? ODB_WRITE_OBJECT_REPLACE : 0));
}

static int generic_receive_object_stream(struct odb_pack_ingest *ingest,
					 const struct object_id *oid UNUSED,
					 enum object_type type UNUSED,
					 struct odb_write_stream *stream,
					 unsigned long size)
{
	struct object_id written;

	/*
	 * Stream the object straight into the backend (e.g. a helper's
	 * put-stream) without ever holding it in memory. Only blobs are
	 * streamed, so the type is implied; odb_write_object_stream hashes the
	 * content as it goes and yields the resulting oid, which equals the one
	 * index-pack already computed.
	 */
	return odb_write_object_stream(ingest->source->odb, stream, size,
				       &written);
}

static struct packed_git *generic_commit(struct odb_pack_ingest *ingest,
					 const struct odb_received_pack *pack,
					 struct strbuf *report)
{
	const struct git_hash_algo *algo = ingest->source->odb->repo->hash_algo;
	const char *report_token = NULL;

	/*
	 * Set up the keep BEFORE committing, so the just-ingested objects never
	 * become visible to a concurrent prune without their keep already in place
	 * (closing the window between the commit and the keep). Two halves, both
	 * before the commit:
	 *
	 *  - the marker file under objects/pack named for the received pack's hash;
	 *    git's keep lifecycle owns it, it is reported below as "keep\t<hash>"
	 *    so fetch records it in pack_lockfiles and transport_unlock_pack unlinks
	 *    it once refs are committed (the analogue of files' install_packfile keep
	 *    and a remote helper's "lock <file>"). Its content is the keep reason,
	 *    exactly as files writes it.
	 *
	 *  - the pack membership, recorded through the source: a source that explodes
	 *    the pack has no pack file to scope the keep to, so it records which
	 *    objects arrived in this pack and its prune spares them while the marker
	 *    is live (the relational form of files keeping a kept pack's objects).
	 *    Sent on the still-open ingest transaction, so membership and objects
	 *    commit together. The files source keeps the pack on disk and leaves the
	 *    callback NULL.
	 *
	 * Gated on `report`: the marker is only useful once its token reaches fetch,
	 * which releases it, writing one no caller tracks would leak it.
	 */
	if (report && pack->keep_msg) {
		struct repository *repo = ingest->source->odb->repo;

		write_special_file(repo, "keep", pack->keep_msg, NULL,
				   pack->pack_hash, &report_token);
		/*
		 * The marker above is git's keep lifecycle, shared by every
		 * backend. The membership below is the backend's own: a source
		 * that explodes the pack records which objects arrived so its
		 * prune can spare them; the files source keeps the pack on disk
		 * and uses the no-op default. Hand it the pack so a backend that
		 * needs the set walks it itself, building nothing on the common
		 * files path.
		 */
		odb_source_keep_pack(ingest->source, pack);
	}

	odb_transaction_commit(ingest->transaction);

	/*
	 * A promisor pack records that its objects come from a promisor remote.
	 * The files source captures this with a ".promisor" file beside the
	 * pack; a source that explodes the pack instead records it per object,
	 * so route the marking through the source. Sources with no promisor
	 * concept use the no-op default.
	 */
	if (pack->promisor_msg) {
		struct oidset oids = OIDSET_INIT;
		uint32_t i;

		for (i = 0; i < pack->nr_objects; i++)
			oidset_insert(&oids, &pack->objects[i]->oid);
		odb_source_mark_objects_promisor(ingest->source, &oids);
		oidset_clear(&oids);
	}

	/*
	 * The received pack was exploded into the backend, so the temporary
	 * pack file is not part of any backend's storage; discard it.
	 */
	if (pack->pack_tmp_name)
		unlink(pack->pack_tmp_name);

	/*
	 * Report the installed pack line index-pack emits. With a keep, report the
	 * marker's token + the pack hash so fetch tracks the lock and later releases
	 * it (transport_unlock_pack); without one, no packfile was installed, so
	 * report the all-zero hash to keep the "pack\t<hash>" output well-formed.
	 */
	if (report) {
		if (report_token)
			strbuf_addf(report, "%s\t%s\n", report_token,
				    hash_to_hex_algop(pack->pack_hash, algo));
		else
			strbuf_addf(report, "pack\t%0*d\n", (int)algo->hexsz, 0);
	}

	return NULL;
}

struct odb_pack_ingest *odb_pack_ingest_begin_generic(struct odb_source *source)
{
	struct odb_pack_ingest *ingest;

	CALLOC_ARRAY(ingest, 1);
	strbuf_init(&ingest->prepared, 0);
	ingest->source = source;
	ingest->transaction = odb_transaction_begin(source->odb);
	ingest->receive_object = generic_receive_object;
	ingest->receive_object_stream = generic_receive_object_stream;
	ingest->receive_object_prepared = generic_receive_object_prepared;
	ingest->commit = generic_commit;

	return ingest;
}

void odb_pack_ingest_set_repack(struct odb_pack_ingest *ingest)
{
	ingest->repack = 1;
}

void odb_pack_ingest_set_loosen_if_at_most(struct odb_pack_ingest *ingest,
					   uint32_t at_most)
{
	/*
	 * No session means the caller is not installing into a source; match the
	 * other wrappers and no-op rather than dereferencing.
	 */
	if (!ingest)
		return;
	ingest->loosen_if_at_most = at_most;
}
