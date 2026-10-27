#!/usr/bin/env python3
# Nodes that already have some of the same mails, with UIDs assigned
# independently, e.g. after a partial copy (an old sync, an interrupted
# first session, files copied by another tool). The first session must
# match the shared mails and end up with every mail on both nodes.

import argparse
import os
import shutil
import subprocess
import sys
import time

import cluster as cl
import workload as wl
from phase1 import wait_quiet


def maildir(node, user):
    return os.path.join(node.dir, "home", user, "Maildir")


def seed(node, user, prefix, count, start=0):
    cur = os.path.join(maildir(node, user), "cur")
    for sub in ("cur", "new", "tmp"):
        os.makedirs(os.path.join(maildir(node, user), sub), exist_ok=True)
    now = int(time.time())
    names = []
    for i in range(start, start + count):
        data = ("From: seed@example.com\nTo: %s\nMessage-ID: <%s-%d-%s@mesh>"
                "\nSubject: %s %d\n\nbody %d\n" %
                (user, prefix, i, user, prefix, i, i)).encode()
        name = "%d.M%dP3.%s,S=%d:2,S" % (now, i, prefix, len(data))
        with open(os.path.join(cur, name), "wb") as f:
            f.write(data)
        names.append(name)
    return names


def run(args):
    users = ["user1"]
    settings = list(args.set or [])
    cluster = cl.Cluster(2, mesh=True, userdb_users=users,
                         extra=lambda node: "\n".join(settings))
    cluster.setup()
    node1, node2 = cluster.node(1), cluster.node(2)
    names = seed(node1, "user1", "a", args.mails)
    # node2 has every third of node1's mails, in a different order, so
    # their UIDs differ, and mails of its own
    shared = names[::3]
    os.makedirs(os.path.join(maildir(node2, "user1"), "cur"))
    for sub in ("new", "tmp"):
        os.makedirs(os.path.join(maildir(node2, "user1"), sub))
    for name in reversed(shared):
        shutil.copy(os.path.join(maildir(node1, "user1"), "cur", name),
                    os.path.join(maildir(node2, "user1"), "cur", name))
    if args.index_first:
        # give node2's copies UIDs before node2 learns the rest
        for node in (node1, node2):
            subprocess.run(["chown", "-R", "vmail:vmail",
                            os.path.join(node.dir, "home")], check=True)
            node.write_config()
        for node in (node1, node2):
            node.doveadm("-o", "mail_plugins=", "index", "-u", "user1",
                         "INBOX", check=False)
    seed(node2, "user1", "b", args.extra)
    for node in (node1, node2):
        subprocess.run(["chown", "-R", "vmail:vmail",
                        os.path.join(node.dir, "home")], check=True)
    want = args.mails + args.extra
    cluster.start()
    end = time.time() + args.timeout
    while time.time() < end:
        counts = [len(wl.node_state(n, "user1").get("INBOX", []))
                  for n in cluster.nodes]
        if counts == [want, want]:
            break
        time.sleep(1)
    quiet = wait_quiet(cluster, args.timeout)
    counts = [len(wl.node_state(n, "user1").get("INBOX", []))
              for n in cluster.nodes]
    files = [len(os.listdir(os.path.join(maildir(n, "user1"), sub)))
             for n in cluster.nodes for sub in ("cur", "new")]
    problems = []
    if quiet is None:
        problems.append("mesh didn't go quiet")
    if counts != [want, want]:
        problems.append("INBOX counts %s, want %d (files cur/new: %s)" %
                        (counts, want, files))
    rows = node1.mesh_admin("VERIFY", "user1")
    if not rows or rows[0][1] != "ok":
        problems.append("verify: %s" % rows[:2])
    print("counts %s want %d files %s" % (counts, want, files))
    print("problems: %d" % len(problems))
    for p in problems:
        print("  PROBLEM:", p)
    if not args.keep:
        cluster.teardown()
    return 0 if not problems else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mails", type=int, default=300)
    p.add_argument("--extra", type=int, default=2)
    p.add_argument("--index-first", action="store_true",
                   help="index both nodes' copies before node2 gets its "
                   "own mails")
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--set", action="append",
                   help="extra dovecot.conf line for every node")
    p.add_argument("--keep", action="store_true")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
