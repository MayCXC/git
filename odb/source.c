#include "git-compat-util.h"
#include "object-file.h"
#include "odb/source-files.h"
#include "odb/source.h"
#include "packfile.h"
#include "repository.h"

/*
 * Object database storage backends, indexed by odb_storage_format.
 * Follows the same pattern as refs_backends[] in refs.c.
 */
typedef struct odb_source *(*odb_source_new_fn)(struct object_database *odb,
					       const char *path,
					       bool local);

static const struct {
	const char *name;
	odb_source_new_fn new_source;
} odb_backends[] = {
	[ODB_STORAGE_FORMAT_FILES] = {
		.name = "files",
		.new_source = odb_source_files_new_base,
	},
};

static const char *find_odb_storage_backend_name(
	enum odb_storage_format format)
{
	if (format < ARRAY_SIZE(odb_backends))
		return odb_backends[format].name;
	return NULL;
}

enum odb_storage_format odb_storage_format_by_name(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(odb_backends); i++)
		if (odb_backends[i].name && !strcmp(odb_backends[i].name, name))
			return i;
	return ODB_STORAGE_FORMAT_UNKNOWN;
}

const char *odb_storage_format_to_name(enum odb_storage_format format)
{
	const char *name = find_odb_storage_backend_name(format);
	if (!name)
		return "unknown";
	return name;
}

struct odb_source *odb_source_new(struct object_database *odb,
				  const char *path,
				  bool local)
{
	enum odb_storage_format format = odb->repo->odb_storage_format;

	if (format != ODB_STORAGE_FORMAT_UNKNOWN &&
	    format < ARRAY_SIZE(odb_backends) &&
	    odb_backends[format].new_source)
		return odb_backends[format].new_source(odb, path, local);

	/* Default to files backend */
	return odb_source_files_new_base(odb, path, local);
}

void odb_source_init(struct odb_source *source,
		     struct object_database *odb,
		     enum odb_storage_format type,
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
