#!/bin/sh

test_description='ODB helper backend basics'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# Write a combined helper that handles both ODB and ref commands.
# The helper name is "testgit", so git discovers it as "git-local-testgit".
write_helper () {
	write_script "$1" <<-'EOF'
	OBJDIR="$1/helper-objects"
	REFDIR="$1/helper-refs"
	REFLOGDIR="$1/helper-reflogs"
	mkdir -p "$OBJDIR" "$REFDIR" "$REFLOGDIR"
	while IFS= read -r line; do
		case "$line" in
		capabilities)
			printf '%s\n' get put put-stream have info list-objects \
				read list transaction create \
				reflog-read reflog-read-reverse reflog-append \
				reflog-exists reflog-delete reflog-list ""
			;;
		info\ *)
			oid="${line#info }"
			if test -f "$OBJDIR/$oid.meta"; then
				cat "$OBJDIR/$oid.meta"
			else
				echo missing
			fi
			;;
		get\ *)
			oid="${line#get }"
			if test -f "$OBJDIR/$oid.meta"; then
				cat "$OBJDIR/$oid.meta"
				cat "$OBJDIR/$oid"
			else
				echo missing
			fi
			;;
		put\ *)
			# put <oid> <type> <size>
			oid=$(echo "$line" | cut -d' ' -f2)
			type=$(echo "$line" | cut -d' ' -f3)
			size=$(echo "$line" | cut -d' ' -f4)
			printf '%s %s\n' "$type" "$size" >"$OBJDIR/$oid.meta"
			head -c "$size" >"$OBJDIR/$oid"
			echo "$oid"
			;;
		have\ *)
			oid="${line#have }"
			if test -f "$OBJDIR/$oid.meta"; then
				echo true
			else
				echo false
			fi
			;;
		list-objects)
			for meta in "$OBJDIR"/*.meta; do
				test -f "$meta" || continue
				oid=$(basename "$meta" .meta)
				read -r type size <"$meta"
				printf '%s %s %s\n' "$oid" "$type" "$size"
			done
			echo ""
			;;
		read\ *)
			refname="${line#read }"
			file="$REFDIR/$(echo "$refname" | tr / _)"
			if test -f "$file"; then
				cat "$file"
			else
				echo missing
			fi
			;;
		list|list\ *)
			prefix="${line#list}"
			prefix="${prefix# }"
			for f in "$REFDIR"/*; do
				test -f "$f" || continue
				refname=$(basename "$f" | tr _ /)
				case "$refname" in
				"$prefix"*)
					val=$(cat "$f")
					case "$val" in
					symref\ *)
						target="${val#symref }"
						echo "$refname symref $target"
						;;
					*)
						echo "$refname $val"
						;;
					esac
					;;
				esac
			done
			echo ""
			;;
		create)
			echo ok
			;;
		transaction-begin)
			TXUPDATES=""
			echo ok
			;;
		transaction-update\ *|transaction-create\ *|\
		transaction-delete\ *|transaction-create-symref\ *)
			TXUPDATES="$TXUPDATES
$line"
			echo ok
			;;
		transaction-prepare)
			echo ok
			;;
		transaction-finish)
			echo "$TXUPDATES" | while IFS= read -r upd; do
				test -z "$upd" && continue
				case "$upd" in
				transaction-update\ *|transaction-create\ *)
					set -- $upd
					shift
					refname="$1"
					new_oid="$2"
					file="$REFDIR/$(echo "$refname" | tr / _)"
					echo "$new_oid" >"$file"
					;;
				transaction-delete\ *)
					set -- $upd
					shift
					refname="$1"
					file="$REFDIR/$(echo "$refname" | tr / _)"
					rm -f "$file"
					;;
				transaction-create-symref\ *)
					set -- $upd
					shift
					refname="$1"
					target="$2"
					file="$REFDIR/$(echo "$refname" | tr / _)"
					echo "symref $target" >"$file"
					;;
				esac
			done
			echo ok
			;;
		transaction-abort)
			TXUPDATES=""
			echo ok
			;;
		put-stream\ *)
			type=$(echo "$line" | cut -d' ' -f2)
			size=$(echo "$line" | cut -d' ' -f3)
			tmpfile=$(mktemp "$OBJDIR/tmp.XXXXXX")
			head -c "$size" >"$tmpfile"
			oid=$(git hash-object -t "$type" --stdin <"$tmpfile")
			printf '%s %s\n' "$type" "$size" >"$OBJDIR/$oid.meta"
			mv "$tmpfile" "$OBJDIR/$oid"
			echo "$oid"
			;;
		reflog-read\ *)
			refname="${line#reflog-read }"
			file="$REFLOGDIR/$(echo "$refname" | tr / _)"
			if test -f "$file"; then cat "$file"; fi
			echo ""
			;;
		reflog-read-reverse\ *)
			refname="${line#reflog-read-reverse }"
			file="$REFLOGDIR/$(echo "$refname" | tr / _)"
			if test -f "$file"; then
				sed -n '1!G;h;$p' "$file"
			fi
			echo ""
			;;
		reflog-append\ *)
			rest="${line#reflog-append }"
			refname="${rest%% *}"
			entry="${rest#* }"
			file="$REFLOGDIR/$(echo "$refname" | tr / _)"
			echo "$entry" >>"$file"
			echo ok
			;;
		reflog-exists\ *)
			refname="${line#reflog-exists }"
			file="$REFLOGDIR/$(echo "$refname" | tr / _)"
			if test -f "$file"; then echo true; else echo false; fi
			;;
		reflog-delete\ *)
			refname="${line#reflog-delete }"
			file="$REFLOGDIR/$(echo "$refname" | tr / _)"
			rm -f "$file"
			echo ok
			;;
		reflog-list)
			for f in "$REFLOGDIR"/*; do
				test -f "$f" || continue
				basename "$f" | tr _ /
			done
			echo ""
			;;
		"")
			;;
		esac
	done
	EOF
}

test_expect_success 'setup helper in PATH' '
	mkdir helper-bin &&
	write_helper helper-bin/git-local-testgit &&
	PATH="$PWD/helper-bin:$PATH" &&
	export PATH
'

# Helper to create a repo with the ODB helper backend configured.
setup_odb_helper_repo () {
	git init "$1" &&
	git config --file "$1/.git/config" core.repositoryformatversion 1 &&
	git config --file "$1/.git/config" extensions.objectStorage helper &&
	git config --file "$1/.git/config" extensions.localHelper testgit
}

test_expect_success 'config: extensions.objectStorage = helper with localHelper is accepted' '
	test_when_finished "rm -rf repo" &&
	setup_odb_helper_repo repo &&
	git -C repo rev-parse --git-dir
'

test_expect_success 'hash-object: write through helper backend' '
	test_when_finished "rm -rf repo" &&
	setup_odb_helper_repo repo &&
	echo "hello world" >repo/file &&
	oid=$(git -C repo hash-object -w --stdin <repo/file) &&
	test -n "$oid" &&
	test -f repo/.git/objects/helper-objects/$oid
'

test_expect_success 'cat-file: read through helper backend' '
	test_when_finished "rm -rf repo" &&
	setup_odb_helper_repo repo &&
	echo "hello world" >repo/file &&
	oid=$(git -C repo hash-object -w --stdin <repo/file) &&
	echo "hello world" >expect &&
	git -C repo cat-file blob $oid >actual &&
	test_cmp expect actual
'

test_expect_success 'cat-file -t: type through helper backend' '
	test_when_finished "rm -rf repo" &&
	setup_odb_helper_repo repo &&
	echo "hello world" >repo/file &&
	oid=$(git -C repo hash-object -w --stdin <repo/file) &&
	echo blob >expect &&
	git -C repo cat-file -t $oid >actual &&
	test_cmp expect actual
'

test_expect_success 'hash-object: write and read back larger content' '
	test_when_finished "rm -rf repo" &&
	setup_odb_helper_repo repo &&
	test_seq 1 500 >repo/bigfile &&
	oid=$(git -C repo hash-object -w --stdin <repo/bigfile) &&
	git -C repo cat-file blob $oid >actual &&
	test_cmp repo/bigfile actual
'

test_done
