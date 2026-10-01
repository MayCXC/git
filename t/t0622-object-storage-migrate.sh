#!/bin/sh

test_description='git odb migrate moves objects between object storages'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# git-local-testgit ships with t0620 and is found via PATH, the same way
# git-remote-testgit is in t5801.
PATH="$TEST_DIRECTORY/t0620:$PATH"

helper_objects () {
	echo "$(git -C "$1" rev-parse --path-format=absolute --git-common-dir)/helper-objects"
}

# The files store of the repository holds no objects: no directory of loose
# objects, no packs.
assert_files_store_empty () {
	objects="$(git -C "$1" rev-parse --path-format=absolute --git-path objects)" &&
	find "$objects" \( -path "$objects/??" -o -type f -name "*.pack" \) >files-store &&
	test_must_be_empty files-store
}

# The helper of the repository stores no objects.
assert_helper_empty () {
	find "$(helper_objects "$1")" -name "*.meta" >helper-store &&
	test_must_be_empty helper-store
}

test_expect_success 'migrate refuses an unknown object storage' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	test_must_fail git -C repo odb migrate --object-storage=db://x 2>err &&
	test_grep "unknown object storage" err
'

test_expect_success 'migrate refuses the object storage in use' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	test_must_fail git -C repo odb migrate --object-storage=files 2>err &&
	test_grep "already uses" err &&
	git init --object-storage=testgit helper &&
	test_when_finished "rm -rf helper" &&
	test_must_fail git -C helper odb migrate --object-storage=helper://testgit 2>err &&
	test_grep "already uses" err
'

test_expect_success 'migrate moves the loose objects of a repository into a helper' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	test_commit -C repo one &&
	test_commit -C repo two &&
	git -C repo rev-list --all --objects >expect &&
	git -C repo odb migrate --object-storage=testgit &&
	echo helper://testgit >expect-storage &&
	git -C repo config extensions.objectStorage >actual-storage &&
	test_cmp expect-storage actual-storage &&
	assert_files_store_empty repo &&
	git -C repo rev-list --all --objects >actual &&
	test_cmp expect actual &&
	git -C repo fsck
'

test_expect_success 'migrate moves the packfiles of a repository into a helper' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	test_commit -C repo one &&
	test_commit -C repo two &&
	git -C repo repack -ad &&
	git -C repo rev-list --all --objects >expect &&
	git -C repo odb migrate --object-storage=helper://testgit &&
	assert_files_store_empty repo &&
	git -C repo rev-list --all --objects >actual &&
	test_cmp expect actual &&
	git -C repo fsck
'

test_expect_success 'migrate leaves a kept packfile in the objects directory' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	test_commit -C repo one &&
	git -C repo repack -ad &&
	for pack in repo/.git/objects/pack/*.pack
	do
		>"${pack%.pack}.keep" || return 1
	done &&
	git -C repo odb migrate --object-storage=testgit &&
	ls repo/.git/objects/pack/*.pack &&
	git -C repo fsck
'

test_expect_success 'migrate moves the objects of a helper into packfiles' '
	test_when_finished "rm -rf repo" &&
	git init --object-storage=testgit repo &&
	test_commit -C repo one &&
	test_commit -C repo two &&
	git -C repo rev-list --all --objects >expect &&
	git -C repo odb migrate --object-storage=files &&
	test_must_fail git -C repo config extensions.objectStorage &&
	ls repo/.git/objects/pack/*.pack &&
	assert_helper_empty repo &&
	git -C repo rev-list --all --objects >actual &&
	test_cmp expect actual &&
	git -C repo fsck
'

test_expect_success 'migrate moves the objects of a helper into another' '
	test_when_finished "rm -rf repo bin" &&
	mkdir bin &&
	ln -s "$TEST_DIRECTORY/t0620/git-local-testgit" bin/git-local-testgit2 &&
	git init --object-storage=testgit repo &&
	test_commit -C repo one &&
	git -C repo rev-list --all --objects >expect &&
	(
		PATH="$(pwd)/bin:$PATH" &&
		export PATH &&
		git -C repo odb migrate --object-storage=testgit2 &&
		echo helper://testgit2 >expect-storage &&
		git -C repo config extensions.objectStorage >actual-storage &&
		test_cmp expect-storage actual-storage &&
		assert_files_store_empty repo &&
		git -C repo rev-list --all --objects >actual &&
		test_cmp expect actual &&
		git -C repo fsck
	)
'

test_expect_success 'migrate keeps the promisor objects of a helper promisor objects' '
	test_when_finished "rm -rf server repo" &&
	git init server &&
	test_commit -C server one &&
	git -C server config uploadpack.allowFilter true &&
	git -C server config uploadpack.allowAnySHA1InWant true &&
	git clone --object-storage=testgit --no-local --filter=blob:none \
		"file://$(pwd)/server" repo &&
	git -C repo gc &&
	git -C repo odb migrate --object-storage=files &&
	ls repo/.git/objects/pack/*.promisor &&
	git -C repo fsck
'

test_expect_success 'migrate keeps the references the helper stores' '
	test_when_finished "rm -rf repo" &&
	git init --ref-storage-format=testgit --object-storage=testgit repo &&
	test_commit -C repo one &&
	git -C repo rev-parse HEAD >expect &&
	git -C repo odb migrate --object-storage=files &&
	git -C repo rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C repo fsck
'

test_done
