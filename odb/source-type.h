#ifndef ODB_SOURCE_TYPE_H
#define ODB_SOURCE_TYPE_H

/*
 * Extracted from odb/source.h to avoid circular includes with
 * repository.h which needs the enum for struct repository fields.
 */
enum odb_source_type {
	/*
	 * The "unknown" type, which should never be in use. This type mostly
	 * exists to catch cases where the type field remains zeroed out.
	 */
	ODB_SOURCE_UNKNOWN,

	/* The "files" backend that uses loose objects and packfiles. */
	ODB_SOURCE_FILES,

	/* An external helper process (git-local-<name>). */
	ODB_SOURCE_HELPER,
};

#endif
