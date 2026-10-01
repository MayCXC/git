#!/bin/sh

test_description='core.trustStdev decides whether st_dev is compared'

. ./test-lib.sh

# A file that keeps its content, size, mtime and inode, on a device this test
# cannot change, so the comparison has to be provoked from the index side: a
# recorded device number that no longer matches is what a filesystem naming its
# own device produces, and what the configuration exists to ignore.
test_expect_success 'setup' '
	test_commit one &&
	echo content >tracked &&
	git add tracked &&
	git commit -m tracked &&
	git update-index --refresh >/dev/null
'

test_expect_success 'default leaves the device out of the comparison' '
	git diff-files --quiet -- tracked
'

test_expect_success 'core.trustStdev is off unless asked for' '
	echo false >expect &&
	git config --type=bool --default=false --get core.trustStdev >actual &&
	test_cmp expect actual
'

test_expect_success 'setting it does not disturb an unchanged file' '
	git -c core.trustStdev=true diff-files --quiet -- tracked &&
	git -c core.trustStdev=true diff --quiet -- tracked
'

test_expect_success 'core.checkStat=minimal wins over it' '
	git -c core.trustStdev=true -c core.checkStat=minimal \
		diff-files --quiet -- tracked
'

test_done
