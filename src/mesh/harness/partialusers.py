#!/usr/bin/env python3
# Nodes whose userdbs list different users: a user is replicated only
# between the nodes that have it, and a user added to or removed from a
# node's userdb starts or stops being replicated to it.
#
#   node1: all, u12, u13         node2: all, u12        node3: all, u13, u3
#
# Then u3 is added to node1's userdb (node1 must get u3's mails) and u12 is
# removed from node2's (node2 must stop getting u12's new mails).

import argparse
import os
import smtplib
import sys
import time

import cluster as cl
import workload as wl
from phase1 import wait_quiet

USERS = {
    "node1": ["all", "u12", "u13"],
    "node2": ["all", "u12"],
    "node3": ["all", "u13", "u3"],
}
REFRESH_SECS = 3


def seed(node, user, prefix, count):
    cur = os.path.join(node.dir, "home", user, "Maildir", "cur")
    for sub in ("cur", "new", "tmp"):
        os.makedirs(os.path.join(os.path.dirname(cur), sub), exist_ok=True)
    now = int(time.time())
    ids = []
    for i in range(count):
        msgid = "%s-%d-%s@mesh" % (prefix, i, user)
        data = ("From: seed@example.com\nTo: %s\nMessage-ID: <%s>\n"
                "Subject: %s %d\n\nbody\n" % (user, msgid, prefix, i)).encode()
        name = "%d.M%dP4.%s,S=%d:2,S" % (now, i, prefix, len(data))
        with open(os.path.join(cur, name), "wb") as f:
            f.write(data)
        ids.append(msgid)
    os.system("chown -R vmail:vmail %s" % os.path.join(node.dir, "home"))
    return ids


def deliver(node, user, msgid):
    lmtp = smtplib.LMTP("127.0.0.1", node.lmtp_port, timeout=30)
    lmtp.sendmail("sender@example.com", [user],
                  wl.make_message(msgid, "partial"))
    lmtp.quit()


def msgids(node, user):
    try:
        state = wl.node_state(node, user)
    except RuntimeError:
        # the userdb change may not be visible for a moment
        return set()
    return set(m[1] for m in state.get("INBOX", []))


def has_home(node, user):
    return os.path.exists(os.path.join(node.dir, "home", user))


def write_users(node, users):
    path = os.path.join(node.dir, "users")
    lines = open(path).read().splitlines()
    template = lines[0].split(":", 1)[1]
    with open(path, "w") as f:
        for user in users:
            f.write("%s:%s\n" % (user, template.replace(
                "/home/" + USERS["node1"][0], "/home/" + user)))


def run(args):
    settings = ["mesh_userlist_refresh_interval = %ds" % REFRESH_SECS] + \
        (args.set or [])
    cluster = cl.Cluster(3, mesh=True,
                         userdb_users=lambda node: USERS[node.name],
                         extra=lambda node: "\n".join(settings))
    cluster.setup()
    n = {node.name: node for node in cluster.nodes}
    problems = []

    def check(cond, what):
        print("%s %s" % ("ok  " if cond else "FAIL", what), flush=True)
        if not cond:
            problems.append(what)

    def holders(user):
        return [name for name, users in USERS.items() if user in users]

    # each user's mails start on one of its nodes
    want = {
        "all": set(seed(n["node2"], "all", "s", 10)),
        "u12": set(seed(n["node1"], "u12", "s", 10)),
        "u13": set(seed(n["node3"], "u13", "s", 10)),
        "u3": set(seed(n["node3"], "u3", "s", 10)),
    }
    cluster.start()

    def converged():
        for user, ids in want.items():
            for name in holders(user):
                if msgids(n[name], user) != ids:
                    return False
        return True

    def wait_converged(what):
        end = time.time() + args.timeout
        while time.time() < end and not converged():
            time.sleep(1)
        ok = converged()
        check(ok, what)
        if not ok:
            for user, ids in want.items():
                for name in holders(user):
                    got = msgids(n[name], user)
                    if got != ids:
                        print("     %s/%s: %d mails, want %d" %
                              (name, user, len(got), len(ids)))

    wait_converged("each user's mails reach exactly the nodes that have it")
    for user in want:
        for name in n:
            if name not in holders(user):
                check(not has_home(n[name], user),
                      "%s gets nothing of %s, which it doesn't have" %
                      (name, user))

    # deliveries replicate among the user's nodes
    deliver(n["node2"], "u12", "d1-u12@mesh")
    want["u12"].add("d1-u12@mesh")
    deliver(n["node1"], "u13", "d1-u13@mesh")
    want["u13"].add("d1-u13@mesh")
    deliver(n["node3"], "all", "d1-all@mesh")
    want["all"].add("d1-all@mesh")
    wait_converged("deliveries reach the user's other nodes")

    # add u3 to node1, remove u12 from node2
    USERS["node1"].append("u3")
    write_users(n["node1"], USERS["node1"])
    USERS["node2"].remove("u12")
    write_users(n["node2"], USERS["node2"])
    wait_converged("a user added to a node's userdb is copied to it")
    deliver(n["node1"], "u12", "d2-u12@mesh")
    time.sleep(REFRESH_SECS + 5)
    with open(os.path.join(n["node2"].dir, "home", "u12", "Maildir",
                           "dovecot-uidlist")) as f:
        uidlist = f.read()
    check("d2-u12" not in uidlist and len(msgids(n["node1"], "u12")) ==
          len(want["u12"]) + 1,
          "a user removed from a node's userdb isn't synced to it anymore")
    want["u12"].add("d2-u12@mesh")

    if wait_quiet(cluster, args.timeout) is None:
        problems.append("mesh didn't go quiet")
    for user in want:
        rows = n[holders(user)[0]].mesh_admin("VERIFY", user)
        check(rows and rows[0][1] == "ok", "%s: mesh verify ok" % user)
    bad = [line for node in cluster.nodes for line in node.log().splitlines()
           if "Panic:" in line or "Fatal:" in line or
           "User doesn't exist" in line or
           ("Syncing" in line and "failed" in line)]
    check(not bad, "no panics, failed sessions or unknown users in the "
          "logs%s" % ((": " + bad[0]) if bad else ""))
    print("problems: %d" % len(problems))
    if not args.keep:
        cluster.teardown()
    return 0 if not problems else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--timeout", type=float, default=60)
    p.add_argument("--set", action="append",
                   help="extra dovecot.conf line for every node")
    p.add_argument("--keep", action="store_true")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
