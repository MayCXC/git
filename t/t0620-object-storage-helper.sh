#!/bin/sh

test_description='local helper object-storage backend (extensions.objectStorage=helper)

Exercises the pluggable object database "helper" backend end to end against the
git-local-testgit helper, which stores each object as a file under
"<object-dir>/helper-objects". Covers writing objects locally and receiving
them over fetch (full, thin, fsck and shallow packs).'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# git-local-testgit lives alongside this script and is found via PATH, the same
# way git-remote-testgit is in t5801.
PATH="$TEST_DIRECTORY/t0620:$PATH"

# These tests run against git's own bundled shell helper, git-local-testgit. It
# is git's reference implementation of the protocol, exercising the backend end
# to end with no dependency on any out-of-tree helper. Storage behaviors a
# minimal file stub cannot model (delta representation, reachability bitmaps,
# kept-pack membership, a recorded last-used timestamp) are covered where a real
# helper implementation lives, not here. testgit is hash-agnostic, so it stores
# sha256 objects directly and the compat test needs no separate probe.

# Create a repository whose object storage is the git-local-testgit helper. The
# backend is named outright in extensions.objectStorage, the way a transport is
# named; "testgit" selects git-local-testgit.
create_helper_repo () {
	git init "$1" &&
	git -C "$1" config core.repositoryFormatVersion 1 &&
	git -C "$1" config extensions.objectStorage testgit
}

# All objects of the repository must live in the helper store, never loose.
assert_no_loose_objects () {
	find "$1/.git/objects" -type d -name "??" >loose-dirs &&
	test_must_be_empty loose-dirs
}

# The helper stores objects under its store at the common directory (shared
# with the ref helper), not under the object directory.
assert_object_in_helper () {
	test_path_is_file "$1/.git/helper-objects/$2"
}

test_expect_success 'setup files server repo' '
	git init server &&
	test_seq 1 200 >server/big &&
	git -C server add big &&
	git -C server commit -m v1
'

test_expect_success 'helper backend stores written objects, none loose' '
	create_helper_repo store &&
	(
		cd store &&
		echo content >file &&
		git add file &&
		git commit -m one &&
		echo content >expect &&
		git cat-file -p HEAD:file >actual &&
		test_cmp expect actual
	) &&
	assert_object_in_helper store "$(git -C store rev-parse HEAD:file)" &&
	assert_no_loose_objects store
'

test_expect_success 'log and diff work over the helper backend' '
	(
		cd store &&
		echo more >>file &&
		git commit -am two &&
		git log --oneline >log &&
		test_line_count = 2 log &&
		git diff --stat HEAD^ HEAD >diff &&
		grep file diff
	)
'

# Resolving an abbreviated object id needs the disambiguator to see only the
# objects matching the prefix; helper_for_each_object must honor the
# for_each_object prefix (the list-objects protocol streams every id, so git
# filters the returned ids) rather than offering all of them, which would make
# every short id ambiguous.
test_expect_success 'abbreviated object id resolves over the helper backend' '
	(
		cd store &&
		full=$(git rev-parse HEAD:file) &&
		short=$(echo "$full" | cut -c1-10) &&
		echo blob >expect &&
		git cat-file -t "$short" >actual &&
		test_cmp expect actual
	)
'

# With extensions.compatObjectFormat every object also has an id under the
# compat algorithm. Computing and recording that mapping is git's job, not the
# dumb-store helper's: git keeps a storage<->compat loose-object-idx at the
# object directory (even though the objects live in the helper) and resolves
# compat ids through it. So compat works over a helper with no protocol change.
# Needs the Rust-backed compat object conversion.
test_expect_success RUST 'compat object format works over the helper backend' '
	git init --object-format=sha256 compat &&
	git -C compat config core.repositoryFormatVersion 1 &&
	git -C compat config extensions.compatObjectFormat sha1 &&
	git -C compat config extensions.objectStorage testgit &&
	(
		cd compat &&
		echo "Hello World!" >hello &&
		git add hello &&
		git commit -m init &&
		# git keeps the storage<->compat map at the object directory
		test_path_is_file .git/objects/loose-object-idx &&
		# the compat (sha1) id of a helper-stored object resolves back to it
		blob1=$(git rev-parse --output-object-format=sha1 HEAD:hello) &&
		echo blob >expect_type &&
		git cat-file -t "$blob1" >actual_type &&
		test_cmp expect_type actual_type &&
		echo "Hello World!" >expect_content &&
		git cat-file -p "$blob1" >actual_content &&
		test_cmp expect_content actual_content &&
		# an abbreviated compat id resolves too (the helper honors the
		# for_each_object prefix and offers each object'\''s compat id)
		git cat-file -t "$(echo "$blob1" | cut -c1-12)" >actual_abbr &&
		test_cmp expect_type actual_abbr
	) &&
	assert_no_loose_objects compat &&
	assert_object_in_helper compat "$(git -C compat rev-parse HEAD:hello)"
'

# Cloning a *local* helper-backed repository must not take the local-clone
# shortcut. That shortcut hardlinks or copies the source's loose objects and
# packs into the destination, but a helper keeps every object in its own store
# (here under helper-objects, none loose), so the shortcut would copy refs over
# an empty object database. clone.c reads the source's extensions.objectStorage
# and, when it names a non-files backend, declines the optimization and fetches
# over the transport, the same fallback a helper *destination* reaches by
# declining odb_source_local_clone. This mirrors the shallow-source guard above.
test_expect_success 'clone of a local helper-backed source fetches over the transport' '
	test_when_finished "rm -rf clone-src clone-default clone-local" &&
	create_helper_repo clone-src &&
	test_commit -C clone-src one &&
	test_commit -C clone-src two &&
	assert_no_loose_objects clone-src &&
	git -C clone-src rev-parse HEAD >expect &&

	# A default clone of a local path would normally be a local clone; the
	# helper source forces the transport so the objects actually transfer.
	git clone clone-src clone-default &&
	git -C clone-default rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C clone-default fsck &&
	echo two >expect_content &&
	git -C clone-default show HEAD:two.t >actual_content &&
	test_cmp expect_content actual_content &&

	# An explicit --local cannot be honored against a helper source: git
	# reports the backend reason (like the shallow case) and falls back
	# rather than producing an empty clone.
	git clone --local clone-src clone-local 2>warn &&
	test_grep "object backend, ignoring --local" warn &&
	git -C clone-local rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C clone-local fsck
'

# --object-storage propagates to submodule clones the same way --ref-format
# does, so a helper superproject'\''s submodules are cloned into the helper too
# (clone.c -> git submodule update -> git submodule--helper clone -> git clone).
# The backend name carries the helper identity, so no separate flag rides along.
test_expect_success 'clone --recurse-submodules propagates object storage to submodules' '
	test_when_finished "rm -rf sub-src super-src super-clone" &&
	git init sub-src &&
	test_commit -C sub-src sub-content &&
	git init super-src &&
	test_commit -C super-src super-content &&
	git -C super-src -c protocol.file.allow=always \
		submodule add "$(pwd)/sub-src" sub &&
	git -C super-src commit -m "add submodule" &&
	git -c protocol.file.allow=always clone --quiet \
		--object-storage=testgit \
		--recurse-submodules super-src super-clone &&
	# superproject objects went to the helper, none loose
	assert_no_loose_objects super-clone &&
	# the submodule was cloned into the helper too, none loose
	test_path_is_dir super-clone/.git/modules/sub &&
	find super-clone/.git/modules/sub/objects -type d -name "??" >sub-loose &&
	test_must_be_empty sub-loose &&
	# and its content is served from the helper (submodule checked out)
	test_path_is_file super-clone/sub/sub-content.t
'

# A helper has no kept packs, so the kept-pack reachability query (used by
# repack via --no-kept-objects) must report none rather than assuming a files
# pack layout. This exercises the has_object_kept_pack() guard.
test_expect_success 'rev-list --no-kept-objects works over the helper backend' '
	git -C store rev-list --no-kept-objects --all >actual &&
	test_line_count = 2 actual
'

# A read that misses must escalate to a second read that reloads the helper's
# view before querying again, mirroring how the packed store reprepares to pick
# up newly arrived packs (packfile.c packfile_store_read_object_info). A helper
# that answers reads from a snapshot would otherwise never surface an object
# another process committed after that snapshot was taken; the file-backed test
# helper reads live, so we assert the wire behavior (refresh between the reads)
# rather than the snapshot effect.
test_expect_success 'a missed read reloads the helper view before retrying' '
	absent=$(echo not-a-stored-object | git -C store hash-object --stdin) &&
	test_when_finished "rm -f cmdlog" &&
	(
		cd store &&
		test_env GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/cmdlog" \
			test_must_fail git cat-file -t "$absent"
	) &&
	grep -E "^(info |refresh$)" cmdlog >reads &&
	cat >expect <<-EOF &&
	info $absent
	refresh
	info $absent
	EOF
	test_cmp expect reads
'

test_expect_success 'fetch a full pack into the helper backend' '
	create_helper_repo client &&
	git -C client remote add origin "$(pwd)/server" &&
	git -C client fetch origin &&
	git -C client rev-parse origin/main >actual &&
	git -C server rev-parse main >expect &&
	test_cmp expect actual &&
	assert_no_loose_objects client &&
	test_path_is_missing client/.git/objects/pack/*.pack
'

test_expect_success 'fetch resolves a thin pack against helper-held bases' '
	# A near-identical blob makes the server send the update as a delta
	# against the v1 blob, which only the helper store holds.
	echo "line 201" >>server/big &&
	git -C server commit -am v2 &&
	git -C client fetch origin &&
	git -C server cat-file -p main:big >expect &&
	git -C client cat-file -p origin/main:big >actual &&
	test_cmp expect actual &&
	assert_no_loose_objects client
'

test_expect_success 'fetch with fsckObjects verifies into the helper backend' '
	echo "line 202" >>server/big &&
	git -C server commit -am v3 &&
	git -C client -c fetch.fsckObjects=true fetch origin &&
	git -C server rev-parse main >expect &&
	git -C client rev-parse origin/main >actual &&
	test_cmp expect actual
'

# A blob larger than core.bigFileThreshold reaches index-pack with no in-memory
# buffer (data == NULL); it must be streamed object-by-object into the helper
# store rather than dropped. Setting the threshold to 4 makes an ordinary blob
# take that path.
test_expect_success 'fetch streams a large blob into the helper backend' '
	test_seq 1 500 >server/large &&
	git -C server add large &&
	git -C server commit -m large-blob &&
	create_helper_repo bigclient &&
	git -C bigclient config core.bigFileThreshold 4 &&
	git -C bigclient remote add origin "$(pwd)/server" &&
	test_when_finished "rm -f biglog" &&
	GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/biglog" \
		git -C bigclient fetch origin &&
	git -C server rev-parse main:large >expect-oid &&
	git -C bigclient rev-parse origin/main:large >actual-oid &&
	test_cmp expect-oid actual-oid &&
	git -C server cat-file -p main:large >expect-blob &&
	git -C bigclient cat-file -p origin/main:large >actual-blob &&
	test_cmp expect-blob actual-blob &&
	assert_object_in_helper bigclient "$(git -C server rev-parse main:large)" &&
	assert_no_loose_objects bigclient &&
	test_path_is_missing bigclient/.git/objects/pack/*.pack &&
	# testgit logs its wire commands, so assert the blob took the streaming
	# put-stream path rather than a buffered put.
	grep "^put-stream " biglog
'

# A multi-megabyte blob exercises the streaming READ across many spool
# iterations: cat-file -p streams via odb_stream_blob_to_fd -> the helper's
# read_object_stream, which drains the whole object off the shared pipe into a
# tempfile (so a re-entrant helper read cannot desync the pipe) before serving
# it. A >1MB blob covers the spool loop's chunk boundaries, beyond the tiny blob
# the fetch-streaming test above round-trips.
test_expect_success 'large object reads stream through the helper across spool chunks' '
	create_helper_repo streamread &&
	test_seq 1 200000 >big.in &&
	blob=$(git -C streamread hash-object -w --stdin <big.in) &&
	git -C streamread cat-file -p "$blob" >big.out &&
	test_cmp big.in big.out
'

test_expect_success 'GIT_DEFAULT_OBJECT_STORAGE names the helper backend' '
	GIT_DEFAULT_OBJECT_STORAGE=testgit git init env-store &&
	echo testgit >expect-os &&
	git -C env-store config extensions.objectStorage >actual-os &&
	test_cmp expect-os actual-os &&
	test_must_fail git -C env-store config extensions.refStorage &&
	echo 1 >expect-version &&
	git -C env-store config core.repositoryformatversion >actual-version &&
	test_cmp expect-version actual-version &&
	(
		cd env-store &&
		echo hi >file &&
		git add file &&
		git commit -m one
	) &&
	assert_object_in_helper env-store "$(git -C env-store rev-parse HEAD:file)" &&
	assert_no_loose_objects env-store
'

test_expect_success 'git init --object-storage=<helper> initializes a helper repo' '
	git init --object-storage=testgit cli-store &&
	echo testgit >expect-os &&
	git -C cli-store config extensions.objectStorage >actual-os &&
	test_cmp expect-os actual-os &&
	test_must_fail git -C cli-store config extensions.refStorage &&
	(
		cd cli-store &&
		echo hi >file &&
		git add file &&
		git commit -m one
	) &&
	assert_object_in_helper cli-store "$(git -C cli-store rev-parse HEAD:file)" &&
	assert_no_loose_objects cli-store
'

test_expect_success 'reinitializing with a different object storage backend fails' '
	test_must_fail git init --object-storage=files cli-store 2>err &&
	test_grep "different object storage backend" err
'

test_expect_success 'clone --object-storage=<helper> clones into a helper repo' '
	git clone --object-storage=testgit server clone-store &&
	echo testgit >expect &&
	git -C clone-store config extensions.objectStorage >actual &&
	test_cmp expect actual &&
	git -C server rev-parse HEAD >expect-head &&
	git -C clone-store rev-parse HEAD >actual-head &&
	test_cmp expect-head actual-head &&
	assert_no_loose_objects clone-store &&
	test_path_is_missing clone-store/.git/objects/pack/*.pack
'

test_expect_success 'worktree on a helper repo shares the helper object store' '
	create_helper_repo wt-main &&
	test_commit -C wt-main one &&
	git -C wt-main worktree add ../wt-linked &&
	test_commit -C wt-linked two &&
	git -C wt-linked log --oneline >actual &&
	test_line_count = 2 actual &&
	assert_object_in_helper wt-main "$(git -C wt-linked rev-parse HEAD:two.t)" &&
	assert_no_loose_objects wt-main
'

test_expect_success 'plain init is unaffected by the helper backend' '
	git init plain-store &&
	test_must_fail git -C plain-store config extensions.objectStorage
'

test_expect_success 'objectStorage rejects a value with a path' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	git -C repo config set core.repositoryformatversion 1 &&
	git -C repo config set extensions.objectStorage "sqlite@/home/sqlite" &&
	test_must_fail git -C repo rev-parse --git-dir 2>err &&
	test_grep "invalid value for ${SQ}extensions.objectstorage${SQ}" err
'

test_expect_success 'objectStorage rejects a value ending with a colon' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	git -C repo config set core.repositoryformatversion 1 &&
	git -C repo config set extensions.objectStorage "sqlite:" &&
	test_must_fail git -C repo rev-parse --git-dir 2>err &&
	test_grep "invalid value for ${SQ}extensions.objectstorage${SQ}" err
'

test_expect_success 'objectStorage rejects a scheme-style value' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	git -C repo config set core.repositoryformatversion 1 &&
	git -C repo config set extensions.objectStorage "db://.git" &&
	test_must_fail git -C repo rev-parse --git-dir 2>err &&
	test_grep "invalid value for ${SQ}extensions.objectstorage${SQ}" err
'

test_expect_success 'objects are read from a files alternate of a helper repo' '
	git init alt-store &&
	blob=$(echo "from the alternate" | git -C alt-store hash-object -w --stdin) &&
	create_helper_repo with-alt &&
	mkdir -p with-alt/.git/objects/info &&
	echo "$(pwd)/alt-store/.git/objects" >with-alt/.git/objects/info/alternates &&
	echo "from the alternate" >expect &&
	git -C with-alt cat-file -p "$blob" >actual &&
	test_cmp expect actual
'

test_expect_success 'partial fetch records promisor objects through the helper' '
	git -C server config uploadpack.allowFilter true &&
	create_helper_repo partial &&
	git -C partial remote add origin "$(pwd)/server" &&
	git -C partial fetch --filter=blob:none origin &&
	# The blobs were filtered out, so the received trees and commits are
	# promisor objects; walking history must therefore tolerate the absent
	# blobs rather than treating them as a broken object graph.
	git -C partial rev-list --objects --all --missing=print >objs &&
	grep "^?$(git -C server rev-parse HEAD:big)" objs &&
	# The promisor annotation is what lets that walk succeed: excluding
	# promisor objects drops the received commit and tree.
	git -C partial rev-list --objects --all --exclude-promisor-objects >kept &&
	test_must_be_empty kept &&
	assert_no_loose_objects partial &&
	test_path_is_missing partial/.git/objects/pack/*.pack
'

test_expect_success 'lazy fetch of a missing promisor object works on a helper' '
	git -C server config uploadpack.allowFilter true &&
	create_helper_repo lazyfetch &&
	git -C lazyfetch remote add origin "$(pwd)/server" &&
	git -C lazyfetch fetch --filter=blob:none origin &&
	blob=$(git -C server rev-parse HEAD:big) &&
	# Filtered out: the blob is a promisor object, absent without a lazy fetch.
	test_must_fail env GIT_NO_LAZY_FETCH=1 git -C lazyfetch cat-file -e "$blob" &&
	# Accessing it triggers an on-demand fetch that lands in the helper store.
	git -C lazyfetch cat-file -p "$blob" >got &&
	git -C server cat-file -p "$blob" >want &&
	test_cmp want got &&
	# It is now present locally, served by the helper without another fetch.
	GIT_NO_LAZY_FETCH=1 git -C lazyfetch cat-file -e "$blob" &&
	assert_object_in_helper lazyfetch "$blob"
'

test_expect_success 'shallow fetch and unshallow into the helper backend' '
	create_helper_repo shallow &&
	git -C shallow remote add origin "$(pwd)/server" &&
	git -C shallow fetch --depth=1 origin &&
	test_path_is_file shallow/.git/shallow &&
	git -C shallow cat-file -t "$(git -C server rev-parse main)" >type &&
	grep commit type &&
	git -C shallow fetch --unshallow origin &&
	test_path_is_missing shallow/.git/shallow
'

# Push is the receive-side counterpart of fetch. receive-pack stages the
# incoming objects in a files quarantine and only migrates them into the store
# once the push is accepted. A helper primary would ignore that quarantine, so
# during receive the helper is demoted to a read-through secondary and the files
# quarantine becomes the write target; migrate_quarantine then ingests the
# staged objects into the helper (preserving any pack deltas) on accept, and a
# rejected push drops the quarantine with the helper never touched.
test_expect_success 'push into a helper repo migrates the staged pack' '
	git init pushsrc &&
	# Two commits with two blob versions, so the push sends a small pack.
	printf "%08000d\nAAAA\n" 0 >pushsrc/f.txt &&
	git -C pushsrc add f.txt &&
	git -C pushsrc commit -q -m one &&
	printf "%08000d\nBBBB\n" 0 >pushsrc/f.txt &&
	git -C pushsrc add f.txt &&
	git -C pushsrc commit -q -m two &&
	create_helper_repo pushdst &&
	# unpackLimit=1 forces index-pack, so the push is staged as a pack in the
	# quarantine, then migrated into the helper store.
	git -C pushdst config transfer.unpackLimit 1 &&
	# Push to a non-checked-out branch so receive is unrestricted.
	git -C pushsrc push "$PWD/pushdst" main:refs/heads/incoming &&
	git -C pushsrc rev-parse main >expect &&
	git -C pushdst rev-parse incoming >actual &&
	test_cmp expect actual &&
	git -C pushsrc cat-file blob main:f.txt >expect-blob &&
	git -C pushdst cat-file blob incoming:f.txt >actual-blob &&
	test_cmp expect-blob actual-blob &&
	git -C pushdst fsck &&
	# Migrated into the helper: nothing loose, no files pack left behind.
	assert_no_loose_objects pushdst &&
	test_path_is_missing pushdst/.git/objects/pack/*.pack
'

test_expect_success 'push of a small pack (exploded to loose) into a helper repo' '
	git init pushsmallsrc &&
	test_commit -C pushsmallsrc only &&
	create_helper_repo pushsmalldst &&
	# A high unpackLimit makes receive explode the push into loose objects in
	# the quarantine; migrate ingests them whole.
	git -C pushsmalldst config transfer.unpackLimit 10000 &&
	git -C pushsmallsrc push "$PWD/pushsmalldst" main:refs/heads/incoming &&
	git -C pushsmallsrc rev-parse main >expect &&
	git -C pushsmalldst rev-parse incoming >actual &&
	test_cmp expect actual &&
	git -C pushsmalldst fsck &&
	assert_no_loose_objects pushsmalldst &&
	assert_object_in_helper pushsmalldst \
		"$(git -C pushsmallsrc rev-parse main:only.t)"
'

test_expect_success 'a rejected push leaves the helper object store untouched' '
	git init pushrejsrc &&
	test_commit -C pushrejsrc seed &&
	create_helper_repo pushrejdst &&
	git -C pushrejdst config transfer.unpackLimit 1 &&
	# Seed an accepted push so the helper store is already populated.
	git -C pushrejsrc push "$PWD/pushrejdst" main:refs/heads/incoming &&
	# A second push of new objects, rejected by a failing pre-receive hook.
	test_commit -C pushrejsrc doomed &&
	doomed=$(git -C pushrejsrc rev-parse main:doomed.t) &&
	write_script pushrejdst/.git/hooks/pre-receive <<-\EOF &&
	exit 1
	EOF
	test_must_fail git -C pushrejsrc push "$PWD/pushrejdst" main:refs/heads/incoming &&
	# The quarantine isolated the staged objects: nothing new reached the
	# helper, and what was there before is intact.
	git -C pushrejdst fsck &&
	test_must_fail git -C pushrejdst cat-file -e "$doomed" &&
	git -C pushrejdst cat-file -e "$(git -C pushrejsrc rev-parse seed)" &&
	assert_no_loose_objects pushrejdst
'

test_expect_success 'gc optimizes the helper object store' '
	create_helper_repo gcstore &&
	test_commit -C gcstore one &&
	test_commit -C gcstore two &&
	git -C gcstore gc &&
	# Objects remain readable through the helper after optimizing.
	git -C gcstore cat-file -t HEAD >type &&
	grep commit type
'

test_expect_success 'gc sends the optimize command to the helper' '
	create_helper_repo optstore &&
	test_commit -C optstore opt &&
	test_when_finished "rm -f optlog" &&
	(
		cd optstore &&
		GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/optlog" git gc
	) &&
	grep "^optimize$" optlog
'

test_expect_success 'gc --prune sends the prune command to the helper' '
	create_helper_repo gcprunelogstore &&
	test_commit -C gcprunelogstore gcp &&
	test_when_finished "rm -f gcprunelog" &&
	(
		cd gcprunelogstore &&
		GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/gcprunelog" git gc --prune=now
	) &&
	grep "^prune " gcprunelog
'

test_expect_success "gc --prune sweeps unreachable objects from the helper store" '
	create_helper_repo gcprunestore &&
	# Disable reflogs so resetting orphans objects immediately, with nothing
	# but the refs keeping anything reachable.
	git -C gcprunestore config core.logAllRefUpdates false &&
	test_commit -C gcprunestore base &&
	test_commit -C gcprunestore doomed &&
	doomed=$(git -C gcprunestore rev-parse HEAD:doomed.t) &&
	git -C gcprunestore reset --hard HEAD^ &&
	# test_commit also left a tag pointing at the doomed commit; drop it so
	# nothing references the doomed objects.
	git -C gcprunestore tag -d doomed &&
	git -C gcprunestore cat-file -e "$doomed" &&
	git -C gcprunestore gc --prune=now &&
	# Reachable history survives and the store stays intact.
	git -C gcprunestore cat-file -t HEAD >type &&
	grep commit type &&
	git -C gcprunestore fsck &&
	# The unreachable object, past its grace window at --prune=now, is swept.
	test_must_fail git -C gcprunestore cat-file -e "$doomed"
'

test_expect_success 'repack optimizes the helper object store' '
	create_helper_repo repackstore &&
	test_commit -C repackstore one &&
	test_commit -C repackstore two &&
	# "git repack" is the object-store analog of "git pack-refs": it routes
	# through the maintenance vtable, so on a helper primary it optimizes
	# the helper store rather than failing as a files-only command.
	git -C repackstore repack -d &&
	git -C repackstore cat-file -t HEAD >type &&
	grep commit type
'

test_expect_success 'repack sends the optimize command to the helper' '
	create_helper_repo repackoptstore &&
	test_commit -C repackoptstore opt &&
	test_when_finished "rm -f repackoptlog" &&
	(
		cd repackoptstore &&
		GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/repackoptlog" git repack -d
	) &&
	grep "^optimize$" repackoptlog
'

test_expect_success 'count-objects -v reports the local pack layout on the helper backend' '
	create_helper_repo countstore &&
	test_commit -C countstore one &&
	# "git count-objects -v" inspects the local on-disk pack layout, which it
	# now gathers by dispatch (odb_count_packs) rather than by walking files
	# packs directly. A helper keeps its objects in its own store, so the
	# files layout is empty: the command must report cleanly, with no packs.
	git -C countstore count-objects -v >out &&
	grep "^count: 0" out &&
	grep "^packs: 0" out
'

test_expect_success 'fsck verifies the helper object store' '
	create_helper_repo fsckstore &&
	test_commit -C fsckstore one &&
	test_commit -C fsckstore two &&
	git -C fsckstore fsck 2>err &&
	test_must_be_empty err
'

test_expect_success 'commit-graph write enumerates the helper object store' '
	create_helper_repo cgstore &&
	test_commit -C cgstore one &&
	test_commit -C cgstore two &&
	test_commit -C cgstore three &&
	# Default "write" finds commits by enumerating the consolidated object
	# store through the source vtable; a helper has no loose tier, so this
	# must reach the helper rather than being skipped as a non-files source.
	git -C cgstore commit-graph write &&
	# testgit has no commit-graph capability, so git writes its own file.
	test_path_is_file cgstore/.git/objects/info/commit-graph &&
	git -C cgstore commit-graph verify
'

test_expect_success 'fsck sends the verify command to the helper' '
	create_helper_repo fscklog &&
	test_commit -C fscklog fl &&
	test_when_finished "rm -f fscklog.txt" &&
	(
		cd fscklog &&
		GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/fscklog.txt" git fsck
	) &&
	grep "^verify$" fscklog.txt
'

test_expect_success 'fsck detects a corrupted object in the helper store' '
	create_helper_repo fsckbad &&
	test_commit -C fsckbad bad &&
	blob=$(git -C fsckbad rev-parse HEAD:bad.t) &&
	objfile="fsckbad/.git/helper-objects/$blob" &&
	read -r type size <"$objfile.meta" &&
	# Overwrite the content with the same number of bytes so the helper wire
	# framing stays intact while the object no longer hashes to its name.
	test_copy_bytes "$size" </dev/zero | tr "\0" X >"$objfile" &&
	test_must_fail git -C fsckbad fsck
'

test_expect_success 'clone --no-local serves a pack generated from a helper object store' '
	create_helper_repo servesrc &&
	test_commit -C servesrc one &&
	test_commit -C servesrc two &&
	# --no-local forces the upload-pack/pack-objects transport instead of
	# the file-copy shortcut, so the served pack is generated by reading
	# objects out of the helper store.
	git clone --no-local servesrc servedst &&
	git -C servesrc rev-parse HEAD >expect &&
	git -C servedst rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C servedst log --oneline >log &&
	test_line_count = 2 log &&
	git -C servedst fsck
'

test_expect_success 'bundle create generates a pack from a helper object store' '
	create_helper_repo bundlesrc &&
	test_commit -C bundlesrc a &&
	test_commit -C bundlesrc b &&
	# The bundle pack is generated by reading objects out of the helper
	# store; a complete-history bundle proves every reachable object made
	# it into the generated pack.
	git -C bundlesrc bundle create "$TRASH_DIRECTORY/all.bundle" --all &&
	git -C bundlesrc bundle verify "$TRASH_DIRECTORY/all.bundle" >verify &&
	grep "records a complete history" verify &&
	git -C bundlesrc bundle list-heads "$TRASH_DIRECTORY/all.bundle" >heads &&
	grep "refs/heads/main" heads
'

test_expect_success 'fast-import writes into the helper object store' '
	create_helper_repo fistore &&
	blob=$(git -C server rev-parse HEAD:big) &&
	git -C server fast-export --all >stream &&
	git -C fistore fast-import <stream &&
	# The imported objects are exploded into the helper, none loose.
	git -C server cat-file -p "$blob" >expect &&
	git -C fistore cat-file -p "$blob" >actual &&
	test_cmp expect actual &&
	assert_no_loose_objects fistore
'

test_expect_success 'fast-import streams a large blob into the helper store' '
	create_helper_repo fibig &&
	blob=$(git -C server rev-parse HEAD:big) &&
	git -C server fast-export --all >bigstream &&
	# A low bigFileThreshold forces fast-import down its streaming-blob path,
	# which must still reach the helper (it is read back out of the pack).
	git -C fibig -c core.bigFileThreshold=1 fast-import <bigstream &&
	git -C server cat-file -p "$blob" >expect &&
	git -C fibig cat-file -p "$blob" >actual &&
	test_cmp expect actual &&
	assert_no_loose_objects fibig
'

test_done
