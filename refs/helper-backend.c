/*
 * Ref helper backend: delegate ref storage to an external process.
 *
 * Uses the shared helper process (helper.c) which also handles ODB
 * operations in the same binary, matching how remote helpers handle
 * both refs and objects.
 */
#define USE_THE_REPOSITORY_VARIABLE

#include "../git-compat-util.h"
#include "../config.h"
#include "../environment.h"
#include "../gettext.h"
#include "../hash.h"
#include "../hex.h"
#include "../ident.h"
#include "../object.h"
#include "../helper.h"
#include "../repository.h"
#include "../strbuf.h"
#include "../strmap.h"
#include "refs-internal.h"

/*
 * Backend-private ref-update flags used by the transaction-splitting logic
 * below. These bit values are coordinated with the files and reftable backends
 * (which each carry their own copies) so they never collide with the shared
 * REF_* flags in refs-internal.h.
 */
#define REF_IS_PRUNING		(1 << 4)
#define REF_UPDATE_VIA_HEAD	(1 << 8)

/*
 * Two-backend design matching reftable: shared refs go through
 * main_hp (at commondir), per-worktree refs through wt_hp (at
 * worktree gitdir). For the main worktree, wt_hp is NULL and
 * everything goes through main_hp.
 */
struct helper_ref_store {
	struct ref_store base;
	struct helper_process *main_hp;
	struct helper_process *wt_hp;
	struct strmap worktree_helpers;
	unsigned int store_flags;
	/* main_hp is a private process this store owns (a standalone migration
	 * destination), not the shared repo->ref_local_helper. */
	unsigned int owns_main_hp;
	enum log_refs_config log_all_ref_updates;
};

/*
 * Lazy-load a helper process for another worktree, matching reftable's
 * backend_for_worktree(). Cached in the worktree_helpers strmap.
 */
static struct helper_process *helper_for_worktree(
		struct helper_ref_store *refs, const char *worktree_name)
{
	struct helper_process *hp;
	struct strbuf wt_dir = STRBUF_INIT;
	const char *name = refs->main_hp->name;

	hp = strmap_get(&refs->worktree_helpers, worktree_name);
	if (hp)
		return hp;

	strbuf_addf(&wt_dir, "%s/worktrees/%s",
		    refs->base.repo->commondir, worktree_name);

	hp = xcalloc(1, sizeof(*hp));
	helper_process_init(hp, name, wt_dir.buf);
	strmap_put(&refs->worktree_helpers, worktree_name, hp);

	strbuf_release(&wt_dir);
	return hp;
}

/*
 * Route a ref to the correct helper process, matching how reftable's
 * backend_for() routes to main_backend vs worktree_backend via
 * parse_worktree_ref(). Returns the helper process and sets
 * *bare_ref to the refname to send (stripping worktree prefixes).
 */
static struct helper_process *helper_for_ref(struct helper_ref_store *refs,
					     const char *refname,
					     const char **bare_ref)
{
	const char *wt_name;
	int wt_name_len;
	const char *bare;

	switch (parse_worktree_ref(refname, &wt_name, &wt_name_len, &bare)) {
	case REF_WORKTREE_CURRENT:
		*bare_ref = bare;
		if (refs->wt_hp)
			return refs->wt_hp;
		return refs->main_hp;
	case REF_WORKTREE_OTHER: {
		struct strbuf wt_name_buf = STRBUF_INIT;
		struct helper_process *wt;

		strbuf_add(&wt_name_buf, wt_name, wt_name_len);
		wt = helper_for_worktree(refs, wt_name_buf.buf);
		strbuf_release(&wt_name_buf);
		*bare_ref = bare;
		return wt;
	}
	case REF_WORKTREE_MAIN:
		*bare_ref = bare;
		return refs->main_hp;
	case REF_WORKTREE_SHARED:
		*bare_ref = refname;
		return refs->main_hp;
	}
	*bare_ref = refname;
	return refs->main_hp;
}

/* ---- ref_storage_be callbacks ---- */

static struct helper_ref_store *helper_downcast(struct ref_store *ref_store,
						unsigned int required_flags,
						const char *caller)
{
	struct helper_ref_store *refs;

	if (ref_store->be != &refs_be_helper)
		BUG("ref_store is type \"%s\" not \"helper\" in %s",
		    ref_store->be->name, caller);

	refs = (struct helper_ref_store *)ref_store;

	if ((refs->store_flags & required_flags) != required_flags)
		BUG("operation %s requires abilities 0x%x, but only have 0x%x",
		    caller, required_flags, refs->store_flags);

	return refs;
}

static struct ref_store *helper_ref_store_init(struct repository *repo,
					       const char *payload UNUSED,
					       const char *gitdir,
					       const struct ref_store_init_options *opts)
{
	struct helper_ref_store *refs = xcalloc(1, sizeof(*refs));
	const char *name = opts->name;

	/*
	 * The helper program is the backend selector, named the same way as the
	 * object helper (extensions.objectStorage) or a remote helper (its URL
	 * scheme): any name git does not recognize as a builtin ref backend.
	 * ref_store_init() routed here precisely because `name` is such a name, so
	 * it is always present and helper_process_init() copies it.
	 */
	if (!name || !*name)
		BUG("helper ref backend selected without a helper name");

	base_ref_store_init(&refs->base, repo, gitdir, &refs_be_helper);
	refs->store_flags = opts->access_flags;
	refs->log_all_ref_updates = opts->log_all_ref_updates;
	strmap_init(&refs->worktree_helpers);

	if (opts->standalone) {
		/*
		 * A self-contained store at its own gitdir (a migration
		 * destination), not the repository's primary ref store: own a
		 * private process for `name` and add no per-worktree helpers.
		 */
		refs->main_hp = xcalloc(1, sizeof(*refs->main_hp));
		helper_process_init(refs->main_hp, name, gitdir);
		refs->owns_main_hp = 1;
		return &refs->base;
	}

	/*
	 * Set up helper processes matching reftable's two-backend model.
	 *
	 * main_hp: the repo-level ref helper at commondir, handles shared
	 * refs (branches, tags) and main worktree refs. It is the ref
	 * backend's own process (repo->ref_local_helper), independent of
	 * the object helper even when both name the same program.
	 *
	 * wt_hp: per-worktree helper at the worktree gitdir, handles
	 * per-worktree refs (HEAD, bisect, etc.). Only created
	 * for linked worktrees (gitdir != commondir).
	 */
	if (!repo->ref_local_helper) {
		const char *main_dir = repo->commondir ? repo->commondir : gitdir;
		repo->ref_local_helper = xcalloc(1, sizeof(*repo->ref_local_helper));
		helper_process_init(repo->ref_local_helper, name, main_dir);
	} else if (!repo->ref_local_helper->gitdir) {
		const char *main_dir = repo->commondir ? repo->commondir : gitdir;
		repo->ref_local_helper->gitdir = xstrdup(main_dir);
	}
	refs->main_hp = repo->ref_local_helper;

	/* Linked worktree: create separate helper for per-worktree refs */
	if (repo->commondir && strcmp(gitdir, repo->commondir)) {
		refs->wt_hp = xcalloc(1, sizeof(*refs->wt_hp));
		helper_process_init(refs->wt_hp, name, gitdir);
	}

	return &refs->base;
}

static void helper_ref_store_release(struct ref_store *ref_store)
{
	struct helper_ref_store *refs =
		(struct helper_ref_store *)ref_store;

	/*
	 * main_hp is normally shared via repo->ref_local_helper, so don't release
	 * it; a standalone store (migration destination) owns its private one and
	 * must. wt_hp is owned by this ref store (allocated in init for linked
	 * worktrees), so release it here.
	 */
	if (refs->owns_main_hp && refs->main_hp) {
		helper_process_release(refs->main_hp);
		free(refs->main_hp);
		refs->main_hp = NULL;
	}
	if (refs->wt_hp) {
		helper_process_release(refs->wt_hp);
		free(refs->wt_hp);
		refs->wt_hp = NULL;
	}

	{
		struct hashmap_iter iter;
		struct strmap_entry *entry;
		strmap_for_each_entry(&refs->worktree_helpers, &iter, entry) {
			struct helper_process *hp = entry->value;
			helper_process_release(hp);
			free(hp);
		}
		strmap_clear(&refs->worktree_helpers, 0);
	}
}

static int helper_create_on_disk(struct ref_store *ref_store,
				 int flags UNUSED,
				 struct strbuf *err)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_WRITE, __func__);
	struct helper_process *hp = refs->main_hp;
	struct strbuf line = STRBUF_INIT;

	helper_process_ensure(hp);

	/*
	 * A helper's ref store is created implicitly when it first opens its
	 * backing store, so the explicit "create" step is optional: a helper that
	 * does not advertise it needs nothing done (mirrors the worktree helper
	 * below). One that does advertise it gets the round-trip.
	 */
	if (hp->cap_create) {
		helper_process_send(hp, "create\n");
		if (helper_process_readline(hp, &line) == EOF) {
			strbuf_addstr(err, "ref helper EOF during create");
			strbuf_release(&line);
			return -1;
		}

		if (starts_with(line.buf, "error ")) {
			strbuf_addstr(err, line.buf + 6);
			strbuf_release(&line);
			return -1;
		}
	}

	if (refs->wt_hp) {
		helper_process_ensure(refs->wt_hp);
		if (refs->wt_hp->cap_create) {
			helper_process_send(refs->wt_hp, "create\n");
			if (helper_process_readline(refs->wt_hp, &line) == EOF) {
				strbuf_addstr(err, "worktree ref helper EOF during create");
				strbuf_release(&line);
				return -1;
			}
			if (starts_with(line.buf, "error ")) {
				strbuf_addstr(err, line.buf + 6);
				strbuf_release(&line);
				return -1;
			}
		}
	}

	strbuf_release(&line);
	return 0;
}

static int helper_remove_on_disk(struct ref_store *ref_store,
				 struct strbuf *err)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_WRITE, __func__);
	struct helper_process *hp = refs->main_hp;
	struct strbuf line = STRBUF_INIT;

	helper_process_ensure(hp);

	if (!hp->cap_remove) {
		strbuf_addstr(err, "ref helper does not support remove");
		return -1;
	}

	helper_process_send(hp, "remove\n");
	if (helper_process_readline(hp, &line) == EOF ||
	    strcmp(line.buf, "ok")) {
		if (starts_with(line.buf, "error "))
			strbuf_addstr(err, line.buf + 6);
		else
			strbuf_addstr(err, "ref helper remove failed");
		strbuf_release(&line);
		return -1;
	}

	if (refs->wt_hp) {
		helper_process_ensure(refs->wt_hp);
		if (refs->wt_hp->cap_remove) {
			helper_process_send(refs->wt_hp, "remove\n");
			if (helper_process_readline(refs->wt_hp, &line) == EOF ||
			    strcmp(line.buf, "ok")) {
				if (starts_with(line.buf, "error "))
					strbuf_addstr(err, line.buf + 6);
				else
					strbuf_addstr(err, "worktree ref helper remove failed");
				strbuf_release(&line);
				return -1;
			}
		}
	}

	strbuf_release(&line);
	return 0;
}

/* ---- Read operations ---- */

static int helper_read_raw_ref(struct ref_store *ref_store,
			       const char *refname,
			       struct object_id *oid,
			       struct strbuf *referent,
			       unsigned int *type,
			       int *failure_errno)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_READ, __func__);
	const char *bare;
	struct helper_process *hp = helper_for_ref(refs, refname, &bare);
	struct strbuf line = STRBUF_INIT;

	helper_process_ensure(hp);

	if (!hp->cap_read) {
		*failure_errno = ENOENT;
		return -1;
	}

	helper_process_send(hp, "read %s\n", bare);

	if (helper_process_readline(hp, &line) == EOF) {
		*failure_errno = ENOENT;
		strbuf_release(&line);
		return -1;
	}

	if (!strcmp(line.buf, "missing")) {
		*failure_errno = ENOENT;
		strbuf_release(&line);
		return -1;
	}

	if (starts_with(line.buf, "symref ")) {
		strbuf_reset(referent);
		strbuf_addstr(referent, line.buf + 7);
		*type = REF_ISSYMREF;
		strbuf_release(&line);
		return 0;
	}

	/* Regular ref: line is hex oid */
	if (get_oid_hex_any(line.buf, oid) == GIT_HASH_UNKNOWN) {
		*failure_errno = EINVAL;
		strbuf_release(&line);
		return -1;
	}
	*type = 0;
	strbuf_release(&line);
	return 0;
}

static int helper_read_symbolic_ref(struct ref_store *ref_store,
				    const char *refname,
				    struct strbuf *referent)
{
	struct object_id oid;
	unsigned int type = 0;
	int failure_errno = 0;

	if (helper_read_raw_ref(ref_store, refname, &oid, referent,
				&type, &failure_errno))
		return -1;
	if (!(type & REF_ISSYMREF))
		return NOT_A_SYMREF;
	return 0;
}

/* ---- Iterator ---- */

/*
 * The iterator eagerly reads all lines from the helper's "list" response
 * into an in-memory array. This frees the shared pipe immediately so ODB
 * commands can use it without interleaving with ref iteration.
 */

struct helper_ref_entry {
	char *line;
};

struct helper_ref_iterator {
	struct ref_iterator base;
	struct ref_store *ref_store;
	struct helper_process *hp;
	const struct git_hash_algo *hash_algo;
	unsigned int flags;
	struct helper_ref_entry *entries;
	size_t nr, pos, alloc;
	int listed;
	struct object_id oid;
	char *current_refname;
	char *current_target;
	char *prefix;
	char *seek_to;
};

/*
 * Issue the "list" request lazily, on the first advance() after begin()/seek()
 * have settled the prefix. This lets the helper filter to the prefix (sending
 * only the refs under it, like a SQL "WHERE refname LIKE 'prefix%'"), rather
 * than streaming every ref when only a subref is wanted; the helper returns
 * them in refname order, so we then seek to the requested start.
 */
static void helper_ref_iterator_list(struct helper_ref_iterator *iter)
{
	struct strbuf line = STRBUF_INIT;
	size_t i;

	if (iter->listed)
		return;
	iter->listed = 1;

	if (iter->prefix && *iter->prefix)
		helper_process_send(iter->hp, "list %s\n", iter->prefix);
	else
		helper_process_send(iter->hp, "list\n");

	while (helper_process_readline(iter->hp, &line) != EOF) {
		if (!line.len)
			break;
		ALLOC_GROW(iter->entries, iter->nr + 1, iter->alloc);
		iter->entries[iter->nr++].line = xstrdup(line.buf);
	}
	strbuf_release(&line);

	/* The helper lists in refname order; position at the first ref >= seek_to. */
	if (iter->seek_to) {
		for (i = 0; i < iter->nr; i++)
			if (strcmp(iter->entries[i].line, iter->seek_to) >= 0)
				break;
		iter->pos = i;
		FREE_AND_NULL(iter->seek_to);
	}
}

static int helper_ref_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct helper_ref_iterator *iter =
		(struct helper_ref_iterator *)ref_iterator;

	helper_ref_iterator_list(iter);

	while (iter->pos < iter->nr) {
		const char *p = iter->entries[iter->pos++].line;
		const char *sp = strchr(p, ' ');
		int ref_flags = 0;

		if (!sp)
			return ITER_ERROR;

		free(iter->current_refname);
		iter->current_refname = xstrndup(p, sp - p);
		free(iter->current_target);
		iter->current_target = NULL;
		sp++;

		if (starts_with(sp, "symref ")) {
			const char *referent;

			iter->current_target = xstrdup(sp + 7);
			ref_flags |= REF_ISSYMREF;

			/*
			 * Resolve symref OID via refs_resolve_ref_unsafe,
			 * matching how reftable and files backends handle
			 * symrefs during iteration.
			 */
			referent = refs_resolve_ref_unsafe(
				iter->ref_store, iter->current_refname,
				RESOLVE_REF_READING, &iter->oid, &ref_flags);
			if (!referent) {
				oidclr(&iter->oid, iter->hash_algo);
				ref_flags |= REF_ISBROKEN;
			}
		} else {
			if (get_oid_hex_any(sp, &iter->oid) == GIT_HASH_UNKNOWN)
				return ITER_ERROR;
		}

		/* Filter broken/dangling refs, matching reftable backend */
		if (iter->flags & REFS_FOR_EACH_OMIT_DANGLING_SYMREFS &&
		    ref_flags & REF_ISSYMREF &&
		    ref_flags & REF_ISBROKEN)
			continue;

		if (!(iter->flags & REFS_FOR_EACH_INCLUDE_BROKEN) &&
		    !ref_resolves_to_object(iter->current_refname,
					    iter->ref_store->repo,
					    &iter->oid, ref_flags))
			continue;

		iter->base.ref.name = iter->current_refname;
		iter->base.ref.target = iter->current_target;
		iter->base.ref.oid = &iter->oid;
		iter->base.ref.flags = ref_flags;

		return ITER_OK;
	}

	return ITER_DONE;
}

static int helper_ref_iterator_seek(struct ref_iterator *ref_iterator,
				    const char *refname,
				    unsigned int flags)
{
	struct helper_ref_iterator *iter =
		(struct helper_ref_iterator *)ref_iterator;
	size_t i;

	/*
	 * Mirror the reftable backend's seek: drop any prefix and optionally
	 * adopt a new one. The list is then re-issued lazily on the next
	 * advance() so the helper can filter to the new prefix, after which we
	 * position at the first ref >= refname.
	 */
	FREE_AND_NULL(iter->prefix);
	if (flags & REF_ITERATOR_SEEK_SET_PREFIX)
		iter->prefix = xstrdup_or_null(refname);

	for (i = 0; i < iter->nr; i++)
		free(iter->entries[i].line);
	FREE_AND_NULL(iter->entries);
	iter->nr = 0;
	iter->pos = 0;
	iter->alloc = 0;
	iter->listed = 0;
	free(iter->seek_to);
	iter->seek_to = xstrdup_or_null(refname);

	return 0;
}

static void helper_ref_iterator_release(struct ref_iterator *ref_iterator)
{
	struct helper_ref_iterator *iter =
		(struct helper_ref_iterator *)ref_iterator;
	size_t i;
	for (i = 0; i < iter->nr; i++)
		free(iter->entries[i].line);
	free(iter->entries);
	free(iter->current_refname);
	free(iter->current_target);
	free(iter->prefix);
	free(iter->seek_to);
}

static struct ref_iterator_vtable helper_ref_iterator_vtable = {
	.advance = helper_ref_iterator_advance,
	.seek = helper_ref_iterator_seek,
	.release = helper_ref_iterator_release,
};

static struct ref_iterator *helper_iterator_for_hp(
		struct ref_store *ref_store,
		struct helper_process *hp,
		const char *prefix, unsigned int flags)
{
	struct helper_ref_iterator *iter;

	helper_process_ensure(hp);

	if (!hp->cap_list)
		return empty_ref_iterator_begin();

	CALLOC_ARRAY(iter, 1);
	base_ref_iterator_init(&iter->base, &helper_ref_iterator_vtable);
	iter->ref_store = ref_store;
	iter->hp = hp;
	iter->hash_algo = ref_store->repo->hash_algo;
	iter->flags = flags;
	iter->prefix = xstrdup_or_null(prefix);
	iter->seek_to = xstrdup_or_null(prefix);

	/*
	 * The "list" request is deferred to the first advance() (see
	 * helper_ref_iterator_list). Callers typically create the iterator with
	 * an empty prefix and then seek() to the real one, so listing lazily
	 * lets the helper filter to the final prefix and return its refs in
	 * refname order, instead of streaming every ref to be filtered here. The
	 * helper returns refs already sorted (see helper.h), as the ref-iterator
	 * contract requires, matching how reftable/files trust their storage.
	 */
	return &iter->base;
}

/*
 * Begin iterating refs, merging main and worktree backends like
 * reftable_be_iterator_begin() and files_ref_iterator_begin().
 */
static struct ref_iterator *helper_ref_iterator_begin(
		struct ref_store *ref_store,
		const char *prefix, const char **exclude_patterns UNUSED,
		unsigned int flags)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_READ, __func__);
	struct ref_iterator *main_iter;

	main_iter = helper_iterator_for_hp(ref_store, refs->main_hp, prefix, flags);

	if (!refs->wt_hp)
		return main_iter;

	/*
	 * Merge per-worktree refs with shared refs, matching the
	 * pattern from reftable and files backends.
	 */
	return merge_ref_iterator_begin(
		helper_iterator_for_hp(ref_store, refs->wt_hp, prefix, flags),
		main_iter, ref_iterator_select, NULL);
}

/* ---- Transactions ---- */

/*
 * Split a HEAD update so its reflog entry is recorded when the branch HEAD
 * points at is updated. Mirrors the files backend's split_head_update(); each
 * ref backend carries its own copy of this transaction-splitting logic in
 * git 2.55 (reftable inlines the equivalent).
 *
 * head_ref was resolved once at transaction start, so (like the symref
 * resolution in helper_transaction_prepare) this is best-effort across the
 * helper boundary. The split HEAD update is REF_LOG_ONLY: it records a reflog
 * entry, not a value, so a HEAD concurrently repointed away is at worst a
 * benign reflog-accuracy race with nothing to corrupt. We therefore do not
 * carry the files backend's REF_LOG_VIA_SPLIT recheck, which exists only
 * because files locks refs individually and re-reads HEAD under that lock to
 * refuse a racily-updated HEAD; reftable, resolving under its stack lock,
 * likewise omits it.
 */
static enum ref_transaction_error split_head_update(struct ref_update *update,
						     struct ref_transaction *transaction,
						     const char *head_ref,
						     struct strbuf *err)
{
	struct ref_update *new_update;

	if ((update->flags & REF_LOG_ONLY) ||
	    (update->flags & REF_SKIP_CREATE_REFLOG) ||
	    (update->flags & REF_IS_PRUNING) ||
	    (update->flags & REF_UPDATE_VIA_HEAD))
		return 0;

	if (strcmp(update->refname, head_ref))
		return 0;

	if (string_list_has_string(&transaction->refnames, "HEAD")) {
		strbuf_addf(err,
			    "multiple updates for 'HEAD' (including one "
			    "via its referent '%s') are not allowed",
			    update->refname);
		return REF_TRANSACTION_ERROR_NAME_CONFLICT;
	}

	new_update = ref_transaction_add_update(
			transaction, "HEAD",
			update->flags | REF_LOG_ONLY | REF_NO_DEREF,
			&update->new_oid, &update->old_oid, &update->peeled,
			NULL, NULL, update->committer_info, update->msg);
	new_update->parent_update = update;

	if (strcmp(new_update->refname, "HEAD"))
		BUG("%s unexpectedly not 'HEAD'", new_update->refname);

	return 0;
}

/*
 * update is for a symref that points at referent and doesn't have REF_NO_DEREF
 * set. Split it into a REF_LOG_ONLY|REF_NO_DEREF update of the symref plus a new
 * update of the referent (itself subject to splitting later). Mirrors the files
 * backend's split_symref_update().
 */
static enum ref_transaction_error split_symref_update(struct ref_update *update,
						      const char *referent,
						      struct ref_transaction *transaction,
						      struct strbuf *err)
{
	struct ref_update *new_update;
	unsigned int new_flags;

	if (string_list_has_string(&transaction->refnames, referent)) {
		strbuf_addf(err,
			    "multiple updates for '%s' (including one "
			    "via symref '%s') are not allowed",
			    referent, update->refname);
		return REF_TRANSACTION_ERROR_NAME_CONFLICT;
	}

	new_flags = update->flags;
	if (!strcmp(update->refname, "HEAD"))
		new_flags |= REF_UPDATE_VIA_HEAD;

	new_update = ref_transaction_add_update(
			transaction, referent, new_flags,
			update->new_target ? NULL : &update->new_oid,
			update->old_target ? NULL : &update->old_oid,
			&update->peeled, update->new_target, update->old_target,
			NULL, update->msg);

	new_update->parent_update = update;

	update->flags |= REF_LOG_ONLY | REF_NO_DEREF;

	return 0;
}

/*
 * The set of helpers that began a transaction in prepare(): main, the current
 * worktree, and any other-worktree helpers an update routed to. Carried via
 * transaction->backend_data so prepare's commit phase, finish, and abort drive
 * exactly those helpers through their own 2-phase commit. A helper that never
 * began is never sent finish/abort (which would hang or disturb the wrong
 * process), and every worktree an update routes to runs inside its own
 * transaction, keeping the update atomic across worktrees.
 */
struct helper_transaction_data {
	struct helper_process **began;
	size_t nr, alloc;
};

static int helper_txn_began(struct helper_transaction_data *data,
			    struct helper_process *hp)
{
	size_t i;
	for (i = 0; i < data->nr; i++)
		if (data->began[i] == hp)
			return 1;
	return 0;
}

static void helper_transaction_data_free(struct ref_transaction *transaction)
{
	struct helper_transaction_data *data = transaction->backend_data;
	if (!data)
		return;
	free(data->began);
	free(data);
	transaction->backend_data = NULL;
}

/*
 * Begin a transaction on hp the first time anything routes to it (a value
 * update or a reflog entry), recording it in data->began so prepare/finish/abort
 * cover it, and so every involved store runs its own 2-phase commit. Idempotent
 * per helper. Used by both the value-update loop and helper_reflog_update: a
 * reflog-only routing (a linked worktree's split HEAD reflog, which goes to the
 * worktree helper while the shared branch value goes to the main helper) must
 * begin its helper here, or the entry is silently dropped. Returns 0, -1 on a
 * begin protocol error.
 */
static int helper_ensure_txn(struct helper_transaction_data *data,
			     struct helper_process *hp, struct strbuf *err)
{
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	if (helper_txn_began(data, hp))
		return 0;

	helper_process_send(hp, "transaction-begin\n");
	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok")) {
		strbuf_addf(err, "transaction-begin failed: %s",
			    line.len ? line.buf : "EOF");
		ret = -1;
	} else {
		ALLOC_GROW(data->began, data->nr + 1, data->alloc);
		data->began[data->nr++] = hp;
	}
	strbuf_release(&line);
	return ret;
}

static int helper_reflog_update(struct helper_ref_store *refs,
				struct helper_transaction_data *data,
				struct ref_update *update, struct strbuf *err);

static int helper_transaction_prepare(struct ref_store *ref_store,
				      struct ref_transaction *transaction,
				      struct strbuf *err)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_WRITE, __func__);
	struct strbuf line = STRBUF_INIT;
	char *head_ref = NULL;
	int head_type = 0;
	int ret = 0;
	struct helper_transaction_data *data;
	size_t i;

	helper_process_ensure(refs->main_hp);
	/*
	 * The per-worktree helper is spawned lazily by the update loop below,
	 * only if an update actually routes to it; most transactions touch only
	 * shared refs (main_hp), so do not spawn wt_hp up front.
	 */

	if (!refs->main_hp->cap_transaction) {
		strbuf_addstr(err, "ref helper does not support transactions");
		return -1;
	}

	/*
	 * Reject directory/file conflicts before writing anything, matching the
	 * files and reftable backends: refs/heads/x and refs/heads/x/y cannot
	 * coexist. refs_verify_refnames_available() runs the check over our own
	 * ref store through the generic iterator (helper_ref_iterator /
	 * helper_read_raw_ref), so the helper need not enforce it. Collect the
	 * refs this transaction creates, those that do not yet exist and do
	 * not expect an existing value, the same set reftable's
	 * prepare_single_update() checks; an update of an existing name needs no
	 * check (the name is already in use by itself). Done before backend_data
	 * is allocated, so a conflict returns cleanly with nothing to abort.
	 */
	{
		struct string_list refnames_to_check = STRING_LIST_INIT_NODUP;
		struct strbuf referent = STRBUF_INIT;
		for (i = 0; i < transaction->nr; i++) {
			struct ref_update *update = transaction->updates[i];
			struct object_id cur;
			unsigned int rtype = 0;
			int ignore_errno;
			if (update->flags & REF_LOG_ONLY)
				continue;
			if ((update->flags & REF_HAVE_NEW) &&
			    is_null_oid(&update->new_oid))
				continue;	/* a deletion frees the name */
			if (ref_update_expects_existing_old_ref(update))
				continue;	/* updates an existing name */
			strbuf_reset(&referent);
			if (!refs_read_raw_ref(ref_store, update->refname, &cur,
					       &referent, &rtype, &ignore_errno))
				continue;	/* already exists: not a new name */
			/*
			 * Carry the update index so a conflict rejects exactly this
			 * update (refs_verify_refnames_available reads it back via
			 * item->util), the way reftable's prepare_single_update does.
			 */
			{
				struct string_list_item *item =
					string_list_append(&refnames_to_check, update->refname);
				item->util = xmalloc(sizeof(i));
				memcpy(item->util, &i, sizeof(i));
			}
		}
		/*
		 * Pass the transaction so a conflict rejects the offending update:
		 * with REF_TRANSACTION_ALLOW_FAILURE the other updates still apply
		 * (the value-update and reflog loops skip a set rejection_err),
		 * otherwise it fails the whole transaction, matching files/reftable.
		 */
		ret = refs_verify_refnames_available(
			ref_store, &refnames_to_check, &transaction->refnames,
			NULL, transaction,
			transaction->flags & REF_TRANSACTION_FLAG_INITIAL, err);
		string_list_clear(&refnames_to_check, 1);
		strbuf_release(&referent);
		if (ret < 0)
			return ret;
	}

	CALLOC_ARRAY(data, 1);
	transaction->backend_data = data;

	/*
	 * Resolve HEAD once before the loop, matching files backend's
	 * files_transaction_prepare(). If HEAD is a symref, record the
	 * target so split_head_update-style logic can create HEAD
	 * reflog entries when the target branch is updated.
	 */
	head_ref = refs_resolve_refdup(ref_store, "HEAD",
				       RESOLVE_REF_NO_RECURSE,
				       NULL, &head_type);
	if (head_ref && !(head_type & REF_ISSYMREF))
		FREE_AND_NULL(head_ref);

	/*
	 * Route each update to the correct helper, matching reftable's
	 * per-backend transaction splitting. Begin transactions lazily.
	 * Note: transaction->nr may grow as synthetic updates are added
	 * (same pattern as files and reftable backends).
	 */
	for (i = 0; i < transaction->nr; i++) {
		struct ref_update *update = transaction->updates[i];
		const char *refname = update->refname;
		const char *bare;
		struct helper_process *hp;
		char new_hex[GIT_MAX_HEXSZ + 1];
		char old_hex[GIT_MAX_HEXSZ + 1];

		/*
		 * A D/F conflict (below) may have rejected this update when the
		 * caller allowed partial failure (REF_TRANSACTION_ALLOW_FAILURE);
		 * skip it so it is neither written nor logged, as reftable's apply
		 * skips updates with a rejection_err.
		 */
		if (update->rejection_err)
			continue;

		/*
		 * Split symref updates using the shared implementation.
		 * This matches files' split_symref_update() and reftable's
		 * prepare_single_update(): the symref becomes LOG_ONLY and
		 * a new update for the referent is appended to the
		 * transaction (processed in a later iteration).
		 *
		 * Unlike files, which reads the symref target under the ref lock
		 * (lock_raw_ref), this resolution crosses the helper boundary via
		 * a plain "read" and so is best-effort, like the HEAD resolution
		 * above: it only selects WHICH ref the value write routes to.
		 * Correctness does not rely on it being race-free. split_symref_update
		 * carries the original old-value onto the referent update, which the
		 * helper verifies atomically under its own lock at commit (see
		 * helper.h "Helper obligations"). So a symref repointed concurrently
		 * cannot corrupt the store: the value lands on the branch resolved at
		 * command start (a benign race outcome), and the harmful cases (the
		 * ref turned into a symref under us, or the target was deleted) become
		 * an old-value mismatch the helper rejects. We deliberately do NOT add
		 * a backend-side recheck; it could not be made race-free across the
		 * boundary anyway (a TOCTOU), and the atomic old-value check is the
		 * contract that does cross it.
		 */
		if (!update->new_target && !(update->flags & REF_NO_DEREF)) {
			struct strbuf referent = STRBUF_INIT;
			struct object_id dummy_oid;
			unsigned int type = 0;
			int dummy_errno = 0;
			if (!helper_read_raw_ref(ref_store, refname,
						 &dummy_oid, &referent,
						 &type, &dummy_errno) &&
			    (type & REF_ISSYMREF)) {
				ret = split_symref_update(
					update, referent.buf, transaction, err);
			}
			strbuf_release(&referent);
			if (ret)
				break;
		}

		/* Add HEAD reflog entry when its target branch is updated */
		if (head_ref) {
			ret = split_head_update(
				update, transaction, head_ref, err);
			if (ret)
				break;
		}

		/* Skip log-only updates (symref originals after splitting) */
		if (update->flags & REF_LOG_ONLY)
			continue;

		hp = helper_for_ref(refs, refname, &bare);

		/*
		 * Begin a transaction on this helper the first time any update
		 * routes to it, the main store, the current worktree, or
		 * another worktree's helper alike, so every involved store
		 * runs its own 2-phase commit and the update stays atomic.
		 */
		if (helper_ensure_txn(data, hp, err)) {
			ret = -1;
			break;
		}

		if (update->new_target) {
			const char *tgt_bare;
			helper_for_ref(refs, update->new_target, &tgt_bare);
			/*
			 * Resolve the symref's pre-update value for its reflog
			 * entry (helper_reflog_update logs symref updates such as
			 * a HEAD retarget by checkout or branch -m) before the
			 * create-symref below changes the ref; an unborn ref
			 * leaves the old oid null, as files records it.
			 */
			refs_resolve_ref_unsafe(ref_store, refname,
						RESOLVE_REF_READING,
						&update->old_oid, NULL);
			if (update->msg && *update->msg)
				helper_process_send(hp,
					"transaction-create-symref %s %s %s\n",
					bare, tgt_bare, update->msg);
			else
				helper_process_send(hp,
					"transaction-create-symref %s %s\n",
					bare, tgt_bare);
		} else if (update->flags & REF_HAVE_NEW &&
			   is_null_oid(&update->new_oid)) {
			oid_to_hex_r(old_hex, &update->old_oid);
			if (update->msg && *update->msg)
				helper_process_send(hp,
					"transaction-delete %s %s %s\n",
					bare, old_hex, update->msg);
			else
				helper_process_send(hp,
					"transaction-delete %s %s\n",
					bare, old_hex);
		} else if (update->flags & REF_HAVE_OLD) {
			oid_to_hex_r(new_hex, &update->new_oid);
			oid_to_hex_r(old_hex, &update->old_oid);
			if (update->msg && *update->msg)
				helper_process_send(hp,
					"transaction-update %s %s %s %s\n",
					bare, new_hex, old_hex,
					update->msg);
			else
				helper_process_send(hp,
					"transaction-update %s %s %s\n",
					bare, new_hex, old_hex);
		} else {
			/*
			 * A forced update (no old-value precondition) carries a
			 * null update->old_oid, but a reflog entry must record the
			 * ref's actual prior value, as files does. The transaction
			 * lock is held (helper_ensure_txn above), so resolving the
			 * current value now is race-free, and this create sends no
			 * old to the helper, so reusing update->old_oid as the
			 * captured prior (read by helper_reflog_update) does not
			 * affect the write. A missing ref leaves it null, correct
			 * for a true creation. Mirrors the new_target path above.
			 */
			refs_resolve_ref_unsafe(ref_store, update->refname,
						RESOLVE_REF_READING,
						&update->old_oid, NULL);
			oid_to_hex_r(new_hex, &update->new_oid);
			if (update->msg && *update->msg)
				helper_process_send(hp,
					"transaction-create %s %s %s\n",
					bare, new_hex, update->msg);
			else
				helper_process_send(hp,
					"transaction-create %s %s\n",
					bare, new_hex);
		}

		if (helper_process_readline(hp, &line) == EOF ||
		    strcmp(line.buf, "ok")) {
			strbuf_addf(err,
				    "ref helper rejected update for '%s': %s",
				    update->refname,
				    line.len ? line.buf : "EOF");
			ret = -1;
			break;
		}
	}

	/*
	 * Log reflog entries inside the still-open transaction(s), so each lands
	 * atomically with its ref update and an abort discards both, matching
	 * files (reflog written under the ref lock) and reftable (log records in
	 * the same transaction). helper_reflog_update begins the routed helper on
	 * demand (helper_ensure_txn) when an entry will be written: a logged ref
	 * does NOT always share a helper with a begun value update, a linked
	 * worktree's split HEAD reflog routes to the worktree helper while the
	 * shared branch value routes to the main helper, so gating on "already
	 * began" here dropped that worktree HEAD reflog entry.
	 */
	for (i = 0; !ret && i < transaction->nr; i++) {
		if (helper_reflog_update(refs, data, transaction->updates[i], err))
			ret = -1;
	}

	/* Prepare every begun helper; abort them all on any failure. */
	for (i = 0; !ret && i < data->nr; i++) {
		helper_process_send(data->began[i], "transaction-prepare\n");
		if (helper_process_readline(data->began[i], &line) == EOF ||
		    strcmp(line.buf, "ok")) {
			if (starts_with(line.buf, "error "))
				strbuf_addstr(err, line.buf + 6);
			else
				strbuf_addf(err, "transaction-prepare failed: %s",
					    line.len ? line.buf : "EOF");
			ret = -1;
		}
	}

	if (ret) {
		for (i = 0; i < data->nr; i++) {
			helper_process_send(data->began[i], "transaction-abort\n");
			helper_process_readline(data->began[i], &line);
		}
	}

	free(head_ref);
	strbuf_release(&line);

	/*
	 * On success mark the transaction PREPARED, exactly as the files and
	 * reftable backends do at the end of their transaction_prepare. The shared
	 * ref_transaction_prepare() does not set this; without it a caller that
	 * prepares explicitly and then commits (e.g. refs_update_symref ->
	 * ref_transaction_prepare + ref_transaction_commit) leaves the transaction
	 * OPEN, so commit re-runs the whole prepare, re-sending every command and
	 * re-logging each reflog entry. The begun-helper set stays in
	 * transaction->backend_data for finish/abort. On failure the helpers were
	 * already aborted inline and the transaction stays OPEN (so neither finish
	 * nor abort runs), so release the set here.
	 */
	if (ret)
		helper_transaction_data_free(transaction);
	else
		transaction->state = REF_TRANSACTION_PREPARED;
	return ret;
}

static int helper_reflog_exists(struct ref_store *ref_store, const char *refname);

/*
 * Record a reflog entry for one committed update, matching the files backend's
 * logging decision: log when forced, when the ref is in an autocreate
 * namespace (gated by log_all_ref_updates), or when a reflog already exists.
 * A symref update (new_target, e.g. a HEAD retarget by checkout or branch -m)
 * logs with its resolved old (captured in prepare before the update applied)
 * and new oids, mirroring files' log_ref_write new_target path; a dangling
 * target is skipped, as files skips it.
 */
static int helper_reflog_update(struct helper_ref_store *refs,
				struct helper_transaction_data *data,
				struct ref_update *update, struct strbuf *err)
{
	struct helper_process *hp;
	const char *bare, *committer;
	enum log_refs_config cfg;
	struct object_id new_oid;
	char old_hex[GIT_MAX_HEXSZ + 1], new_hex[GIT_MAX_HEXSZ + 1];
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	/* A D/F-rejected update (partial failure) is not applied, so it is not
	 * logged either. */
	if (update->rejection_err)
		return 0;

	if (update->flags & REF_SKIP_CREATE_REFLOG)
		return 0;

	if (update->new_target) {
		/* Resolve the new target for the entry; skip a dangling symref. */
		if (!refs_resolve_ref_unsafe(&refs->base, update->new_target,
					     RESOLVE_REF_READING, &new_oid, NULL))
			return 0;
	} else if (update->flags & REF_HAVE_NEW) {
		oidcpy(&new_oid, &update->new_oid);
	} else {
		return 0;
	}

	cfg = refs->log_all_ref_updates;
	if (cfg == LOG_REFS_UNSET)
		cfg = is_bare_repository() ? LOG_REFS_NONE : LOG_REFS_NORMAL;

	if (!(update->flags & REF_FORCE_CREATE_REFLOG) &&
	    !should_autocreate_reflog(cfg, update->refname) &&
	    !helper_reflog_exists(&refs->base, update->refname))
		return 0;

	hp = helper_for_ref(refs, update->refname, &bare);
	/*
	 * Gate on the negotiated reflog capability: sending reflog-append to a
	 * helper that does not store reflogs can block waiting for a reply it will
	 * not send.
	 */
	if (!hp->cap_reflog)
		return 0;
	/*
	 * Begin a transaction on the routed helper if this entry is the only
	 * thing reaching it (a linked worktree's split HEAD reflog), so the entry
	 * is written inside a 2-phase commit and covered by finish/abort. No-op
	 * when a value update already began it. Done after the logging-policy
	 * early returns above, so a helper that writes no reflog never begins an
	 * empty transaction.
	 */
	if (helper_ensure_txn(data, hp, err))
		return -1;
	oid_to_hex_r(old_hex, &update->old_oid);
	oid_to_hex_r(new_hex, &new_oid);
	committer = update->committer_info ? update->committer_info
					   : git_committer_info(0);

	if (update->msg && *update->msg)
		helper_process_send(hp, "reflog-append %s %s %s %s\t%s\n",
				    bare, old_hex, new_hex, committer,
				    update->msg);
	else
		helper_process_send(hp, "reflog-append %s %s %s %s\n",
				    bare, old_hex, new_hex, committer);

	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok")) {
		strbuf_addf(err, "ref helper reflog-append failed for '%s': %s",
			    update->refname, line.len ? line.buf : "EOF");
		ret = -1;
	}
	strbuf_release(&line);
	return ret;
}

static int helper_transaction_finish(struct ref_store *ref_store UNUSED,
				     struct ref_transaction *transaction,
				     struct strbuf *err)
{
	struct helper_transaction_data *data = transaction->backend_data;
	struct strbuf line = STRBUF_INIT;
	int ret = 0;
	size_t i;

	/*
	 * Reflog entries were already recorded inside the transaction during
	 * prepare, so finish just commits each helper that began one.
	 */
	for (i = 0; data && i < data->nr; i++) {
		helper_process_send(data->began[i], "transaction-finish\n");
		if (helper_process_readline(data->began[i], &line) == EOF ||
		    strcmp(line.buf, "ok")) {
			if (!ret) {
				if (starts_with(line.buf, "error "))
					strbuf_addstr(err, line.buf + 6);
				else
					strbuf_addf(err, "transaction-finish failed: %s",
						    line.len ? line.buf : "EOF");
				ret = -1;
			}
		}
	}

	helper_transaction_data_free(transaction);
	strbuf_release(&line);
	transaction->state = REF_TRANSACTION_CLOSED;
	return ret;
}

static int helper_transaction_abort(struct ref_store *ref_store UNUSED,
				    struct ref_transaction *transaction,
				    struct strbuf *err UNUSED)
{
	struct helper_transaction_data *data = transaction->backend_data;
	struct strbuf line = STRBUF_INIT;
	size_t i;

	for (i = 0; data && i < data->nr; i++) {
		helper_process_send(data->began[i], "transaction-abort\n");
		helper_process_readline(data->began[i], &line);
	}
	helper_transaction_data_free(transaction);
	strbuf_release(&line);
	transaction->state = REF_TRANSACTION_CLOSED;
	return 0;
}

/* ---- Optional operations ---- */

static int helper_optimize(struct ref_store *ref_store UNUSED,
			   struct refs_optimize_opts *opts UNUSED)
{
	return 0;
}

static int helper_optimize_required(struct ref_store *ref_store UNUSED,
				    struct refs_optimize_opts *opts UNUSED,
				    bool *required)
{
	*required = 0;
	return 0;
}

/*
 * Send one reflog entry for a rename/copy over an open transaction, honouring
 * the same logging policy as helper_reflog_update (log when forced by an
 * existing reflog or an autocreate namespace). Returns 0 on success, including
 * when policy says not to log; -1 on a helper protocol error.
 */
static int helper_send_rename_reflog(struct helper_process *hp,
				     struct helper_ref_store *refs,
				     const char *bare, const char *refname,
				     const struct object_id *old_oid,
				     const struct object_id *new_oid,
				     const char *msg, struct strbuf *line)
{
	enum log_refs_config cfg = refs->log_all_ref_updates;
	char old_hex[GIT_MAX_HEXSZ + 1], new_hex[GIT_MAX_HEXSZ + 1];
	const char *committer;

	if (!hp->cap_reflog)
		return 0;
	if (cfg == LOG_REFS_UNSET)
		cfg = is_bare_repository() ? LOG_REFS_NONE : LOG_REFS_NORMAL;
	if (!should_autocreate_reflog(cfg, refname) &&
	    !helper_reflog_exists(&refs->base, refname))
		return 0;

	oid_to_hex_r(old_hex, old_oid);
	oid_to_hex_r(new_hex, new_oid);
	committer = git_committer_info(0);
	if (msg && *msg)
		helper_process_send(hp, "reflog-append %s %s %s %s\t%s\n",
				    bare, old_hex, new_hex, committer, msg);
	else
		helper_process_send(hp, "reflog-append %s %s %s %s\n",
				    bare, old_hex, new_hex, committer);
	if (helper_process_readline(hp, line) == EOF || strcmp(line->buf, "ok"))
		return -1;
	return 0;
}

/*
 * Shared implementation for rename and copy. Like files (files_copy_or_rename_ref)
 * and reftable (write_copy_table), this is a dedicated path rather than a route
 * through the generic transaction: rename has no transaction primitive and the
 * reflog history cannot be expressed as ref updates. rename/copy operate on a
 * single ref store (builtin/branch.c always calls them on the main store, and
 * retargets every worktree HEAD itself via replace_each_worktree_head_symref),
 * so everything here drives one helper transaction.
 *
 * The verbs run in order inside one savepoint, so the carried history precedes
 * this operation's own entry and an abort discards all of it together:
 *   create newref (+ delete oldref on rename, under the value we just read so a
 *   concurrent change is rejected atomically), then reflog-copy the history
 *   (moved on rename so oldref keeps none, duplicated on copy), then the
 *   rename/copy's own reflog entry. (HEAD's own reflog entry on a current-branch
 *   rename comes from the caller's HEAD retarget via refs_update_symref, the same
 *   path as "checkout"; the helper does not yet log HEAD on a symref update.)
 * Logging is driven explicitly here, so a deleted oldref leaves no stub. A
 * helper lacking the optional reflog-copy capability simply does not carry the
 * history (dumb-store graceful absence); symref sources have no oid history.
 */
static int helper_rename_or_copy_ref(struct ref_store *ref_store,
				     const char *oldref,
				     const char *newref,
				     const char *logmsg,
				     int copy)
{
	struct helper_ref_store *refs =
		helper_downcast(ref_store, REF_STORE_WRITE, __func__);
	struct object_id oid;
	struct strbuf referent = STRBUF_INIT;
	struct strbuf line = STRBUF_INIT;
	unsigned int type = 0;
	int failure_errno = 0;
	int ret = 0;
	struct helper_process *hp;
	const char *old_bare, *new_bare;
	char old_hex[GIT_MAX_HEXSZ + 1];

	if (helper_read_raw_ref(ref_store, oldref, &oid, &referent,
				&type, &failure_errno)) {
		strbuf_release(&referent);
		return error(_("cannot %s '%s': ref does not exist"),
			     copy ? "copy" : "rename", oldref);
	}
	if (copy && (type & REF_ISSYMREF)) {
		strbuf_release(&referent);
		return error(_("refname %s is a symbolic ref, copying it is not supported"),
			     oldref);
	}

	hp = helper_for_ref(refs, newref, &new_bare);
	helper_for_ref(refs, oldref, &old_bare);
	helper_process_ensure(hp);
	if (!hp->cap_transaction) {
		strbuf_release(&referent);
		return error("ref helper does not support transactions");
	}

	helper_process_send(hp, "transaction-begin\n");
	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok")) {
		ret = error("transaction-begin failed: %s", line.len ? line.buf : "EOF");
		goto cleanup;
	}

	if (type & REF_ISSYMREF) {
		const char *tgt_bare;
		helper_for_ref(refs, referent.buf, &tgt_bare);
		helper_process_send(hp, "transaction-create-symref %s %s\n",
				    new_bare, tgt_bare);
	} else {
		oid_to_hex_r(old_hex, &oid);
		helper_process_send(hp, "transaction-create %s %s\n", new_bare, old_hex);
	}
	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok")) {
		ret = error("ref helper rejected create for '%s': %s",
			    newref, line.len ? line.buf : "EOF");
		goto abort;
	}

	if (!copy) {
		if (type & REF_ISSYMREF) {
			helper_process_send(hp, "transaction-delete %s\n", old_bare);
		} else {
			oid_to_hex_r(old_hex, &oid);
			helper_process_send(hp, "transaction-delete %s %s\n", old_bare, old_hex);
		}
		if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok")) {
			ret = error("ref helper rejected delete for '%s': %s",
				    oldref, line.len ? line.buf : "EOF");
			goto abort;
		}
	}

	/* Carry the history (moved on rename, duplicated on copy). */
	if (!(type & REF_ISSYMREF) && hp->cap_reflog) {
		helper_process_send(hp, "reflog-copy %s %s %d\n",
				    old_bare, new_bare, copy ? 0 : 1);
		if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok")) {
			ret = error("ref helper reflog-copy failed for '%s' -> '%s': %s",
				    oldref, newref, line.len ? line.buf : "EOF");
			goto abort;
		}
	}

	/* The rename/copy's own entry on newref. */
	if (!(type & REF_ISSYMREF) &&
	    helper_send_rename_reflog(hp, refs, new_bare, newref,
				      null_oid(ref_store->repo->hash_algo),
				      &oid, logmsg, &line)) {
		ret = error("ref helper reflog-append failed for '%s'", newref);
		goto abort;
	}

	helper_process_send(hp, "transaction-prepare\n");
	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok")) {
		ret = error("transaction-prepare failed: %s", line.len ? line.buf : "EOF");
		goto abort;
	}
	helper_process_send(hp, "transaction-finish\n");
	if (helper_process_readline(hp, &line) == EOF || strcmp(line.buf, "ok"))
		ret = error("transaction-finish failed: %s", line.len ? line.buf : "EOF");
	goto cleanup;

abort:
	helper_process_send(hp, "transaction-abort\n");
	helper_process_readline(hp, &line);
cleanup:
	strbuf_release(&line);
	strbuf_release(&referent);
	return ret;
}

static int helper_rename_ref(struct ref_store *ref_store,
			     const char *oldref,
			     const char *newref,
			     const char *logmsg)
{
	return helper_rename_or_copy_ref(ref_store, oldref, newref, logmsg, 0);
}

static int helper_copy_ref(struct ref_store *ref_store,
			   const char *oldref,
			   const char *newref,
			   const char *logmsg)
{
	return helper_rename_or_copy_ref(ref_store, oldref, newref, logmsg, 1);
}

/* ---- Reflog operations ---- */

/*
 * Parse a reflog line in the standard format:
 *   <old-oid> SP <new-oid> SP <committer> SP <timestamp> SP <tz> TAB <msg>
 * Returns 0 on success, -1 on parse failure.
 */
/*
 * Parse one reflog line in place, mirroring the contract of the files
 * backend's show_one_reflog_ent(): the committer is terminated at
 * "name <email>" and the message keeps its trailing newline, which the
 * each_reflog_ent_fn consumers (e.g. "git reflog show") rely on. The buffer
 * is mutated, so it must be writable and newline-terminated.
 */
static int parse_helper_reflog_line(const struct git_hash_algo *algo,
				    char *line,
				    struct object_id *ooid,
				    struct object_id *noid,
				    const char **committer,
				    timestamp_t *timestamp,
				    int *tz,
				    const char **message)
{
	const char *p = line;
	char *email_end;
	char *ts_end;

	/* old SP new SP name <email> SP time TAB msg LF */
	if (parse_oid_hex_algop(p, ooid, &p, algo) || *p++ != ' ')
		return -1;
	if (parse_oid_hex_algop(p, noid, &p, algo) || *p++ != ' ')
		return -1;

	*committer = p;

	email_end = strchr(p, '>');
	if (!email_end || email_end[1] != ' ')
		return -1;

	*timestamp = parse_timestamp(email_end + 2, &ts_end, 10);
	if (!ts_end || *ts_end != ' ')
		return -1;

	*tz = strtol(ts_end + 1, &ts_end, 10);

	email_end[1] = '\0';

	if (*ts_end == '\t')
		*message = ts_end + 1;
	else
		*message = ts_end;

	return 0;
}

/*
 * Read the entire reflog reply the caller just requested into memory, then
 * invoke fn for each entry. The reply MUST be fully buffered before fn runs:
 * fn reads objects through the same helper, so calling it mid-stream would
 * re-enter the shared pipe and desync it (the same eager-read rationale as
 * helper_ref_iterator_list). With reverse set, entries are replayed in the
 * opposite of the received order.
 */
static int helper_replay_reflog(struct helper_process *hp,
				const struct git_hash_algo *algo,
				const char *refname, int reverse,
				each_reflog_ent_fn fn, void *cb_data)
{
	struct strbuf line = STRBUF_INIT;
	struct strbuf *entries = NULL;
	size_t nr = 0, alloc = 0, i;
	int ret = 0;

	while (helper_process_readline(hp, &line) != EOF) {
		if (!line.len)
			break;
		strbuf_addch(&line, '\n');	/* restore the LF the protocol strips */
		ALLOC_GROW(entries, nr + 1, alloc);
		strbuf_init(&entries[nr], line.len);
		strbuf_addbuf(&entries[nr], &line);
		nr++;
	}
	strbuf_release(&line);

	for (i = 0; i < nr && !ret; i++) {
		size_t idx = reverse ? nr - 1 - i : i;
		struct object_id ooid, noid;
		const char *committer_info, *message;
		timestamp_t timestamp;
		int tz_val;

		if (parse_helper_reflog_line(algo, entries[idx].buf, &ooid, &noid,
					     &committer_info, &timestamp,
					     &tz_val, &message))
			continue;
		ret = fn(refname, &ooid, &noid, committer_info, timestamp,
			 tz_val, message, cb_data);
	}

	for (i = 0; i < nr; i++)
		strbuf_release(&entries[i]);
	free(entries);
	return ret;
}

static int helper_for_each_reflog_ent(struct ref_store *ref_store,
				      const char *refname,
				      each_reflog_ent_fn fn,
				      void *cb_data)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_READ, __func__);
	const char *bare;
	struct helper_process *hp = helper_for_ref(refs, refname, &bare);

	helper_process_ensure(hp);

	if (!hp->cap_reflog)
		return 0;

	helper_process_send(hp, "reflog-read %s\n", bare);
	return helper_replay_reflog(hp, ref_store->repo->hash_algo, refname, 0,
				    fn, cb_data);
}

static int helper_for_each_reflog_ent_reverse(
		struct ref_store *ref_store,
		const char *refname,
		each_reflog_ent_fn fn,
		void *cb_data)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_READ, __func__);
	const char *bare;
	struct helper_process *hp = helper_for_ref(refs, refname, &bare);

	helper_process_ensure(hp);

	/* When the helper can send newest-first, buffer that reply and replay in
	 * received order; otherwise read oldest-first and replay in reverse. */
	if (hp->cap_reflog_read_reverse) {
		helper_process_send(hp, "reflog-read-reverse %s\n", bare);
		return helper_replay_reflog(hp, ref_store->repo->hash_algo,
					    refname, 0, fn, cb_data);
	}

	if (!hp->cap_reflog)
		return 0;

	helper_process_send(hp, "reflog-read %s\n", bare);
	return helper_replay_reflog(hp, ref_store->repo->hash_algo, refname, 1,
				    fn, cb_data);
}

/* ---- Reflog iterator ---- */

struct helper_reflog_iterator {
	struct ref_iterator base;
	char **names;
	size_t nr, pos, alloc;
};

static int helper_reflog_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct helper_reflog_iterator *iter =
		(struct helper_reflog_iterator *)ref_iterator;

	if (iter->pos >= iter->nr)
		return ITER_DONE;

	/* names[] outlives this call (freed in release), so the ref name
	 * stays valid until the next advance, per the iterator contract. */
	iter->base.ref.name = iter->names[iter->pos++];
	return ITER_OK;
}

static int helper_reflog_iterator_seek(struct ref_iterator *ref_iterator UNUSED,
				       const char *prefix UNUSED,
				       unsigned int flags UNUSED)
{
	BUG("helper reflog iterator cannot be seeked");
	return -1;
}

static void helper_reflog_iterator_release(struct ref_iterator *ref_iterator)
{
	struct helper_reflog_iterator *iter =
		(struct helper_reflog_iterator *)ref_iterator;
	size_t i;
	/* The whole reflog-list response was consumed in begin(); no pipe to
	 * drain here. */
	for (i = 0; i < iter->nr; i++)
		free(iter->names[i]);
	free(iter->names);
}

static struct ref_iterator_vtable helper_reflog_iterator_vtable = {
	.advance = helper_reflog_iterator_advance,
	.seek = helper_reflog_iterator_seek,
	.release = helper_reflog_iterator_release,
};

static struct ref_iterator *helper_reflog_iterator_begin(
		struct ref_store *ref_store)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_READ, __func__);
	struct helper_process *hp = refs->main_hp;
	struct helper_reflog_iterator *iter;
	struct strbuf line = STRBUF_INIT;
	struct strbuf last = STRBUF_INIT;

	helper_process_ensure(hp);

	if (!hp->cap_reflog)
		return empty_ref_iterator_begin();

	CALLOC_ARRAY(iter, 1);
	base_ref_iterator_init(&iter->base, &helper_reflog_iterator_vtable);

	/*
	 * Read the entire reflog-list response up front, before returning to the
	 * caller. The single helper pipe is shared, so streaming names lazily
	 * across advance() calls desyncs the stream if a consumer's callback
	 * re-enters the helper (git fsck, git refs migrate). Mirrors
	 * helper_ref_iterator_list. Dedup consecutive names and drop malformed
	 * ones here, as the lazy version did; reflog-list is refname-sorted by
	 * contract (helper.h), so consecutive dedup is sufficient.
	 */
	helper_process_send(hp, "reflog-list\n");
	while (helper_process_readline(hp, &line) != EOF) {
		if (!line.len)
			break;
		if (iter->nr && !strcmp(line.buf, last.buf))
			continue;
		if (check_refname_format(line.buf, REFNAME_ALLOW_ONELEVEL))
			continue;
		strbuf_reset(&last);
		strbuf_addstr(&last, line.buf);
		ALLOC_GROW(iter->names, iter->nr + 1, iter->alloc);
		iter->names[iter->nr++] = xstrdup(line.buf);
	}
	strbuf_release(&line);
	strbuf_release(&last);

	return &iter->base;
}

static int helper_reflog_exists(struct ref_store *ref_store,
				const char *refname)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_READ, __func__);
	const char *bare;
	struct helper_process *hp = helper_for_ref(refs, refname, &bare);
	struct strbuf line = STRBUF_INIT;
	int exists;

	helper_process_ensure(hp);

	if (!hp->cap_reflog)
		return 0;

	helper_process_send(hp, "reflog-exists %s\n", bare);
	if (helper_process_readline(hp, &line) == EOF) {
		strbuf_release(&line);
		return 0;
	}
	exists = !strcmp(line.buf, "true");
	strbuf_release(&line);
	return exists;
}

static int helper_create_reflog(struct ref_store *ref_store UNUSED,
				const char *refname UNUSED,
				struct strbuf *err UNUSED)
{
	/* Reflog creation is implicit on first append. */
	return 0;
}

static int helper_delete_reflog(struct ref_store *ref_store,
				const char *refname)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_WRITE, __func__);
	const char *bare;
	struct helper_process *hp = helper_for_ref(refs, refname, &bare);
	struct strbuf line = STRBUF_INIT;

	helper_process_ensure(hp);

	if (!hp->cap_reflog)
		return 0;

	helper_process_send(hp, "reflog-delete %s\n", bare);
	helper_process_readline(hp, &line);
	strbuf_release(&line);
	return 0;
}

static int helper_reflog_expire(struct ref_store *ref_store UNUSED,
				const char *refname UNUSED,
				unsigned int flags UNUSED,
				reflog_expiry_prepare_fn prepare_fn UNUSED,
				reflog_expiry_should_prune_fn should_prune_fn UNUSED,
				reflog_expiry_cleanup_fn cleanup_fn UNUSED,
				void *policy_cb_data UNUSED)
{
	/* Expiry is delegated to the helper's own GC. */
	return 0;
}

static int helper_fsck(struct ref_store *ref_store UNUSED,
		       struct fsck_options *o UNUSED,
		       struct worktree *wt UNUSED)
{
	/* Integrity checking is delegated to the helper. */
	return 0;
}

/* ---- Backend registration ---- */

struct ref_storage_be refs_be_helper = {
	.name = "helper",
	.init = helper_ref_store_init,
	.release = helper_ref_store_release,
	.create_on_disk = helper_create_on_disk,
	.remove_on_disk = helper_remove_on_disk,

	.transaction_prepare = helper_transaction_prepare,
	.transaction_finish = helper_transaction_finish,
	.transaction_abort = helper_transaction_abort,

	.optimize = helper_optimize,
	.optimize_required = helper_optimize_required,
	.rename_ref = helper_rename_ref,
	.copy_ref = helper_copy_ref,

	.iterator_begin = helper_ref_iterator_begin,
	.read_raw_ref = helper_read_raw_ref,
	.read_symbolic_ref = helper_read_symbolic_ref,

	.reflog_iterator_begin = helper_reflog_iterator_begin,
	.for_each_reflog_ent = helper_for_each_reflog_ent,
	.for_each_reflog_ent_reverse = helper_for_each_reflog_ent_reverse,
	.reflog_exists = helper_reflog_exists,
	.create_reflog = helper_create_reflog,
	.delete_reflog = helper_delete_reflog,
	.reflog_expire = helper_reflog_expire,

	.fsck = helper_fsck,

};
