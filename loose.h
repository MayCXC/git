#ifndef LOOSE_H
#define LOOSE_H

#include "khash.h"

struct repository;
struct object_database;
struct odb_source_loose;
struct object_id;
struct git_hash_algo;

struct loose_object_map {
	kh_oid_map_t *to_compat;
	kh_oid_map_t *to_storage;
};

void loose_object_map_init(struct loose_object_map **map);
void loose_object_map_clear(struct loose_object_map **map);

/*
 * Resolve `src` to its equivalent under `dest_algo` using the object
 * database's storage<->compat map. Returns 0 and fills `dest`, -1 if unknown.
 */
int repo_loose_object_map_oid(struct repository *repo,
			      const struct object_id *src,
			      const struct git_hash_algo *dest_algo,
			      struct object_id *dest);

/*
 * Record `oid`<->`compat_oid` in the object database's compat map and append it
 * to the loose-object-idx at the primary object directory. `loose` is the files
 * source the object is loose in (so its cache learns the compat id), or NULL
 * when the object lives in a non-files backend (the helper) and git keeps only
 * the map. Called by git, never by a storage backend, after writing an object.
 */
int repo_add_loose_object_map(struct object_database *odb,
			      struct odb_source_loose *loose,
			      const struct object_id *oid,
			      const struct object_id *compat_oid);

/*
 * Load the storage<->compat map from every object directory into the odb-level
 * map (the primary object_dir plus each alternate). Idempotent.
 */
int repo_read_loose_object_map(struct repository *repo);

/*
 * Load one object directory's loose-object-idx into the odb-level compat map.
 * Both are called directly by repo_read_loose_object_map(), not via a vtable
 * method. loose_source_read_compat_map() reads a files source's own directory
 * and feeds its abbreviation cache; odb_read_object_dir_compat_map() reads the
 * primary object_dir when the primary backend is not a files source (a
 * helper), where there is no loose source to carry the map.
 */
int loose_source_read_compat_map(struct odb_source_loose *loose);
int odb_read_object_dir_compat_map(struct object_database *odb);

#endif
