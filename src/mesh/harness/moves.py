#!/usr/bin/env python3
# Moving mails between folders must not transfer their bodies again: the
# other nodes already have them, in the source folder. Seed a user with
# mails, let the mesh replicate them, move them all to another folder on
# one node, and count the mail bodies dsync transferred for that.

import argparse
import os
import sys
import time

import cluster as cl
import workload as wl
from phase1 import wait_quiet


def seed_maildir(node, user, count, size):
    """Write mails straight into the node's Maildir before it starts."""
    cur = os.path.join(node.dir, "home", user, "Maildir", "cur")
    os.makedirs(cur, exist_ok=True)
    os.makedirs(os.path.join(os.path.dirname(cur), "new"), exist_ok=True)
    os.makedirs(os.path.join(os.path.dirname(cur), "tmp"), exist_ok=True)
    body = ("x" * 76 + "\n") * max(1, size // 77)
    now = int(time.time())
    for i in range(count):
        data = ("From: seed@example.com\nTo: %s\n"
                "Message-ID: <seed-%d-%s@mesh>\n"
                "Subject: seed %d\n\n%s" % (user, i, user, i, body)).encode()
        # S= must be the file's size, W= its size with CRLF line ends
        name = "%d.M%dP1.seed,S=%d,W=%d:2,S" % (
            now, i, len(data), len(data) + data.count(b"\n"))
        with open(os.path.join(cur, name), "wb") as f:
            f.write(data)
    os.system("chown -R vmail:vmail %s" % os.path.join(node.dir, "home"))


def transferred(cluster, offsets):
    """Mail bodies dsync received over the network since the offsets."""
    n = 0
    for node in cluster.nodes:
        for line in node.log()[offsets[node.name]:].splitlines():
            # the remote side's debug lines are logged again by the
            # local side, prefixed with dsync-remote
            if "import mail uid" in line and "dsync-remote" not in line:
                n += 1
    return n


def run(args):
    extra = "mesh_dsync_debug = yes\n" + "\n".join(args.set or [])
    cluster = cl.Cluster(args.nodes, extra=lambda node: extra, mesh=True)
    cluster.setup()
    user = "user1"
    seed_maildir(cluster.node(1), user, args.mails, args.size)
    cluster.start()
    time.sleep(3)
    node1 = cluster.node(1)
    node1.mesh_admin("SYNC", user)
    if wait_quiet(cluster, args.timeout) is None:
        print("initial sync didn't finish")
        return 1
    initial = transferred(cluster, {n.name: 0 for n in cluster.nodes})
    print("initial replication: %d mails transferred" % initial)

    mover = cluster.node(args.move_on)
    offsets = {n.name: len(n.log()) for n in cluster.nodes}
    start = time.time()
    mover.doveadm("move", "-u", user, "Archive", "mailbox", "INBOX", "all")
    quiet = wait_quiet(cluster, args.timeout)
    secs = time.time() - start
    moved = transferred(cluster, offsets)
    print("moved %d mails INBOX -> Archive on %s: replicated in %.1fs, "
          "%d mail bodies transferred" % (args.mails, mover.name, secs,
                                          moved))

    problems = []
    if quiet is None:
        problems.append("mesh didn't go quiet after the move")
    rows = node1.mesh_admin("VERIFY", user)
    if not rows or rows[0][1] != "ok":
        problems.append("mesh verify: %s" % rows[:3])
    for node in cluster.nodes:
        state = wl.node_state(node, user)
        n_inbox = len(state.get("INBOX", []))
        n_archive = len(state.get("Archive", []))
        if n_inbox != 0 or n_archive != args.mails:
            problems.append("%s: INBOX=%d Archive=%d" %
                            (node.name, n_inbox, n_archive))
    if moved > args.max_transferred:
        problems.append("%d bodies transferred (allowed %d)" %
                        (moved, args.max_transferred))
    print("problems: %d" % len(problems))
    for p in problems:
        print("  PROBLEM:", p)
    if not args.keep:
        cluster.teardown()
    return 0 if not problems else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--nodes", type=int, default=3)
    p.add_argument("--mails", type=int, default=500)
    p.add_argument("--size", type=int, default=20000,
                   help="approximate mail size in bytes")
    p.add_argument("--move-on", default="node2",
                   help="node where the user moves the mails")
    p.add_argument("--max-transferred", type=int, default=0)
    p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--set", action="append")
    p.add_argument("--keep", action="store_true")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
