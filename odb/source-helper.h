#ifndef ODB_SOURCE_HELPER_H
#define ODB_SOURCE_HELPER_H

#include "odb/source.h"

struct helper_process;

/*
 * The helper object database source delegates object operations to
 * the shared helper_process on struct repository. The same process
 * also handles ref operations, matching how remote helpers share
 * one transport for refs and objects.
 *
 * See helper.h for the full protocol documentation.
 */
struct odb_source_helper {
	struct odb_source base;
	struct helper_process *hp;
};

/* Allocate and initialize a new helper source. */
struct odb_source_helper *odb_source_helper_new(struct object_database *odb,
						const char *helper_name,
						const char *path,
						bool local);

/* Wrapper returning the base odb_source pointer, for use in dispatch tables. */
struct odb_source *odb_source_helper_new_base(struct object_database *odb,
					      const char *path,
					      bool local);

#endif
