#ifndef ODB_SOURCE_H
#define ODB_SOURCE_H

#include "object.h"
#include "odb.h"

struct object_id;
struct git_hash_algo;
struct odb_read_stream;
struct odb_pack_ingest;
struct odb_received_pack;
struct packfile_store;
struct packed_git;
struct strvec;
struct fsck_options;
struct oidset;
struct packfile_list_entry;
struct multi_pack_index;
struct string_list;
struct commit;
struct bitmap_index;
struct write_commit_graph_context;

/* Options "git clone" passes to odb_source local_clone, mirroring its CLI flags:
 * --shared records the source as an alternate instead of copying; otherwise
 * --no-hardlinks forces a copy over a hardlink, and --local makes a failed
 * hardlink fatal rather than falling back to a copy. */
struct odb_local_clone_opts {
	unsigned shared:1;
	unsigned no_hardlinks:1;
	unsigned local_forced:1;
};

/* Per-object emit callback for odb_source_stream_reuse(): invoked once per
 * reused object, in pack_pos order, with its type, pack-entry size, delta base
 * (NULL if not a delta) and verbatim compressed content. Returns non-zero to
 * abort the stream. */
typedef int (*odb_reuse_emit_fn)(void *data, enum object_type type,
				 unsigned long size,
				 const struct object_id *base_oid,
				 const void *content, unsigned long clen);

/*
 * The source is the part of the object database that stores the actual
 * objects. It thus encapsulates the logic to read and write the specific
 * on-disk format. An object database can have multiple sources:
 *
 *   - The primary source, which is typically located in "$GIT_DIR/objects".
 *     This is where new objects are usually written to.
 *
 *   - Alternate sources, which are configured via "objects/info/alternates" or
 *     via the GIT_ALTERNATE_OBJECT_DIRECTORIES environment variable. These
 *     alternate sources are only used to read objects.
 */
struct odb_source {
	struct odb_source *next;

	/* Object database that owns this object source. */
	struct object_database *odb;

	/*
	 * Figure out whether this is the local source of the owning
	 * repository, which would typically be its ".git/objects" directory.
	 * This local object directory is usually where objects would be
	 * written to.
	 */
	bool local;

	/*
	 * This object store is ephemeral, so there is no need to fsync.
	 */
	int will_destroy;

	/*
	 * Path to the source. If this is a relative path, it is relative to
	 * the current working directory.
	 */
	char *path;

	/*
	 * This callback is expected to free the underlying object database source and
	 * all associated resources. The function will never be called with a NULL pointer.
	 */
	void (*free)(struct odb_source *source);

	/*
	 * This callback is expected to close any open resources, like for
	 * example file descriptors or connections. The source is expected to
	 * still be usable after it has been closed. Closed resources may need
	 * to be reopened in that case.
	 */
	void (*close)(struct odb_source *source);

	/*
	 * This callback is expected to clear underlying caches of the object
	 * database source. The function is called when the repository has for
	 * example just been repacked so that new objects will become visible.
	 */
	void (*reprepare)(struct odb_source *source);

	/*
	 * This callback is expected to read object information from the object
	 * database source. The object info will be partially populated with
	 * pointers for each bit of information that was requested by the
	 * caller.
	 *
	 * The flags field is a combination of `OBJECT_INFO` flags. Only the
	 * following fields need to be handled by the backend:
	 *
	 *   - `OBJECT_INFO_QUICK` indicates it is fine to use caches without
	 *     re-verifying the data.
	 *
	 *   - `OBJECT_INFO_SECOND_READ` indicates that the initial object
	 *     lookup has failed and that the object sources should check
	 *     whether any of its on-disk state has changed that may have
	 *     caused the object to appear. Sources are free to ignore the
	 *     second read in case they know that the first read would have
	 *     already surfaced the object without reloading any on-disk state.
	 *
	 * The callback is expected to return a negative error code in case
	 * reading the object has failed, 0 otherwise.
	 */
	int (*read_object_info)(struct odb_source *source,
				const struct object_id *oid,
				struct object_info *oi,
				enum object_info_flags flags);

	/*
	 * This callback is expected to create a new read stream that can be
	 * used to stream the object identified by the given ID.
	 *
	 * The callback is expected to return a negative error code in case
	 * creating the object stream has failed, 0 otherwise.
	 */
	int (*read_object_stream)(struct odb_read_stream **out,
				  struct odb_source *source,
				  const struct object_id *oid);

	/*
	 * Read this source's slice of the storage<->compat object-id map (the
	 * SHA-1<->SHA-256 loose-object-idx) into the odb-level map. Each backend
	 * does it its own way: a files source reads the idx in its own object
	 * directory and feeds its abbreviation cache; a source that stores objects
	 * in its own backing store (a helper) reads the idx kept at its object
	 * directory with no loose cache. A source with no persistent compat map
	 * (the in-memory source) keeps the no-op installed by odb_source_init().
	 * Dispatched per source so repo_read_loose_object_map() need not branch on
	 * the backend type. Returns 0 on success, a negative error code otherwise.
	 */
	int (*read_compat_map)(struct odb_source *source);

	/*
	 * This callback is expected to iterate over all objects stored in this
	 * source and invoke the callback function for each of them. It is
	 * valid to yield the same object multiple time. A non-zero exit code
	 * from the object callback shall abort iteration.
	 *
	 * The optional `request` structure should serve as a template for
	 * looking up object info for every individual iterated object. It
	 * should not be modified directly and should instead be copied into a
	 * separate `struct object_info` that gets passed to the callback. If
	 * the caller passes a `NULL` pointer then the object itself shall not
	 * be read.
	 *
	 * The callback is expected to return a negative error code in case the
	 * iteration has failed to read all objects, 0 otherwise. When the
	 * callback function returns a non-zero error code then that error code
	 * should be returned.
	 */
	int (*for_each_object)(struct odb_source *source,
			       const struct object_info *request,
			       odb_for_each_object_cb cb,
			       void *cb_data,
			       const struct odb_for_each_object_options *opts);

	/*
	 * This callback is expected to count objects in the given object
	 * database source. The callback function does not have to guarantee
	 * that only unique objects are counted. The result shall be assigned
	 * to the `out` pointer.
	 *
	 * Accepts `enum odb_count_objects_flag` flags to alter the behaviour.
	 *
	 * The callback is expected to return 0 on success, or a negative error
	 * code otherwise.
	 */
	int (*count_objects)(struct odb_source *source,
			     enum odb_count_objects_flags flags,
			     unsigned long *out);

	/*
	 * This callback reports, via `*out`, the number of "loose" objects in
	 * the source, i.e. those not stored in a pack: the files backend's loose
	 * tier (honoring ODB_COUNT_OBJECTS_APPROXIMATE, which samples a shard and
	 * scales it). Sources with no loose tier (e.g. a helper) keep the no-op
	 * installed by odb_source_init() and report zero. Used by the GC loose
	 * object heuristics.
	 *
	 * The callback is expected to return 0 on success, or a negative error
	 * code otherwise.
	 */
	int (*count_loose_objects)(struct odb_source *source,
				   enum odb_count_objects_flags flags,
				   unsigned long *out);

	/*
	 * Iterate this source's "loose" (non-packed) objects, invoking the same
	 * callbacks for_each_loose_file_in_source() would: obj_cb per loose object,
	 * cruft_cb per stray non-object file, subdir_cb per fanout subdir. The files
	 * source walks its loose object directory; a source with no loose tier (e.g.
	 * a helper) keeps the no-op default installed by odb_source_init() and
	 * iterates nothing, so the GC / prune / count-objects loose paths dispatch
	 * here with no branch on the backend type. A non-zero callback return aborts
	 * iteration and is returned; 0 when iteration completes.
	 */
	int (*for_each_loose_object)(struct odb_source *source,
				     int (*obj_cb)(const struct object_id *oid,
						   const char *path, void *data),
				     int (*cruft_cb)(const char *basename,
						     const char *path, void *data),
				     int (*subdir_cb)(unsigned int nr,
						      const char *path, void *data),
				     void *data);

	/*
	 * This callback accumulates the source's local packfile layout (pack
	 * count, contained object count, on-disk size) into `report`. The files
	 * source reports its local packs; sources without packfiles (e.g. a
	 * helper) keep the no-op installed by odb_source_init() and add nothing.
	 * Used by "git count-objects -v".
	 */
	void (*count_packs)(struct odb_source *source,
			    struct odb_pack_report *report);

	/*
	 * This callback is expected to find the minimum required length to
	 * make the given object ID unique.
	 *
	 * The callback is expected to return a negative error code in case it
	 * failed, 0 otherwise.
	 */
	int (*find_abbrev_len)(struct odb_source *source,
			       const struct object_id *oid,
			       unsigned min_length,
			       unsigned *out);

	/*
	 * This callback is expected to freshen the given object so that its
	 * last access time is set to the current time. This is used to ensure
	 * that objects that are recent will not get garbage collected even if
	 * they were unreachable.
	 *
	 * Returns 0 in case the object does not exist, 1 in case the object
	 * has been freshened.
	 */
	int (*freshen_object)(struct odb_source *source,
			      const struct object_id *oid);

	/*
	 * This callback is expected to persist the given object into the
	 * object source. In case the object already exists it shall be
	 * freshened.
	 *
	 * The flags field is a combination of `WRITE_OBJECT` flags.
	 *
	 * The resulting object ID (and optionally the compatibility object ID)
	 * shall be written into the out pointers. The callback is expected to
	 * return 0 on success, a negative error code otherwise.
	 */
	int (*write_object)(struct odb_source *source,
			    const void *buf, unsigned long len,
			    enum object_type type,
			    struct object_id *oid,
			    struct object_id *compat_oid,
			    enum odb_write_object_flags flags);

	/*
	 * Persist an object already in git's native pack storage form: the
	 * compressed entry bytes (`compressed`/`clen`) exactly as a pack holds
	 * them, `usize` the uncompressed length (the pack entry-header size), and
	 * `base_oid` the delta base (NULL for a whole object). The source stores
	 * them verbatim, the recompress-free receive path, the per-object analog
	 * of the files backend keeping a received pack's compressed entries. git
	 * already prepared the bytes (index-pack's entry on receive, pack-objects'
	 * on repack), so the source neither compresses nor resolves. `compat_oid`
	 * is the precomputed compatibility-hash id (or NULL when the repo tracks no
	 * compat algorithm), since the source stores opaque bytes and cannot hash
	 * them; a source that keeps a git-side compat map records it, exactly as
	 * write_object does. NULL write_prepared (the odb_source_init default) means
	 * the source does not store prepared bytes; the caller falls back to a
	 * resolved write. Returns 0 on success, a negative error code otherwise.
	 */
	int (*write_prepared)(struct odb_source *source,
			      const struct object_id *oid,
			      const struct object_id *compat_oid,
			      enum object_type type, unsigned long usize,
			      const struct object_id *base_oid,
			      const void *compressed, unsigned long clen,
			      enum odb_write_object_flags flags);

	/*
	 * Read a stored git-format delta for `oid`: its base oid, the COMPRESSED
	 * delta bytes (the caller owns `*delta`, `*delta_len` their byte count) and
	 * `*raw_len` the uncompressed delta length, without resolving it. The read
	 * companion to write_prepared (a source that stored a compressed delta via
	 * put-raw), and the analog of pulling a reusable delta straight out of a pack:
	 * the bytes come back compressed so pack-objects reuses them verbatim on a send
	 * pack (its z_delta_size cached-delta path, 0 inflate, 0 recompress) and a
	 * push migrate copies them straight into the destination. The caller matches
	 * `*raw_len` against the delta size it planned, exactly as it vets a pack's own
	 * reusable delta. The default installed by odb_source_init() reports "not stored
	 * as a delta" (returns 0), correct for a source that resolves objects on read
	 * (the files loose tier); the files packfile path slices the compressed delta
	 * out of the pack, and a helper serves it via get-delta. Returns 1 with the outs
	 * filled when a stored delta exists, 0 when the object is not stored as a delta,
	 * a negative value on error.
	 */
	int (*read_object_delta)(struct odb_source *source,
				 const struct object_id *oid,
				 struct object_id *base_oid,
				 void **delta, unsigned long *delta_len,
				 unsigned long *raw_len);

	/*
	 * Stream the reusable pack_pos prefix [0, end_pos) for source-backed pack
	 * reuse: invoke `emit` once per object, in pack_pos order, with its type,
	 * pack-entry size, delta base (NULL if not a delta) and verbatim compressed
	 * content. The bulk analog of read_object_delta (one stored delta) and of
	 * the files backend copying a contiguous .pack region; the caller frames
	 * each into the output pack (REF_DELTA + base for a delta, plain header
	 * otherwise). A source that serves a source bitmap implements this: the
	 * bitmap is by construction over the bit-ordered objects, so the same
	 * gc-time capture that produced it can stream them back. end_pos is the
	 * window reuse_window granted at setup. The files source uses the no-op
	 * default and reuses via the packfile region copy instead. Returns 0 on
	 * success, negative otherwise.
	 */
	int (*stream_reuse)(struct odb_source *source, uint32_t end_pos,
			    odb_reuse_emit_fn emit, void *data);

	/*
	 * The leading run of objects this source will bulk-reuse via stream_reuse,
	 * given the candidate prefix length the caller computed from the result
	 * bitmap (the contiguous all-result run, in pack_pos order). The source
	 * returns how many of those it will actually stream, 0 to decline; the
	 * caller marks exactly that window for reuse and emits the rest per-object.
	 * This keeps the reuse decision inside the source (serving a bitmap and bulk
	 * reuse are independent, a helper advertises them separately), so the caller
	 * engages reuse on the returned window without testing a capability. The
	 * no-op default declines (returns 0); the files source uses it and reuses
	 * via the packfile region copy instead.
	 */
	uint32_t (*reuse_window)(struct odb_source *source, uint32_t candidate);

	/*
	 * The multi-pack-index operations. A multi-pack-index indexes ".pack"
	 * files, so it is a files-backend concept: the files source implements each
	 * by calling its midx machinery, while the no-op default does nothing (a
	 * source that stores objects in its own backing store, like a helper, has no
	 * packfiles to index). "git multi-pack-index" and gc dispatch these on the
	 * source they operate on, so the files path runs the midx code unconditionally
	 * and every other backend declines, with no branch on the backend type. Each
	 * returns 0 on success (the no-op default included), non-zero on failure.
	 * write takes a NULL packs_to_include for a full index; compact resolves its
	 * endpoints (by checksum) from the source's own midx chain.
	 */
	int (*multi_pack_index_write)(struct odb_source *source,
				      struct string_list *packs_to_include,
				      const char *preferred_pack_name,
				      const char *refs_snapshot,
				      const char *incremental_base,
				      unsigned flags);
	int (*multi_pack_index_compact)(struct odb_source *source,
					const char *from_checksum,
					const char *to_checksum,
					const char *incremental_base,
					unsigned flags);
	int (*multi_pack_index_verify)(struct odb_source *source, unsigned flags);
	int (*multi_pack_index_expire)(struct odb_source *source, unsigned flags);
	int (*multi_pack_index_repack)(struct odb_source *source,
				       size_t batch_size, unsigned flags);

	/*
	 * This callback is expected to persist the given object stream into
	 * the object source.
	 *
	 * The resulting object ID shall be written into the out pointer. The
	 * callback is expected to return 0 on success, a negative error code
	 * otherwise.
	 */
	int (*write_object_stream)(struct odb_source *source,
				   struct odb_write_stream *stream, size_t len,
				   struct object_id *oid);

	/*
	 * This callback is expected to create a new transaction that can be
	 * used to write objects to. The objects shall only be persisted into
	 * the object database when the transcation's commit function is
	 * called. Otherwise, the objects shall be discarded.
	 *
	 * Returns 0 on success, in which case the `*out` pointer will have
	 * been populated with the object database transaction. Returns a
	 * negative error code otherwise.
	 */
	int (*begin_transaction)(struct odb_source *source,
				 struct odb_transaction **out);

	/*
	 * This callback is expected to begin a pack-ingest session that
	 * installs a received packfile (git's universal wire format) into the
	 * source. The session receives each resolved object via its
	 * receive_object callback and is finalized via its commit callback. The
	 * generic implementation explodes the pack into individual objects; the
	 * files source overrides it to keep the received pack as-is.
	 */
	struct odb_pack_ingest *(*begin_pack_ingest)(struct odb_source *source);

	/*
	 * Install a fully downloaded and verified loose object that currently
	 * lives, in on-disk loose format, at temp_path (e.g. an object fetched
	 * by the dumb-http walker). The files source overrides this to rename
	 * the file into its loose layout (zero-copy); the generic implementation
	 * inflates it and writes it through the source's own write_object path
	 * (e.g. a helper "put").
	 */
	int (*install_loose_object)(struct odb_source *source,
				    const char *temp_path,
				    const struct object_id *oid);

	/*
	 * Integrate a pack just fetched and indexed by the dumb-http walker
	 * (index-pack has already ingested its objects through this source).
	 * The files source overrides this to register the on-disk pack in its
	 * in-core pack list (a targeted install, no directory rescan); the
	 * generic implementation discards the files-only descriptor and
	 * reprepares so the ingested objects become visible. Takes ownership
	 * of p.
	 */
	void (*note_received_pack)(struct odb_source *source,
				   struct packed_git *p);

	/*
	 * Integrate a pack just written and indexed locally (e.g. by index-pack
	 * with --fsck-objects), identified by its index path, so its objects are
	 * readable for the in-process verification that follows. The files source
	 * loads the on-disk pack into its in-core list (a targeted, deduplicated
	 * load, no directory rescan); the generic implementation reprepares so the
	 * objects its pack-ingest path stored become visible.
	 */
	void (*note_indexed_pack)(struct odb_source *source,
				  const char *idx_path);

	/*
	 * This callback is expected to record the given objects as promisor
	 * objects in the source's native way, so that they (and the absent
	 * objects they may reference) are retained and never reported missing.
	 * It is invoked after a promisor pack is ingested, with two object sets:
	 * the received pack's own objects (obtained from a promisor remote), and
	 * the local objects that pack references (which must likewise be
	 * retained even though nothing local refers to them yet).
	 *
	 * The files source gathers the objects into a ".promisor" packfile; a
	 * helper source records them over its protocol. A source with no
	 * promisor concept uses the no-op default. `oids` is the set to mark;
	 * the backend selects from it whatever it still needs to record.
	 * Returns 0 on success, a negative error code otherwise.
	 *
	 * (The files source marks the received pack itself with a ".promisor"
	 * file at ingest time, so for that set it is reached only for the
	 * referenced local objects; a helper, which explodes the pack, is
	 * reached for both.)
	 */
	int (*mark_objects_promisor)(struct odb_source *source,
				     struct oidset *oids);

	/*
	 * Record that the objects of the just-received `pack` arrived together
	 * (identified by its trailing hash), so a backend that explodes the pack can
	 * still answer "is this object in this pack?" the way the files source reads
	 * it from the pack file. It is invoked while a keep is requested, just before
	 * the ingest transaction commits, so the membership is in place the instant
	 * the objects become visible: while the matching pack-<hash>.keep marker is
	 * live, the source's prune spares these objects (the relational form of files
	 * keeping a .keep'd pack's objects). Taking the pack (rather than a prebuilt
	 * oid set) lets a backend that needs the membership walk `pack->objects`
	 * itself, so the common files path builds nothing. The files source keeps the
	 * pack on disk and uses the no-op default; a source with no keep concept does
	 * likewise. Returns 0 on success, a negative error code otherwise.
	 */
	int (*keep_pack)(struct odb_source *source,
			 const struct odb_received_pack *pack);

	/*
	 * This callback is expected to optimize the storage of the source, for
	 * example by repacking loose objects and packs into a single pack (the
	 * files source) or by compacting a backing database (a helper source).
	 * The flags field is a combination of `ODB_OPTIMIZE_*` flags.
	 *
	 * The default installed by odb_source_init() is a no-op, so sources
	 * that have nothing to optimize need not implement this. Returns 0 on
	 * success, a negative error code otherwise.
	 */
	int (*optimize)(struct odb_source *source,
			struct odb_optimize_opts *opts);

	/*
	 * This callback is expected to report, via `*required`, whether the
	 * source would benefit from a call to its optimize callback. It is used
	 * to decide whether automatic maintenance ("gc --auto") should run.
	 *
	 * The default installed by odb_source_init() reports that no
	 * optimization is required. Returns 0 on success, a negative error code
	 * otherwise.
	 */
	int (*optimize_required)(struct odb_source *source,
				 struct odb_optimize_opts *opts,
				 bool *required);

	/*
	 * Migrate the accepted objects of a push quarantine into this source. A
	 * push always stages in a files quarantine (a tmp-objdir at
	 * `quarantine_path`) so the connectivity check and hooks can read the
	 * objects before acceptance; on accept they move here. The files source
	 * renames the quarantine's loose objects and packs into its object
	 * directory in place. A source that stores objects in its own medium (a
	 * helper) copies them in instead, reading the already-resolved quarantine
	 * objects and storing each, deltas preserved, without re-resolving the
	 * pack. The default does nothing, for a source that needs no migration.
	 * Returns 0 on success, a negative error code otherwise.
	 */
	int (*migrate_quarantine)(struct odb_source *source,
				  const char *quarantine_path);

	/*
	 * Install this freshly-constructed primary source into the object
	 * database's source list (set odb->sources and sources_tail), adding any
	 * receive-time staging the backend needs. The default installs the source
	 * as the sole primary. The helper overrides it: when a quarantine is
	 * active (GIT_QUARANTINE_PATH set) it puts a files quarantine in front as
	 * the write primary and keeps itself as a read-through secondary, since its
	 * own writes do not land in GIT_OBJECT_DIRECTORY and so are not captured by
	 * the quarantine; the accepted objects are ingested via migrate_quarantine.
	 */
	void (*prepare_source_list)(struct odb_source *source,
				    struct object_database *odb);

	/*
	 * This callback is expected to verify the integrity of the objects
	 * stored in this source (the storage side of "git fsck"): the files
	 * source scans its loose objects, while a helper source asks the helper
	 * to enumerate its objects and to check its backing store. Each
	 * enumerated object's contents are handed to `cb` (the caller's
	 * per-object fsck handler) for content checking; storage-format problems
	 * are reported through the fsck_options callbacks.
	 *
	 * The default installed by odb_source_init() is a no-op, so sources with
	 * nothing format-specific to verify need not implement this. Returns 0
	 * on success, a negative error code otherwise.
	 */
	int (*verify)(struct odb_source *source, struct fsck_options *o,
		      odb_verify_cb cb, void *cb_data);

	/*
	 * This callback prunes unreachable objects from the source's storage (the
	 * storage side of "git prune"). git computes reachability (refs, reflogs,
	 * and the index) and hands the source the set of ITS objects it found
	 * unreachable; the source deletes those created at or before `expire`,
	 * comparing against its own timestamps (the files source the loose mtime,
	 * a helper its stored time) so a write that just landed a not-yet-
	 * referenced object is preserved. The files source unlinks the matching
	 * loose objects (packed objects are pruned by repack, not here); a helper
	 * source asks the helper to delete them.
	 *
	 * `removed`, when not NULL, is filled with exactly the objects this source
	 * removed, so the caller can report precisely what was pruned rather than
	 * the whole candidate set (the loose-and-expired subset for files, the
	 * helper's own expired subset for a helper). With `dry_run` set the source
	 * populates `removed` the same way but deletes nothing, which is how
	 * "git prune --dry-run" reports what it would remove.
	 *
	 * The default installed by odb_source_init() is a no-op, so sources with
	 * nothing to prune need not implement this. Returns 0 on success, a
	 * negative error code otherwise.
	 */
	int (*remove_objects)(struct odb_source *source,
			      struct oidset *prune, timestamp_t expire,
			      int dry_run, struct oidset *removed);

	/*
	 * Tidy this source's storage after "git prune" removed unreachable
	 * objects: the files source drops stale tmp_* files, now-empty fanout
	 * dirs, and loose objects made redundant by a pack (cruft is a
	 * files-storage concept). `dry_run` reports without removing; `verbose`
	 * reports removals. The default installed by odb_source_init() is a
	 * no-op, so a source that stores objects in its own backing store (a
	 * helper) has no such cruft and need not implement this; odb_prune_cruft()
	 * dispatches it per local source with no branch on the backend type.
	 * Returns 0 on success, a negative error code otherwise.
	 */
	int (*prune_cruft)(struct odb_source *source, timestamp_t expire,
			   int dry_run, int verbose);

	/*
	 * Remove this source's entire on-disk object storage. It is called after
	 * an object-storage migration has copied every object into a new backend
	 * and switched the repository over to it, leaving this source's store
	 * redundant. The files source deletes its loose object directories, its
	 * packs (and multi-pack-index) and its commit-graph, while preserving the
	 * alternates configuration (which is storage location, not storage). The
	 * default installed by odb_source_init() is a no-op, for a source with no
	 * separable on-disk store of its own (a helper keeps its objects in its
	 * own backing store). Returns 0 on success, a negative error code otherwise.
	 */
	int (*remove_storage)(struct odb_source *source);

	/*
	 * This callback is expected to open this source's reachability bitmap
	 * (its multi-pack-index bitmap if present, else one of its pack
	 * bitmaps), filling `bitmap_git`. Bitmaps accelerate object
	 * enumeration; the files source provides one from its packfile layout,
	 * while sources without one (e.g. a helper) keep the no-op installed by
	 * odb_source_init() and report none, so the caller falls back to a full
	 * walk. Returns 0 if a bitmap was opened, a negative value otherwise.
	 */
	int (*open_bitmap)(struct odb_source *source,
			   struct bitmap_index *bitmap_git);

	/*
	 * Fetch one commit's stored reachability bitmap entry (the helper's
	 * per-commit commit_bitmap rows) for the lazy/selective serve path. On
	 * success the malloc'd EWAH bytes land in *ewah (length in *ewah_len;
	 * caller frees), *xor_base the xor-base commit oid (null_oid for a full
	 * entry), *flags the entry flags; returns 0, or -1 when the source has no
	 * entry for this commit (or no per-commit bitmaps, the no-op default).
	 */
	int (*get_commit_bitmap)(struct odb_source *source,
				 const struct object_id *commit_oid,
				 struct object_id *xor_base, int *flags,
				 void **ewah, size_t *ewah_len);

	/*
	 * On-demand bitmap orderings (the source analogue of a pack's mmap'd .idx).
	 * pos_of_oid returns the object's bit position in the source bitmap, or -1 if
	 * it is not bitmapped (the want/have lookup). resolve_bits hands the source a
	 * set of `n` bits and calls emit(oid) for each, in ascending bit order, so an
	 * operation resolves exactly the bits it touches in one batch rather than
	 * fetching all N at open. Both are no-ops (return -1) on a source without
	 * orderings.
	 */
	int (*pos_of_oid)(struct odb_source *source, const struct object_id *oid);
	int (*resolve_bits)(struct odb_source *source,
			    const uint32_t *bits, size_t n,
			    void (*emit)(const struct object_id *oid, void *data),
			    void *data);

	/*
	 * Persist this source's commit-graph generation numbers, the one datum a
	 * commit-graph holds that is not derivable from the commit objects
	 * (tree/parents/date are read back from the commits on access). Each backend
	 * does it its own way: the files source writes a commit-graph file
	 * (write_commit_graph_to_file via commit_graph_ctx_commits); a helper
	 * captures the generations into its store and writes no file (everything in
	 * the DB), or falls back to the file when it lacks the graph capability. The
	 * write path dispatches here unconditionally. Returns 0 on success, negative
	 * on error.
	 */
	int (*store_commit_graph)(struct odb_source *source,
				  struct write_commit_graph_context *ctx);

	/*
	 * Report whether this source supplies commit-graph generation numbers
	 * (a helper that stored them). When true, generation_numbers_enabled()
	 * treats the repo as having generations even with no commit-graph file,
	 * and the commit parser faults each generation via get_commit_generation.
	 * The default returns 0.
	 */
	int (*provides_commit_generations)(struct odb_source *source);

	/*
	 * Populate this source from a local source repository's objects, the
	 * fast path "git clone" takes for a local source instead of the transport.
	 * The files source hardlinks or copies the object files straight into its
	 * object directory (honoring opts: --shared records an alternate, otherwise
	 * --no-hardlinks/--local control linking) and returns 0. A source that keeps
	 * objects in its own medium (a helper) takes the default, which declines
	 * (returns -1) so the caller falls back to the transport, ingesting the
	 * objects through the protocol. This mirrors migrate_quarantine: an object
	 * population the files source does in place and others receive over the wire.
	 */
	int (*local_clone)(struct odb_source *source, const char *src_repo,
			   const struct odb_local_clone_opts *opts);

	/*
	 * Fault one commit's generation number from this source into *gen.
	 * Returns 0 on success, -1 if the source has no generation for it.
	 */
	int (*get_commit_generation)(struct odb_source *source,
				     const struct object_id *oid,
				     timestamp_t *gen);

	/*
	 * This callback is expected to report whether the given object is
	 * "kept" in this source, i.e. pinned against maintenance so that gc and
	 * repack will neither remove nor repack it. For the files source this
	 * means the object lives in a pack marked with a ".keep" file, selected
	 * by `flags` (a combination of `kept_pack_type` bits); other backends
	 * have no kept concept and report not-kept via the default installed by
	 * odb_source_init(). Returns non-zero if the object is kept, 0 otherwise.
	 */
	int (*is_object_kept)(struct odb_source *source,
			      const struct object_id *oid,
			      unsigned flags);

	/*
	 * This callback reports whether the source already holds the pack with
	 * the given wire hash, so a re-fetch can be skipped (its objects are
	 * present). The files source checks its packs by hash; a backend that
	 * does not track received-pack identity (e.g. a helper) keeps the no-op
	 * installed by odb_source_init() and reports not-held, so the caller
	 * re-fetches (ingestion is idempotent). Returns non-zero if held, 0
	 * otherwise.
	 */
	int (*has_received_pack)(struct odb_source *source,
				 const unsigned char *hash);

	/*
	 * This callback reports whether the source already preserves a copy of
	 * the given object that makes adding another cruft copy (with mtime
	 * `mtime`) unnecessary: the files source checks its kept packs selected
	 * by `flags` (a non-cruft pack preserves the object unconditionally; a
	 * cruft pack preserves it when its recorded mtime is at least `mtime`).
	 * Sources without packfiles keep the no-op installed by
	 * odb_source_init() and report 0. Returns non-zero if a surviving copy
	 * is preserved, 0 otherwise.
	 */
	int (*cruft_object_preserved)(struct odb_source *source,
				      const struct object_id *oid,
				      unsigned flags,
				      uint32_t mtime);

	/*
	 * This callback reports whether the source stores the given object as a
	 * promisor object (an object received from a promisor remote, which is
	 * therefore present and connectivity-complete by construction). The
	 * files source reports objects living in a ".promisor" pack; a helper
	 * reports objects it holds marked as promisor. The connectivity check
	 * uses this to skip a wanted ref tip that arrived this way instead of
	 * sending it to rev-list. Other backends report not-promisor via the
	 * default installed by odb_source_init(). Returns non-zero if the object
	 * is a stored promisor object, 0 otherwise.
	 */
	int (*is_promisor_object)(struct odb_source *source,
				  const struct object_id *oid);

	/*
	 * This callback is expected to read the list of alternate object
	 * database sources connected to it and write them into the `strvec`.
	 *
	 * The result is expected to be paths to the alternates. All paths must
	 * be resolved to absolute paths.
	 *
	 * The callback is expected to return 0 on success, a negative error
	 * code otherwise.
	 */
	int (*read_alternates)(struct odb_source *source,
			       struct strvec *out);

	/*
	 * This callback is expected to persist the singular alternate passed
	 * to it into its list of alternates. Any pre-existing alternates are
	 * expected to remain active. Subsequent calls to `read_alternates` are
	 * thus expected to yield the pre-existing list of alternates plus the
	 * newly added alternate appended to its end.
	 *
	 * The callback is expected to return 0 on success, a negative error
	 * code otherwise.
	 */
	int (*write_alternate)(struct odb_source *source,
			       const char *alternate);
};

/*
 * Allocate and initialize a new source for the given object database located
 * at `path`. `local` indicates whether or not the source is the local and thus
 * primary object source of the object database.
 */
struct odb_source *odb_source_new(struct object_database *odb,
				  const char *path,
				  bool local);

/*
 * Like odb_source_new(), but construct a primary source for the explicitly given
 * backend name rather than the object database's currently configured one. This
 * builds the destination store of an object-storage migration. A name with no
 * registered backend is a helper name (git-local-<name>), as in odb_source_new().
 */
struct odb_source *odb_source_new_named(struct object_database *odb,
					const char *path,
					const char *name);


/*
 * Initialize the source for the given object database located at `path`.
 * `local` indicates whether or not the source is the local and thus primary
 * object source of the object database.
 *
 * This function is only supposed to be called by specific object source
 * implementations.
 */
void odb_source_init(struct odb_source *source,
		     struct object_database *odb,
		     const char *path,
		     bool local);

/*
 * Free the object database source, releasing all associated resources and
 * freeing the structure itself.
 */
void odb_source_free(struct odb_source *source);

/*
 * Release the object database source, releasing all associated resources.
 *
 * This function is only supposed to be called by specific object source
 * implementations.
 */
void odb_source_release(struct odb_source *source);

/*
 * Close the object database source without releasing he underlying data. The
 * source can still be used going forward, but it first needs to be reopened.
 * This can be useful to reduce resource usage.
 */
static inline void odb_source_close(struct odb_source *source)
{
	source->close(source);
}

/*
 * Reprepare the object database source and clear any caches. Depending on the
 * backend used this may have the effect that concurrently-written objects
 * become visible.
 */
static inline void odb_source_reprepare(struct odb_source *source)
{
	source->reprepare(source);
}

/*
 * Read an object from the object database source identified by its object ID.
 * Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_read_object_info(struct odb_source *source,
					      const struct object_id *oid,
					      struct object_info *oi,
					      enum object_info_flags flags)
{
	return source->read_object_info(source, oid, oi, flags);
}

/*
 * Create a new read stream for the given object ID. Returns 0 on success, a
 * negative error code otherwise.
 */
static inline int odb_source_read_object_stream(struct odb_read_stream **out,
						struct odb_source *source,
						const struct object_id *oid)
{
	return source->read_object_stream(out, source, oid);
}

/*
 * Read this source's slice of the SHA-1<->SHA-256 compat object-id map into the
 * odb-level map. Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_read_compat_map(struct odb_source *source)
{
	return source->read_compat_map(source);
}

/*
 * Iterate through all objects contained in the given source and invoke the
 * callback function for each of them. Returning a non-zero code from the
 * callback function aborts iteration. There is no guarantee that objects
 * are only iterated over once.
 *
 * The optional `request` structure serves as a template for retrieving the
 * object info for each indvidual iterated object and will be populated as if
 * `odb_source_read_object_info()` was called on the object. It will not be
 * modified, the callback will instead be invoked with a separate `struct
 * object_info` for every object. Object info will not be read when passing a
 * `NULL` pointer.
 *
 * The flags is a bitfield of `ODB_FOR_EACH_OBJECT_*` flags. Not all flags may
 * apply to a specific backend, so whether or not they are honored is defined
 * by the implementation.
 *
 * Returns 0 when all objects have been iterated over, a negative error code in
 * case iteration has failed, or a non-zero value returned from the callback.
 */
static inline int odb_source_for_each_object(struct odb_source *source,
					     const struct object_info *request,
					     odb_for_each_object_cb cb,
					     void *cb_data,
					     const struct odb_for_each_object_options *opts)
{
	return source->for_each_object(source, request, cb, cb_data, opts);
}

/*
 * Count the number of objects in the given object database source.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_count_objects(struct odb_source *source,
					   enum odb_count_objects_flags flags,
					   unsigned long *out)
{
	return source->count_objects(source, flags, out);
}

/*
 * Count the source's loose (non-packed) objects. Sources with no loose tier
 * report zero. Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_count_loose_objects(struct odb_source *source,
						 enum odb_count_objects_flags flags,
						 unsigned long *out)
{
	return source->count_loose_objects(source, flags, out);
}

/*
 * Iterate the given source's loose (non-packed) objects (see the
 * for_each_loose_object callback). A source with no loose tier iterates nothing
 * (the no-op default). A non-zero callback return aborts and is returned.
 */
static inline int odb_source_for_each_loose_object(struct odb_source *source,
						   int (*obj_cb)(const struct object_id *oid,
								 const char *path, void *data),
						   int (*cruft_cb)(const char *basename,
								   const char *path, void *data),
						   int (*subdir_cb)(unsigned int nr,
								    const char *path, void *data),
						   void *data)
{
	return source->for_each_loose_object(source, obj_cb, cruft_cb, subdir_cb,
					     data);
}

/*
 * Accumulate the source's local packfile layout into `report`. Sources without
 * packfiles add nothing.
 */
static inline void odb_source_count_packs(struct odb_source *source,
					  struct odb_pack_report *report)
{
	source->count_packs(source, report);
}

/*
 * Determine the minimum required length to make the given object ID unique in
 * the given source. Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_find_abbrev_len(struct odb_source *source,
					     const struct object_id *oid,
					     unsigned min_len,
					     unsigned *out)
{
	return source->find_abbrev_len(source, oid, min_len, out);
}

/*
 * Freshen an object in the object database by updating its timestamp.
 * Returns 1 in case the object has been freshened, 0 in case the object does
 * not exist.
 */
static inline int odb_source_freshen_object(struct odb_source *source,
					    const struct object_id *oid)
{
	return source->freshen_object(source, oid);
}

/*
 * Write an object into the object database source. Returns 0 on success, a
 * negative error code otherwise. Populates the given out pointers for the
 * object ID and the compatibility object ID, if non-NULL.
 */
static inline int odb_source_write_object(struct odb_source *source,
					  const void *buf, unsigned long len,
					  enum object_type type,
					  struct object_id *oid,
					  struct object_id *compat_oid,
					  enum odb_write_object_flags flags)
{
	return source->write_object(source, buf, len, type, oid,
				    compat_oid, flags);
}

/*
 * Store an object already in git's native pack storage form (compressed entry
 * bytes verbatim). Returns -1 if the source does not implement write_prepared,
 * so the caller can fall back to a resolved write_object. See the write_prepared
 * vtable comment.
 */
static inline int odb_source_write_prepared(struct odb_source *source,
					    const struct object_id *oid,
					    const struct object_id *compat_oid,
					    enum object_type type,
					    unsigned long usize,
					    const struct object_id *base_oid,
					    const void *compressed,
					    unsigned long clen,
					    enum odb_write_object_flags flags)
{
	return source->write_prepared(source, oid, compat_oid, type, usize,
				      base_oid, compressed, clen, flags);
}


static inline int odb_source_read_object_delta(struct odb_source *source,
					       const struct object_id *oid,
					       struct object_id *base_oid,
					       void **delta,
					       unsigned long *delta_len,
					       unsigned long *raw_len)
{
	return source->read_object_delta(source, oid, base_oid, delta, delta_len,
					 raw_len);
}

/*
 * Stream the reusable pack_pos prefix [0, end_pos) for source-backed pack reuse;
 * see the stream_reuse vtable comment. end_pos is the window reuse_window granted
 * at setup. The no-op default returns -1; a source that serves a source bitmap
 * overrides it, returning the callback's outcome.
 */
static inline int odb_source_stream_reuse(struct odb_source *source,
					  uint32_t end_pos,
					  odb_reuse_emit_fn emit, void *data)
{
	return source->stream_reuse(source, end_pos, emit, data);
}

/*
 * The leading run [0, candidate) this source will bulk-reuse via stream_reuse;
 * see the reuse_window vtable comment. Returns the granted window (0 to decline);
 * the no-op default declines.
 */
static inline uint32_t odb_source_reuse_window(struct odb_source *source,
					       uint32_t candidate)
{
	return source->reuse_window(source, candidate);
}

/*
 * The multi-pack-index operations (see the vtable comment). Dispatched on the
 * source the command operates on: the files source runs its midx machinery, any
 * other backend takes the no-op default (returns 0, nothing to index).
 */
static inline int odb_source_multi_pack_index_write(struct odb_source *source,
						    struct string_list *packs_to_include,
						    const char *preferred_pack_name,
						    const char *refs_snapshot,
						    const char *incremental_base,
						    unsigned flags)
{
	return source->multi_pack_index_write(source, packs_to_include,
					      preferred_pack_name, refs_snapshot,
					      incremental_base, flags);
}

static inline int odb_source_multi_pack_index_compact(struct odb_source *source,
						      const char *from_checksum,
						      const char *to_checksum,
						      const char *incremental_base,
						      unsigned flags)
{
	return source->multi_pack_index_compact(source, from_checksum, to_checksum,
						incremental_base, flags);
}

static inline int odb_source_multi_pack_index_verify(struct odb_source *source,
						     unsigned flags)
{
	return source->multi_pack_index_verify(source, flags);
}

static inline int odb_source_multi_pack_index_expire(struct odb_source *source,
						     unsigned flags)
{
	return source->multi_pack_index_expire(source, flags);
}

static inline int odb_source_multi_pack_index_repack(struct odb_source *source,
						     size_t batch_size,
						     unsigned flags)
{
	return source->multi_pack_index_repack(source, batch_size, flags);
}

/*
 * The default read_object_delta installed by odb_source_init(): report that the
 * object is not stored as a delta (return 0). Exposed so a source can delegate
 * to it (e.g. a helper that does not advertise the get-delta capability).
 */
int odb_source_read_object_delta_generic(struct odb_source *source,
					 const struct object_id *oid,
					 struct object_id *base_oid,
					 void **delta, unsigned long *delta_len,
					 unsigned long *raw_len);

static inline int odb_source_install_loose_object(struct odb_source *source,
						   const char *temp_path,
						   const struct object_id *oid)
{
	return source->install_loose_object(source, temp_path, oid);
}

static inline void odb_source_note_received_pack(struct odb_source *source,
						 struct packed_git *p)
{
	source->note_received_pack(source, p);
}

static inline void odb_source_note_indexed_pack(struct odb_source *source,
						const char *idx_path)
{
	source->note_indexed_pack(source, idx_path);
}

/*
 * Write an object into the object database source via a stream. The overall
 * length of the object must be known in advance.
 *
 * Return 0 on success, a negative error code otherwise. Populates the given
 * out pointer for the object ID.
 */
static inline int odb_source_write_object_stream(struct odb_source *source,
						 struct odb_write_stream *stream,
						 size_t len,
						 struct object_id *oid)
{
	return source->write_object_stream(source, stream, len, oid);
}

/*
 * Read the list of alternative object database sources from the given backend
 * and populate the `strvec` with them. The listing is not recursive, that
 * is, if any of the yielded alternate sources has alternates itself, those
 * will not be yielded as part of this function call.
 *
 * Return 0 on success, a negative error code otherwise.
 */
static inline int odb_source_read_alternates(struct odb_source *source,
					     struct strvec *out)
{
	return source->read_alternates(source, out);
}

/*
 * Write and persist a new alternate object database source for the given
 * source. Any preexisting alternates are expected to stay valid, and the new
 * alternate shall be appended to the end of the list.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_write_alternate(struct odb_source *source,
					      const char *alternate)
{
	return source->write_alternate(source, alternate);
}

/*
 * Create a new transaction that can be used to write objects into a temporary
 * staging area. The objects will only be persisted when the transaction is
 * committed.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_begin_transaction(struct odb_source *source,
					       struct odb_transaction **out)
{
	return source->begin_transaction(source, out);
}

/*
 * Optimize the storage of the given source. Returns 0 on success, a negative
 * error code otherwise.
 */
static inline int odb_source_optimize(struct odb_source *source,
				      struct odb_optimize_opts *opts)
{
	return source->optimize(source, opts);
}

/*
 * Report via `*required` whether the given source would benefit from
 * optimization. Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_optimize_required(struct odb_source *source,
					       struct odb_optimize_opts *opts,
					       bool *required)
{
	return source->optimize_required(source, opts, required);
}

/*
 * Prune the given unreachable objects from the source, deleting those created
 * at or before `expire`. Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_remove_objects(struct odb_source *source,
					    struct oidset *prune,
					    timestamp_t expire,
					    int dry_run, struct oidset *removed)
{
	return source->remove_objects(source, prune, expire, dry_run, removed);
}

/*
 * Tidy the given source's storage after a prune (see the prune_cruft callback).
 * A source with no cruft concept uses the no-op default. Returns 0 on success,
 * a negative error code otherwise.
 */
static inline int odb_source_prune_cruft(struct odb_source *source,
					 timestamp_t expire,
					 int dry_run, int verbose)
{
	return source->prune_cruft(source, expire, dry_run, verbose);
}

/*
 * Verify the integrity of the objects stored in the given source, handing each
 * enumerated object's contents to `cb` for content checking. Returns 0 on
 * success, a negative error code otherwise.
 */
static inline int odb_source_verify(struct odb_source *source,
				    struct fsck_options *o,
				    odb_verify_cb cb, void *cb_data)
{
	return source->verify(source, o, cb, cb_data);
}

/*
 * Open this source's reachability bitmap into `bitmap_git`. Returns 0 if one
 * was opened, a negative value when the source has none.
 */
static inline int odb_source_open_bitmap(struct odb_source *source,
					 struct bitmap_index *bitmap_git)
{
	return source->open_bitmap(source, bitmap_git);
}

static inline int odb_source_get_commit_bitmap(struct odb_source *source,
					       const struct object_id *commit_oid,
					       struct object_id *xor_base,
					       int *flags, void **ewah,
					       size_t *ewah_len)
{
	return source->get_commit_bitmap(source, commit_oid, xor_base, flags,
					 ewah, ewah_len);
}

static inline int odb_source_pos_of_oid(struct odb_source *source,
					const struct object_id *oid)
{
	return source->pos_of_oid(source, oid);
}

static inline int odb_source_resolve_bits(struct odb_source *source,
					  const uint32_t *bits, size_t n,
					  void (*emit)(const struct object_id *oid,
						       void *data),
					  void *data)
{
	return source->resolve_bits(source, bits, n, emit, data);
}

/*
 * Hand the source its commit-graph generations. Returns 0 if the source stored
 * them (the caller writes no commit-graph file), a positive value if the source
 * does not store commit-graphs (the caller writes the file as usual), or a
 * negative error code.
 */
static inline int odb_source_store_commit_graph(struct odb_source *source,
						struct write_commit_graph_context *ctx)
{
	return source->store_commit_graph(source, ctx);
}

static inline int odb_source_provides_commit_generations(struct odb_source *source)
{
	return source->provides_commit_generations(source);
}

static inline int odb_source_local_clone(struct odb_source *source,
					 const char *src_repo,
					 const struct odb_local_clone_opts *opts)
{
	return source->local_clone(source, src_repo, opts);
}

static inline int odb_source_get_commit_generation(struct odb_source *source,
						   const struct object_id *oid,
						   timestamp_t *gen)
{
	return source->get_commit_generation(source, oid, gen);
}

/*
 * Report whether the given object is kept (pinned against maintenance) in the
 * given source, selected by `flags` (a combination of `kept_pack_type` bits).
 * Returns non-zero if kept, 0 otherwise.
 */
static inline int odb_source_is_object_kept(struct odb_source *source,
					    const struct object_id *oid,
					    unsigned flags)
{
	return source->is_object_kept(source, oid, flags);
}

/*
 * Report whether the source already holds the pack with the given wire hash
 * (so a re-fetch can be skipped). Returns non-zero if held, 0 otherwise.
 */
static inline int odb_source_has_received_pack(struct odb_source *source,
					       const unsigned char *hash)
{
	return source->has_received_pack(source, hash);
}

/*
 * Report whether the given source already preserves a surviving copy of `oid`
 * (in a non-cruft kept pack, or a cruft pack whose recorded mtime is at least
 * `mtime`), among the kept packs selected by `flags`. Returns non-zero if so,
 * 0 otherwise.
 */
static inline int odb_source_cruft_object_preserved(struct odb_source *source,
						     const struct object_id *oid,
						     unsigned flags,
						     uint32_t mtime)
{
	return source->cruft_object_preserved(source, oid, flags, mtime);
}

/*
 * Report whether the given source stores `oid` as a promisor object (received
 * from a promisor remote). Returns non-zero if so, 0 otherwise.
 */
static inline int odb_source_is_promisor_object(struct odb_source *source,
						const struct object_id *oid)
{
	return source->is_promisor_object(source, oid);
}

/*
 * Mark the given objects as promisor objects in the source's native way (see
 * the mark_objects_promisor callback). A source with no promisor concept uses
 * the no-op default. Returns 0 on success, a negative error code otherwise.
 */
static inline int odb_source_mark_objects_promisor(struct odb_source *source,
						   struct oidset *oids)
{
	return source->mark_objects_promisor(source, oids);
}

/*
 * Record the just-received `pack`'s objects as members of a kept pack (see the
 * keep_pack callback). The files source uses the no-op default. Returns 0 on
 * success, a negative error code otherwise.
 */
static inline int odb_source_keep_pack(struct odb_source *source,
				       const struct odb_received_pack *pack)
{
	return source->keep_pack(source, pack);
}

#endif
