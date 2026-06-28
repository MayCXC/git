#ifndef ODB_SOURCE_LOOSE_H
#define ODB_SOURCE_LOOSE_H

#include "odb/source.h"

struct odb_source_files;
struct object_database;
struct oidtree;

/*
 * An object database source that stores its objects in loose format, one
 * file per object.
 */
struct odb_source_loose {
	struct odb_source base;

	/*
	 * Used to store the results of readdir(3) calls when we are OK
	 * sacrificing accuracy due to races for speed. That includes
	 * object existence with OBJECT_INFO_QUICK, as well as
	 * our search for unique abbreviated hashes. Don't use it for tasks
	 * requiring greater accuracy!
	 *
	 * Be sure to call odb_load_loose_cache() before using.
	 */
	uint32_t subdir_seen[8]; /* 256 bits */
	struct oidtree *cache;

	/* Map between object IDs for loose objects. */
	struct loose_object_map *map;
};

struct odb_source_loose *odb_source_loose_new(struct object_database *odb,
					      const char *path,
					      bool local);

/*
 * Recover the loose backend's per-source struct from its base. Only reached
 * from a loose-source vtable method, where the backend is already guaranteed
 * (the installed vtable is the source's identity), so the cast is unconditional.
 */
static inline struct odb_source_loose *odb_source_loose_downcast(struct odb_source *source)
{
	return container_of(source, struct odb_source_loose, base);
}

/*
 * Ensure the object named by OID is stored as a loose object in SOURCE's
 * files store, reading its content from wherever it currently lives and
 * stamping the loose file with MTIME. Used by pack-objects to preserve
 * objects that would otherwise be dropped when their pack is replaced.
 */
int force_object_loose(struct odb_source *source,
		       const struct object_id *oid, time_t mtime);

#endif
