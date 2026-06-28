#!/bin/sh

test_description='dumb http fetch into a local-helper object store

The dumb-http object walker writes fetched objects into the local object
store. When the local repository uses a local-helper object backend the
writes must go through the helper, loose objects via the install_loose_object
vtable and packs via index-pack ingestion plus a reprepare, rather than the
files loose/pack layout.'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-httpd.sh
start_httpd

# git-local-testgit lives under t0620 and is found via PATH, the same way
# t0620 locates it (git's reference helper; no out-of-tree dependency).
PATH="$TEST_DIRECTORY/t0620:$PATH"

create_helper_repo () {
	git init "$1" &&
	git -C "$1" config core.repositoryFormatVersion 1 &&
	git -C "$1" config extensions.objectStorage testgit
}

# The helper stores objects in its own backend, so a correct ingest leaves no
# objects behind in the files loose-fanout or pack layout.
assert_helper_only () {
	find "$1/.git/objects" -type d -name "??" >loose-dirs &&
	test_must_be_empty loose-dirs &&
	find "$1/.git/objects" -type f -name "*.pack" >packs &&
	test_must_be_empty packs
}

test_expect_success 'setup dumb http repo with packed and loose objects' '
	git init src &&
	test_commit -C src packed-one &&
	test_commit -C src packed-two &&
	git -C src gc -q &&
	test_commit -C src loose-three &&
	cp -R src/.git "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" config core.bare true &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" update-server-info
'

test_expect_success 'clone a dumb http repo into a local-helper primary' '
	git clone --object-storage=testgit \
		"$HTTPD_URL/dumb/repo.git" helper-clone &&
	# the loose tip and the packed history both resolve through the helper
	git -C helper-clone cat-file -e HEAD &&
	git -C helper-clone rev-list --objects --all >/dev/null &&
	assert_helper_only helper-clone
'

test_expect_success 'fetch dumb http objects into an existing helper primary' '
	create_helper_repo helper-fetch &&
	git -C helper-fetch fetch "$HTTPD_URL/dumb/repo.git" &&
	git -C helper-fetch cat-file -e FETCH_HEAD &&
	git -C helper-fetch rev-list --objects FETCH_HEAD >/dev/null &&
	assert_helper_only helper-fetch
'

test_done
