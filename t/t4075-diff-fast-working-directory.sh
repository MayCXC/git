#!/bin/sh

test_description='core.fastWorkingDirectory decides where blob contents are read'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# The setting only chooses between the working directory and an already open
# pack as the source of contents Git holds in both places, so it is consulted
# for a path whose recorded object is packed and whose file is up to date with
# the index.
test_expect_success 'setup' '
	test_commit one file.txt one &&
	test_commit two file.txt two &&
	git repack -ad &&
	git update-index --refresh
'

test_expect_success 'the setting is consulted for a packed and unmodified path' '
	test_must_fail git -c core.fastWorkingDirectory=notabool \
		diff-index --cached -p HEAD~ 2>err &&
	test_grep "bad boolean config value" err
'

test_expect_success 'the contents are the same from either source' '
	git -c core.fastWorkingDirectory=true \
		diff-index --cached -p HEAD~ >expect &&
	test_file_not_empty expect &&
	git -c core.fastWorkingDirectory=false \
		diff-index --cached -p HEAD~ >actual &&
	test_cmp expect actual
'

test_expect_success 'true is the default' '
	git -c core.fastWorkingDirectory=true \
		diff-index --cached -p HEAD~ >expect &&
	git diff-index --cached -p HEAD~ >actual &&
	test_cmp expect actual
'

test_done
