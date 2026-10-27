#!/usr/bin/env python3
# A session whose connection stalls holds the user's lock on both nodes
# until dsync gives up, and saves for the user wait for that lock. Check
# that both nodes give up after mesh_sync_timeout and that saves meanwhile
# wait at most mesh_save_lock_timeout:
#
#  1. node1 pushes a big mailbox to node2, and node1's doveadm server (the
#     dsync client) hangs halfway: node2's dsync-server must give up too.
#  2. the same, but all of node2 hangs: node1 must give up, and see node2
#     go down.
#
# Both nodes must end up with every mail once nothing hangs anymore.

import argparse
import os
import signal
import subprocess
import sys
import time

import cluster as cl
import workload as wl
from phase1 import wait_quiet

SYNC_TIMEOUT = 10
SAVE_LOCK_TIMEOUT = 4


def seed_big_maildir(node, user, prefix, count, size):
    """Write mails straight into the node's Maildir (no notifications)."""
    cur = os.path.join(node.dir, "home", user, "Maildir", "cur")
    for sub in ("cur", "new", "tmp"):
        os.makedirs(os.path.join(os.path.dirname(cur), sub), exist_ok=True)
    pad = ("x" * 75 + "\n") * (size // 76)
    now = int(time.time())
    for i in range(count):
        data = ("From: seed@example.com\nTo: %s\nMessage-ID: <%s-%d-%s@mesh>"
                "\nSubject: %s %d\n\n%s" % (user, prefix, i, user, prefix, i,
                                            pad)).encode()
        name = "%d.M%dP2.%s,S=%d:2," % (now, i, prefix, len(data))
        with open(os.path.join(cur, name), "wb") as f:
            f.write(data)
    subprocess.run(["chown", "-R", "vmail:vmail",
                    os.path.join(node.dir, "home", user)], check=True)


def file_count(node, user):
    count = 0
    for sub in ("cur", "new"):
        try:
            count += len(os.listdir(os.path.join(node.dir, "home", user,
                                                 "Maildir", sub)))
        except FileNotFoundError:
            pass
    return count


def descendants(pid):
    out = subprocess.run(["ps", "-e", "-o", "pid=,ppid="], check=True,
                         capture_output=True, text=True).stdout
    children = {}
    for line in out.splitlines():
        p, pp = (int(x) for x in line.split())
        children.setdefault(pp, []).append(p)
    found, todo = [], [pid]
    while todo:
        p = todo.pop()
        found.append(p)
        todo.extend(children.get(p, []))
    return found


def processes(node, title=None):
    pids = descendants(node.master_pid())
    if title is None:
        return pids
    out = []
    for pid in pids:
        try:
            with open("/proc/%d/cmdline" % pid, "rb") as f:
                if title.encode() in f.read():
                    out.append(pid)
        except OSError:
            pass
    return out


def signal_all(pids, sig):
    for pid in pids:
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            pass


def timed_save(node, user, msgid):
    mail = "From: a@example.com\nMessage-ID: <%s>\nSubject: s\n\nbody\n" % msgid
    start = time.time()
    node.doveadm("save", "-u", user, input=mail, timeout=120)
    return time.time() - start


def wait_mid_transfer(node, user, total, timeout=60):
    """Wait until mails are arriving: saved ones are in tmp/ until dsync
    commits them in batches."""
    tmp = os.path.join(node.dir, "home", user, "Maildir", "tmp")
    end = time.time() + timeout
    while time.time() < end:
        n = file_count(node, user)
        try:
            saving = len(os.listdir(tmp))
        except FileNotFoundError:
            saving = 0
        if n >= total:
            return None
        if saving > 0 or n > 0:
            return n + saving
        time.sleep(0.01)
    return None


def stalled_after(node, start_len, timeout):
    """Seconds until the node logs a stalled dsync, or None."""
    start = time.time()
    while time.time() - start < timeout:
        if "I/O has stalled" in node.log()[start_len:]:
            return time.time() - start
        time.sleep(0.2)
    return None


def run(args):
    users = ["user1", "user2"]
    settings = ["mesh_sync_timeout = %ds" % SYNC_TIMEOUT,
                "mesh_save_lock_timeout = %ds" % SAVE_LOCK_TIMEOUT,
                "mesh_incremental_sync_interval = 0",
                "mesh_full_sync_interval = 0"] + (args.set or [])
    cluster = cl.Cluster(2, mesh=True, userdb_users=users,
                         extra=lambda node: "\n".join(settings))
    cluster.setup()
    node1, node2 = cluster.node(1), cluster.node(2)
    cluster.start()
    problems = []

    def check(cond, what):
        print("%s %s" % ("ok  " if cond else "FAIL", what), flush=True)
        if not cond:
            problems.append(what)

    if wait_quiet(cluster, 60) is None:
        problems.append("mesh didn't go quiet at startup")
    # only node1 starts sessions, so it's the dsync client
    node2.doveadm("mesh", "pause", "node1")

    # 1. node1's dsync client hangs: node2's dsync-server must give up
    seed_big_maildir(node1, "user1", "a", args.mails, args.size)
    log_len = len(node2.log())
    node1.doveadm("mesh", "sync", "user1")
    n = wait_mid_transfer(node2, "user1", args.mails)
    check(n is not None, "caught the first session halfway (%s mails)" % n)
    frozen = processes(node1, "doveadm-server")
    signal_all(frozen, signal.SIGSTOP)
    try:
        took = timed_save(node2, "user1", "save-1a@mesh")
        check(took < SAVE_LOCK_TIMEOUT + 2,
              "a save on node2 waits at most mesh_save_lock_timeout "
              "(%.1fs)" % took)
        secs = stalled_after(node2, log_len, SYNC_TIMEOUT + 10)
        check(secs is not None, "node2's dsync-server gives up after "
              "mesh_sync_timeout (logged after %s s)" % secs)
        took = timed_save(node2, "user1", "save-1b@mesh")
        check(took < 1, "after that, saves on node2 don't wait (%.1fs)"
              % took)
    finally:
        signal_all(frozen, signal.SIGCONT)

    # 2. all of node2 hangs: node1 must give up, and mark node2 down
    seed_big_maildir(node1, "user2", "b", args.mails, args.size)
    log_len = len(node1.log())
    node1.doveadm("mesh", "sync", "user2")
    n = wait_mid_transfer(node2, "user2", args.mails)
    check(n is not None, "caught the second session halfway (%s mails)" % n)
    frozen = processes(node2)
    signal_all(frozen, signal.SIGSTOP)
    try:
        took = timed_save(node1, "user2", "save-2a@mesh")
        check(took < SAVE_LOCK_TIMEOUT + 2,
              "a save on node1 waits at most mesh_save_lock_timeout "
              "(%.1fs)" % took)
        secs = stalled_after(node1, log_len, SYNC_TIMEOUT + 10)
        check(secs is not None, "node1's dsync gives up after "
              "mesh_sync_timeout (logged after %s s)" % secs)
        took = timed_save(node1, "user2", "save-2b@mesh")
        check(took < 1, "after that, saves on node1 don't wait (%.1fs)"
              % took)
        check("Peer node2 is down" in node1.log()[log_len:],
              "node1 marks node2 down")
    finally:
        signal_all(frozen, signal.SIGCONT)

    # nothing hangs anymore: everything must arrive everywhere
    node2.doveadm("mesh", "resume", "node1")
    for user in users:
        node1.doveadm("mesh", "sync", user)
    want = {user: args.mails + 2 for user in users}
    end = time.time() + 120
    while time.time() < end and not all(
            file_count(node, user) >= want[user]
            for node in cluster.nodes for user in users):
        time.sleep(0.5)
    if wait_quiet(cluster, 120) is None:
        problems.append("mesh didn't go quiet at the end")
    for user in users:
        counts = [len(wl.node_state(node, user).get("INBOX", []))
                  for node in cluster.nodes]
        check(counts == [want[user]] * 2, "%s has all %d mails on both "
              "nodes (%s)" % (user, want[user], counts))
        rows = node1.mesh_admin("VERIFY", user)
        check(rows and rows[0][1] == "ok", "%s: mesh verify ok" % user)
    crashes = [l for node in cluster.nodes for l in node.log().splitlines()
               if "Panic:" in l or "Fatal:" in l]
    check(not crashes, "no panics or fatal errors%s" % (
        (": " + crashes[0]) if crashes else ""))
    print("problems: %d" % len(problems))
    if not args.keep:
        cluster.teardown()
    return 0 if not problems else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mails", type=int, default=3000)
    p.add_argument("--size", type=int, default=30000)
    p.add_argument("--set", action="append",
                   help="extra dovecot.conf line for every node")
    p.add_argument("--keep", action="store_true")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
