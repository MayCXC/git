#!/bin/sh

test_description='local helper ref storage backend (extensions.refStorage=helper://<name>)

Exercises the pluggable ref backend "helper" end to end against the
git-local-testgit helper, which keeps refs and reflogs as files under the
repository directory.'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# git-local-testgit ships with this test and is found via PATH, the same way
# git-remote-testgit is in t5801.
PATH="$TEST_DIRECTORY/t0620:$PATH"

# Create a repository whose ref storage is the helper, the same way t0610
# creates a reftable repository with --ref-storage-format.
create_ref_helper_repo () {
	git init --ref-storage-format=helper://testgit "$1"
}

test_expect_success 'init records the helper in extensions.refStorage' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	echo helper://testgit >expect &&
	git -C repo config extensions.refStorage >actual &&
	test_cmp expect actual &&
	echo helper >expect &&
	git -C repo rev-parse --show-ref-storage-format >actual &&
	test_cmp expect actual
'

test_expect_success 'the helper format needs a helper name' '
	test_when_finished "rm -rf repo" &&
	test_must_fail git init --ref-storage-format=helper repo 2>err &&
	test_grep "needs a helper name" err
'

test_expect_success 'a helper that cannot store references is refused' '
	test_when_finished "rm -rf repo bin" &&
	mkdir bin &&
	write_script bin/git-local-noref <<-\EOF &&
	while read line
	do
		test "$line" = capabilities && printf "%s\n" get put ""
	done
	EOF
	(
		PATH="$(pwd)/bin:$PATH" &&
		export PATH &&
		test_must_fail git init --ref-storage-format=helper://noref repo 2>err
	) &&
	test_grep "helper .noref. does not support the .read. capability" err
'

test_expect_success 'a bare helper name selects the helper' '
	test_when_finished "rm -rf repo" &&
	git init --ref-storage-format=testgit repo &&
	echo helper://testgit >expect &&
	git -C repo config extensions.refStorage >actual &&
	test_cmp expect actual &&
	test_commit -C repo first &&
	test_path_is_file repo/.git/helper-refs/refs_heads_main
'

test_expect_success 'a bare helper name in extensions.refStorage selects the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	test_commit -C repo first &&
	git -C repo config extensions.refStorage testgit &&
	git -C repo rev-parse --verify refs/heads/main
'

test_expect_success 'GIT_REF_STORAGE_FORMAT with a bare helper name selects the helper' '
	test_when_finished "rm -rf repo" &&
	GIT_REF_STORAGE_FORMAT=testgit git init repo &&
	echo helper://testgit >expect &&
	git -C repo config extensions.refStorage >actual &&
	test_cmp expect actual
'

test_expect_success 'the gitdir carries the stubs of a non-files format' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	echo "ref: refs/heads/.invalid" >expect &&
	test_cmp expect repo/.git/HEAD &&
	echo "this repository uses the helper format" >expect &&
	test_cmp expect repo/.git/refs/heads
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
	test_cmp expect actual &&
	test_path_is_file repo/.git/helper-refs/refs_heads_main
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
# reflog exactly once, like files and reftable. Guards two things: that symref
# updates are logged at all, and that the transaction PREPARED/CLOSED state is
# set so an explicit prepare-then-commit (refs_update_symref) does not re-run
# prepare and log twice.
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

# Build the same history in a files repository and in a helper repository,
# run the given command in both, and compare their reflogs and branch
# afterwards, entry for entry.
test_reflog_like_files () {
	test_when_finished "rm -rf files helper" &&
	git init --ref-storage-format=files files &&
	create_ref_helper_repo helper &&
	for repo in files helper
	do
		(
			cd $repo &&
			test_commit one &&
			test_commit two &&
			test_commit three &&
			"$@" &&
			for ref in HEAD refs/heads/main
			do
				if git reflog exists $ref
				then
					echo "$ref:" &&
					test-tool ref-store main for-each-reflog-ent $ref ||
					return 1
				fi
			done &&
			git rev-parse main
		) >$repo.out || return 1
	done &&
	test_cmp files.out helper.out
}

test_expect_success 'reflog delete prunes an entry of a helper reflog as files does' '
	test_reflog_like_files git reflog delete main@{1}
'

test_expect_success 'reflog delete --rewrite --updateref through the helper as files does' '
	test_reflog_like_files git reflog delete --rewrite --updateref main@{0}
'

test_expect_success 'reflog expire keeps an emptied helper reflog as files does' '
	test_reflog_like_files git reflog expire --expire=all --all
'

test_expect_success 'reflog expire --dry-run leaves a helper reflog alone' '
	test_reflog_like_files git reflog expire --dry-run --expire=all --all
'

test_expect_success 'an empty reflog is created through the helper' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	test_commit -C repo one &&
	git -C repo -c core.logAllRefUpdates=false checkout -l --orphan orphan &&
	git -C repo reflog exists refs/heads/orphan
'

# A commit on a linked worktree updates the shared branch ref (routed to the
# main helper) while split_head_update emits a REF_LOG_ONLY entry for the
# worktree's own HEAD (routed to the worktree helper). The worktree helper is
# reached by nothing else in the transaction, so the reflog pass must begin a
# transaction on it for the entry to land, as files and reftable record it.
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
	test_grep "commit: wt-change" actual
'

# The payload names the helper rather than an alternate refs directory, so
# adding a worktree creates no directory named after it.
test_expect_success 'a worktree keeps no references under the payload' '
	test_when_finished "rm -rf repo wt" &&
	create_ref_helper_repo repo &&
	test_commit -C repo first &&
	git -C repo worktree add ../wt &&
	test_path_is_missing repo/.git/testgit
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
		test_grep "^rejected refs/heads/dir " out
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
		test_grep "log this create" reflog
	)
'

# Record every ref with its value or symref target, and the reflogs of HEAD
# and the branches, to compare a repository before and after a migration.
snapshot_refs () {
	git -C "$1" for-each-ref --include-root-refs \
		--format="%(refname) %(objectname) %(symref)" >"$2" &&
	for ref in HEAD refs/heads/main refs/heads/other
	do
		git -C "$1" reflog show --format="%H %gs" $ref >>"$2" || return 1
	done
}

setup_migration_repo () {
	git init --ref-storage-format="$2" "$1" &&
	test_commit -C "$1" first &&
	test_commit -C "$1" second &&
	git -C "$1" branch other HEAD~ &&
	git -C "$1" tag -a -m annotated v1 &&
	snapshot_refs "$1" expect
}

for from in files reftable
do
	test_expect_success "refs migrate: $from -> helper" '
		test_when_finished "rm -rf repo" &&
		setup_migration_repo repo $from &&
		git -C repo refs migrate --ref-storage-format=testgit &&
		echo helper://testgit >expect-format &&
		git -C repo config extensions.refStorage >actual-format &&
		test_cmp expect-format actual-format &&
		snapshot_refs repo actual &&
		test_cmp expect actual &&
		echo "this repository uses the helper format" >expect-stub &&
		test_cmp expect-stub repo/.git/refs/heads &&
		test_path_is_missing repo/.git/packed-refs &&
		test_path_is_missing repo/.git/reftable &&
		ls repo/.git >entries &&
		test_grep ! ref_migration entries
	'

	test_expect_success "refs migrate: helper -> $from" '
		test_when_finished "rm -rf repo" &&
		setup_migration_repo repo helper://testgit &&
		git -C repo refs migrate --ref-storage-format=$from &&
		echo $from >expect-format &&
		git -C repo rev-parse --show-ref-storage-format >actual-format &&
		test_cmp expect-format actual-format &&
		if test $from = files
		then
			test_must_fail git -C repo config extensions.refStorage
		else
			echo $from >expect-config &&
			git -C repo config extensions.refStorage >actual-config &&
			test_cmp expect-config actual-config
		fi &&
		snapshot_refs repo actual &&
		test_cmp expect actual &&
		test_path_is_missing repo/.git/helper-refs &&
		test_path_is_missing repo/.git/helper-reflogs
	'
done

test_expect_success 'refs migrate: helper -> another helper' '
	test_when_finished "rm -rf repo bin" &&
	mkdir bin &&
	ln -s "$TEST_DIRECTORY/t0620/git-local-testgit" bin/git-local-testgit2 &&
	setup_migration_repo repo helper://testgit &&
	(
		PATH="$(pwd)/bin:$PATH" &&
		export PATH &&
		git -C repo refs migrate --ref-storage-format=testgit2 &&
		echo helper://testgit2 >expect-format &&
		git -C repo config extensions.refStorage >actual-format &&
		test_cmp expect-format actual-format &&
		snapshot_refs repo actual
	) &&
	test_cmp expect actual
'

test_expect_success 'refs migrate: --no-reflog drops reflogs into the helper' '
	test_when_finished "rm -rf repo" &&
	setup_migration_repo repo files &&
	git -C repo refs migrate --ref-storage-format=testgit --no-reflog &&
	git -C repo rev-parse refs/heads/main refs/heads/other &&
	git -C repo reflog --all >reflogs &&
	test_must_be_empty reflogs
'

test_expect_success 'refs migrate: --dry-run to a helper leaves the source unchanged' '
	test_when_finished "rm -rf repo" &&
	setup_migration_repo repo files &&
	git -C repo refs migrate --ref-storage-format=testgit --dry-run >out &&
	test_grep "dry-run migration" out &&
	dir=$(sed -n "s/.*found at .\(.*\).$/\1/p" out) &&
	test_path_is_file "repo/$dir/helper-refs/refs_heads_main" &&
	test_must_fail git -C repo config extensions.refStorage &&
	test_path_is_missing repo/.git/helper-refs &&
	snapshot_refs repo actual &&
	test_cmp expect actual
'

test_expect_success 'refs migrate: an unknown helper fails and leaves the source intact' '
	test_when_finished "rm -rf repo" &&
	setup_migration_repo repo files &&
	test_must_fail git -C repo refs migrate --ref-storage-format=nonexistent 2>err &&
	test_grep "unable to start helper .nonexistent." err &&
	test_must_fail git -C repo config extensions.refStorage &&
	snapshot_refs repo actual &&
	test_cmp expect actual
'

test_expect_success 'refs migrate: migration to the same helper fails' '
	test_when_finished "rm -rf repo" &&
	create_ref_helper_repo repo &&
	test_must_fail git -C repo refs migrate --ref-storage-format=testgit 2>err &&
	test_grep "already uses .testgit. format" err &&
	test_must_fail git -C repo refs migrate --ref-storage-format=helper://testgit 2>err &&
	test_grep "already uses .helper://testgit. format" err
'

test_done
