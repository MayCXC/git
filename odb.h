#ifndef ODB_H
#define ODB_H

#include "object.h"
#include "oidset.h"
#include "oidmap.h"
#include "string-list.h"
#include "thread-utils.h"

struct cached_object_entry;
struct list_objects_filter_options;
struct loose_object_map;
struct odb_source_files;
struct odb_source_inmemory;
struct packed_git;
struct repository;
struct strbuf;
struct strvec;

/*
 * Set this to 0 to prevent odb_read_object_info_extended() from fetching missing
 * blobs. This has a difference only if extensions.partialClone is set.
 *
 * Its default value is 1.
 */
extern int fetch_if_missing;

/*
 * Compute the exact path an alternate is at and returns it. In case of
 * error NULL is returned and the human readable error is added to `err`
 * `path` may be relative and should point to $GIT_DIR.
 * `err` must not be null.
 */
char *compute_alternate_path(const char *path, struct strbuf *err);

/*
 * The object database encapsulates access to objects in a repository. It
 * manages one or more sources that store the actual objects which are
 * configured via alternates.
 */
struct object_database {
	/* Repository that owns this database. */
	struct repository *repo;

	/*
	 * State of current current object database transaction. Only one
	 * transaction may be pending at a time. Is NULL when no transaction is
	 * configured.
	 */
	struct odb_transaction *transaction;

	/*
	 * Set of all object directories; the main directory is first (and
	 * cannot be NULL after initialization). Subsequent directories are
	 * alternates.
	 */
	struct odb_source *sources;
	struct odb_source **sources_tail;
	struct kh_odb_path_map *source_by_path;

	/*
	 * The subset of `sources` that store objects in packfiles: the files
	 * sources (the primary when it uses the files backend, plus every
	 * alternate, which are always files). It is maintained as files sources
	 * are created and freed, so the decision of what is a files source stays
	 * confined to the odb_source_new() factory; packfile machinery walks
	 * this list and downcasts each member unconditionally, with no runtime
	 * branch on the source type. Threaded via odb_source_files.next_files.
	 */
	struct odb_source_files *files_sources;
	struct odb_source_files **files_sources_tail;

	/*
	 * The path to the main object directory as reported to the user (e.g.
	 * via repo_get_object_directory()). It is set up front from the primary
	 * source location and stays valid independently of which backend the
	 * primary object source uses, so the reported path is available before
	 * (and without) the primary source being created. May be relative, in
	 * which case it is reparented when the working directory changes.
	 *
	 * It is the canonical main object directory and does not follow a
	 * temporary primary swap (odb_set_temporary_primary_source(), used by
	 * tmp-objdir for quarantine): a quarantine redirects writes through the
	 * swapped primary source and exposes its directory via GIT_OBJECT_DIRECTORY,
	 * but the repository's reported object directory stays the canonical one.
	 */
	char *object_dir;

	/*
	 * When the repository tracks a compatibility hash algorithm
	 * (extensions.compatObjectFormat), this maps each object's storage id
	 * to its equivalent under the compat algorithm and back. It is owned by
	 * the object database, not by any one source: a compat object id is a
	 * property of the object's bytes, and git (never a storage backend)
	 * computes and records it. The on-disk form is a "loose-object-idx"
	 * file per object directory (read from the primary object_dir and every
	 * alternate, appended to at object_dir as objects are written through
	 * any backend). NULL until first use. See loose.c.
	 */
	struct loose_object_map *compat_map;

	int loaded_alternates;

	/*
	 * A list of alternate object directories loaded from the environment;
	 * this should not generally need to be accessed directly, but will
	 * populate the "sources" list when odb_prepare_alternates() is run.
	 */
	char *alternate_db;

	/*
	 * Objects that should be substituted by other objects
	 * (see git-replace(1)).
	 */
	struct oidmap replace_map;
	unsigned replace_map_initialized : 1;
	pthread_mutex_t replace_mutex; /* protect object replace functions */

	struct commit_graph *commit_graph;
	unsigned commit_graph_attempted : 1; /* if loading has been attempted */

	/*
	 * This is meant to hold a *small* number of objects that you would
	 * want odb_read_object() to be able to return, but yet you do not want
	 * to write them into the object store (e.g. a browse-only
	 * application).
	 */
	struct odb_source *inmemory_objects;

	/*
	 * A fast, rough count of the number of objects in the repository.
	 * These two fields are not meant for direct access. Use
	 * odb_count_objects() instead.
	 */
	unsigned long object_count;
	unsigned object_count_flags;
	unsigned object_count_valid : 1;

	/*
	 * Submodule source paths that will be added as additional sources to
	 * allow lookup of submodule objects via the main object database.
	 */
	struct string_list submodule_source_paths;
};

/*
 * Create a new object database for the given repository.
 *
 * If the primary source parameter is set it will override the usual primary
 * object directory derived from the repository's common directory. The
 * alternate sources are expected to be a PATH_SEP-separated list of secondary
 * sources. Note that these alternate sources will be added in addition to, not
 * instead of, the alternates identified by the primary source.
 *
 * Returns the newly created object database.
 */
struct object_database *odb_new(struct repository *repo,
				const char *primary_source,
				const char *alternate_sources);

/* Free the object database and release all resources. */
void odb_free(struct object_database *o);

/*
 * Close the object database and all of its sources so that any held resources
 * will be released. The database can still be used after closing it, in which
 * case these resources may be reallocated.
 */
void odb_close(struct object_database *o);

/*
 * Clear caches, reload alternates and then reload object sources so that new
 * objects may become accessible.
 */
void odb_reprepare(struct object_database *o);

/*
 * Find source by its object directory path. Returns a `NULL` pointer in case
 * the source could not be found.
 */
struct odb_source *odb_find_source(struct object_database *odb, const char *obj_dir);

/* Same as `odb_find_source()`, but dies in case the source doesn't exist. */
struct odb_source *odb_find_source_or_die(struct object_database *odb, const char *obj_dir);

/*
 * Replace the current writable object directory with the specified temporary
 * object directory; returns the former primary source.
 */
struct odb_source *odb_set_temporary_primary_source(struct object_database *odb,
						    const char *dir, int will_destroy);

/*
 * Restore the primary source that was previously replaced by
 * `odb_set_temporary_primary_source()`.
 */
void odb_restore_primary_source(struct object_database *odb,
				struct odb_source *restore_source,
				const char *old_path);

/*
 * Call odb_add_submodule_source_by_path() to add the submodule at the given
 * path to a list. The object stores of all submodules in that list will be
 * added as additional sources in the object store when looking up objects.
 */
void odb_add_submodule_source_by_path(struct object_database *odb,
				      const char *path);

/*
 * Iterate through all alternates of the database and execute the provided
 * callback function for each of them. Stop iterating once the callback
 * function returns a non-zero value, in which case the value is bubbled up
 * from the callback.
 */
typedef int odb_for_each_alternate_fn(struct odb_source *, void *);
int odb_for_each_alternate(struct object_database *odb,
			   odb_for_each_alternate_fn cb, void *payload);

/*
 * Iterate through all alternates of the database and yield their respective
 * references.
 */
typedef void odb_for_each_alternate_ref_fn(const struct object_id *oid, void *);
void odb_for_each_alternate_ref(struct object_database *odb,
				odb_for_each_alternate_ref_fn cb, void *payload);

/*
 * Create a temporary file rooted in the primary alternate's directory, or die
 * on failure. The filename is taken from "pattern", which should have the
 * usual "XXXXXX" trailer, and the resulting filename is written into the
 * "template" buffer. Returns the open descriptor.
 */
int odb_mkstemp(struct object_database *odb,
		struct strbuf *temp_filename, const char *pattern);

/*
 * Ensure the primary object source has been created. The primary source is
 * created lazily because its backend type may be selected from configuration
 * that is not yet available when the object database is first set up.
 */
void odb_prepare_sources(struct object_database *odb);

/*
 * Return the primary object source, creating it on first use. This is the
 * canonical accessor for the primary (and thus for the head of the source
 * list, whose remaining entries are the alternates); it guarantees the
 * primary has been prepared. Mirrors get_main_ref_store() for the ref stores.
 */
struct odb_source *odb_primary_source(struct object_database *odb);

/*
 * Prepare alternate object sources for the given database by reading
 * "objects/info/alternates" and opening the respective sources.
 */
void odb_prepare_alternates(struct object_database *odb);

/*
 * Check whether the object database has any alternates. The primary object
 * source does not count as alternate.
 */
int odb_has_alternates(struct object_database *odb);

/*
 * Add the directory to the on-disk alternates file; the new entry will also
 * take effect in the current process.
 */
void odb_add_to_alternates_file(struct object_database *odb,
				const char *dir);

/*
 * Add the directory to the in-memory list of alternate sources (along with any
 * recursive alternates it points to), but do not modify the on-disk alternates
 * file.
 */
struct odb_source *odb_add_to_alternates_memory(struct object_database *odb,
						const char *dir);

/*
 * Return the source already registered for the given object directory path, or
 * NULL if none is. Matches the path as registered (the same string passed when
 * the source was added), not by filesystem normalization.
 */
struct odb_source *odb_find_source_by_path(struct object_database *odb,
					   const char *path);

/*
 * Read an object from the database. Returns the object data and assigns object
 * type and size to the `type` and `size` pointers, if these pointers are
 * non-NULL. Returns a `NULL` pointer in case the object does not exist.
 *
 * This function dies on corrupt objects; the callers who want to deal with
 * them should arrange to call odb_read_object_info_extended() and give error
 * messages themselves.
 */
void *odb_read_object(struct object_database *odb,
		      const struct object_id *oid,
		      enum object_type *type,
		      unsigned long *size);

void *odb_read_object_peeled(struct object_database *odb,
			     const struct object_id *oid,
			     enum object_type required_type,
			     unsigned long *size,
			     struct object_id *oid_ret);

/*
 * If any source stores `oid` as a git-format delta, return its base oid, the
 * COMPRESSED delta bytes (the caller owns `*delta` and frees it), the compressed
 * length in `*delta_len`, and the uncompressed delta length in `*raw_len` --
 * without resolving or inflating. The read companion to a source's write_prepared:
 * the pack-objects send path reuses the compressed delta verbatim (its
 * z_delta_size cached-delta path) instead of recomputing it, and a push migrate
 * copies it straight in. Returns 1 when a stored delta was found, 0 when no source
 * stores the object as a delta (resolved, or absent here), a negative value on
 * error.
 */
int odb_read_object_delta(struct object_database *odb,
			  const struct object_id *oid,
			  struct object_id *base_oid,
			  void **delta, unsigned long *delta_len,
			  unsigned long *raw_len);

/*
 * Add an object file to the in-memory object store, without writing it
 * to disk.
 *
 * Callers are responsible for calling write_object_file to record the
 * object in persistent storage before writing any other new objects
 * that reference it.
 */
int odb_pretend_object(struct object_database *odb,
		       void *buf, unsigned long len, enum object_type type,
		       struct object_id *oid);

struct object_info {
	/* Request */
	enum object_type *typep;
	unsigned long *sizep;
	off_t *disk_sizep;
	struct object_id *delta_base_oid;
	/*
	 * When the object is stored as a delta against delta_base_oid and the
	 * source knows the raw (uncompressed) git-format delta length, it is
	 * reported here: the companion to delta_base_oid that lets a caller set
	 * up reuse of a stored delta (the pack-objects send path) without first
	 * reading the delta bytes. Left untouched by sources that do not store
	 * deltas individually (the files packfile path reports delta bases
	 * through whence/u.packed instead).
	 */
	unsigned long *delta_size;
	void **contentp;

	/*
	 * The time the given looked-up object has been last modified.
	 *
	 * Note: the mtime may be ambiguous in case the object exists multiple
	 * times in the object database. It is thus _not_ recommended to use
	 * this field outside of contexts where you would read every instance
	 * of the object, like for example with `odb_for_each_object()`. As it
	 * is impossible to say at the ODB level what the intent of the caller
	 * is (e.g. whether to find the oldest or newest object), it is the
	 * responsibility of the caller to disambiguate the mtimes.
	 */
	time_t *mtimep;

	/* Response */
	enum {
		OI_CACHED,
		OI_LOOSE,
		OI_PACKED,
	} whence;
	union {
		/*
		 * struct {
		 * 	... Nothing to expose in this case
		 * } cached;
		 * struct {
		 * 	... Nothing to expose in this case
		 * } loose;
		 */
		struct {
			struct packed_git *pack;
			off_t offset;
			enum packed_object_type {
				PACKED_OBJECT_TYPE_UNKNOWN,
				PACKED_OBJECT_TYPE_FULL,
				PACKED_OBJECT_TYPE_OFS_DELTA,
				PACKED_OBJECT_TYPE_REF_DELTA,
			} type;
		} packed;
	} u;
};

/*
 * Initializer for a "struct object_info" that wants no items. You may
 * also memset() the memory to all-zeroes.
 */
#define OBJECT_INFO_INIT { 0 }

/* Flags that can be passed to `odb_read_object_info_extended()`. */
enum object_info_flags {
	/* Invoke lookup_replace_object() on the given hash. */
	OBJECT_INFO_LOOKUP_REPLACE = (1 << 0),

	/* Do not reprepare object sources when the first lookup has failed. */
	OBJECT_INFO_QUICK = (1 << 1),

	/*
	 * Do not attempt to fetch the object if missing (even if fetch_is_missing is
	 * nonzero).
	 */
	OBJECT_INFO_SKIP_FETCH_OBJECT = (1 << 2),

	/* Die if object corruption (not just an object being missing) was detected. */
	OBJECT_INFO_DIE_IF_CORRUPT = (1 << 3),

	/*
	 * We have already tried reading the object, but it couldn't be found
	 * via any of the attached sources, and are now doing a second read.
	 * This second read asks the individual sources to also evaluate
	 * whether any on-disk state may have changed that may have caused the
	 * object to appear.
	 *
	 * This flag is for internal use, only. The second read only occurs
	 * when `OBJECT_INFO_QUICK` was not passed.
	 */
	OBJECT_INFO_SECOND_READ = (1 << 4),

	/*
	 * Skip the in-memory source, which holds transient objects and
	 * synthesizes the well-known empty tree and empty blob. Those are never
	 * stored on disk, so a caller asking about an object's actual storage
	 * (e.g. has_object_pack(), via object_info.whence) must consult only the
	 * real sources; otherwise the synthesized answer would mask, say, an
	 * empty tree that genuinely lives in a pack.
	 */
	OBJECT_INFO_SKIP_CACHED = (1 << 5),

	/*
	 * This is meant for bulk prefetching of missing blobs in a partial
	 * clone. Implies OBJECT_INFO_SKIP_FETCH_OBJECT and OBJECT_INFO_QUICK.
	 */
	OBJECT_INFO_FOR_PREFETCH = (OBJECT_INFO_SKIP_FETCH_OBJECT | OBJECT_INFO_QUICK),
};

/*
 * Read object info from the object database and populate the `object_info`
 * structure. Returns 0 on success, a negative error code otherwise.
 */
int odb_read_object_info_extended(struct object_database *odb,
				  const struct object_id *oid,
				  struct object_info *oi,
				  enum object_info_flags flags);

/*
 * Read a subset of object info for the given object ID. Returns an `enum
 * object_type` on success, a negative error code otherwise. If successful and
 * `sizep` is non-NULL, then the size of the object will be written to the
 * pointer.
 */
int odb_read_object_info(struct object_database *odb,
			 const struct object_id *oid,
			 unsigned long *sizep);

enum odb_has_object_flags {
	/* Retry packed storage after checking packed and loose storage */
	ODB_HAS_OBJECT_RECHECK_PACKED = (1 << 0),
	/* Allow fetching the object in case the repository has a promisor remote. */
	ODB_HAS_OBJECT_FETCH_PROMISOR = (1 << 1),
};

/*
 * Returns 1 if the object exists. This function will not lazily fetch objects
 * in a partial clone by default.
 */
int odb_has_object(struct object_database *odb,
		   const struct object_id *oid,
		   enum odb_has_object_flags flags);

int odb_freshen_object(struct object_database *odb,
		       const struct object_id *oid);

void odb_assert_oid_type(struct object_database *odb,
			 const struct object_id *oid, enum object_type expect);

/*
 * Enabling the object read lock allows multiple threads to safely call the
 * following functions in parallel: odb_read_object(),
 * odb_read_object_peeled(), odb_read_object_info() and odb().
 *
 * obj_read_lock() and obj_read_unlock() may also be used to protect other
 * section which cannot execute in parallel with object reading. Since the used
 * lock is a recursive mutex, these sections can even contain calls to object
 * reading functions. However, beware that in these cases zlib inflation won't
 * be performed in parallel, losing performance.
 *
 * TODO: odb_read_object_info_extended()'s call stack has a recursive behavior. If
 * any of its callees end up calling it, this recursive call won't benefit from
 * parallel inflation.
 */
void enable_obj_read_lock(void);
void disable_obj_read_lock(void);

extern int obj_read_use_lock;
extern pthread_mutex_t obj_read_mutex;

static inline void obj_read_lock(void)
{
	if(obj_read_use_lock)
		pthread_mutex_lock(&obj_read_mutex);
}

static inline void obj_read_unlock(void)
{
	if(obj_read_use_lock)
		pthread_mutex_unlock(&obj_read_mutex);
}

/* Flags for for_each_*_object(). */
enum odb_for_each_object_flags {
	/* Iterate only over local objects, not alternates. */
	ODB_FOR_EACH_OBJECT_LOCAL_ONLY = (1<<0),

	/* Only iterate over packs obtained from the promisor remote. */
	ODB_FOR_EACH_OBJECT_PROMISOR_ONLY = (1<<1),

	/*
	 * Visit objects within a pack in packfile order rather than .idx order
	 */
	ODB_FOR_EACH_OBJECT_PACK_ORDER = (1<<2),

	/* Only iterate over packs that are not marked as kept in-core. */
	ODB_FOR_EACH_OBJECT_SKIP_IN_CORE_KEPT_PACKS = (1<<3),

	/* Only iterate over packs that do not have .keep files. */
	ODB_FOR_EACH_OBJECT_SKIP_ON_DISK_KEPT_PACKS = (1<<4),

	/*
	 * Populate the callback's object_info with the object's on-disk
	 * location (its `whence` and, for packed objects, the containing pack
	 * and offset) without reading the object's contents, so a caller that
	 * is going to read each object anyway can take a location-keyed fast
	 * path. It is honored only where a location is available for free, i.e.
	 * for packed objects; loose and other sources still pass a NULL
	 * object_info when no read was requested. This lets the object commands
	 * enumerate purely through the source vtable while keeping the
	 * pack+offset hint that a hand-rolled loose/packed split would provide.
	 */
	ODB_FOR_EACH_OBJECT_PROVIDE_LOCATION = (1<<5),

	/*
	 * Iterate only over objects in the backend's consolidated bulk store,
	 * skipping any transient per-object staging tier. Freshly written
	 * objects may live in a staging tier before maintenance gathers them
	 * into the bulk store: the files backend writes objects loose and only
	 * later packs them, so it honors this by visiting just its packed
	 * objects. A backend with no such staging tier (e.g. the helper) keeps
	 * every object in its bulk store and so visits all of them. Callers that
	 * build a cache derived from the settled object set (e.g. the
	 * commit-graph) use this to avoid churning on not-yet-consolidated
	 * objects.
	 */
	ODB_FOR_EACH_OBJECT_BULK_ONLY = (1<<6),
};

/*
 * A callback function that can be used to iterate through objects. If given,
 * the optional `oi` parameter will be populated the same as if you would call
 * `odb_read_object_info()`.
 *
 * Returning a non-zero error code will cause iteration to abort. The error
 * code will be propagated.
 */
typedef int (*odb_for_each_object_cb)(const struct object_id *oid,
				      struct object_info *oi,
				      void *cb_data);

/*
 * Options that can be passed to `odb_for_each_object()` and its
 * backend-specific implementations.
 */
struct odb_for_each_object_options {
	/* A bitfield of `odb_for_each_object_flags`. */
	enum odb_for_each_object_flags flags;

	/*
	 * If set, only iterate through objects whose first `prefix_hex_len`
	 * hex characters matches the given prefix.
	 */
	const struct object_id *prefix;
	size_t prefix_hex_len;

	/*
	 * If set, an objects filter that enumeration may use to avoid yielding
	 * objects the filter would exclude. This is a best-effort optimization
	 * hint, not a guarantee: a source is free to ignore it, so the caller
	 * remains responsible for applying the authoritative filter to the
	 * objects it is handed. A source honors it only where it can prune
	 * cheaply, e.g. the files source consults a pack bitmap. Only the
	 * blob:none, blob:limit and object:type filters are meaningful here.
	 */
	struct list_objects_filter_options *objects_filter;
};

/*
 * Iterate through all objects contained in the object database. Note that
 * objects may be iterated over multiple times in case they are either stored
 * in different backends or in case they are stored in multiple sources.
 * If an object info request is given, then the object info will be read and
 * passed to the callback as if `odb_read_object_info()` was called for the
 * object.
 *
 * Returning a non-zero error code from the callback function will cause
 * iteration to abort. The error code will be propagated.
 *
 * Returns 0 on success, a negative error code in case a failure occurred, or
 * an arbitrary non-zero error code returned by the callback itself.
 */
int odb_for_each_object_ext(struct object_database *odb,
			    const struct object_info *request,
			    odb_for_each_object_cb cb,
			    void *cb_data,
			    const struct odb_for_each_object_options *opts);

/* Same as `odb_for_each_object_ext()` with `opts.flags` set to the given flags. */
int odb_for_each_object(struct object_database *odb,
			const struct object_info *request,
			odb_for_each_object_cb cb,
			void *cb_data,
			enum odb_for_each_object_flags flags);

enum odb_count_objects_flags {
	/*
	 * Instead of providing an accurate count, allow the number of objects
	 * to be approximated. Details of how this approximation works are
	 * subject to the specific source's implementation.
	 */
	ODB_COUNT_OBJECTS_APPROXIMATE = (1 << 0),
};

/*
 * Count the number of objects in the given object database. This object count
 * may double-count objects that are stored in multiple backends, or which are
 * stored multiple times in a single backend.
 *
 * Returns 0 on success, a negative error code otherwise. The number of objects
 * will be assigned to the `out` pointer on success.
 */
int odb_count_objects(struct object_database *odb,
		      enum odb_count_objects_flags flags,
		      unsigned long *out);

/*
 * The on-disk packfile layout of an object database, as reported by
 * odb_count_packs(). This is a files-backend concept (packs, ".idx" sizes);
 * sources without packfiles (e.g. a helper, which stores objects in its own
 * backing store) contribute nothing, leaving the counters at zero. Used by
 * "git count-objects -v" to describe the local pack layout.
 */
struct odb_pack_report {
	unsigned long packs;
	unsigned long objects;
	off_t size;
};

/*
 * Accumulate, into `report`, the local packfile layout of the object
 * database's sources (the packs reported by dumb transports and pruners, i.e.
 * those with a local on-disk pack). Sources with no packfiles add nothing.
 */
void odb_count_packs(struct object_database *odb, struct odb_pack_report *report);

/*
 * Given an object ID, find the minimum required length required to make the
 * object ID unique across the whole object database.
 *
 * The `min_len` determines the minimum abbreviated length that'll be returned
 * by this function. If `min_len < 0`, then the function will set a sensible
 * default minimum abbreviation length.
 *
 * Returns 0 on success, a negative error code otherwise. The computed length
 * will be assigned to `*out`.
 */
int odb_find_abbrev_len(struct object_database *odb,
			const struct object_id *oid,
			int min_len,
			unsigned *out);

/*
 * Flags controlling odb_optimize(). Mirrors the REFS_OPTIMIZE_* flags on the
 * refs side (refs.h); the exact effect of each flag is up to the source.
 *
 * ODB_OPTIMIZE_PRUNE: also prune storage the optimization makes redundant
 *                     (e.g. drop packs whose objects the repack subsumes).
 * ODB_OPTIMIZE_AUTO:  optimize on a best-effort, heuristic basis (as with
 *                     "gc --auto"); the source decides whether and how much
 *                     work to do and may fall back to a full optimization.
 */
#define ODB_OPTIMIZE_PRUNE             (1 << 0)
#define ODB_OPTIMIZE_AUTO              (1 << 1)
#define ODB_OPTIMIZE_AGGRESSIVE        (1 << 2)
#define ODB_OPTIMIZE_QUIET             (1 << 3)
#define ODB_OPTIMIZE_KEEP_LARGEST_PACK (1 << 4)
#define ODB_OPTIMIZE_CRUFT             (1 << 5)
/*
 * ODB_OPTIMIZE_GEOMETRIC: optimize by rolling packs up into a geometric
 * progression (the analog of "git repack --geometric"), using
 * `geometric_split_factor`. The files source decides per the pack geometry
 * whether to merge a suffix of packs or, when every pack would be merged, to
 * fall back to a full all-into-one repack (honoring the cruft/prune flags
 * above). Sources with no packfiles ignore it and optimize their own storage.
 */
#define ODB_OPTIMIZE_GEOMETRIC         (1 << 6)
/*
 * ODB_OPTIMIZE_NO_KEEP_LARGEST_PACK: the caller explicitly asked not to keep
 * the largest pack (git gc --no-keep-largest-pack), overriding any
 * gc.bigPackThreshold the source would otherwise honor. Distinct from the flag
 * being absent, which leaves the source to apply gc.bigPackThreshold.
 */
#define ODB_OPTIMIZE_NO_KEEP_LARGEST_PACK (1 << 7)
/*
 * ODB_OPTIMIZE_MIDX: optimize via multi-pack-index maintenance (the analog of
 * the "incremental-repack" maintenance task): write the multi-pack-index,
 * expire the packs it makes redundant, and repack small packs up to an
 * automatically chosen batch size. The multi-pack-index is a files-pack
 * concept, so the files source does this on its own packs; sources without one
 * ignore it. ODB_OPTIMIZE_QUIET suppresses progress.
 */
#define ODB_OPTIMIZE_MIDX              (1 << 7)

struct repack_opts;

struct odb_optimize_opts {
	unsigned int flags;
	/* With ODB_OPTIMIZE_GEOMETRIC, the geometric progression factor. */
	int geometric_split_factor;
	/*
	 * Prune unreachable objects older than this approxidate. NULL means do
	 * not prune unreachable objects. How a source honors this is up to it;
	 * the files source maps it onto repack's -a / --cruft-expiration.
	 */
	const char *prune_expire;
	/*
	 * With ODB_OPTIMIZE_CRUFT, write objects pruned by this optimization to
	 * this destination (a pack prefix) instead of discarding them. NULL
	 * means discard. Sources without a cruft concept ignore it.
	 */
	const char *expire_to;
	/*
	 * With ODB_OPTIMIZE_CRUFT, cap the size of a newly written cruft area;
	 * 0 means unlimited. Sources without a cruft concept ignore it.
	 */
	unsigned long max_cruft_size;
	/*
	 * Fully-parsed repack options from "git repack" (command line plus
	 * config). When set, the files source repacks with exactly these
	 * instead of deriving maintenance defaults from config, the way
	 * "git pack-refs" hands its parsed options to refs_optimize(). Other
	 * sources have no pack concept and ignore it; it is NULL on the
	 * "git gc" path, where the flags above drive the optimization.
	 */
	struct repack_opts *repack;
};

/*
 * Optimize the storage of the object database's local sources. The exact
 * behavior is up to each source: the files source repacks loose objects and
 * packs, while a helper source asks the helper to optimize its own storage
 * (for example by compacting its backing store). Alternates are borrowed read-only stores
 * and are never optimized. Sources with nothing to optimize, such as the
 * in-memory source, treat this as a no-op.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
int odb_optimize(struct object_database *odb, struct odb_optimize_opts *opts);

/*
 * Report via `*required` whether any local source would benefit from a call to
 * odb_optimize(). Used to decide whether automatic maintenance ("gc --auto")
 * should run.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
int odb_optimize_required(struct object_database *odb,
			  struct odb_optimize_opts *opts,
			  bool *required);

struct fsck_options;

/*
 * Callback invoked by odb_verify() for each object an object source enumerates
 * during verification. It receives the object's already-read raw contents so
 * the caller can content-check it (parse it, walk its links, run the object
 * fsck rules) and record that the object exists. It deliberately matches
 * verify_fn (the callback verify_pack() uses), so a single per-object fsck
 * handler serves loose, packed, and helper-backed objects alike.
 *
 * `buffer` may be NULL for a blob too large to hold in memory. Setting `*eaten`
 * to non-zero hands buffer ownership to the callback, which the source must
 * then not free. Returns 0 if the object is fine, non-zero on a problem; the
 * source keeps iterating regardless so that a single bad object does not mask
 * the rest.
 */
typedef int (*odb_verify_cb)(const struct object_id *oid,
			     enum object_type type,
			     unsigned long size,
			     void *buffer, int *eaten,
			     void *cb_data);

/*
 * Verify the integrity of the object database's stored objects (the storage
 * side of "git fsck"): each source checks its own on-disk format - the files
 * source scans loose objects, a helper source asks the helper to enumerate and
 * to check its backing store. Each enumerated object's contents are passed to
 * `cb` (typically the caller's per-object fsck handler) for content checking;
 * storage-format problems are reported through the fsck_options callbacks.
 * Returns 0 if every source verified clean, a negative error code otherwise.
 */
int odb_verify(struct object_database *odb, struct fsck_options *o,
	       odb_verify_cb cb, void *cb_data);

/*
 * Tidy the cruft left in each local source's on-disk representation after
 * "git prune" removed unreachable objects (stale temp files, empty fanout
 * dirs, redundant loose objects). Dispatched per source: the files source
 * sweeps its object directory; a source that stores objects in its own backing
 * store (a helper) is a no-op. Temp files older than `expire` are removed;
 * `dry_run` reports without removing and `verbose` reports what is removed.
 */
void odb_prune_cruft(struct object_database *odb, timestamp_t expire,
		     int dry_run, int verbose);

/*
 * Record the given objects as promisor objects, so they and the absent
 * objects they may reference are retained and never reported missing.
 * Dispatches to each local source's mark_objects_promisor callback; sources
 * with no promisor concept implement nothing and are skipped. Invoked after a
 * promisor pack is ingested, both for the received objects (the generic
 * pack-ingest commit) and for the local objects that pack references
 * (index-pack).
 */
void odb_mark_objects_promisor(struct object_database *odb, struct oidset *oids);

/*
 * Report whether any source stores `oid` as a promisor object (received from a
 * promisor remote). Dispatches to each source's backend, so it works whatever
 * the primary backend is: the files source checks its ".promisor" packs, a
 * helper reports its promisor-marked objects. The connectivity check uses this
 * to skip a wanted ref tip that arrived in a promisor packfile. Returns
 * non-zero if some source reports the object as promisor, 0 otherwise.
 */
int odb_is_promisor_object(struct object_database *odb,
			   const struct object_id *oid);


enum odb_write_object_flags {
	/*
	 * By default, `odb_write_object()` does not actually write anything
	 * into the object store, but only computes the object ID. This flag
	 * changes that so that the object will be written as a loose object
	 * and persisted.
	 */
	ODB_WRITE_OBJECT_PERSIST = (1 << 0),

	/*
	 * Do not print an error in case something goes wrong.
	 */
	ODB_WRITE_OBJECT_SILENT = (1 << 1),

	/*
	 * Overwrite the object's stored representation in place rather than
	 * keeping an existing copy. Set only by a pack-ingest session running in
	 * repack mode (gc/repack re-deltifying a source's own objects), where the
	 * source owns the object's representation the way files' repack owns its
	 * packs; a normal write is always keep-existing. Sources whose objects are
	 * content-addressed and immutable in representation (files loose objects)
	 * ignore it; a source that stores a chosen representation (a helper storing
	 * full-vs-delta) re-represents the object when it is set.
	 */
	ODB_WRITE_OBJECT_REPLACE = (1 << 2),
};

/*
 * Write an object into the object database. The object is being written into
 * the local alternate of the repository. If provided, the converted object ID
 * as well as the compatibility object ID are written to the respective
 * pointers.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
int odb_write_object_ext(struct object_database *odb,
			 const void *buf, unsigned long len,
			 enum object_type type,
			 struct object_id *oid,
			 struct object_id *compat_oid,
			 enum odb_write_object_flags flags);

/*
 * Store an object that git already prepared in its native pack form: the
 * compressed entry bytes (`compressed`/`clen`) verbatim, `usize` their
 * uncompressed length, `base_oid` the delta base (NULL for a whole object).
 * `oid` is the (already known) object id; `resolved`/`resolved_size` are the
 * reconstructed object, used to compute the compat-hash id and as the fallback
 * payload (may be NULL only when the repo tracks no compat algorithm). The
 * primary source stores the bytes via write_prepared with no compress/resolve;
 * a source lacking write_prepared falls back to a resolved write. The recompress-
 * free receive/migrate path. Returns 0 on success, negative on error.
 */
int odb_write_prepared_ext(struct object_database *odb,
			   const struct object_id *oid,
			   enum object_type type, unsigned long usize,
			   const struct object_id *base_oid,
			   const void *compressed, unsigned long clen,
			   const void *resolved, unsigned long resolved_size,
			   enum odb_write_object_flags flags);

static inline int odb_write_object(struct object_database *odb,
				   const void *buf, unsigned long len,
				   enum object_type type,
				   struct object_id *oid)
{
	return odb_write_object_ext(odb, buf, len, type, oid, NULL, 0);
}

struct odb_write_stream;

int odb_write_object_stream(struct object_database *odb,
			    struct odb_write_stream *stream, size_t len,
			    struct object_id *oid);

void parse_alternates(const char *string,
		      int sep,
		      const char *relative_base,
		      struct strvec *out);

#endif /* ODB_H */
