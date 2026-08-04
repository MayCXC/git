#define USE_THE_REPOSITORY_VARIABLE

#include "unit-test.h"
#include "lib-oid.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-loose.h"
#include "repository.h"
#include "strbuf.h"

static struct object_database *odb;
static struct strbuf objdir = STRBUF_INIT;

void test_odb_loose__initialize(void)
{
	repo_set_hash_algo(the_repository, cl_setup_hash_algo());
	odb = odb_new(the_repository, 0);
	strbuf_addstr(&objdir, "loose-objects");
	cl_must_pass(mkdir(objdir.buf, 0777));
}

void test_odb_loose__cleanup(void)
{
	odb_free(odb);
	strbuf_release(&objdir);
}

/*
 * A quick existence check answers from a cached readdir of the fanout
 * directory. Once that cache has been populated for a subdirectory, an object
 * written into it has to be recorded there too, or it stays invisible to the
 * process that wrote it.
 */
void test_odb_loose__quick_sees_an_object_we_wrote(void)
{
	struct odb_source_loose *source = odb_source_loose_new(odb, objdir.buf, true);
	const char *content = "written after the cache was loaded";
	size_t len = strlen(content);
	struct object_id oid;

	hash_object_file(the_repository->hash_algo, content, len, OBJ_BLOB, &oid);

	/* Populates the cache for this object's fanout directory. */
	cl_must_fail(odb_source_read_object_info(&source->base, &oid, NULL,
						 OBJECT_INFO_QUICK));

	cl_must_pass(odb_source_write_object(&source->base, content, len,
					     OBJ_BLOB, &oid, NULL, NULL, 0));

	cl_must_pass(odb_source_read_object_info(&source->base, &oid, NULL,
						 OBJECT_INFO_QUICK));
}
