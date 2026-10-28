#!/bin/sh
# Copy the mail files in a plan from compare-manifests.py into the node the
# plan is for. Run it where all nodes are reachable, e.g.:
#
#   fill-missing.sh plan-node2.txt node2 \
#       node1='ssh root@10.0.0.1' node2='ssh root@10.0.0.2' \
#       node3='ssh -p 220 root@10.0.0.3'
#
# Each NAME=COMMAND says how to run a command on a node ("sh -c" for this
# machine). Files are streamed with tar through this machine. Existing files
# are never overwritten. Folders that didn't exist get the owner of the
# user's Maildir. Run it while Dovecot and Syncthing are stopped on the
# nodes involved, so that no mail changes its name meanwhile.
#
# Environment: VMAIL (default /home/vmail).
set -eu

if [ $# -lt 3 ]; then
	sed -n '2,15p' "$0" >&2
	exit 64
fi
plan=$1
dest=$2
shift 2
vmail=${VMAIL:-/home/vmail}

node_cmd() {
	for spec in "$@"; do
		case "$spec" in
		"$want="*) echo "${spec#*=}"; return 0;;
		esac
	done
	return 1
}

want=$dest
dest_cmd=$(node_cmd "$@") || { echo "No command for $dest" >&2; exit 64; }

status=0
for src in $(cut -f1 "$plan" | sort -u); do
	want=$src
	src_cmd=$(node_cmd "$@") || { echo "No command for $src" >&2; exit 64; }
	count=$(awk -F '\t' -v s="$src" '$1 == s' "$plan" | wc -l)
	echo "$src -> $dest: $count files"
	# shellcheck disable=SC2086
	if ! awk -F '\t' -v s="$src" '$1 == s { print $2 }' "$plan" |
		$src_cmd "cd '$vmail' && tar --numeric-owner -cf - -T -" |
		$dest_cmd "cd '$vmail' && tar --numeric-owner --keep-old-files -xpf -"; then
		echo "Some files couldn't be copied from $src (renamed or" \
			"deleted since the manifest?): run manifest.py and" \
			"compare-manifests.py again" >&2
		status=1
	fi
done

# folders created by tar: Maildir subdirectories and owners
# shellcheck disable=SC2086
cut -f2 "$plan" | sed 's|/[^/]*/[^/]*$||' | sort -u |
	$dest_cmd "cd '$vmail' && while read -r dir; do
		user=\$(echo \"\$dir\" | cut -d/ -f1-2)
		mkdir -p \"\$dir/cur\" \"\$dir/new\" \"\$dir/tmp\"
		chown --reference=\"\$user\" \"\$dir\" \"\$dir/cur\" \"\$dir/new\" \"\$dir/tmp\"
	done"
exit $status
