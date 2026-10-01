#include "git-compat-util.h"
#include "abspath.h"
#include "dir.h"
#include "helper.h"
#include "object-file.h"
#include "odb/source-files.h"
#include "odb/source-helper.h"
#include "odb/source.h"
#include "packfile.h"
#include "repository.h"
#include "strbuf.h"

static const char * const odb_source_names_by_type[] = {
	[ODB_SOURCE_UNKNOWN] = "unknown",
	[ODB_SOURCE_FILES] = "files",
	[ODB_SOURCE_LOOSE] = "loose",
	[ODB_SOURCE_PACKED] = "packed",
	[ODB_SOURCE_INMEMORY] = "in-memory",
	[ODB_SOURCE_HELPER] = "helper",
};

const char *odb_source_type_to_name(enum odb_source_type type)
{
	const char *name;
	if (type < 0 || type >= ARRAY_SIZE(odb_source_names_by_type))
		type = ODB_SOURCE_UNKNOWN;
	name = odb_source_names_by_type[type];
	if (!name)
		BUG("name missing in `odb_source_names_by_type` for '%d'", type);
	return name;
}

static bool is_repo_object_directory(struct repository *repo, const char *path)
{
	struct strbuf objects = STRBUF_INIT, a = STRBUF_INIT, b = STRBUF_INIT;
	bool ret;

	strbuf_addf(&objects, "%s/objects", repo->commondir);
	if (!strbuf_realpath_forgiving(&a, path, 0))
		strbuf_addstr(&a, path);
	if (!strbuf_realpath_forgiving(&b, objects.buf, 0))
		strbuf_addbuf(&b, &objects);
	ret = !fspathcmp(a.buf, b.buf);

	strbuf_release(&objects);
	strbuf_release(&a);
	strbuf_release(&b);
	return ret;
}

struct odb_source *odb_source_new(struct object_database *odb,
				  const char *path,
				  bool local)
{
	struct repository *repo = odb->repo;
	const char *helper;

	if (repo->object_storage && repo->commondir &&
	    skip_prefix(repo->object_storage, "helper://", &helper) &&
	    is_repo_object_directory(repo, path))
		return &odb_source_helper_new(odb, helper, path, local)->base;
	return &odb_source_files_new(odb, path, local)->base;
}

int odb_object_storage_parse(const char *uri, char **canonical)
{
	const char *name;

	*canonical = NULL;
	if (!strcmp(uri, "files"))
		return 0;
	if (!skip_prefix(uri, "helper://", &name)) {
		if (strstr(uri, "://") || !strcmp(uri, "helper"))
			return -1;
		name = uri;
	}
	if (!is_helper_name(name))
		return -1;
	*canonical = xstrfmt("helper://%s", name);
	return 0;
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
