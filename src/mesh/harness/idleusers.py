#!/usr/bin/env python3
# Users that nobody touches must still be synced to every node, e.g. to a
# new or emptied node, and copies that differ must be merged. Seed users'
# Maildirs directly on the first node (no change notifications), give one
# user different mails on the first two nodes, start the mesh without
# triggering anything, and check that every node ends up with every mail.

import argparse
import os
import sys
import time

import cluster as cl
import workload as wl
from phase1 import wait_quiet


def seed_maildir(node, user, prefix, count):
    """Write mails straight into the node's Maildir before it starts."""
    maildir = os.path.join(node.dir, "home", user, "Maildir")
    for sub in ("cur", "new", "tmp"):
        os.makedirs(os.path.join(maildir, sub), exist_ok=True)
    now = int(time.time())
    msgids = []
    for i in range(count):
        msgid = "%s-%d-%s@mesh" % (prefix, i, user)
        data = ("From: seed@example.com\nTo: %s\nMessage-ID: <%s>\n"
                "Subject: %s %d\n\nbody %d\n" %
                (user, msgid, prefix, i, i)).encode()
        name = "%d.M%dP1.%s,S=%d,W=%d:2,S" % (
            now, i, prefix, len(data), len(data) + data.count(b"\n"))
        with open(os.path.join(maildir, "cur", name), "wb") as f:
            f.write(data)
        msgids.append(msgid)
    os.system("chown -R vmail:vmail %s" % os.path.join(node.dir, "home"))
    return msgids


def file_count(node, user):
    cur = os.path.join(node.dir, "home", user, "Maildir", "cur")
    try:
        return len(os.listdir(cur))
    except FileNotFoundError:
        return 0


def run(args):
    users = ["user%d" % i for i in range(1, args.users + 1)]
    merged_user = users[-1]
    cluster = cl.Cluster(args.nodes, mesh=True, userdb_users=users,
                         extra=lambda node: "\n".join(args.set or []))
    cluster.setup()
    node1, node2 = cluster.node(1), cluster.node(2)
    want = {}
    for user in users:
        want[user] = set(seed_maildir(node1, user, "n1", args.mails))
    # different mails on the second node: the copies must be merged
    want[merged_user] |= set(seed_maildir(node2, merged_user, "n2",
                                          args.mails))
    cluster.start()

    # Nothing is triggered: the startup sync must find the users.
    start = time.time()
    while time.time() - start < args.timeout:
        if all(file_count(node, user) >= len(want[user])
               for node in cluster.nodes for user in users):
            break
        time.sleep(1)
    quiet = wait_quiet(cluster, args.timeout)
    print("replicated after %.1fs" % (time.time() - start))

    problems = []
    if quiet is None:
        problems.append("mesh didn't go quiet")
    for node in cluster.nodes:
        if "Couldn't list the users" in node.log():
            problems.append("%s couldn't list the users" % node.name)
        for user in users:
            state = wl.node_state(node, user)
            got = set(m[1] for m in state.get("INBOX", []))
            if got != want[user]:
                problems.append("%s/%s: %d mails, want %d (missing %s)" % (
                    node.name, user, len(got), len(want[user]),
                    sorted(want[user] - got)[:3]))
    for user in users:
        rows = node1.mesh_admin("VERIFY", user)
        if not rows or rows[0][1] != "ok":
            problems.append("%s: mesh verify: %s" % (user, rows[:3]))
    print("problems: %d" % len(problems))
    for p in problems:
        print("  PROBLEM:", p)
    if not args.keep:
        cluster.teardown()
    return 0 if not problems else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--nodes", type=int, default=3)
    p.add_argument("--users", type=int, default=4)
    p.add_argument("--mails", type=int, default=20)
    p.add_argument("--timeout", type=float, default=180)
    p.add_argument("--set", action="append",
                   help="extra dovecot.conf line for every node")
    p.add_argument("--keep", action="store_true")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
