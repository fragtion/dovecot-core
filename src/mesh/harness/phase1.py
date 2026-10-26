#!/usr/bin/env python3
# Phase 1: run the workload against nodes running the mesh service and
# plugin, with no scripted syncs, then wait for the mesh to go quiet and
# check that every node converged.

import argparse
import re
import sys
import threading
import time

import cluster as cl
import workload as wl
from phase0 import anchor_for


def mesh_quiet(cluster):
    """TRUE if no node has pending or running syncs."""
    for node in cluster.live_nodes():
        status = dict(node.mesh_admin("STATUS"))
        if status.get("pending") != "0" or status.get("running") != "0":
            return False
    return True


def wait_quiet(cluster, timeout, stable_secs=2.0):
    start = time.time()
    quiet_since = None
    while time.time() - start < timeout:
        if mesh_quiet(cluster):
            if quiet_since is None:
                quiet_since = time.time()
            elif time.time() - quiet_since >= stable_secs:
                return time.time() - start
        else:
            quiet_since = None
        time.sleep(0.2)
    return None


def run(args):
    extra = "\n".join(args.set or [])
    cluster = cl.Cluster(args.nodes, extra=lambda node: extra, mesh=True,
                         topology=args.topology,
                         lmtp_route=args.lmtp_route, tls=args.tls)
    cluster.setup()
    cluster.start()
    time.sleep(2)
    users = ["user%d" % i for i in range(1, args.users + 1)]
    history = wl.History()

    picker = None
    if args.affinity:
        picker = lambda user: anchor_for(user, cluster.live_nodes())
    threads = [wl.Deliverer(cluster, users, history, args.deliveries,
                            args.delivery_rate, seed=1000 + i,
                            node_picker=picker)
               for i in range(args.deliverers)]
    for i, user in enumerate(users):
        for j in range(args.actors_per_user):
            threads.append(wl.ImapActor(cluster, user, history,
                                        args.duration, seed=i * 10 + j))
    start = time.time()
    for t in threads:
        t.start()
    stopped = None
    if args.kill_node:
        # Crash a node a third into the run and bring it back later.
        time.sleep(args.duration / 3)
        stopped = cluster.node(args.kill_node)
        stopped.stop(hard=True)
        print("%s killed" % stopped.name, flush=True)
        time.sleep(args.duration / 3)
        stopped.start()
        print("%s restarted" % stopped.name, flush=True)
    for t in threads:
        t.join()
    work_secs = time.time() - start

    quiet_secs = wait_quiet(cluster, args.quiesce_timeout)
    problems = wl.check_convergence(cluster, users, history)
    # The mesh's own verifier must agree that the nodes converged.
    verifier = cluster.live_nodes()[0]
    for user in users:
        rows = verifier.mesh_admin("VERIFY", user)
        result = rows[0][1] if rows and rows[0][0] == "R" else "?"
        if result != "ok":
            differing = [r[1] for r in rows
                         if r[0] == "M" and r[3] != "match"]
            problems.append("%s: mesh verify says %s (%s)" % (
                user, result, ", ".join(differing)))
    errors = cluster.errors()

    print("topology=%s nodes=%d users=%d affinity=%s lmtp-route=%s "
          "tls=%s kill=%s" % (
              args.topology, args.nodes, args.users, args.affinity,
              args.lmtp_route, args.tls, args.kill_node))
    # where LMTP deliveries were saved: on the user's anchor or elsewhere
    on_anchor = total = 0
    for node in cluster.nodes:
        for line in node.log().splitlines():
            m = re.search(r"lmtp\(([^)]+)\)<.*: saved mail to", line)
            if m is None:
                continue
            total += 1
            if anchor_for(m.group(1), cluster.nodes).name == node.name:
                on_anchor += 1
    print("lmtp: %d of %d deliveries saved on the user's anchor" %
          (on_anchor, total))
    print("work: %.1fs, ops=%d, delivered=%d appended=%d moved=%d "
          "copied=%d expunged=%d" % (
              work_secs, history.ops, len(history.delivered),
              len(history.appended), len(history.moved),
              sum(history.copied.values()), len(history.expunged)))
    print("quiet after: %s" % ("%.1fs" % quiet_secs if quiet_secs is not None
                               else "TIMEOUT"))
    for node in cluster.nodes:
        peers = node.mesh_admin("PEERS")
        print("  %s peers: %s" % (node.name, ", ".join(
            "%s %s ok=%s failed=%s" % (p[0], p[1], p[7], p[8])
            for p in peers)))
    print("client errors: %d" % len(history.errors))
    for e in history.errors[:5]:
        print("  client error:", e)
    for e in errors[:20]:
        print("  log:", e)
    print("problems: %d" % len(problems))
    for p in problems[:40]:
        print("  PROBLEM:", p)
    if not args.keep:
        cluster.teardown()
    return 0 if not problems and quiet_secs is not None else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--nodes", type=int, default=3)
    p.add_argument("--users", type=int, default=3)
    p.add_argument("--topology", choices=["anchor", "full"],
                   default="anchor")
    p.add_argument("--duration", type=float, default=20)
    p.add_argument("--deliveries", type=int, default=30)
    p.add_argument("--deliverers", type=int, default=2)
    p.add_argument("--delivery-rate", type=float, default=10.0)
    p.add_argument("--actors-per-user", type=int, default=2)
    p.add_argument("--affinity", action="store_true",
                   help="deliver to the user's anchor (harness picks it)")
    p.add_argument("--lmtp-route", action="store_true",
                   help="LMTP proxies recipients to their anchor "
                   "(mesh-route.lua)")
    p.add_argument("--kill-node", help="crash and restart this node")
    p.add_argument("--tls", action="store_true",
                   help="TLS between the nodes (dsync and mesh HTTP)")
    p.add_argument("--quiesce-timeout", type=float, default=120)
    p.add_argument("--set", action="append",
                   help="extra dovecot.conf line for every node")
    p.add_argument("--keep", action="store_true")
    sys.exit(run(p.parse_args()))


if __name__ == "__main__":
    main()
