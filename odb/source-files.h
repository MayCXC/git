#ifndef ODB_SOURCE_FILES_H
#define ODB_SOURCE_FILES_H

#include "odb/source.h"

struct odb_source_loose;
struct packfile_store;
struct packfile_list_entry;
struct packed_git;
struct object_database;

/*
 * The files object database source uses a combination of loose objects and
 * packfiles. It is the default backend used by Git to store objects.
 */
struct odb_source_files {
	struct odb_source base;
	struct odb_source_loose *loose;

	/*
	 * This source's packfiles. Packfiles are a files-backend concept (other
	 * backends, e.g. a helper, have none), so this lives on the files source
	 * rather than the generic base, mirroring packed_ref_store on
	 * files_ref_store. Owned by the source.
	 */
	struct packfile_store *packed;

	/* Links the odb's list of files sources; see object_database.files_sources. */
	struct odb_source_files *next_files;
};

/* Allocate and initialize a new object source. */
struct odb_source_files *odb_source_files_new(struct object_database *odb,
					      const char *path,
					      bool local);

/*
 * Recover the files backend's per-source struct from its base. Only reached
 * from a files-source vtable method, where the backend is already guaranteed
 * (the installed vtable is the source's identity), so the cast is unconditional.
 */
static inline struct odb_source_files *odb_source_files_downcast(struct odb_source *source)
{
	return container_of(source, struct odb_source_files, base);
}

#endif
