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

# Every test here turns on whether a refresh rewrote the index, and a refresh
# rewrites it for any entry it finds racily clean, whatever that entry's
# identity. So the file is dated well before the index that records it.
test_expect_success 'setup' '
	echo content >tracked &&
	test-tool chmtime =-60 tracked &&
	git add tracked &&
	git commit -m tracked
'

test_expect_success 'an index this system wrote is trusted for identity' '
	git update-index --refresh >/dev/null &&
	reinode tracked &&
	test_expect_code 1 git diff-files --quiet -- tracked
'

# A moved mtime makes the other system's refresh verify the file and write the
# index, which then names that system whatever the identity fields say.
test_expect_success 'an index another system wrote is not' '
	test-tool chmtime -1 tracked &&
	GIT_TEST_SYSTEM_NAME=OtherSystem git update-index --refresh >/dev/null &&
	reinode tracked &&
	git diff-files --quiet -- tracked
'

test_expect_success 'a refresh takes over the index another system wrote' '
	test-tool chmtime -1 tracked &&
	GIT_TEST_SYSTEM_NAME=OtherSystem git update-index --refresh >/dev/null &&
	reinode tracked &&
	git update-index --refresh >/dev/null &&
	reinode tracked &&
	test_expect_code 1 git diff-files --quiet -- tracked
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

test_lazy_prereq BOOT_ID 'test -r /proc/sys/kernel/random/boot_id'

# An inode number names one file only within one mount during one boot, so
# where the system has a boot ID the name carries it, and the mount holding
# the worktree wherever statx reports one.
test_expect_success BOOT_ID 'the name a write records carries the boot and the mount' '
	test_when_finished "rm -f trace.event" &&
	GIT_TRACE2_EVENT="$(pwd)/trace.event" git update-index --force-write-index &&
	boot=$(cat /proc/sys/kernel/random/boot_id) &&
	test_grep -E "\"key\":\"write/system\",\"value\":\"[^\"]*, boot $boot(, mount [0-9]+)?\"" trace.event
'

test_done
