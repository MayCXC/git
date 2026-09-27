#!/bin/sh

test_description='local helper object storage (extensions.objectStorage=helper://<name>)

Exercises the object source of the "helper" backend end to end against the
git-local-testgit helper, which keeps each object as a file under
"<gitdir>/helper-objects". Objects written through the object database go to
the helper directly; packfiles and loose objects written in the files layout,
as index-pack and receive-pack do, go to the files store of the source until
optimizing it takes them into the helper.'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# git-local-testgit ships with t0620 and is found via PATH, the same way
# git-remote-testgit is in t5801.
PATH="$TEST_DIRECTORY/t0620:$PATH"

create_helper_repo () {
	git init --object-storage=testgit "$@"
}

# The files store of the repository holds no objects: no directory of loose
# objects, no packs.
assert_files_store_empty () {
	objects="$(git -C "$1" rev-parse --path-format=absolute --git-path objects)" &&
	find "$objects" \( -path "$objects/??" -o -type f -name "*.pack" \) >files-store &&
	test_must_be_empty files-store
}

assert_object_in_helper () {
	test_path_is_file "$(git -C "$1" rev-parse --path-format=absolute --git-common-dir)/helper-objects/$2"
}

# The helper keeps the time an object was last written as the modification
# time of its .meta file; move it back by the given number of seconds.
age_object_in_helper () {
	test-tool chmtime "-$3" "$(git -C "$1" rev-parse --path-format=absolute --git-common-dir)/helper-objects/$2.meta"
}

test_expect_success 'setup files server repo' '
	git init server &&
	test_seq 1 200 >server/big &&
	git -C server add big &&
	git -C server commit -m v1
'

test_expect_success 'init records the helper in extensions.objectStorage' '
	test_when_finished "rm -rf repo plain" &&
	create_helper_repo repo &&
	echo helper://testgit >expect &&
	git -C repo config extensions.objectStorage >actual &&
	test_cmp expect actual &&
	echo 1 >expect &&
	git -C repo config core.repositoryFormatVersion >actual &&
	test_cmp expect actual &&
	git init plain &&
	git -C plain config get --default=unset extensions.refStorage >expect &&
	git -C repo config get --default=unset extensions.refStorage >actual &&
	test_cmp expect actual
'

test_expect_success 'init with helper://<name> records the same' '
	test_when_finished "rm -rf repo" &&
	git init --object-storage=helper://testgit repo &&
	echo helper://testgit >expect &&
	git -C repo config extensions.objectStorage >actual &&
	test_cmp expect actual
'

test_expect_success 'a bare helper name in extensions.objectStorage selects the helper' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	git -C repo config core.repositoryFormatVersion 1 &&
	git -C repo config extensions.objectStorage testgit &&
	blob=$(echo content | git -C repo hash-object -w --stdin) &&
	assert_object_in_helper repo "$blob" &&
	assert_files_store_empty repo
'

test_expect_success 'GIT_DEFAULT_OBJECT_STORAGE names the helper for a new repository' '
	test_when_finished "rm -rf repo" &&
	GIT_DEFAULT_OBJECT_STORAGE=testgit git init repo &&
	echo helper://testgit >expect &&
	git -C repo config extensions.objectStorage >actual &&
	test_cmp expect actual
'

test_expect_success 'reinitializing keeps the object storage and refuses another one' '
	test_when_finished "rm -rf repo" &&
	create_helper_repo repo &&
	git init --object-storage=testgit repo &&
	test_must_fail git init --object-storage=files repo 2>err &&
	test_grep "different object storage" err &&
	echo helper://testgit >expect &&
	git -C repo config extensions.objectStorage >actual &&
	test_cmp expect actual
'

test_expect_success 'plain init records no object storage' '
	test_when_finished "rm -rf repo" &&
	git init repo &&
	test_must_fail git -C repo config extensions.objectStorage
'

for value in "sqlite@/home/sqlite" "sqlite:" "db://.git" "files://objects" "helper" "helper://"
do
	test_expect_success "extensions.objectStorage rejects '$value'" '
		test_when_finished "rm -rf repo" &&
		git init repo &&
		git -C repo config core.repositoryFormatVersion 1 &&
		git -C repo config extensions.objectStorage "$value" &&
		test_must_fail git -C repo rev-parse --git-dir 2>err &&
		test_grep "invalid value for ${SQ}extensions.objectstorage${SQ}" err
	'
done

test_expect_success 'a helper that cannot store objects is refused' '
	test_when_finished "rm -rf repo bin" &&
	mkdir bin &&
	write_script bin/git-local-noobj <<-\EOF &&
	while read line
	do
		test "$line" = capabilities && printf "%s\n" read list transaction ""
	done
	EOF
	(
		PATH="$(pwd)/bin:$PATH" &&
		export PATH &&
		git init --object-storage=noobj repo &&
		test_must_fail git -C repo hash-object -w --stdin </dev/null 2>err
	) &&
	test_grep "helper .noobj. does not support the .get. capability" err
'

test_expect_success 'objects written through the object database go to the helper' '
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
	assert_object_in_helper store "$(git -C store rev-parse HEAD)" &&
	assert_files_store_empty store
'

test_expect_success 'log and diff work over the helper' '
	(
		cd store &&
		echo more >>file &&
		git commit -am two &&
		git log --oneline >log &&
		test_line_count = 2 log &&
		git diff --stat HEAD^ HEAD >diff &&
		test_grep file diff
	)
'

test_expect_success 'an abbreviated object ID resolves over the helper' '
	full=$(git -C store rev-parse HEAD:file) &&
	short=$(echo "$full" | cut -c1-10) &&
	echo blob >expect &&
	git -C store cat-file -t "$short" >actual &&
	test_cmp expect actual
'

test_expect_success RUST 'compatibility object IDs resolve over the helper' '
	test_when_finished "rm -rf compat" &&
	git init --object-format=sha256 --object-storage=testgit compat &&
	git -C compat config extensions.compatObjectFormat sha1 &&
	(
		cd compat &&
		echo "Hello World!" >hello &&
		git add hello &&
		git commit -m init &&
		test_path_is_file .git/objects/loose-object-idx &&
		blob1=$(git rev-parse --output-object-format=sha1 HEAD:hello) &&
		echo blob >expect &&
		git cat-file -t "$blob1" >actual &&
		test_cmp expect actual &&
		git cat-file -t "$(echo "$blob1" | cut -c1-12)" >actual &&
		test_cmp expect actual
	) &&
	assert_object_in_helper compat "$(git -C compat rev-parse HEAD:hello)"
'

# A helper may answer reads from a snapshot, so a miss makes it refresh the
# snapshot before the object database reads again.
test_expect_success 'a missed read refreshes the helper before reading again' '
	absent=$(echo not-a-stored-object | git -C store hash-object --stdin) &&
	test_when_finished "rm -f cmdlog" &&
	test_must_fail env GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/cmdlog" \
		git -C store cat-file -t "$absent" &&
	grep -E "^(info |refresh$)" cmdlog >reads &&
	cat >expect <<-EOF &&
	info $absent
	refresh
	info $absent
	EOF
	test_cmp expect reads
'

test_expect_success 'a large object is streamed into and out of the helper' '
	test_when_finished "rm -rf streamed biglog" &&
	create_helper_repo streamed &&
	test_seq 1 20000 >big.in &&
	blob=$(GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/biglog" \
	       git -C streamed -c core.bigFileThreshold=1k hash-object -w --stdin <big.in) &&
	test_grep "^put-stream blob " biglog &&
	assert_object_in_helper streamed "$blob" &&
	git -C streamed cat-file -p "$blob" >big.out &&
	test_cmp big.in big.out
'

test_expect_success 'fetch writes a pack into the files store, maintenance moves it into the helper' '
	create_helper_repo client &&
	git -C client remote add origin "$(pwd)/server" &&
	git -C client -c fetch.unpackLimit=1 fetch --no-auto-maintenance origin &&
	git -C client rev-parse origin/main >actual &&
	git -C server rev-parse main >expect &&
	test_cmp expect actual &&
	ls client/.git/objects/pack/*.pack &&
	git -C client maintenance run --auto &&
	assert_files_store_empty client &&
	assert_object_in_helper client "$(git -C server rev-parse main:big)" &&
	git -C client fsck
'

test_expect_success 'fetch resolves a thin pack against bases in the helper' '
	echo "line 201" >>server/big &&
	git -C server commit -am v2 &&
	git -C client -c fetch.unpackLimit=1 fetch origin &&
	git -C server cat-file -p main:big >expect &&
	git -C client cat-file -p origin/main:big >actual &&
	test_cmp expect actual &&
	git -C client gc &&
	assert_files_store_empty client &&
	git -C client cat-file -p origin/main:big >actual &&
	test_cmp expect actual
'

test_expect_success 'setup a server whose pack holds deltas' '
	git init deltasrv &&
	test_seq 1 300 >deltasrv/f &&
	git -C deltasrv add f &&
	git -C deltasrv commit -m one &&
	test_seq 1 301 >deltasrv/f &&
	git -C deltasrv commit -am two &&
	git -C deltasrv repack -ad &&
	git -C deltasrv cat-file --batch-all-objects \
		--batch-check="%(deltabase)" >deltabases &&
	test_grep -v $ZERO_OID deltabases
'

test_expect_success 'the deltas of a pack stay deltas in the helper' '
	test_when_finished "rm -rf deltacl" &&
	create_helper_repo deltacl &&
	git -C deltacl -c fetch.unpackLimit=1 fetch --no-auto-maintenance \
		"$(pwd)/deltasrv" main:src &&
	git -C deltacl gc &&
	assert_files_store_empty deltacl &&
	git -C deltacl cat-file --batch-all-objects \
		--batch-check="%(deltabase)" >deltabases &&
	test_grep -v $ZERO_OID deltabases &&
	git -C deltacl fsck
'

test_expect_success 'a helper without put-raw takes the objects of a pack whole' '
	test_when_finished "rm -rf wholecl" &&
	create_helper_repo wholecl &&
	git -C wholecl -c fetch.unpackLimit=1 fetch --no-auto-maintenance \
		"$(pwd)/deltasrv" main:src &&
	GIT_LOCAL_TESTGIT_CAPABILITIES="get info put have list-objects optimize" \
		git -C wholecl gc &&
	assert_files_store_empty wholecl &&
	git -C wholecl cat-file --batch-all-objects \
		--batch-check="%(deltabase)" >deltabases &&
	test_grep ! -v $ZERO_OID deltabases &&
	git -C wholecl fsck
'

# index-pack completes a thin pack by appending the bases it lacks, after the
# deltas against them. With the first pack kept, the base of the new delta
# moves into the helper only from the end of the second pack, so the delta
# waits for it. The helper lacks "replace", so that gc does not go on to
# store the objects the way it finds best.
test_expect_success 'a delta of a thin pack waits for the base appended to it' '
	test_when_finished "rm -rf thinsrv thincl" &&
	git init thinsrv &&
	test_seq 1 300 >thinsrv/f &&
	git -C thinsrv add f &&
	git -C thinsrv commit -m one &&
	create_helper_repo thincl &&
	git -C thincl -c fetch.unpackLimit=1 fetch --no-auto-maintenance \
		"$(pwd)/thinsrv" main:src &&
	for pack in thincl/.git/objects/pack/*.pack
	do
		>"${pack%.pack}.keep" || return 1
	done &&
	test_seq 1 301 >thinsrv/f &&
	git -C thinsrv commit -am two &&
	git -C thincl -c fetch.unpackLimit=1 fetch --no-auto-maintenance \
		"$(pwd)/thinsrv" main:src &&
	GIT_LOCAL_TESTGIT_CAPABILITIES="get info put put-raw have list-objects" \
		git -C thincl gc &&
	git -C thincl rev-parse src~1:f >expect &&
	git -C thincl rev-parse src:f >blob &&
	git -C thincl cat-file --batch-check="%(deltabase)" <blob >actual &&
	test_cmp expect actual &&
	git -C thinsrv cat-file -p main:f >expect &&
	git -C thincl cat-file -p src:f >actual &&
	test_cmp expect actual &&
	git -C thincl fsck
'

test_expect_success 'fetch with fsckObjects checks objects into the helper repository' '
	echo "line 202" >>server/big &&
	git -C server commit -am v3 &&
	git -C client -c fetch.fsckObjects=true -c fetch.unpackLimit=1 fetch origin &&
	git -C server rev-parse main >expect &&
	git -C client rev-parse origin/main >actual &&
	test_cmp expect actual
'

test_expect_success 'a pack kept with a .keep file stays in the files store' '
	test_when_finished "rm -rf keeper" &&
	create_helper_repo keeper &&
	git -C keeper -c fetch.unpackLimit=1 fetch --no-auto-maintenance \
		"$(pwd)/server" main:refs/heads/kept &&
	pack=$(ls keeper/.git/objects/pack/*.pack) &&
	touch "${pack%.pack}.keep" &&
	git -C keeper gc &&
	test_path_is_file "$pack" &&
	find keeper/.git/helper-objects -name "*.meta" >stored &&
	test_must_be_empty stored &&
	rm "${pack%.pack}.keep" &&
	git -C keeper gc &&
	assert_files_store_empty keeper &&
	git -C keeper fsck
'

test_expect_success 'extensions.preciousObjects keeps the files store as it is' '
	test_when_finished "rm -rf precious" &&
	create_helper_repo precious &&
	git -C precious -c fetch.unpackLimit=1 fetch --no-auto-maintenance \
		"$(pwd)/server" main:refs/heads/fetched &&
	git -C precious config extensions.preciousObjects true &&
	git -C precious maintenance run --task=gc &&
	ls precious/.git/objects/pack/*.pack
'

test_expect_success 'clone --object-storage clones into the helper' '
	test_when_finished "rm -rf cloned" &&
	git clone --object-storage=testgit --no-local server cloned &&
	echo helper://testgit >expect &&
	git -C cloned config extensions.objectStorage >actual &&
	test_cmp expect actual &&
	git -C server rev-parse HEAD >expect &&
	git -C cloned rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C cloned gc &&
	assert_files_store_empty cloned &&
	git -C cloned fsck
'

test_expect_success 'a local clone into the helper repository drains like a fetch' '
	test_when_finished "rm -rf localcloned" &&
	git clone --object-storage=testgit server localcloned &&
	git -C localcloned gc &&
	assert_files_store_empty localcloned &&
	assert_object_in_helper localcloned "$(git -C server rev-parse HEAD:big)" &&
	git -C localcloned fsck
'

test_expect_success 'a worktree shares the objects of the helper' '
	test_when_finished "rm -rf wt-main wt-linked" &&
	create_helper_repo wt-main &&
	test_commit -C wt-main one &&
	git -C wt-main worktree add ../wt-linked &&
	test_commit -C wt-linked two &&
	git -C wt-linked log --oneline >actual &&
	test_line_count = 2 actual &&
	assert_object_in_helper wt-main "$(git -C wt-linked rev-parse HEAD:two.t)" &&
	assert_files_store_empty wt-main
'

test_expect_success 'objects are read from a files alternate of the helper repository' '
	test_when_finished "rm -rf alt-store with-alt" &&
	git init alt-store &&
	blob=$(echo "from the alternate" | git -C alt-store hash-object -w --stdin) &&
	create_helper_repo with-alt &&
	echo "$(pwd)/alt-store/.git/objects" >with-alt/.git/objects/info/alternates &&
	echo "from the alternate" >expect &&
	git -C with-alt cat-file -p "$blob" >actual &&
	test_cmp expect actual
'

test_expect_success 'a partial fetch keeps its objects promisor objects in the helper' '
	test_when_finished "rm -rf partial" &&
	git -C server config uploadpack.allowFilter true &&
	create_helper_repo partial &&
	git -C partial remote add origin "$(pwd)/server" &&
	git -C partial fetch --no-auto-maintenance --filter=blob:none origin &&
	git -C partial rev-list --objects --all --exclude-promisor-objects >kept &&
	test_must_be_empty kept &&
	git -C partial gc &&
	assert_files_store_empty partial &&
	git -C partial rev-list --objects --all --missing=print >objs &&
	test_grep "^?$(git -C server rev-parse HEAD:big)" objs &&
	git -C partial rev-list --objects --all --exclude-promisor-objects >kept &&
	test_must_be_empty kept
'

test_expect_success 'a missing promisor object is fetched on demand' '
	test_when_finished "rm -rf lazy" &&
	create_helper_repo lazy &&
	git -C lazy remote add origin "$(pwd)/server" &&
	git -C lazy fetch --filter=blob:none origin &&
	blob=$(git -C server rev-parse HEAD:big) &&
	test_must_fail env GIT_NO_LAZY_FETCH=1 git -C lazy cat-file -e "$blob" &&
	git -C lazy cat-file -p "$blob" >actual &&
	git -C server cat-file -p "$blob" >expect &&
	test_cmp expect actual &&
	GIT_NO_LAZY_FETCH=1 git -C lazy cat-file -e "$blob"
'

test_expect_success 'shallow fetch and unshallow into the helper repository' '
	test_when_finished "rm -rf shallow" &&
	create_helper_repo shallow &&
	git -C shallow remote add origin "$(pwd)/server" &&
	git -C shallow fetch --depth=1 origin &&
	test_path_is_file shallow/.git/shallow &&
	git -C shallow gc &&
	git -C shallow fetch --unshallow origin &&
	test_path_is_missing shallow/.git/shallow &&
	git -C shallow fsck
'

test_expect_success 'setup push source' '
	git init pushsrc &&
	printf "%08000d\nAAAA\n" 0 >pushsrc/f.txt &&
	git -C pushsrc add f.txt &&
	git -C pushsrc commit -q -m one &&
	printf "%08000d\nBBBB\n" 0 >pushsrc/f.txt &&
	git -C pushsrc add f.txt &&
	git -C pushsrc commit -q -m two
'

for limit in 1 10000
do
	test_expect_success "push into the helper repository with unpackLimit $limit" '
		test_when_finished "rm -rf pushdst" &&
		create_helper_repo --bare pushdst &&
		git -C pushdst config transfer.unpackLimit $limit &&
		git -C pushdst config receive.autogc false &&
		git -C pushsrc push "$PWD/pushdst" main &&
		git -C pushsrc rev-parse main >expect &&
		git -C pushdst rev-parse main >actual &&
		test_cmp expect actual &&
		git -C pushdst fsck &&
		git -C pushdst gc &&
		assert_files_store_empty pushdst &&
		git -C pushsrc cat-file blob main:f.txt >expect &&
		git -C pushdst cat-file blob main:f.txt >actual &&
		test_cmp expect actual
	'
done

test_expect_success 'a loose object arriving while gc moves the others stays' '
	test_when_finished "rm -rf racing donor" &&
	create_helper_repo racing &&
	test_commit -C racing base &&
	git init donor &&
	blobs=$(for i in 1 2 3
		do
			echo $i | git -C donor hash-object -w --stdin || return 1
		done | sort) &&
	present=$(echo "$blobs" | tail -n 1) &&
	arriving=$(echo "$blobs" | head -n 1) &&
	test $(echo $present | cut -c1-2) != $(echo $arriving | cut -c1-2) &&
	src="$PWD/donor/.git/objects/$(test_oid_to_path $present)" &&
	dst="$PWD/racing/.git/objects/$(test_oid_to_path $present)" &&
	mkdir -p "${dst%/*}" &&
	cp "$src" "$dst" &&

	# The other object arrives in a directory gc has looked in already,
	# as gc stores the first one in the helper.
	src="$PWD/donor/.git/objects/$(test_oid_to_path $arriving)" &&
	dst="$PWD/racing/.git/objects/$(test_oid_to_path $arriving)" &&
	GIT_LOCAL_TESTGIT_ON_COMMAND="case \"\$line\" in put*)
		test -e \"$PWD/arrived\" || {
			mkdir -p \"${dst%/*}\" && cp \"$src\" \"$dst\" &&
			>\"$PWD/arrived\"
		};;
	esac" git -C racing gc &&
	test_path_is_file arrived &&
	assert_object_in_helper racing $present &&
	test_path_is_missing "racing/.git/objects/$(test_oid_to_path $present)" &&
	test_path_is_missing "racing/.git/objects/$(echo $present | cut -c1-2)" &&
	test_path_is_file "$dst" &&
	git -C racing cat-file -e $arriving
'

test_expect_success 'hooks in the quarantine of a push read the objects of the helper' '
	test_when_finished "rm -rf hooked" &&
	create_helper_repo --bare hooked &&
	git -C pushsrc push "$PWD/hooked" main~1:refs/heads/main &&
	git -C hooked gc &&
	assert_files_store_empty hooked &&
	old=$(git -C pushsrc rev-parse main~1:f.txt) &&
	write_script hooked/hooks/pre-receive <<-EOF &&
	git cat-file -e $old
	EOF
	git -C pushsrc push "$PWD/hooked" main
'

test_expect_success 'a rejected push leaves the helper repository untouched' '
	test_when_finished "rm -rf rejecting" &&
	create_helper_repo --bare rejecting &&
	git -C pushsrc push "$PWD/rejecting" main~1:refs/heads/main &&
	write_script rejecting/hooks/pre-receive <<-\EOF &&
	exit 1
	EOF
	test_must_fail git -C pushsrc push "$PWD/rejecting" main &&
	test_must_fail git -C rejecting cat-file -e "$(git -C pushsrc rev-parse main:f.txt)" &&
	git -C rejecting cat-file -e "$(git -C pushsrc rev-parse main~1:f.txt)" &&
	git -C rejecting fsck
'

test_expect_success 'gc has the helper store the objects written whole as deltas' '
	test_when_finished "rm -rf deltified" &&
	create_helper_repo deltified &&
	test_seq 1 300 >deltified/f &&
	git -C deltified add f &&
	git -C deltified commit -m one &&
	test_seq 1 301 >deltified/f &&
	git -C deltified commit -am two &&
	git -C deltified cat-file --batch-all-objects \
		--batch-check="%(deltabase)" >before &&
	test_grep ! -v $ZERO_OID before &&
	git -C deltified gc &&
	assert_files_store_empty deltified &&
	git -C deltified cat-file --batch-all-objects \
		--batch-check="%(deltabase)" >after &&
	test_grep -v $ZERO_OID after &&
	test_seq 1 301 >expect &&
	git -C deltified cat-file -p HEAD:f >actual &&
	test_cmp expect actual &&
	git -C deltified fsck
'

test_expect_success 'gc leaves how a helper without replace stores its objects' '
	test_when_finished "rm -rf asis" &&
	create_helper_repo asis &&
	test_seq 1 300 >asis/f &&
	git -C asis add f &&
	git -C asis commit -m one &&
	test_seq 1 301 >asis/f &&
	git -C asis commit -am two &&
	GIT_LOCAL_TESTGIT_CAPABILITIES="get info put put-raw have list-objects optimize" \
		git -C asis gc &&
	git -C asis cat-file --batch-all-objects \
		--batch-check="%(deltabase)" >after &&
	test_grep ! -v $ZERO_OID after
'

test_expect_success 'gc sends the helper the optimize command' '
	test_when_finished "rm -rf optimized optlog" &&
	create_helper_repo optimized &&
	test_commit -C optimized one &&
	GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/optlog" git -C optimized gc &&
	test_grep "^optimize$" optlog
'

test_expect_success 'fsck checks the objects of the helper and has it verify its store' '
	test_when_finished "rm -rf checked fscklog" &&
	create_helper_repo checked &&
	test_commit -C checked one &&
	test_commit -C checked two &&
	GIT_LOCAL_TESTGIT_LOG="$TRASH_DIRECTORY/fscklog" git -C checked fsck 2>err &&
	test_must_be_empty err &&
	test_grep "^verify$" fscklog
'

test_expect_success 'prune removes the unreachable objects of the helper' '
	test_when_finished "rm -rf pruned" &&
	create_helper_repo pruned &&
	test_commit -C pruned one &&
	old=$(echo old | git -C pruned hash-object -w --stdin) &&
	new=$(echo new | git -C pruned hash-object -w --stdin) &&
	age_object_in_helper pruned $old 86400 &&
	git -C pruned prune --expire=1.hour.ago &&
	test_must_fail git -C pruned cat-file -e $old &&
	git -C pruned cat-file -e $new &&
	git -C pruned fsck &&
	git -C pruned prune &&
	test_must_fail git -C pruned cat-file -e $new &&
	git -C pruned cat-file -e HEAD:one.t
'

test_expect_success 'prune --dry-run names the objects of the helper it would remove' '
	test_when_finished "rm -rf dryrun" &&
	create_helper_repo dryrun &&
	test_commit -C dryrun one &&
	blob=$(echo stray | git -C dryrun hash-object -w --stdin) &&
	git -C dryrun prune --dry-run >actual &&
	echo "$blob blob" >expect &&
	test_cmp expect actual &&
	git -C dryrun cat-file -e $blob
'

test_expect_success 'an object written again is freshened and outlives the prune' '
	test_when_finished "rm -rf fresh" &&
	create_helper_repo fresh &&
	test_commit -C fresh one &&
	blob=$(echo again | git -C fresh hash-object -w --stdin) &&
	age_object_in_helper fresh $blob 86400 &&
	echo again | git -C fresh hash-object -w --stdin &&
	git -C fresh prune --expire=1.hour.ago &&
	git -C fresh cat-file -e $blob
'

test_expect_success 'gc prunes an unreachable object of the helper past gc.pruneExpire' '
	test_when_finished "rm -rf gcpruned" &&
	create_helper_repo gcpruned &&
	test_commit -C gcpruned one &&
	blob=$(echo stale | git -C gcpruned hash-object -w --stdin) &&
	age_object_in_helper gcpruned $blob 2592000 &&
	git -C gcpruned gc &&
	test_must_fail git -C gcpruned cat-file -e $blob &&
	git -C gcpruned fsck
'

test_expect_success 'fsck finds a damaged object in the helper' '
	test_when_finished "rm -rf damaged" &&
	create_helper_repo damaged &&
	test_commit -C damaged bad &&
	blob=$(git -C damaged rev-parse HEAD:bad.t) &&
	object="damaged/.git/helper-objects/$blob" &&
	read -r type size <"$object.meta" &&
	test_copy_bytes "$size" </dev/zero | tr "\0" X >"$object" &&
	test_must_fail git -C damaged fsck
'

test_expect_success 'rev-list --no-kept-objects works over the helper' '
	git -C store rev-list --no-kept-objects --all >actual &&
	test_line_count = 2 actual
'

test_expect_success 'count-objects reports the files store of the helper repository' '
	git -C store count-objects -v >out &&
	test_grep "^count: 0" out &&
	test_grep "^packs: 0" out
'

test_expect_success 'commit-graph write --reachable reads commits from the helper' '
	test_when_finished "rm -rf graphed" &&
	create_helper_repo graphed &&
	test_commit -C graphed one &&
	test_commit -C graphed two &&
	git -C graphed commit-graph write --reachable &&
	test_path_is_file graphed/.git/objects/info/commit-graph &&
	git -C graphed commit-graph verify
'

test_expect_success 'gc writes a commit-graph for the commits of the helper' '
	test_when_finished "rm -rf graphed" &&
	create_helper_repo graphed &&
	test_commit -C graphed one &&
	test_commit -C graphed two &&
	git -C graphed gc &&
	assert_files_store_empty graphed &&
	test_path_is_file graphed/.git/objects/info/commit-graph &&
	git -C graphed commit-graph verify
'

test_expect_success 'clone --no-local serves a pack generated from the helper' '
	test_when_finished "rm -rf served" &&
	git clone --no-local store served &&
	git -C store rev-parse HEAD >expect &&
	git -C served rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C served fsck
'

test_expect_success 'a local clone of the helper repository fetches its objects' '
	test_when_finished "rm -rf localclone" &&
	git clone store localclone &&
	git -C store rev-parse HEAD >expect &&
	git -C localclone rev-parse HEAD >actual &&
	test_cmp expect actual &&
	git -C localclone fsck
'

test_expect_success 'clone --local of the helper repository fetches with a warning' '
	test_when_finished "rm -rf localclone" &&
	git clone --local store localclone 2>err &&
	test_grep "stores its objects in a helper, ignoring --local" err &&
	git -C localclone fsck
'

test_expect_success 'clone --shared of the helper repository is refused' '
	test_when_finished "rm -rf sharedclone" &&
	test_must_fail git clone --shared store sharedclone 2>err &&
	test_grep "cannot share the objects" err
'

test_expect_success 'the helper repository is refused as a reference' '
	test_when_finished "rm -rf referenced" &&
	test_must_fail git clone --reference store --no-local store referenced 2>err &&
	test_grep "stores its objects in a helper" err
'

test_expect_success 'bundle create generates a pack from the helper' '
	git -C store bundle create "$TRASH_DIRECTORY/all.bundle" --all &&
	git -C store bundle verify "$TRASH_DIRECTORY/all.bundle" >verify &&
	test_grep "records a complete history" verify
'

test_expect_success 'fast-import writes a pack into the files store of the helper repository' '
	test_when_finished "rm -rf imported" &&
	create_helper_repo imported &&
	git -C server fast-export --all >stream &&
	git -C imported fast-import <stream &&
	blob=$(git -C server rev-parse HEAD:big) &&
	git -C server cat-file -p "$blob" >expect &&
	git -C imported cat-file -p "$blob" >actual &&
	test_cmp expect actual &&
	git -C imported gc &&
	assert_files_store_empty imported &&
	assert_object_in_helper imported "$blob"
'

test_done
