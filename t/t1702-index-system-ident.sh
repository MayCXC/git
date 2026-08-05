#!/bin/sh

test_description='index records the system that wrote it'

. ./test-lib.sh

# A file that keeps its content, size and mtime but is given a new inode, which
# is the difference the identity fields exist to notice and the only one they
# can notice on their own.
reinode () {
	mtime=$(test-tool chmtime --get "$1") &&
	cp "$1" "$1.tmp" &&
	mv "$1.tmp" "$1" &&
	test-tool chmtime "=$mtime" "$1"
}

test_expect_success 'setup' '
	test_commit one &&
	echo content >tracked &&
	git add tracked &&
	git commit -m tracked
'

test_expect_success 'an index this system wrote is trusted for identity' '
	git update-index --refresh >/dev/null &&
	reinode tracked &&
	test_expect_code 1 git diff-files --quiet -- tracked
'

test_expect_success 'an index another system wrote is not' '
	GIT_TEST_SYSTEM_NAME=OtherSystem git update-index --refresh >/dev/null &&
	reinode tracked &&
	git diff-files --quiet -- tracked
'

test_expect_success 'the content is what it always was' '
	echo content >expect &&
	test_cmp expect tracked &&
	git diff --quiet -- tracked
'

test_expect_success 'core.checkStat still turns the comparison off' '
	git update-index --refresh >/dev/null &&
	reinode tracked &&
	git -c core.checkStat=minimal diff-files --quiet -- tracked
'

test_done
