#include "builtin.h"
#include "gettext.h"
#include "odb.h"
#include "parse-options.h"
#include "strbuf.h"

#define ODB_MIGRATE_USAGE \
	N_("git odb migrate --object-storage=<storage>")

static int cmd_odb_migrate(int argc, const char **argv, const char *prefix,
			   struct repository *repo)
{
	const char * const migrate_usage[] = {
		ODB_MIGRATE_USAGE,
		NULL,
	};
	const char *storage = NULL;
	struct option options[] = {
		OPT_STRING_F(0, "object-storage", &storage, N_("storage"),
			     N_("specify the object storage to migrate to"),
			     PARSE_OPT_NONEG),
		OPT_END(),
	};
	struct strbuf err = STRBUF_INIT;
	int ret = 0;

	argc = parse_options(argc, argv, prefix, options, migrate_usage, 0);
	if (argc)
		usage(_("too many arguments"));
	if (!storage)
		usage(_("missing --object-storage=<storage>"));

	if (repo_migrate_object_storage(repo, storage, &err) < 0)
		ret = error("%s", err.buf);

	strbuf_release(&err);
	return ret;
}

int cmd_odb(int argc,
	    const char **argv,
	    const char *prefix,
	    struct repository *repo)
{
	const char * const odb_usage[] = {
		ODB_MIGRATE_USAGE,
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option opts[] = {
		OPT_SUBCOMMAND("migrate", &fn, cmd_odb_migrate),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, opts, odb_usage, 0);
	return fn(argc, argv, prefix, repo);
}
