#!/bin/sh

test_description='local helper ref-storage backend (extensions.refStorage=helper)

Exercises the pluggable ref backend "helper" end to end against the
git-local-testgit helper (the comprehensive odb+ref helper shared with t0620),
which keeps refs and reflogs as files under the repository directory.'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# git-local-testgit ships with t0620 and is found via PATH, the same way
# git-remote-testgit is in t5801.
PATH="$TEST_DIRECTORY/t0620:$PATH"

# As in t0620, these tests run against git's own bundled git-local-testgit with
# no dependency on any out-of-tree helper, the model t5801 uses for remote
# helpers.

# Create a repository whose ref storage is the helper, the same way t0610
# creates a reftable repository with --ref-format. Any name git does not
# recognize as a builtin ref format selects git-local-<name>, so "testgit" names
# the helper directly in extensions.refStorage; there is no separate key.
create_ref_helper_repo () {
	git init --ref-format=testgit "$1"
}

test_expect_success 'init --ref-format=<helper> records the extension' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	echo testgit >expect &&
	git -C repo config extensions.refStorage >actual &&
	test_cmp expect actual &&
	test_must_fail git -C repo config extensions.objectStorage
'

test_expect_success 'commit writes refs through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first
	) &&
	git -C repo rev-parse HEAD >expect &&
	git -C repo rev-parse refs/heads/main >actual &&
	test_cmp expect actual
'

test_expect_success 'branch and for-each-ref through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first &&
		git branch topic
	) &&
	git -C repo for-each-ref --format="%(refname)" refs/heads/ >actual &&
	cat >expect <<-\EOF &&
	refs/heads/main
	refs/heads/topic
	EOF
	test_cmp expect actual
'

test_expect_success 'delete a ref through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first &&
		git branch doomed &&
		git branch -D doomed
	) &&
	test_must_fail git -C repo rev-parse --verify -q refs/heads/doomed
'

test_expect_success 'rename a branch through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first &&
		git branch topic
	) &&
	topic_oid=$(git -C repo rev-parse topic) &&
	git -C repo branch -m topic renamed &&
	test_must_fail git -C repo rev-parse --verify -q refs/heads/topic &&
	echo "$topic_oid" >expect &&
	git -C repo rev-parse refs/heads/renamed >actual &&
	test_cmp expect actual
'

# A rename carries the source ref's whole reflog onto the new name and leaves
# none behind, matching files (which moves the reflog file) and reftable; this
# rename's own entry sits on top of the carried history. Exercises reflog-copy.
test_expect_success 'rename carries the reflog history through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		git branch topic &&
		git checkout topic &&
		test_commit two &&
		test_commit three &&
		git checkout main
	) &&
	git -C repo reflog show --format="%gs" refs/heads/topic >before &&
	git -C repo branch -m topic renamed &&
	test_must_fail git -C repo reflog exists refs/heads/topic &&
	git -C repo reflog exists refs/heads/renamed &&
	git -C repo reflog show --format="%gs" refs/heads/renamed >after &&
	tail -n +2 after >after-history &&
	test_cmp before after-history
'

# A copy duplicates the source ref's reflog onto the new name (git branch -c)
# and leaves the source reflog intact.
test_expect_success 'copy duplicates the reflog history through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		git branch topic &&
		git checkout topic &&
		test_commit two &&
		git checkout main
	) &&
	git -C repo reflog show --format="%gs" refs/heads/topic >before &&
	git -C repo branch -c topic dup &&
	git -C repo reflog exists refs/heads/topic &&
	git -C repo reflog show --format="%gs" refs/heads/topic >after-src &&
	test_cmp before after-src &&
	git -C repo reflog exists refs/heads/dup &&
	git -C repo reflog show --format="%gs" refs/heads/dup >after-dup &&
	tail -n +2 after-dup >after-dup-history &&
	test_cmp before after-dup-history
'

# Renaming the checked-out branch: HEAD follows to the new name (caller side)
# and the carry-over still leaves the old name reflog-less. Guards the
# interaction between the deletion-stub suppression and split_head_update.
test_expect_success 'rename the current branch carries reflog and moves HEAD' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		git checkout -b topic &&
		test_commit two
	) &&
	git -C repo reflog show --format="%gs" refs/heads/topic >before &&
	git -C repo branch -m renamed &&
	echo refs/heads/renamed >expect &&
	git -C repo symbolic-ref HEAD >actual &&
	test_cmp expect actual &&
	test_must_fail git -C repo reflog exists refs/heads/topic &&
	git -C repo reflog show --format="%gs" refs/heads/renamed >after &&
	tail -n +2 after >after-history &&
	test_cmp before after-history
'

# The helper enforces the old-value precondition the backend forwards: a
# compare-and-swap update whose expected old value is stale must be rejected
# atomically (the only race-free place to check is across the boundary, in the
# helper), leaving the ref unchanged.
test_expect_success 'update with a stale expected value is rejected' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		test_commit two
	) &&
	current=$(git -C repo rev-parse refs/heads/main) &&
	stale=$(git -C repo rev-parse refs/heads/main~1) &&
	test_must_fail git -C repo update-ref refs/heads/main "$stale" "$stale" &&
	echo "$current" >expect &&
	git -C repo rev-parse refs/heads/main >actual &&
	test_cmp expect actual
'

test_expect_success 'symbolic HEAD is read through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first
	) &&
	echo refs/heads/main >expect &&
	git -C repo symbolic-ref HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'reflog is recorded and read through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first &&
		test_commit second
	) &&
	git -C repo reflog exists refs/heads/main &&
	cat >expect <<-\EOF &&
	commit: second
	commit (initial): first
	EOF
	git -C repo reflog show --format="%gs" main >actual &&
	test_cmp expect actual &&
	cat >expect-committer <<-EOF &&
	$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	$GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>
	EOF
	git -C repo reflog show --format="%gn <%ge>" main >actual-committer &&
	test_cmp expect-committer actual-committer
'

# A symref update (HEAD retarget by checkout / branch -m) is logged to HEAD's
# reflog exactly once, like files/reftable. Guards two things: that symref
# updates are logged at all (helper_reflog_update used to skip new_target
# updates), and that the transaction PREPARED/CLOSED state is set so an explicit
# prepare-then-commit (refs_update_symref) does not re-run prepare and log twice.
test_expect_success 'HEAD reflog records symref updates once' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		git checkout -b topic &&
		test_commit two &&
		git branch -m renamed
	) &&
	cat >expect <<-\EOF &&
	Branch: renamed refs/heads/topic to refs/heads/renamed
	commit: two
	checkout: moving from main to topic
	commit (initial): one
	EOF
	git -C repo reflog show HEAD --format="%gs" >actual &&
	test_cmp expect actual
'

# A commit on a linked worktree updates the shared branch ref (routed to the
# main helper) while split_head_update emits a REF_LOG_ONLY entry for the
# worktree's own HEAD (routed to the worktree helper). The worktree helper is
# reached by nothing else in the transaction, so the reflog pass must begin a
# transaction on it (helper_ensure_txn) for the entry to land; gating reflog
# emission on a value update having already begun the helper dropped the
# worktree HEAD reflog entry, which files and reftable both record.
test_expect_success 'commit on a linked worktree records its HEAD reflog through the helper' '
	test_when_finished "rm -rf repo wt" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first
	) &&
	git -C repo worktree add ../wt &&
	(
		cd wt &&
		test_commit wt-change
	) &&
	git -C wt reflog exists HEAD &&
	git -C wt reflog show HEAD --format="%gs" >actual &&
	grep "commit: wt-change" actual
'

# "git branch"/"git tag" iterate a single ref class by seeking the iterator to
# a prefix, unlike for-each-ref which iterates all refs; this exercises the
# helper iterator's seek + prefix filtering.
test_expect_success 'branch and tag listing through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first &&
		git branch topic &&
		git tag v1
	) &&
	cat >expect <<-\EOF &&
	* main
	  topic
	EOF
	git -C repo branch >actual &&
	test_cmp expect actual &&
	cat >expect-tag <<-\EOF &&
	first
	v1
	EOF
	git -C repo tag >actual-tag &&
	test_cmp expect-tag actual-tag
'

test_expect_success 'branch listing spans worktrees through the helper' '
	test_when_finished "rm -rf repo wt" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first
	) &&
	git -C repo worktree add ../wt &&
	cat >expect <<-\EOF &&
	* main
	+ wt
	EOF
	git -C repo branch >actual &&
	test_cmp expect actual
'

# A repository whose objects AND refs are both the testgit helper runs two
# independent helper processes, one per subsystem; the ref helper must root at
# the common directory so refs do not scatter, which a worktree exposes.
test_expect_success 'combined object and ref helper repo supports worktrees' '
	test_when_finished "rm -rf combined wt2" &&
	git init --ref-format=testgit --object-storage=testgit combined &&
	echo testgit >expect-helper &&
	git -C combined config extensions.objectStorage >actual-obj &&
	test_cmp expect-helper actual-obj &&
	git -C combined config extensions.refStorage >actual-ref &&
	test_cmp expect-helper actual-ref &&
	(
		cd combined &&
		test_commit first
	) &&
	git -C combined worktree add ../wt2 &&
	cat >expect <<-\EOF &&
	* main
	+ wt2
	EOF
	git -C combined branch >actual &&
	test_cmp expect actual &&
	git -C wt2 log --oneline >wtlog &&
	test_line_count = 1 wtlog
'

# Regression: gc/prune reachability marks reflog entries, and for each one it
# parses the commit object through the SAME helper that is mid-stream reading
# the reflog. The reflog reply must be fully buffered before the callback runs,
# or the object read re-enters and desyncs the shared pipe (seen as a spurious
# "invalid object type" on a garbled oid). Needs both helpers: object-only
# repos read the reflog from files, so they never exercise this path.
test_expect_success 'gc/prune walk the reflog through the helper without desyncing' '
	test_when_finished "rm -rf reflogwalk" &&
	git init --ref-format=testgit --object-storage=testgit reflogwalk &&
	(
		cd reflogwalk &&
		test_commit one &&
		test_commit two &&
		test_commit three
	) &&
	git -C reflogwalk gc &&
	git -C reflogwalk prune --expire=now &&
	git -C reflogwalk fsck &&
	git -C reflogwalk rev-list --all >walked &&
	test_line_count = 3 walked
'

test_expect_success 'directory/file ref conflicts are rejected through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit first &&
		git update-ref refs/heads/dir/leaf HEAD &&
		# a ref cannot be created where another exists as a directory
		test_must_fail git update-ref refs/heads/dir HEAD 2>err &&
		test_grep -i "conflict\|exist\|D/F\|directory" err &&
		# nor a ref under one that already exists as a file
		git update-ref refs/heads/file HEAD &&
		test_must_fail git update-ref refs/heads/file/leaf HEAD 2>err2 &&
		test_grep -i "conflict\|exist\|D/F\|directory" err2 &&
		# the conflicting refs were not created
		test_must_fail git rev-parse --verify refs/heads/dir &&
		test_must_fail git rev-parse --verify refs/heads/file/leaf
	)
'

test_expect_success 'batch updates reject only the D/F-conflicting ref through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		oid=$(git rev-parse HEAD) &&
		git update-ref refs/heads/dir/leaf HEAD &&
		# --batch-updates allows partial failure: the D/F conflict on
		# refs/heads/dir is rejected per-ref while refs/heads/ok still applies.
		printf "create refs/heads/dir %s\ncreate refs/heads/ok %s\n" \
			"$oid" "$oid" |
			git update-ref --stdin --batch-updates >out &&
		test "$(git rev-parse refs/heads/ok)" = "$oid" &&
		test_must_fail git rev-parse --verify refs/heads/dir &&
		grep "^rejected refs/heads/dir " out
	)
'

test_expect_success 'forced update records the real prior value in the reflog' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		first=$(git rev-parse HEAD) &&
		git update-ref refs/heads/x "$first" &&
		test_commit two &&
		second=$(git rev-parse HEAD) &&
		# a forced update with no old-value precondition (REF_HAVE_OLD unset)
		git update-ref refs/heads/x "$second" &&
		# the reflog old value must be the real prior oid, not zeros, so
		# x@{1} resolves to it
		echo "$first" >expect &&
		git rev-parse "refs/heads/x@{1}" >actual &&
		test_cmp expect actual
	)
'

test_expect_success 'create with a reflog message and no old precondition is not misread as a CAS' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	(
		cd repo &&
		test_commit one &&
		oid=$(git rev-parse HEAD) &&
		# update-ref -m with no old precondition sends the message as the
		# create verb trailer; the helper must ignore it, not read its first
		# token as an expected old value (which would fail the create)
		git update-ref -m "log this create" refs/heads/made "$oid" &&
		echo "$oid" >expect &&
		git rev-parse refs/heads/made >actual &&
		test_cmp expect actual &&
		git reflog show refs/heads/made >reflog &&
		grep "log this create" reflog
	)
'

test_expect_success 'refs migrate: files -> helper backend' '
	test_when_finished "rm -rf repo" &&
	git init --ref-format=files repo &&
	test_commit -C repo first &&
	test_commit -C repo second &&
	git -C repo branch other &&
	git -C repo rev-parse HEAD refs/heads/main refs/heads/other >expect &&
	git -C repo reflog show HEAD >expect-reflog-head &&
	git -C repo reflog show refs/heads/main >expect-reflog-main &&
	git -C repo reflog show refs/heads/other >expect-reflog-other &&

	git -C repo refs migrate --ref-format=testgit &&

	echo testgit >expect-fmt &&
	git -C repo config extensions.refStorage >actual-fmt &&
	test_cmp expect-fmt actual-fmt &&
	git -C repo rev-parse HEAD refs/heads/main refs/heads/other >actual &&
	test_cmp expect actual &&
	# the reflogs are migrated into the helper alongside the refs
	git -C repo reflog show HEAD >actual-reflog-head &&
	test_cmp expect-reflog-head actual-reflog-head &&
	git -C repo reflog show refs/heads/main >actual-reflog-main &&
	test_cmp expect-reflog-main actual-reflog-main &&
	git -C repo reflog show refs/heads/other >actual-reflog-other &&
	test_cmp expect-reflog-other actual-reflog-other &&
	# the migrated-away files store is gone
	test_path_is_missing repo/.git/refs/heads/main &&
	test_path_is_missing repo/.git/packed-refs
'

test_expect_success 'refs migrate: helper backend -> files' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	test_commit -C repo first &&
	test_commit -C repo second &&
	git -C repo branch other &&
	git -C repo rev-parse HEAD refs/heads/main refs/heads/other >expect &&
	git -C repo reflog show HEAD >expect-reflog-head &&
	git -C repo reflog show refs/heads/main >expect-reflog-main &&
	git -C repo reflog show refs/heads/other >expect-reflog-other &&

	git -C repo refs migrate --ref-format=files &&

	test_must_fail git -C repo config extensions.refStorage &&
	git -C repo rev-parse HEAD refs/heads/main refs/heads/other >actual &&
	test_cmp expect actual &&
	git -C repo show-ref --verify refs/heads/main &&
	# the reflogs migrate back to the files backend alongside the refs
	git -C repo reflog show HEAD >actual-reflog-head &&
	test_cmp expect-reflog-head actual-reflog-head &&
	git -C repo reflog show refs/heads/main >actual-reflog-main &&
	test_cmp expect-reflog-main actual-reflog-main &&
	git -C repo reflog show refs/heads/other >actual-reflog-other &&
	test_cmp expect-reflog-other actual-reflog-other
'

test_expect_success 'refs migrate: --no-reflog drops reflogs into the helper' '
	test_when_finished "rm -rf repo" &&
	git init --ref-format=files repo &&
	test_commit -C repo first &&
	test_commit -C repo second &&
	# the source repository has reflogs
	git -C repo reflog show HEAD >reflogs &&
	test_line_count = 2 reflogs &&

	git -C repo refs migrate --ref-format=testgit --no-reflog &&

	# the refs still migrate, but the reflogs are dropped
	git -C repo rev-parse HEAD refs/heads/main >actual &&
	git -C repo reflog --all >reflogs &&
	test_must_be_empty reflogs
'

test_expect_success 'refs migrate: --dry-run to a helper leaves the source unchanged' '
	test_when_finished "rm -rf repo" &&
	git init --ref-format=files repo &&
	test_commit -C repo first &&
	git -C repo rev-parse HEAD refs/heads/main >expect &&

	git -C repo refs migrate --ref-format=testgit --dry-run >out &&
	test_grep "dry-run migration" out &&

	# the source repository is still the files backend, untouched
	test_must_fail git -C repo config extensions.refStorage &&
	test_path_is_file repo/.git/refs/heads/main &&
	git -C repo rev-parse HEAD refs/heads/main >actual &&
	test_cmp expect actual
'

test_expect_success 'refs migrate: unknown helper fails and leaves the source intact' '
	test_when_finished "rm -rf repo" &&
	git init --ref-format=files repo &&
	test_commit -C repo first &&
	git -C repo branch other &&
	git -C repo rev-parse HEAD refs/heads/main refs/heads/other >expect &&

	# an unknown format defers to git-local-<name>, exactly like an unknown
	# URL scheme; the missing helper fails when the destination store is built,
	# which is before the source ref store is touched
	test_must_fail git -C repo refs migrate --ref-format=nonexistent 2>err &&
	test_grep "unable to start helper .nonexistent." err &&

	# the source ref store is intact: still files, every ref and its reflog read
	test_must_fail git -C repo config extensions.refStorage &&
	git -C repo rev-parse HEAD refs/heads/main refs/heads/other >actual &&
	test_cmp expect actual &&
	git -C repo reflog show HEAD >reflogs &&
	test_line_count = 1 reflogs
'

test_expect_success 'refs migrate: migration to same helper format fails' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	test_must_fail git -C repo refs migrate --ref-format=testgit 2>err &&
	test_grep "already uses .testgit. format" err
'

test_done
