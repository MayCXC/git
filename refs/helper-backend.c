/*
 * Ref helper backend: delegate ref storage to an external process.
 *
 * Uses the shared helper process (helper.c) which also handles ODB
 * operations in the same binary, matching how remote helpers handle
 * both refs and objects.
 */
#include "../git-compat-util.h"
#include "../config.h"
#include "../gettext.h"
#include "../hash.h"
#include "../hex.h"
#include "../object.h"
#include "../helper.h"
#include "../repository.h"
#include "../strbuf.h"
#include "../strmap.h"
#include "refs-internal.h"
#include "helper-backend.h"

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
					       const char *payload,
					       const char *gitdir,
					       unsigned int flags)
{
	struct helper_ref_store *refs = xcalloc(1, sizeof(*refs));
	const char *name;

	/*
	 * The helper name comes from repo->local_helper (set during
	 * config reading from extensions.localHelper), from the ref
	 * storage payload (URI-style GIT_REFERENCE_BACKEND=helper://name),
	 * or as a fallback from the repo config (for worktree
	 * subprocesses where the early allocation did not run).
	 */
	if (repo->local_helper)
		name = repo->local_helper->name;
	else if (payload && *payload)
		name = payload;
	else {
		char *cfg_name = NULL;
		repo_config_get_string(repo, "extensions.localhelper",
				       &cfg_name);
		if (cfg_name && *cfg_name)
			name = cfg_name;
		else
			die(_("ref helper backend requires extensions.localHelper"));
	}

	base_ref_store_init(&refs->base, repo, gitdir, &refs_be_helper);
	refs->store_flags = flags;

	/*
	 * Set up helper processes matching reftable's two-backend model.
	 *
	 * main_hp: shared helper at commondir, handles shared refs
	 * (branches, tags) and main worktree refs. Also shared with
	 * the ODB via repo->local_helper.
	 *
	 * wt_hp: per-worktree helper at the worktree gitdir, handles
	 * per-worktree refs (HEAD, bisect, etc.). Only created
	 * for linked worktrees (gitdir != commondir).
	 */
	if (!repo->local_helper) {
		const char *main_dir = repo->commondir ? repo->commondir : gitdir;
		repo->local_helper = xcalloc(1, sizeof(*repo->local_helper));
		helper_process_init(repo->local_helper, name, main_dir);
	} else if (!repo->local_helper->gitdir) {
		const char *main_dir = repo->commondir ? repo->commondir : gitdir;
		repo->local_helper->gitdir = xstrdup(main_dir);
	}
	refs->main_hp = repo->local_helper;
	strmap_init(&refs->worktree_helpers);

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
	 * main_hp is shared via repo->local_helper, so don't release it.
	 * wt_hp is owned by this ref store (allocated in init for linked
	 * worktrees), so release it here.
	 */
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

	if (!hp->cap_create) {
		strbuf_addstr(err, "ref helper does not support create");
		return -1;
	}

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
		return -1;
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
	const struct git_hash_algo *hash_algo;
	unsigned int flags;
	struct helper_ref_entry *entries;
	size_t nr, pos;
	struct object_id oid;
	char *current_refname;
	char *current_target;
};

static int helper_ref_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct helper_ref_iterator *iter =
		(struct helper_ref_iterator *)ref_iterator;

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

static int helper_ref_iterator_seek(struct ref_iterator *ref_iterator UNUSED,
				    const char *prefix UNUSED,
				    unsigned int flags UNUSED)
{
	return -1;
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
	struct strbuf line = STRBUF_INIT;
	size_t alloc = 0;

	helper_process_ensure(hp);

	if (!hp->cap_list)
		return empty_ref_iterator_begin();

	CALLOC_ARRAY(iter, 1);
	base_ref_iterator_init(&iter->base, &helper_ref_iterator_vtable);
	iter->ref_store = ref_store;
	iter->hash_algo = ref_store->repo->hash_algo;
	iter->flags = flags;

	/*
	 * Send list command and eagerly read all lines into memory.
	 * This frees the shared pipe for ODB commands immediately.
	 */
	if (prefix && *prefix)
		helper_process_send(hp, "list %s\n", prefix);
	else
		helper_process_send(hp, "list\n");

	while (helper_process_readline(hp, &line) != EOF) {
		if (!line.len)
			break;
		ALLOC_GROW(iter->entries, iter->nr + 1, alloc);
		iter->entries[iter->nr++].line = xstrdup(line.buf);
	}
	strbuf_release(&line);

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

static int helper_transaction_prepare(struct ref_store *ref_store,
				      struct ref_transaction *transaction,
				      struct strbuf *err)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_WRITE, __func__);
	struct strbuf line = STRBUF_INIT;
	char *head_ref = NULL;
	int head_type = 0;
	int ret = 0;
	int main_active = 0, wt_active = 0;
	size_t i;

	helper_process_ensure(refs->main_hp);
	if (refs->wt_hp)
		helper_process_ensure(refs->wt_hp);

	if (!refs->main_hp->cap_transaction) {
		strbuf_addstr(err, "ref helper does not support transactions");
		return -1;
	}

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
		 * Split symref updates using the shared implementation.
		 * This matches files' split_symref_update() and reftable's
		 * prepare_single_update(): the symref becomes LOG_ONLY and
		 * a new update for the referent is appended to the
		 * transaction (processed in a later iteration).
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
				ret = refs_transaction_split_symref_update(
					update, referent.buf, transaction, err);
			}
			strbuf_release(&referent);
			if (ret)
				break;
		}

		/* Add HEAD reflog entry when its target branch is updated */
		if (head_ref) {
			ret = refs_transaction_split_head_update(
				update, transaction, head_ref, err);
			if (ret)
				break;
		}

		/* Skip log-only updates (symref originals after splitting) */
		if (update->flags & REF_LOG_ONLY)
			continue;

		hp = helper_for_ref(refs, refname, &bare);

		/* Lazily begin transaction on each helper as needed */
		if (hp == refs->main_hp && !main_active) {
			helper_process_send(hp, "transaction-begin\n");
			if (helper_process_readline(hp, &line) == EOF ||
			    strcmp(line.buf, "ok")) {
				strbuf_addf(err, "transaction-begin failed: %s",
					    line.len ? line.buf : "EOF");
				ret = -1;
				break;
			}
			main_active = 1;
		} else if (hp == refs->wt_hp && !wt_active) {
			helper_process_send(hp, "transaction-begin\n");
			if (helper_process_readline(hp, &line) == EOF ||
			    strcmp(line.buf, "ok")) {
				strbuf_addf(err, "worktree transaction-begin failed: %s",
					    line.len ? line.buf : "EOF");
				ret = -1;
				break;
			}
			wt_active = 1;
		}

		if (update->new_target) {
			const char *tgt_bare;
			helper_for_ref(refs, update->new_target, &tgt_bare);
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

	/* Prepare all active transactions */
	if (!ret && main_active) {
		helper_process_send(refs->main_hp, "transaction-prepare\n");
		if (helper_process_readline(refs->main_hp, &line) == EOF ||
		    strcmp(line.buf, "ok")) {
			if (starts_with(line.buf, "error "))
				strbuf_addstr(err, line.buf + 6);
			else
				strbuf_addf(err, "transaction-prepare failed: %s",
					    line.len ? line.buf : "EOF");
			ret = -1;
		}
	}
	if (!ret && wt_active) {
		helper_process_send(refs->wt_hp, "transaction-prepare\n");
		if (helper_process_readline(refs->wt_hp, &line) == EOF ||
		    strcmp(line.buf, "ok")) {
			if (starts_with(line.buf, "error "))
				strbuf_addstr(err, line.buf + 6);
			else
				strbuf_addf(err, "worktree transaction-prepare failed: %s",
					    line.len ? line.buf : "EOF");
			ret = -1;
			/* Abort the main transaction that already prepared */
			if (main_active) {
				helper_process_send(refs->main_hp, "transaction-abort\n");
				helper_process_readline(refs->main_hp, &line);
			}
		}
	}

	/* Abort all active transactions on failure */
	if (ret) {
		if (main_active) {
			helper_process_send(refs->main_hp, "transaction-abort\n");
			helper_process_readline(refs->main_hp, &line);
		}
		if (wt_active) {
			helper_process_send(refs->wt_hp, "transaction-abort\n");
			helper_process_readline(refs->wt_hp, &line);
		}
	}

	free(head_ref);
	strbuf_release(&line);
	return ret;
}

static int helper_transaction_finish(struct ref_store *ref_store,
				     struct ref_transaction *transaction UNUSED,
				     struct strbuf *err)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, 0, __func__);
	struct strbuf line = STRBUF_INIT;
	int ret = 0;

	helper_process_send(refs->main_hp, "transaction-finish\n");
	if (helper_process_readline(refs->main_hp, &line) == EOF ||
	    strcmp(line.buf, "ok")) {
		if (starts_with(line.buf, "error "))
			strbuf_addstr(err, line.buf + 6);
		else
			strbuf_addf(err, "transaction-finish failed: %s",
				    line.len ? line.buf : "EOF");
		ret = -1;
	}

	if (refs->wt_hp) {
		helper_process_send(refs->wt_hp, "transaction-finish\n");
		if (helper_process_readline(refs->wt_hp, &line) == EOF ||
		    strcmp(line.buf, "ok")) {
			if (!ret) {
				if (starts_with(line.buf, "error "))
					strbuf_addstr(err, line.buf + 6);
				else
					strbuf_addf(err, "worktree transaction-finish failed: %s",
						    line.len ? line.buf : "EOF");
				ret = -1;
			}
		}
	}

	strbuf_release(&line);
	return ret;
}

static int helper_transaction_abort(struct ref_store *ref_store,
				    struct ref_transaction *transaction UNUSED,
				    struct strbuf *err UNUSED)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, 0, __func__);
	struct strbuf line = STRBUF_INIT;

	helper_process_send(refs->main_hp, "transaction-abort\n");
	helper_process_readline(refs->main_hp, &line);
	if (refs->wt_hp) {
		helper_process_send(refs->wt_hp, "transaction-abort\n");
		helper_process_readline(refs->wt_hp, &line);
	}
	strbuf_release(&line);
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
 * Shared implementation for rename and copy. Both read the old ref,
 * create the new one via a transaction, and optionally delete the old.
 */
static int helper_rename_or_copy_ref(struct ref_store *ref_store,
				     const char *oldref,
				     const char *newref,
				     const char *logmsg,
				     int delete_old)
{
	struct object_id oid;
	struct strbuf referent = STRBUF_INIT;
	struct strbuf errbuf = STRBUF_INIT;
	unsigned int type = 0;
	int failure_errno = 0;
	int ret;
	struct ref_transaction *transaction;

	if (helper_read_raw_ref(ref_store, oldref, &oid, &referent,
				&type, &failure_errno))
		return error(_("cannot %s '%s': ref does not exist"),
			     delete_old ? "rename" : "copy", oldref);

	transaction = ref_store_transaction_begin(ref_store, 0, &errbuf);
	if (!transaction) {
		error("%s", errbuf.buf);
		strbuf_release(&errbuf);
		strbuf_release(&referent);
		return -1;
	}

	if (type & REF_ISSYMREF)
		ret = ref_transaction_update(transaction, newref,
					     NULL, NULL,
					     referent.buf, NULL,
					     0, logmsg, &errbuf);
	else
		ret = ref_transaction_update(transaction, newref,
					     &oid, NULL,
					     NULL, NULL,
					     0, logmsg, &errbuf);
	if (!ret && delete_old)
		ret = ref_transaction_delete(transaction, oldref,
					     NULL, NULL, 0,
					     logmsg, &errbuf);
	if (!ret)
		ret = ref_transaction_commit(transaction, &errbuf);

	if (ret)
		error("%s", errbuf.buf);

	ref_transaction_free(transaction);
	strbuf_release(&errbuf);
	strbuf_release(&referent);
	return ret;
}

static int helper_rename_ref(struct ref_store *ref_store,
			     const char *oldref,
			     const char *newref,
			     const char *logmsg)
{
	return helper_rename_or_copy_ref(ref_store, oldref, newref, logmsg, 1);
}

static int helper_copy_ref(struct ref_store *ref_store,
			   const char *oldref,
			   const char *newref,
			   const char *logmsg)
{
	return helper_rename_or_copy_ref(ref_store, oldref, newref, logmsg, 0);
}

/* ---- Reflog operations ---- */

/*
 * Parse a reflog line in the standard format:
 *   <old-oid> SP <new-oid> SP <committer> SP <timestamp> SP <tz> TAB <msg>
 * Returns 0 on success, -1 on parse failure.
 */
static int parse_helper_reflog_line(const struct git_hash_algo *algo,
				    const char *line,
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

	if (*ts_end == '\t')
		*message = ts_end + 1;
	else
		*message = ts_end;

	return 0;
}

static int helper_for_each_reflog_ent(struct ref_store *ref_store,
				      const char *refname,
				      each_reflog_ent_fn fn,
				      void *cb_data)
{
	struct helper_ref_store *refs = helper_downcast(ref_store, REF_STORE_READ, __func__);
	const char *bare;
	struct helper_process *hp = helper_for_ref(refs, refname, &bare);
	struct strbuf line = STRBUF_INIT;

	helper_process_ensure(hp);

	if (!hp->cap_reflog_read)
		return 0;

	helper_process_send(hp, "reflog-read %s\n", bare);

	while (helper_process_readline(hp, &line) != EOF) {
		struct object_id ooid, noid;
		const char *committer_info, *message;
		timestamp_t timestamp;
		int tz_val;
		int ret;

		if (!line.len)
			break;

		if (parse_helper_reflog_line(ref_store->repo->hash_algo,
					     line.buf, &ooid, &noid,
					     &committer_info, &timestamp,
					     &tz_val, &message))
			continue;

		ret = fn(refname, &ooid, &noid, committer_info, timestamp,
			 tz_val, message, cb_data);
		if (ret) {
			helper_process_drain(hp);
			strbuf_release(&line);
			return ret;
		}
	}
	strbuf_release(&line);
	return 0;
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
	struct strbuf line = STRBUF_INIT;

	helper_process_ensure(hp);

	if (hp->cap_reflog_read_reverse) {
		/* Stream directly: helper sends entries newest-first */
		helper_process_send(hp, "reflog-read-reverse %s\n", bare);

		while (helper_process_readline(hp, &line) != EOF) {
			struct object_id ooid, noid;
			const char *committer_info, *message;
			timestamp_t timestamp;
			int tz_val;
			int ret;

			if (!line.len)
				break;

			if (parse_helper_reflog_line(ref_store->repo->hash_algo,
						     line.buf, &ooid, &noid,
						     &committer_info,
						     &timestamp,
						     &tz_val, &message))
				continue;

			ret = fn(refname, &ooid, &noid, committer_info,
				 timestamp, tz_val, message, cb_data);
			if (ret) {
				helper_process_drain(hp);
				strbuf_release(&line);
				return ret;
			}
		}
		strbuf_release(&line);
		return 0;
	}

	/* Fallback: collect all entries and replay in reverse */
	if (!hp->cap_reflog_read)
		return 0;

	{
		struct strbuf *entries = NULL;
		size_t nr = 0, alloc = 0;
		size_t i;

		helper_process_send(hp, "reflog-read %s\n", bare);

		while (helper_process_readline(hp, &line) != EOF) {
			if (!line.len)
				break;
			ALLOC_GROW(entries, nr + 1, alloc);
			strbuf_init(&entries[nr], line.len);
			strbuf_addbuf(&entries[nr], &line);
			nr++;
		}
		strbuf_release(&line);

		for (i = nr; i > 0; i--) {
			struct object_id ooid, noid;
			const char *committer_info, *message;
			timestamp_t timestamp;
			int tz_val;
			int ret;

			if (parse_helper_reflog_line(ref_store->repo->hash_algo,
						     entries[i - 1].buf,
						     &ooid, &noid,
						     &committer_info,
						     &timestamp,
						     &tz_val, &message))
				continue;

			ret = fn(refname, &ooid, &noid, committer_info,
				 timestamp, tz_val, message, cb_data);
			if (ret) {
				size_t j;
				for (j = 0; j < nr; j++)
					strbuf_release(&entries[j]);
				free(entries);
				return ret;
			}
		}

		for (i = 0; i < nr; i++)
			strbuf_release(&entries[i]);
		free(entries);
		return 0;
	}
}

/* ---- Reflog iterator ---- */

struct helper_reflog_iterator {
	struct ref_iterator base;
	struct helper_process *hp;
	struct strbuf line;
	struct strbuf last_name;
	char *current_refname;
	int done;
};

static int helper_reflog_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct helper_reflog_iterator *iter =
		(struct helper_reflog_iterator *)ref_iterator;

	if (iter->done)
		return ITER_DONE;

	free(iter->current_refname);
	iter->current_refname = NULL;

	while (helper_process_readline(iter->hp, &iter->line) != EOF) {
		const char *name;

		if (!iter->line.len) {
			iter->done = 1;
			return ITER_DONE;
		}

		name = iter->line.buf;

		/* Deduplicate: skip if same as last produced name */
		if (!strcmp(name, iter->last_name.buf))
			continue;

		if (check_refname_format(name,
					 REFNAME_ALLOW_ONELEVEL))
			continue;

		strbuf_reset(&iter->last_name);
		strbuf_addstr(&iter->last_name, name);
		iter->current_refname = xstrdup(name);
		iter->base.ref.name = iter->current_refname;
		return ITER_OK;
	}

	iter->done = 1;
	return ITER_DONE;
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
	if (!iter->done)
		helper_process_drain(iter->hp);
	strbuf_release(&iter->line);
	strbuf_release(&iter->last_name);
	free(iter->current_refname);
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

	helper_process_ensure(hp);

	if (!hp->cap_reflog_list)
		return empty_ref_iterator_begin();

	CALLOC_ARRAY(iter, 1);
	base_ref_iterator_init(&iter->base, &helper_reflog_iterator_vtable);
	strbuf_init(&iter->line, 0);
	strbuf_init(&iter->last_name, 0);
	iter->hp = hp;

	helper_process_send(hp, "reflog-list\n");

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

	if (!hp->cap_reflog_exists)
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

	if (!hp->cap_reflog_delete)
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
