#!/usr/bin/env python3
"""Find mails that some nodes are missing, before the mesh starts.

The mesh's first session between two nodes has no state. In it, mails that
one node lacks below UIDs where both nodes have the same mails are taken
for deletions and deleted on the other node too. A copy made by other means
(Syncthing) that is missing files can look like that, so fill in the
missing files first: then every node has every mail of the users it shares
with others, and the first sessions only merge.

For every user that two or more nodes have (per their userdb files), this
lists the mail files missing on a node, folder by folder, and writes one
copy plan per node: plan-<node>.txt, lines "<source node><TAB><path>",
paths relative to the mail root. fill-missing.sh carries them out. A node
that has the user in its userdb but no Maildir for it at all isn't filled:
the mesh copies the whole mailbox to it, which is safe.

Usage:
  compare-manifests.py --node NAME=MANIFEST[,USERDB] ... [--plans DIR]

MANIFEST is manifest.py's output (.gz is fine); USERDB the node's userdb
passwd-file (without it, the users with a Maildir on the node).
"""

import argparse
import collections
import gzip
import os
import sys


def open_text(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", errors="replace")
    return open(path, errors="replace")


def read_users(path):
    users = set()
    with open_text(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                users.add(line.split(":", 1)[0].lower())
    return users


def read_manifest(path):
    """user -> folder -> base -> (sub, file name)"""
    mails = collections.defaultdict(lambda: collections.defaultdict(dict))
    with open_text(path) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 5:
                continue
            user, folder, sub, base, fname = parts
            mails[user.lower()][folder][base] = (sub, fname)
    return mails


def relpath(user, folder, sub, fname):
    local, domain = user.rsplit("@", 1)
    fdir = "" if folder == "INBOX" else "." + folder
    return "/".join(p for p in (domain, local, fdir, sub, fname) if p)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--node", action="append", required=True,
                   help="NAME=MANIFEST[,USERDB]")
    p.add_argument("--plans", default=".",
                   help="directory for the plan-<node>.txt files")
    args = p.parse_args()

    nodes = []
    for spec in args.node:
        name, rest = spec.split("=", 1)
        manifest, _, userdb = rest.partition(",")
        mails = read_manifest(manifest)
        users = read_users(userdb) if userdb else set(mails)
        nodes.append((name, mails, users))

    plans = {name: [] for name, _, _ in nodes}
    all_users = sorted(set().union(*(users for _, _, users in nodes)))
    shared = missing_total = elsewhere_total = 0
    print("%-40s %s" % ("user", "  ".join(
        "%12s" % name for name, _, _ in nodes)))
    for user in all_users:
        having = [(n, m) for n, m, u in nodes if user in u]
        if len(having) < 2:
            continue
        shared += 1
        # only nodes that have a Maildir for the user get filled
        with_mail = [(n, m) for n, m in having if user in m]
        folders = sorted(set().union(*(m[user].keys() for _, m in with_mail)))
        counts = {}
        for name, mails in having:
            if user not in mails:
                counts[name] = "no Maildir"
                continue
            mine = mails[user]
            all_bases = set().union(*(f.keys() for f in mine.values()))
            missing = elsewhere = 0
            for folder in folders:
                have = mine.get(folder, {})
                # the first other node that has a mail provides it
                sources = {}
                for src, smails in with_mail:
                    if src == name:
                        continue
                    for base, (sub, fname) in \
                            smails[user].get(folder, {}).items():
                        if base not in have and base not in sources:
                            sources[base] = (src, sub, fname)
                for base, (src, sub, fname) in sorted(sources.items()):
                    missing += 1
                    if base in all_bases:
                        # moved on one node but not (yet) on this one:
                        # it ends up in both folders
                        elsewhere += 1
                    plans[name].append(
                        (src, relpath(user, folder, sub, fname)))
            counts[name] = ("ok" if missing == 0 else
                            "%d missing" % missing +
                            (" (%d moved)" % elsewhere if elsewhere else ""))
            missing_total += missing
            elsewhere_total += elsewhere
        if any(c != "ok" for c in counts.values()):
            print("%-40s %s" % (user, "  ".join(
                "%12s" % counts.get(name, "-") for name, _, _ in nodes)))

    print()
    print("users on two or more nodes: %d" % shared)
    print("mail files to copy: %d (of which %d are in another folder on the "
          "destination, and end up in both)" % (missing_total,
                                                 elsewhere_total))
    for name, plan in plans.items():
        path = os.path.join(args.plans, "plan-%s.txt" % name)
        with open(path, "w") as f:
            for src, rel in plan:
                f.write("%s\t%s\n" % (src, rel))
        print("%s: %d files to copy in (%s)" % (name, len(plan), path))
    sys.exit(1 if missing_total else 0)


if __name__ == "__main__":
    main()
