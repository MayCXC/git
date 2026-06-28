#ifndef ODB_SOURCE_HELPER_H
#define ODB_SOURCE_HELPER_H

#include "odb/source.h"
#include "oidset.h"

struct helper_process;

/*
 * The helper object database source delegates object operations to the
 * per-repository object helper process (repo->odb_local_helper). Refs are
 * served by an independent process (repo->ref_local_helper), even when both
 * name the same git-local-<name> program.
 *
 * See helper.h for the full protocol documentation.
 */
struct odb_source_helper {
	struct odb_source base;
	struct helper_process *hp;

	/*
	 * Set of promisor objects, lazily loaded from "list-objects
	 * --promisor-only" by is_promisor_object and invalidated on reprepare.
	 * promisor_loaded distinguishes "not yet queried" from "queried, empty".
	 */
	struct oidset promisor_objects;
	int promisor_loaded;
};

/* Allocate and initialize a new helper source. */
struct odb_source_helper *odb_source_helper_new(struct object_database *odb,
						const char *helper_name,
						const char *path,
						bool local);

/* Construct the helper source for git-local-<name>, returning its odb_source base. */
struct odb_source *odb_source_helper_new_base(struct object_database *odb,
					      const char *name,
					      const char *path,
					      bool local);

#endif
