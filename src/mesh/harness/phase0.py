#!/usr/bin/env python3
# Phase 0: does plain dsync converge with 3+ nodes?
#
# Drives "doveadm sync" between nodes from a script (no mesh service), in
# either the per-user anchor topology or a full mesh, while LMTP deliveries
# and IMAP clients hit random nodes. Afterwards it quiesces, runs final sync
# rounds and checks that every node holds the same mail with no duplicates,
# losses or reused UIDs.

import argparse
import hashlib
import itertools
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import cluster as cl
import workload as wl


def anchor_for(user, nodes):
    """Rendezvous hashing: the node with the highest hash(user, node)."""
    return max(nodes, key=lambda n: hashlib.sha1(
        ("%s\0%s" % (user, n.name)).encode()).digest())


class ScriptedSync:
    def __init__(self, cluster, users, topology, history):
        self.cluster = cluster
        self.users = users
        self.topology = topology
        self.history = history
        self.states = {}
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.blocked = set()
        self.runs = 0
        self.debug = None
        self.failures = []
        self.retries = 0

    def pairs(self, user):
        """Return (master, slave) pairs to run, in order. A stateful dsync
        only sends mailboxes that changed on the master since its saved
        state, so every node must push its own changes as the master,
        keeping its own state per peer. Mesh replication triggers these
        pushes on change events; here every push runs every round."""
        nodes = self.cluster.live_nodes()
        if self.topology == "anchor":
            anchor = anchor_for(user, nodes)
            spokes = [n for n in nodes if n is not anchor]
            return [(n, anchor) for n in spokes] + \
                [(anchor, n) for n in spokes]
        return list(itertools.permutations(nodes, 2))

    def sync_pair(self, user, a, b):
        if frozenset((a.name, b.name)) in self.blocked:
            return
        key = (user, a.name, b.name)
        with self.lock:
            state = self.states.get(key, "")
        extra = ["-D"] if self.debug else []
        with self.history.user_lock(a, user), self.history.user_lock(b, user):
            res = a.doveadm(*extra, "sync", "-u", user, "-l", "60",
                            "-s", state, b.doveadm_location, check=False,
                            timeout=120)
        with self.lock:
            self.runs += 1
            if self.debug:
                with open(self.debug, "a") as f:
                    f.write("=== run %d %s %s->%s rc=%d\n" % (
                        self.runs, user, a.name, b.name, res.returncode))
                    f.write(res.stderr)
        if res.returncode == 0:
            lines = [l for l in res.stdout.splitlines() if l.strip()]
            if lines:
                with self.lock:
                    self.states[key] = lines[-1].strip()
        elif res.returncode == 2:
            # Changes happened during the sync; run again later.
            with self.lock:
                self.retries += 1
        else:
            with self.lock:
                self.failures.append("%s %s->%s rc=%d: %s" % (
                    user, a.name, b.name, res.returncode,
                    res.stderr.strip()[-300:]))

    def sync_user(self, user):
        for a, b in self.pairs(user):
            self.sync_pair(user, a, b)

    def round(self, pool):
        list(pool.map(self.sync_user, self.users))

    def loop(self, interval):
        with ThreadPoolExecutor(max_workers=len(self.users)) as pool:
            while not self.stop.is_set():
                self.round(pool)
                self.stop.wait(interval)


def run(args):
    extra = "\n".join(args.set or [])
    cluster = cl.Cluster(args.nodes, extra=lambda node: extra)
    cluster.setup()
    cluster.start()
    users = ["user%d" % i for i in range(1, args.users + 1)]
    history = wl.History()
    sync = ScriptedSync(cluster, users, args.topology, history)
    sync.debug = args.debug_log
    history.serialize = args.serialize

    picker = None
    if args.single_delivery_node:
        picker = lambda user: cluster.nodes[0]
    elif args.affinity:
        picker = lambda user: anchor_for(user, cluster.live_nodes())
    threads = [wl.Deliverer(cluster, users, history, args.deliveries,
                            args.delivery_rate, seed=1000 + i,
                            node_picker=picker)
               for i in range(args.deliverers)]
    for i, user in enumerate(users):
        for j in range(args.actors_per_user):
            threads.append(wl.ImapActor(cluster, user, history,
                                        args.duration, seed=i * 10 + j))
    if args.seed_users:
        # Create every user's mailboxes on node1 and copy them everywhere,
        # so the run doesn't start with independently created INBOXes.
        for user in users:
            cluster.nodes[0].doveadm("mailbox", "status", "-u", user,
                                     "messages", "INBOX")
        with ThreadPoolExecutor(max_workers=len(users)) as pool:
            for _ in range(2):
                sync.round(pool)
    syncer = threading.Thread(target=sync.loop, args=(args.interval,),
                              daemon=True)
    start = time.time()
    syncer.start()
    for t in threads:
        t.start()
    if args.partition:
        # Cut node1 off from everyone for the middle third of the run.
        time.sleep(args.duration / 3)
        others = [n.name for n in cluster.nodes if n.name != "node1"]
        sync.blocked = {frozenset(("node1", o)) for o in others}
        print("partition: node1 isolated", flush=True)
        time.sleep(args.duration / 3)
        sync.blocked = set()
        print("partition healed", flush=True)
    for t in threads:
        t.join()
    sync.stop.set()
    syncer.join()
    work_secs = time.time() - start

    # Quiesce: sync until two consecutive rounds leave states unchanged.
    rounds = 0
    with ThreadPoolExecutor(max_workers=len(users)) as pool:
        prev = None
        while rounds < 10:
            sync.round(pool)
            rounds += 1
            cur = dict(sync.states)
            if cur == prev:
                break
            prev = cur
    problems = wl.check_convergence(cluster, users, history)
    errors = cluster.errors()
    print("topology=%s nodes=%d users=%d affinity=%s partition=%s" % (
        args.topology, args.nodes, args.users, args.affinity,
        args.partition))
    print("work: %.1fs, ops=%d, delivered=%d appended=%d moved=%d "
          "copied=%d expunged=%d" % (
              work_secs, history.ops, len(history.delivered),
              len(history.appended), len(history.moved),
              sum(history.copied.values()), len(history.expunged)))
    print("syncs: runs=%d retries=%d failures=%d final_rounds=%d" % (
        sync.runs, sync.retries, len(sync.failures), rounds))
    print("client errors: %d" % len(history.errors))
    for e in history.errors[:5]:
        print("  client error:", e)
    for f in sync.failures[:10]:
        print("  sync failure:", f)
    for e in errors[:20]:
        print("  log:", e)
    print("problems: %d" % len(problems))
    for p in problems[:40]:
        print("  PROBLEM:", p)
    if not args.keep:
        cluster.teardown()
    return 0 if not problems and not sync.failures else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--nodes", type=int, default=3)
    p.add_argument("--users", type=int, default=3)
    p.add_argument("--topology", choices=["anchor", "mesh"],
                   default="anchor")
    p.add_argument("--duration", type=float, default=30)
    p.add_argument("--deliveries", type=int, default=100)
    p.add_argument("--deliverers", type=int, default=2)
    p.add_argument("--delivery-rate", type=float, default=10.0)
    p.add_argument("--actors-per-user", type=int, default=2)
    p.add_argument("--interval", type=float, default=0.2)
    p.add_argument("--affinity", action="store_true",
                   help="deliver each user's mail to its anchor node")
    p.add_argument("--partition", action="store_true")
    p.add_argument("--keep", action="store_true")
    p.add_argument("--serialize", action="store_true",
                   help="don't let local saves run during a user's dsync")
    p.add_argument("--set", action="append",
                   help="extra dovecot.conf line for every node")
    p.add_argument("--debug-log", help="write dsync -D output here")
    p.add_argument("--seed-users", action="store_true",
                   help="create mailboxes on node1 and sync before the run")
    p.add_argument("--single-delivery-node", action="store_true",
                   help="deliver all mail to node1")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
