#define USE_THE_REPOSITORY_VARIABLE
#include "builtin.h"
#include "gettext.h"
#include "odb.h"
#include "odb/source.h"
#include "parse-options.h"
#include "repository.h"
#include "strbuf.h"

#define ODB_MIGRATE_USAGE \
	N_("git odb migrate --object-storage=<name>")

static int cmd_odb_migrate(int argc, const char **argv, const char *prefix,
			   struct repository *repo UNUSED)
{
	const char * const migrate_usage[] = {
		ODB_MIGRATE_USAGE,
		NULL,
	};
	const char *name = NULL;
	struct option options[] = {
		OPT_STRING_F(0, "object-storage", &name, N_("name"),
			N_("object storage backend to convert to (\"files\" or a helper name)"),
			PARSE_OPT_NONEG),
		OPT_END(),
	};
	struct strbuf errbuf = STRBUF_INIT;
	int err;

	argc = parse_options(argc, argv, prefix, options, migrate_usage, 0);
	if (argc)
		usage(_("too many arguments"));
	if (!name)
		usage(_("missing --object-storage=<name>"));

	if (repo_migrate_object_storage_format(the_repository, name, &errbuf) < 0) {
		err = error("%s", errbuf.buf);
		goto out;
	}

	err = 0;

out:
	strbuf_release(&errbuf);
	return err;
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
