#include "git-compat-util.h"
#include "hash.h"
#include "path.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-loose.h"
#include "hex.h"
#include "repository.h"
#include "wrapper.h"
#include "gettext.h"
#include "loose.h"
#include "lockfile.h"
#include "oidtree.h"

static const char *loose_object_header = "# loose-object-idx\n";

static inline int should_use_loose_object_map(struct repository *repo)
{
	return repo->compat_hash_algo && repo->gitdir;
}

void loose_object_map_init(struct loose_object_map **map)
{
	struct loose_object_map *m;
	m = xmalloc(sizeof(**map));
	m->to_compat = kh_init_oid_map();
	m->to_storage = kh_init_oid_map();
	*map = m;
}

static int insert_oid_pair(kh_oid_map_t *map, const struct object_id *key, const struct object_id *value)
{
	khiter_t pos;
	int ret;
	struct object_id *stored;

	pos = kh_put_oid_map(map, *key, &ret);

	/* This item already exists in the map. */
	if (ret == 0)
		return 0;

	stored = xmalloc(sizeof(*stored));
	oidcpy(stored, value);
	kh_value(map, pos) = stored;
	return 1;
}

/*
 * Record one storage<->compat object-id pair in the odb-level compat map. When
 * a loose files source is given, also note the compat id in its existence and
 * abbreviation cache so a compat id resolves directly against that source's
 * loose objects; pass NULL for an object directory with no loose source (e.g.
 * the primary object_dir of a helper-backed repository, where git keeps the
 * map even though the objects themselves live in the helper).
 */
static int insert_compat_pair(struct object_database *odb,
			      struct odb_source_loose *loose,
			      const struct object_id *oid,
			      const struct object_id *compat_oid)
{
	struct loose_object_map *map;
	int inserted = 0;

	if (!odb->compat_map)
		loose_object_map_init(&odb->compat_map);
	map = odb->compat_map;

	inserted |= insert_oid_pair(map->to_compat, oid, compat_oid);
	inserted |= insert_oid_pair(map->to_storage, compat_oid, oid);
	if (inserted && loose) {
		if (!loose->cache) {
			ALLOC_ARRAY(loose->cache, 1);
			oidtree_init(loose->cache);
		}
		oidtree_insert(loose->cache, compat_oid, NULL);
	}

	return inserted;
}

/*
 * Read the loose-object-idx at object directory `dir` into the odb-level compat
 * map. `loose` (may be NULL) is the files source backing `dir`, whose cache
 * then also learns the compat ids.
 */
static int load_compat_idx(struct repository *repo, const char *dir,
			   struct odb_source_loose *loose)
{
	struct object_database *odb = repo->objects;
	struct strbuf buf = STRBUF_INIT, path = STRBUF_INIT;
	FILE *fp;

	insert_compat_pair(odb, loose, repo->hash_algo->empty_tree,
			   repo->compat_hash_algo->empty_tree);
	insert_compat_pair(odb, loose, repo->hash_algo->empty_blob,
			   repo->compat_hash_algo->empty_blob);
	insert_compat_pair(odb, loose, repo->hash_algo->null_oid,
			   repo->compat_hash_algo->null_oid);

	strbuf_addf(&path, "%s/loose-object-idx", dir);
	fp = fopen(path.buf, "rb");
	if (!fp) {
		strbuf_release(&path);
		return 0;
	}

	errno = 0;
	if (strbuf_getwholeline(&buf, fp, '\n') || strcmp(buf.buf, loose_object_header))
		goto err;
	while (!strbuf_getline_lf(&buf, fp)) {
		const char *p;
		struct object_id oid, compat_oid;
		if (parse_oid_hex_algop(buf.buf, &oid, &p, repo->hash_algo) ||
		    *p++ != ' ' ||
		    parse_oid_hex_algop(p, &compat_oid, &p, repo->compat_hash_algo) ||
		    p != buf.buf + buf.len)
			goto err;
		insert_compat_pair(odb, loose, &oid, &compat_oid);
	}

	fclose(fp);
	strbuf_release(&buf);
	strbuf_release(&path);
	return errno ? -1 : 0;
err:
	fclose(fp);
	strbuf_release(&buf);
	strbuf_release(&path);
	return -1;
}

int loose_source_read_compat_map(struct odb_source_loose *loose)
{
	struct repository *repo = loose->base.odb->repo;

	if (!should_use_loose_object_map(repo))
		return 0;
	/* A loose source's path is its object directory. */
	return load_compat_idx(repo, loose->base.path, loose);
}

int odb_read_object_dir_compat_map(struct object_database *odb)
{
	struct repository *repo = odb->repo;

	if (!should_use_loose_object_map(repo))
		return 0;
	/*
	 * Used by a non-files primary (e.g. a helper): the objects live in the
	 * backend, but git keeps their compat map in a loose-object-idx at the
	 * object directory, with no loose source and thus no cache.
	 */
	return load_compat_idx(repo, odb->object_dir, NULL);
}

int repo_read_loose_object_map(struct repository *repo)
{
	struct object_database *odb = repo->objects;
	struct odb_source *source;

	if (!should_use_loose_object_map(repo))
		return 0;

	odb_prepare_alternates(odb);

	/*
	 * The storage<->compat map is owned by the object database; each object
	 * directory persists its slice in a loose-object-idx. Dispatch per source
	 * so each backend reads its own slice the right way (a files source from
	 * its loose directory, feeding its abbreviation cache; a helper from the
	 * idx at its object directory, with no loose cache) without branching on
	 * the backend type.
	 */
	for (source = odb_primary_source(odb); source; source = source->next)
		if (odb_source_read_compat_map(source) < 0)
			return -1;

	return 0;
}

static int write_one_object(struct object_database *odb,
			    const struct object_id *oid,
			    const struct object_id *compat_oid)
{
	struct lock_file lock;
	int fd;
	struct stat st;
	struct strbuf buf = STRBUF_INIT, path = STRBUF_INIT;

	strbuf_addf(&path, "%s/loose-object-idx", odb->object_dir);
	hold_lock_file_for_update_timeout(&lock, path.buf, LOCK_DIE_ON_ERROR, -1);

	fd = open(path.buf, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0)
		goto errout;
	if (fstat(fd, &st) < 0)
		goto errout;
	if (!st.st_size && write_in_full(fd, loose_object_header, strlen(loose_object_header)) < 0)
		goto errout;

	strbuf_addf(&buf, "%s %s\n", oid_to_hex(oid), oid_to_hex(compat_oid));
	if (write_in_full(fd, buf.buf, buf.len) < 0)
		goto errout;
	if (close(fd))
		goto errout;
	adjust_shared_perm(odb->repo, path.buf);
	rollback_lock_file(&lock);
	strbuf_release(&buf);
	strbuf_release(&path);
	return 0;
errout:
	error_errno(_("failed to write loose object index %s"), path.buf);
	close(fd);
	rollback_lock_file(&lock);
	strbuf_release(&buf);
	strbuf_release(&path);
	return -1;
}

int repo_add_loose_object_map(struct object_database *odb,
			      struct odb_source_loose *loose,
			      const struct object_id *oid,
			      const struct object_id *compat_oid)
{
	if (!should_use_loose_object_map(odb->repo))
		return 0;

	if (insert_compat_pair(odb, loose, oid, compat_oid))
		return write_one_object(odb, oid, compat_oid);
	return 0;
}

int repo_loose_object_map_oid(struct repository *repo,
			      const struct object_id *src,
			      const struct git_hash_algo *to,
			      struct object_id *dest)
{
	struct loose_object_map *cmap = repo->objects->compat_map;
	kh_oid_map_t *map;
	khiter_t pos;

	if (!cmap)
		return -1;
	map = (to == repo->compat_hash_algo) ? cmap->to_compat : cmap->to_storage;
	pos = kh_get_oid_map(map, *src);
	if (pos < kh_end(map)) {
		oidcpy(dest, kh_value(map, pos));
		return 0;
	}
	return -1;
}

void loose_object_map_clear(struct loose_object_map **map)
{
	struct loose_object_map *m = *map;
	struct object_id *oid;

	if (!m)
		return;

	kh_foreach_value(m->to_compat, oid, free(oid));
	kh_foreach_value(m->to_storage, oid, free(oid));
	kh_destroy_oid_map(m->to_compat);
	kh_destroy_oid_map(m->to_storage);
	free(m);
	*map = NULL;
}
