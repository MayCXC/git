#include "git-compat-util.h"
#include "object-file.h"
#include "odb/source-files.h"
#include "odb/source.h"
#include "packfile.h"
#include "repository.h"

struct odb_source *odb_source_new(struct object_database *odb,
				  const char *path,
				  bool local)
{
	enum odb_source_type format = odb->repo->odb_source_type;

	if (format != ODB_SOURCE_UNKNOWN &&
	    format != ODB_SOURCE_FILES)
		return odb_source_new_for_type(odb, format, path, local);
	return &odb_source_files_new(odb, path, local)->base;
}

struct odb_source *odb_source_new_for_type(struct object_database *odb,
					   enum odb_source_type type,
					   const char *path,
					   bool local)
{
	switch (type) {
	case ODB_SOURCE_FILES:
		return &odb_source_files_new(odb, path, local)->base;
	case ODB_SOURCE_HELPER:
		BUG("helper ODB source not yet implemented");
	case ODB_SOURCE_UNKNOWN:
		BUG("unknown ODB source type");
	}
	BUG("unhandled ODB source type %d", type);
}

void odb_source_init(struct odb_source *source,
		     struct object_database *odb,
		     enum odb_source_type type,
		     const char *path,
		     bool local)
{
	source->odb = odb;
	source->type = type;
	source->local = local;
	source->path = xstrdup(path);
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
