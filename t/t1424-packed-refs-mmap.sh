#!/bin/sh

test_description='core.mmapPreventsDelete decides how packed-refs is mapped'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup' '
	test_commit one &&
	git branch a-branch &&
	git pack-refs --all
'

test_expect_success 'the setting is consulted when packed refs are read' '
	test_must_fail git -c core.mmapPreventsDelete=notabool for-each-ref 2>err &&
	test_grep "bad boolean config value" err
'

test_expect_success 'the refs read the same either way' '
	git for-each-ref --format="%(refname)" >expect &&
	git -c core.mmapPreventsDelete=true for-each-ref --format="%(refname)" >actual &&
	test_cmp expect actual
'

# Where a mapped file cannot be deleted or renamed over, packed-refs is copied
# and unmapped rather than held, so rewriting it is the operation that tells
# the two strategies apart.
test_expect_success 'packed-refs is rewritten while it is being read' '
	git -c core.mmapPreventsDelete=true branch b-branch &&
	git -c core.mmapPreventsDelete=true pack-refs --all &&
	git -c core.mmapPreventsDelete=true branch -d b-branch &&
	git -c core.mmapPreventsDelete=true pack-refs --all &&
	git rev-parse --verify refs/heads/a-branch
'

# Checking the file reads it into a snapshot that belongs to no store, so the
# answer is asked of the store being checked rather than of the snapshot.
test_expect_success 'the file is read the same way when it is checked' '
	git init fsck-repo &&
	test_commit -C fsck-repo fsck-one &&
	git -C fsck-repo pack-refs --all &&
	git -C fsck-repo -c core.mmapPreventsDelete=true refs verify &&
	printf "# pack-refs wit: peeled fully-peeled sorted \n" \
		>fsck-repo/.git/packed-refs &&
	test_must_fail git -C fsck-repo -c core.mmapPreventsDelete=true \
		refs verify 2>err &&
	test_grep "badPackedRefHeader" err
'

test_expect_success 'setup a superproject and a submodule' '
	git init upstream-sub &&
	test_commit -C upstream-sub sub-one content.txt findme &&
	git init super &&
	test_commit -C super super-one &&
	git -C super -c protocol.file.allow=always \
		submodule add ../upstream-sub sub &&
	git -C super commit -m "add sub" &&
	git -C super/sub pack-refs --all
'

# The superproject and the submodule are two repositories reached by one
# process, and each answers for the filesystem it sits on, so the answer is
# read from the repository the packed-refs file belongs to rather than from
# whichever repository the process started in.
test_expect_success "a submodule is asked for its own answer" '
	git -C super/sub config core.mmapPreventsDelete true &&
	git -C super grep --cached --recurse-submodules findme
'

test_expect_success "the superproject is not asked for the submodule's" '
	git -C super/sub config core.mmapPreventsDelete false &&
	git -C super config core.mmapPreventsDelete true &&
	git -C super grep --cached --recurse-submodules findme
'

test_done
