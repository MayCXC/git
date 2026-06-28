#!/bin/sh

test_description='migrate the object storage backend (git odb migrate)

Exercises repo_migrate_object_storage_format end to end: converting a
files-backed repository to the pluggable "helper" object backend, plus
the input validation. Like t0620 this runs against
git-local-testgit, git'"'"'s bundled reference object-storage helper, so the
git-core mechanism is covered with no dependency on any out-of-tree helper;
the sqlite helper is exercised in sqlite-git'"'"'s own suite.'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# git-local-testgit lives alongside the t0620 script and is found via PATH, the
# same way that test locates it.
PATH="$TEST_DIRECTORY/t0620:$PATH"

# testgit stores each object as a file under "<common-dir>/helper-objects".
assert_object_in_helper () {
	test_path_is_file "$1/.git/helper-objects/$2"
}

test_expect_success 'setup a files-backed repository with history' '
	git init repo &&
	test_commit -C repo one &&
	test_commit -C repo two &&
	test_commit -C repo three &&
	git -C repo rev-list --objects --all >expect-objects &&
	git -C repo rev-parse HEAD >expect-head
'

test_expect_success 'migrate a files repository to the helper backend' '
	git -C repo odb migrate --object-storage=testgit &&
	test "$(git -C repo config core.repositoryformatversion)" = "1" &&
	test "$(git -C repo config extensions.objectStorage)" = "testgit" &&
	# every object round-tripped into the helper store
	git -C repo rev-list --objects --all >got-objects &&
	test_cmp expect-objects got-objects &&
	git -C repo fsck &&
	test "$(git -C repo rev-parse HEAD)" = "$(cat expect-head)" &&
	assert_object_in_helper repo "$(git -C repo rev-parse HEAD)"
'

test_expect_success 'migration discarded the source object store' '
	# the now-redundant files store is gone, so reads are genuinely served by
	# the helper, not by stale loose objects or packs left behind.
	find repo/.git/objects -type f >remaining-files &&
	test_must_be_empty remaining-files &&
	git -C repo fsck &&
	git -C repo rev-list --objects --all >after-objects &&
	test_cmp expect-objects after-objects &&
	echo three >expect-blob &&
	git -C repo show HEAD:three.t >actual-blob &&
	test_cmp expect-blob actual-blob
'

test_expect_success 'migrate to a missing helper fails without losing the source' '
	git init bad &&
	test_commit -C bad x &&
	# "bogus" is an open backend name like any other; with no git-local-bogus
	# on PATH the destination cannot be built, and the failed migration must
	# leave the original files store intact and the config unchanged.
	test_must_fail git -C bad odb migrate --object-storage=bogus &&
	git -C bad fsck &&
	test "$(git -C bad rev-parse HEAD)" = "$(git -C bad rev-parse x)" &&
	test_must_fail git -C bad config extensions.objectStorage
'

test_expect_success 'migrate rejects converting to the current backend' '
	test_must_fail git -C repo odb migrate --object-storage=testgit 2>err &&
	test_grep "current and new object storage are the same" err
'

test_done
