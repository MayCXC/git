#include "git-compat-util.h"
#include "gettext.h"
#include "config.h"
#include "delta.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "odb/pack-ingest.h"
#include "odb/source-files.h"
#include "odb/source.h"
#include "packfile.h"
#include "repository.h"
#include "strbuf.h"

struct odb_source *odb_source_new(struct object_database *odb,
				  const char *path,
				  bool local)
{
	return &odb_source_files_new(odb, path, local)->base;
}


static int odb_source_optimize_noop(struct odb_source *source UNUSED,
				    struct odb_optimize_opts *opts UNUSED)
{
	return 0;
}

static int odb_source_optimize_required_noop(struct odb_source *source UNUSED,
					     struct odb_optimize_opts *opts UNUSED,
					     bool *required)
{
	*required = false;
	return 0;
}

static int odb_source_verify_noop(struct odb_source *source UNUSED,
				  struct fsck_options *o UNUSED,
				  odb_verify_cb cb UNUSED,
				  void *cb_data UNUSED)
{
	return 0;
}

static int odb_source_read_compat_map_noop(struct odb_source *source UNUSED)
{
	return 0;
}

static int odb_source_remove_objects_noop(struct odb_source *source UNUSED,
					  struct oidset *prune UNUSED,
					  timestamp_t expire UNUSED,
					  int dry_run UNUSED,
					  struct oidset *removed UNUSED)
{
	return 0;
}

static int odb_source_remove_storage_noop(struct odb_source *source UNUSED)
{
	return 0;
}

static int odb_source_prune_cruft_noop(struct odb_source *source UNUSED,
				       timestamp_t expire UNUSED,
				       int dry_run UNUSED, int verbose UNUSED)
{
	return 0;
}

static int odb_source_is_object_kept_noop(struct odb_source *source UNUSED,
					  const struct object_id *oid UNUSED,
					  unsigned flags UNUSED)
{
	return 0;
}

static int odb_source_open_bitmap_noop(struct odb_source *source UNUSED,
				       struct bitmap_index *bitmap_git UNUSED)
{
	return -1;
}

static int odb_source_get_commit_bitmap_noop(struct odb_source *source UNUSED,
					     const struct object_id *commit_oid UNUSED,
					     struct object_id *xor_base UNUSED,
					     int *flags UNUSED, void **ewah UNUSED,
					     size_t *ewah_len UNUSED)
{
	return -1;
}

static int odb_source_pos_of_oid_noop(struct odb_source *source UNUSED,
				      const struct object_id *oid UNUSED)
{
	return -1;
}

static int odb_source_resolve_bits_noop(struct odb_source *source UNUSED,
					const uint32_t *bits UNUSED, size_t n UNUSED,
					void (*emit)(const struct object_id *, void *) UNUSED,
					void *data UNUSED)
{
	return -1;
}

static int odb_source_store_commit_graph_noop(struct odb_source *source UNUSED,
					      struct write_commit_graph_context *ctx UNUSED)
{
	return 0;	/* a source that persists no generations has no commit-graph */
}

static int odb_source_provides_commit_generations_noop(struct odb_source *source UNUSED)
{
	return 0;
}

static int odb_source_get_commit_generation_noop(struct odb_source *source UNUSED,
						 const struct object_id *oid UNUSED,
						 timestamp_t *gen UNUSED)
{
	return -1;
}

static int odb_source_migrate_quarantine_noop(struct odb_source *source UNUSED,
					      const char *quarantine_path UNUSED)
{
	return 0;	/* a source with no separate medium needs no migration */
}

static int odb_source_has_received_pack_noop(struct odb_source *source UNUSED,
					     const unsigned char *hash UNUSED)
{
	return 0;
}

static int odb_source_mark_objects_promisor_noop(struct odb_source *source UNUSED,
						 struct oidset *oids UNUSED)
{
	return 0;
}

static int odb_source_keep_pack_noop(struct odb_source *source UNUSED,
				     const struct odb_received_pack *pack UNUSED)
{
	return 0;
}

static int odb_source_cruft_object_preserved_noop(struct odb_source *source UNUSED,
						  const struct object_id *oid UNUSED,
						  unsigned flags UNUSED,
						  uint32_t mtime UNUSED)
{
	return 0;
}

static int odb_source_is_promisor_object_noop(struct odb_source *source UNUSED,
					      const struct object_id *oid UNUSED)
{
	return 0;
}

static void odb_source_count_packs_noop(struct odb_source *source UNUSED,
					struct odb_pack_report *report UNUSED)
{
}

static int odb_source_count_loose_objects_noop(struct odb_source *source UNUSED,
					       enum odb_count_objects_flags flags UNUSED,
					       unsigned long *out)
{
	*out = 0;
	return 0;
}

static int odb_source_for_each_loose_object_noop(struct odb_source *source UNUSED,
						 each_loose_object_fn obj_cb UNUSED,
						 each_loose_cruft_fn cruft_cb UNUSED,
						 each_loose_subdir_fn subdir_cb UNUSED,
						 void *data UNUSED)
{
	return 0;
}

static int odb_source_write_prepared_noop(struct odb_source *source UNUSED,
					  const struct object_id *oid UNUSED,
					  const struct object_id *compat_oid UNUSED,
					  enum object_type type UNUSED,
					  unsigned long usize UNUSED,
					  const struct object_id *base_oid UNUSED,
					  const void *compressed UNUSED,
					  unsigned long clen UNUSED,
					  enum odb_write_object_flags flags UNUSED)
{
	return -1;
}

static int odb_source_stream_reuse_noop(struct odb_source *source UNUSED,
					uint32_t end_pos UNUSED,
					odb_reuse_emit_fn emit UNUSED,
					void *data UNUSED)
{
	return -1;
}

static uint32_t odb_source_reuse_window_noop(struct odb_source *source UNUSED,
					     uint32_t candidate UNUSED)
{
	return 0;
}

static int odb_source_local_clone_noop(struct odb_source *source UNUSED,
				       const char *src_repo UNUSED,
				       const struct odb_local_clone_opts *opts UNUSED)
{
	return -1;	/* decline: the caller falls back to the transport */
}

/*
 * No-op multi-pack-index defaults: a source with no packfile layout has nothing
 * to index, so each operation succeeds doing nothing. The files source overrides
 * them with its midx machinery.
 */
static int odb_source_multi_pack_index_write_noop(struct odb_source *source UNUSED,
						  struct string_list *packs_to_include UNUSED,
						  const char *preferred_pack_name UNUSED,
						  const char *refs_snapshot UNUSED,
						  const char *incremental_base UNUSED,
						  unsigned flags UNUSED)
{
	return 0;
}

static int odb_source_multi_pack_index_compact_noop(struct odb_source *source UNUSED,
						    const char *from_checksum UNUSED,
						    const char *to_checksum UNUSED,
						    const char *incremental_base UNUSED,
						    unsigned flags UNUSED)
{
	return 0;
}

static int odb_source_multi_pack_index_verify_noop(struct odb_source *source UNUSED,
						   unsigned flags UNUSED)
{
	return 0;
}

static int odb_source_multi_pack_index_expire_noop(struct odb_source *source UNUSED,
						   unsigned flags UNUSED)
{
	return 0;
}

static int odb_source_multi_pack_index_repack_noop(struct odb_source *source UNUSED,
						   size_t batch_size UNUSED,
						   unsigned flags UNUSED)
{
	return 0;
}

/*
 * Default source-list install: the constructed primary is the sole source. A
 * backend that needs receive-time quarantine staging (the helper) overrides
 * this to put a files quarantine in front of itself.
 */
static void odb_source_prepare_source_list_default(struct odb_source *source,
						   struct object_database *odb)
{
	odb->sources = source;
	odb->sources_tail = &source->next;
}

/*
 * Generic install_loose_object: inflate the downloaded loose tempfile and
 * write it through the source's own object-write path. The files source
 * overrides this with a zero-copy rename into its loose layout.
 */
static int odb_install_loose_object_generic(struct odb_source *source,
					    const char *temp_path,
					    const struct object_id *oid)
{
	struct strbuf compressed = STRBUF_INIT;
	struct object_info oi = OBJECT_INFO_INIT;
	enum object_type type;
	unsigned long size;
	git_zstream stream;
	char hdr[MAX_HEADER_LEN];
	struct object_id written;
	void *content;
	int fd, ret = -1;

	fd = open(temp_path, O_RDONLY);
	if (fd < 0)
		return error_errno(_("unable to open %s"), temp_path);
	if (strbuf_read(&compressed, fd, 0) < 0) {
		error_errno(_("unable to read %s"), temp_path);
		close(fd);
		strbuf_release(&compressed);
		return -1;
	}
	close(fd);

	oi.typep = &type;
	oi.sizep = &size;
	if (unpack_loose_header(&stream, (unsigned char *)compressed.buf,
				compressed.len, hdr, sizeof(hdr)) != ULHR_OK ||
	    parse_loose_header(hdr, &oi) < 0 || type < 0) {
		/* unpack_loose_header initializes the stream even when it fails,
		 * so release it on the error path (object-file.c out_inflate). */
		git_inflate_end(&stream);
		strbuf_release(&compressed);
		return error(_("unable to parse loose object %s"), temp_path);
	}
	content = unpack_loose_rest(&stream, hdr, size, oid);
	git_inflate_end(&stream);
	strbuf_release(&compressed);
	if (!content)
		return error(_("unable to inflate %s"), temp_path);

	ret = odb_source_write_object(source, content, size, type, &written,
				      NULL, ODB_WRITE_OBJECT_PERSIST);
	free(content);
	if (!ret)
		unlink(temp_path);
	return ret;
}

/*
 * Generic note_received_pack: index-pack has already ingested the objects into
 * this source, so the files-only pack descriptor does not apply here. Discard
 * it and reprepare so the newly ingested objects become visible. The files
 * source overrides this to register the on-disk pack instead.
 */
static void odb_note_received_pack_generic(struct odb_source *source,
					   struct packed_git *p)
{
	close_pack(p);
	free(p);
	odb_source_reprepare(source);
}

/*
 * Generic note_indexed_pack: there is no on-disk pack to register (the objects
 * were ingested through this source's pack-ingest path), so reprepare to make
 * them visible. The files source overrides this to load the on-disk pack.
 */
static void odb_note_indexed_pack_generic(struct odb_source *source,
					  const char *idx_path UNUSED)
{
	odb_source_reprepare(source);
}

/*
 * Generic read_object_delta: report that the source does not store the object
 * as a delta (return 0). Correct for any source that resolves objects on read;
 * a helper overrides it to return its stored delta (get-delta).
 */
int odb_source_read_object_delta_generic(struct odb_source *source UNUSED,
					 const struct object_id *oid UNUSED,
					 struct object_id *base_oid UNUSED,
					 void **delta UNUSED,
					 unsigned long *delta_len UNUSED,
					 unsigned long *raw_len UNUSED)
{
	return 0;
}

void odb_source_init(struct odb_source *source,
		     struct object_database *odb,
		     const char *path,
		     bool local)
{
	source->odb = odb;
	source->local = local;
	source->path = xstrdup(path);
	/* Generic by default; the files source overrides to keep the pack. */
	source->begin_pack_ingest = odb_pack_ingest_begin_generic;
	/*
	 * Generic by default (inflate the loose tempfile and write it through
	 * the source); the files source overrides with a zero-copy rename.
	 */
	source->install_loose_object = odb_install_loose_object_generic;
	/*
	 * Generic by default (discard the files-only descriptor + reprepare);
	 * the files source overrides to register the on-disk pack.
	 */
	source->note_received_pack = odb_note_received_pack_generic;
	source->note_indexed_pack = odb_note_indexed_pack_generic;
	/*
	 * Report "not a stored delta" by default; a helper overrides it to return
	 * its stored compressed delta (get-delta) so the send path reuses it.
	 */
	source->read_object_delta = odb_source_read_object_delta_generic;
	/* No prepared-bytes store by default (-1: the caller writes the resolved
	 * object); the files packfile path and a helper override it. */
	source->write_prepared = odb_source_write_prepared_noop;
	/* No bulk pack reuse by default (-1: the caller emits each object); a source
	 * that serves a source bitmap overrides it. */
	source->stream_reuse = odb_source_stream_reuse_noop;
	/* Decline reuse by default (0: the caller emits each object); a source that
	 * serves a source bitmap overrides it to grant the streamable window. */
	source->reuse_window = odb_source_reuse_window_noop;
	/* No packfile layout by default, so the multi-pack-index operations no-op
	 * (nothing to index); the files source overrides them with its midx machinery. */
	source->multi_pack_index_write = odb_source_multi_pack_index_write_noop;
	source->multi_pack_index_compact = odb_source_multi_pack_index_compact_noop;
	source->multi_pack_index_verify = odb_source_multi_pack_index_verify_noop;
	source->multi_pack_index_expire = odb_source_multi_pack_index_expire_noop;
	source->multi_pack_index_repack = odb_source_multi_pack_index_repack_noop;
	/* No-op by default; sources with storage to optimize override these. */
	source->optimize = odb_source_optimize_noop;
	source->optimize_required = odb_source_optimize_required_noop;
	/* No-op by default; sources with a format to verify override this. */
	source->verify = odb_source_verify_noop;
	/* No persistent compat map by default; files and helper sources read theirs. */
	source->read_compat_map = odb_source_read_compat_map_noop;
	/* No-op prune by default; sources that prune their storage override this. */
	source->remove_objects = odb_source_remove_objects_noop;
	/* No cruft to tidy by default; only the files source has a loose/pack
	 * layout that accumulates cruft after a prune. */
	source->prune_cruft = odb_source_prune_cruft_noop;
	/* No separable on-disk store to remove by default; the files source
	 * overrides this to delete its loose objects, packs and commit-graph. */
	source->remove_storage = odb_source_remove_storage_noop;
	/* No reachability bitmap by default; the files source provides one. */
	source->open_bitmap = odb_source_open_bitmap_noop;
	source->get_commit_bitmap = odb_source_get_commit_bitmap_noop;
	source->pos_of_oid = odb_source_pos_of_oid_noop;
	source->resolve_bits = odb_source_resolve_bits_noop;
	source->store_commit_graph = odb_source_store_commit_graph_noop;
	source->provides_commit_generations = odb_source_provides_commit_generations_noop;
	/* No filesystem object files to hardlink/copy by default, so a local clone
	 * declines (the caller uses the transport); the files source overrides it. */
	source->local_clone = odb_source_local_clone_noop;
	source->prepare_source_list = odb_source_prepare_source_list_default;
	source->get_commit_generation = odb_source_get_commit_generation_noop;
	source->migrate_quarantine = odb_source_migrate_quarantine_noop;
	/* No received-pack tracking by default; only the files source has packs. */
	source->has_received_pack = odb_source_has_received_pack_noop;
	/* Not-kept by default; only the files source has kept packs. */
	source->is_object_kept = odb_source_is_object_kept_noop;
	/* No cruft kept packs by default; only the files source has them. */
	source->cruft_object_preserved = odb_source_cruft_object_preserved_noop;
	/* Not-promisor by default; sources that store promisor objects override. */
	source->is_promisor_object = odb_source_is_promisor_object_noop;
	/* No promisor marking by default; files and helper sources override. */
	source->mark_objects_promisor = odb_source_mark_objects_promisor_noop;
	/* No relational keep membership by default; the files source keeps the
	 * pack on disk, so only a pack-exploding source (the helper) overrides. */
	source->keep_pack = odb_source_keep_pack_noop;
	/* No packfiles by default; only the files source has a pack layout. */
	source->count_packs = odb_source_count_packs_noop;
	/* No loose tier by default; only the files source has loose objects. */
	source->count_loose_objects = odb_source_count_loose_objects_noop;
	/* No loose tier to iterate by default; only the files source has one. */
	source->for_each_loose_object = odb_source_for_each_loose_object_noop;
}

void odb_source_free(struct odb_source *source)
{
	if (!source)
		return;
	source->free(source);
}

void odb_source_release(struct odb_source *source)
{
	if (!source)
		return;
	free(source->path);
}
