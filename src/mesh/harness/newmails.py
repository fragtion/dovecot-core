#!/usr/bin/env python3
# A mail that is new on both nodes, but under different UIDs, e.g. because
# IMAP clients moved it into the same folder on both, becomes one mail:
# dsync links the two instances and gives them the same UID. It must also
# get the same flags on both nodes. Copy a mail within Sent on both nodes
# (once or twice, after some other mails so the UIDs differ), give each
# instance different flags, and check that one stateful dsync makes the
# nodes identical, and that the next one keeps them so.

import argparse
import sys

import cluster as cl
import workload as wl

# (mails saved first on node1, on node2, instances of the mail on each)
LAYOUTS = [(0, 1, 1), (1, 0, 1), (0, 2, 2), (2, 0, 2), (1, 1, 2),
           (3, 1, 2), (0, 3, 2)]
FLAGS = {"node1": ["\\Answered", "$label1"],
         "node2": ["\\Seen", "\\Flagged $label2"]}


def save(node, user, msgid):
    node.doveadm("save", "-u", user, "-m", "Sent",
                 input=wl.make_message(msgid, "newmails").decode())


def sync(cluster, user, state):
    node1, node2 = cluster.nodes
    res = node1.doveadm("sync", "-u", user, "-s", state,
                        node2.doveadm_location, check=False)
    # 2 = changes during sync, which nothing here makes
    if res.returncode != 0:
        raise RuntimeError("sync %s failed (%d): %s" % (
            user, res.returncode, res.stderr.strip()))
    lines = [l for l in res.stdout.splitlines() if l.strip()]
    return lines[-1].strip() if lines else ""


def sent(node, user):
    return [(uid, msgid, flags) for uid, msgid, guid, flags in
            wl.node_state(node, user).get("Sent", [])]


def run_layout(cluster, user, pads, copies):
    node1, node2 = cluster.nodes
    msgid = "x-%s@mesh" % user
    save(node1, user, msgid)
    state = sync(cluster, user, "")
    for node, pad in zip(cluster.nodes, pads):
        for i in range(pad):
            save(node, user, "pad-%s-%d-%s@mesh" % (node.name, i, user))
        for _ in range(copies):
            node.doveadm("copy", "-u", user, "Sent", "mailbox", "Sent",
                         "uid", "1")
        uids = [uid for uid, m, flags in sent(node, user)
                if m == msgid and uid != 1]
        for uid, flags in zip(uids, FLAGS[node.name]):
            node.doveadm("flags", "add", "-u", user, flags,
                         "mailbox", "Sent", "uid", str(uid))
    before = "node1=%s node2=%s" % (sent(node1, user), sent(node2, user))
    problems = []
    for i in range(2):
        state = sync(cluster, user, state)
        a, b = sent(node1, user), sent(node2, user)
        if a != b:
            problems.append("%s pads=%s copies=%d: Sent differs after "
                            "sync %d: node1=%s node2=%s (before: %s)" % (
                                user, pads, copies, i + 1, a, b, before))
            break
    return problems


def run(args):
    extra = "\n".join(args.set or [])
    cluster = cl.Cluster(2, extra=lambda node: extra)
    cluster.setup()
    cluster.start()
    problems = []
    for i, (pad1, pad2, copies) in enumerate(LAYOUTS):
        problems += run_layout(cluster, "user%d" % (i + 1),
                               (pad1, pad2), copies)
    print("layouts: %d" % len(LAYOUTS))
    print("problems: %d" % len(problems))
    for p in problems:
        print("  PROBLEM:", p)
    if not args.keep:
        cluster.teardown()
    return 0 if not problems else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--set", action="append",
                   help="extra dovecot.conf line for every node")
    p.add_argument("--keep", action="store_true")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
