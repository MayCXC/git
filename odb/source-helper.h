#ifndef ODB_SOURCE_HELPER_H
#define ODB_SOURCE_HELPER_H

#include "helper.h"
#include "odb/source.h"

struct odb_source_files;

/*
 * A source that keeps objects in a git-local-<name> helper program, driven
 * with the object commands of gitlocal-helpers(7). It also has a files store
 * at its own path, which keeps the packfiles and loose objects that code
 * writing the files layout adds to the source, like index-pack does for a
 * fetch or receive-pack for a push. Reads see both; optimizing the source
 * takes the objects of the files store into the helper.
 */
struct odb_source_helper {
	struct odb_source base;

	/* The helper process, started on first use. */
	struct helper_process hp;

	struct odb_source_files *files;
};

/*
 * Create a new object database source at `path`, served by the
 * git-local-<name> helper program, which stores the objects of the common
 * directory of the repository.
 */
struct odb_source_helper *odb_source_helper_new(struct object_database *odb,
						const char *name,
						const char *path,
						bool local);

/*
 * Cast the given object database source to the helper backend. This will
 * cause a BUG in case the source doesn't use this backend.
 */
static inline struct odb_source_helper *odb_source_helper_downcast(struct odb_source *source)
{
	if (source->type != ODB_SOURCE_HELPER)
		BUG("trying to downcast source of type '%s' to '%s'",
		    odb_source_type_to_name(source->type),
		    odb_source_type_to_name(ODB_SOURCE_HELPER));
	return container_of(source, struct odb_source_helper, base);
}

#endif
