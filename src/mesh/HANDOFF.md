# Handoff: containerizing and live-testing Dovecot Mesh

For the agent that packages this branch in Docker, deploys a multi-node
cluster and runs live tests. Read `src/mesh/README.md` first: it
explains how the mesh works and documents every setting. This file covers
what you need beyond that: build, image, cluster layout, configuration,
the test plan, and what to report.

Branch: `claude/relaxed-ritchie-bjjfls` of `fragtion/dovecot-core`
(Dovecot 2.4 development tree, autotools build).

## 1. What was built and its state

| Part | Where | State |
|---|---|---|
| Maildir `new/` handling restored; old Syncthing behaviour behind `maildir_ignore_new_dir` (default no) | `src/lib-storage/index/maildir/` | done, unit tests |
| dsync `dsync_features = relay-changes` (forward changes dsync imported, needed with 3+ nodes) | `src/doveadm/dsync/` | done |
| dsync merges flags of mails both sides already have | `src/doveadm/dsync/dsync-mailbox-import.c` | done |
| dsync `dsync_features = reuse-local-mails` (mails moved or copied to another folder are copied locally, hardlinked with Maildir, instead of being transferred again) | `src/doveadm/dsync/dsync-local-mails.c` | done |
| mesh mail plugin: change notifications, save/dsync locking, delivery wait | `src/plugins/mesh/` | done |
| mesh service: anchor topology, leases, shared pair states, retries, periodic syncs, verifier, HTTP API, web UI, delivery route lookup, TLS | `src/mesh/` | done |
| `doveadm mesh status/peers/users/sync/verify/pause/resume` | `src/doveadm/doveadm-mesh.c` | done |
| Delivery affinity (LMTP proxy to the user's anchor) | `src/mesh/mesh-route.lua` | done, needs Lua build |
| Test harness (real nodes on one host) | `src/mesh/harness/` | done |

Tested on one host with the harness, every scenario ending with all nodes
identical and nothing lost, duplicated or resurrected (`problems: 0`):
3-5 nodes, 2-6 users, 20-90 s of mixed load (LMTP deliveries, IMAP flag
changes, moves, copies, expunges, appends, on random nodes at the same
time), node crash and restart mid-run, `full` topology, delivery
affinity, and TLS between nodes. After the load stops, all nodes are
identical within about 3 seconds. Delivery confirmation adds about 80 ms
per recipient.

**Not yet tested anywhere:** separate machines/containers, real network
latency or loss, network partitions, large mailboxes (10k+ mails, many
folders), many users, long uptimes, real IMAP clients, real MTAs. That's
what the live tests are for.

## 2. Building

Build from this branch with autotools (the meson files don't know about
the mesh yet; don't use meson).

Build dependencies (Debian/Ubuntu names):

```
build-essential autoconf automake libtool pkg-config gettext autopoint
bison flex perl python3 git wget
libssl-dev zlib1g-dev libpcre2-dev libsqlite3-dev liblz4-dev libzstd-dev
liblua5.4-dev
```

```
./autogen.sh            # or: autoreconf -vif
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
    --with-lua=yes --with-sqlite --with-lz4 --with-zstd
make -j$(nproc)
make install DESTDIR=/out   # for a multi-stage image
```

- Building from git downloads Unicode data into `src/lib/ucd/` from
  `https://dovecot.org/res`. If the build host can't reach it, put the
  Unicode 16.0.0 files listed as `UCD_FILES` in `src/lib/Makefile.am`
  there yourself (from `https://www.unicode.org/Public/16.0.0/ucd/`).
- `--with-lua=yes` is needed only for delivery affinity, but build with
  it so that it can be tested.
- Runtime libraries for the final image: `libssl3 zlib1g libpcre2-32-0
  libsqlite3-0 liblz4-1 libzstd1 liblua5.4-0 ca-certificates`.
- Installed mesh files (with the prefixes above):
  `/usr/libexec/dovecot/mesh` (service), `/usr/lib/dovecot/lib25_mesh_plugin.so`,
  `/usr/share/dovecot/mesh-route.lua`, `/usr/bin/doveadm` (with `mesh`
  commands).
- Optional checks in the build stage: `make check` (set `NOVALGRIND=1`).
  `test-http-client-errors` and `test-lua-http-client` can fail for
  environmental reasons (see AGENTS.md); everything else should pass.

## 3. Image and container runtime

- Users: `dovecot` and `dovenull` (Dovecot's internal/login users) and
  `vmail` (owns the mail; give it a fixed UID/GID, e.g. 5000, the same on
  all nodes).
- Run Dovecot in the foreground as root: `dovecot -F`. The mesh service is
  started by the master process (one process, restarted if it dies).
- Logs: `log_path = /dev/stderr` so `docker logs` shows them.
- Persistent volumes per node:
  - `/var/vmail` (mail; Maildir under each user's home)
  - `/var/lib/dovecot` (`state_dir`: contains `mesh.db`, the mesh's
    per-user sync states. Losing it only makes the next syncs stateless,
    i.e. slower, but keep it).
- `base_dir` (`/run/dovecot`) must not be shared between containers.
- Each node needs its own `hostname` setting (and ideally container
  hostname), and clocks in sync (containers use the host clock).
- Health check: `doveadm mesh status` (fails if the service is down), or
  `curl -H "Authorization: Bearer $KEY" http://127.0.0.1:8091/mesh/v1/heartbeat`.

## 4. Cluster layout and configuration

Suggested first deployment: three nodes (`node1..node3`) on one Docker
network, plus a test-client container. Later: a fourth node added at
runtime, and nodes on different hosts with added latency (`tc netem`).

Ports per node:

| Port | What | Exposed to |
|---|---|---|
| 143 / 993 | IMAP | clients |
| 24 | LMTP | MTA / test injector, other nodes (affinity) |
| 12345 | doveadm (dsync between nodes) | other nodes only |
| 8090 | mesh HTTP API + web UI (TLS) | other nodes, admins |
| 8091 | mesh HTTP on 127.0.0.1 only (for mesh-route.lua) | nothing |

Secrets, the same on all nodes: `doveadm_password`, `mesh_api_key`. The
web UI login is any user name with `mesh_api_key` as the password.

TLS: make one CA, and one certificate per node whose subjectAltName
contains the name used as that node's `mesh_peer_host` (e.g. the
container name). Every node trusts the CA via `ssl_client_ca_file`.

Configuration for each node; only the two `PER NODE` settings differ.
It was checked with `doveconf`, and the same setup (with other ports) is
what the harness runs with `--tls --lmtp-route`.

```
dovecot_config_version = 2.4.0
dovecot_storage_version = 2.4.0

# PER NODE
mesh_node_name = node1
hostname = node1.mesh.example

listen = *
log_path = /dev/stderr
protocols {
  imap = yes
  lmtp = yes
}

ssl = yes
ssl_server_cert_file = /etc/dovecot/tls/node.pem
ssl_server_key_file = /etc/dovecot/tls/node.key
ssl_client_ca_file = /etc/dovecot/tls/ca.pem

auth_mechanisms = plain login

# Delivery affinity: must be the first passdb.
lmtp_proxy = yes
passdb mesh-route {
  driver = lua
  lua_file = /usr/share/dovecot/mesh-route.lua
  lua_settings {
    mesh_url = http://127.0.0.1:8091
    mesh_api_key = CHANGE-ME-mesh-secret
  }
}
passdb passwd-file {
  passwd_file_path = /etc/dovecot/users
}
userdb static {
  fields {
    uid = vmail
    gid = vmail
    home = /var/vmail/%{user | domain}/%{user | username}
  }
}

mail_driver = maildir
mail_path = ~/Maildir
mail_uid = vmail
mail_gid = vmail
namespace inbox {
  inbox = yes
  separator = /
  mailbox Drafts {
    special_use = \Drafts
    auto = subscribe
  }
  mailbox Sent {
    special_use = \Sent
    auto = subscribe
  }
  mailbox Trash {
    special_use = \Trash
    auto = subscribe
  }
  mailbox Junk {
    special_use = \Junk
    auto = subscribe
  }
}

service lmtp {
  inet_listener lmtp {
    port = 24
  }
}

mail_plugins {
  notify = yes
  mesh = yes
}
dsync_features = relay-changes reuse-local-mails

doveadm_port = 12345
doveadm_password = CHANGE-ME-doveadm-secret
service doveadm {
  inet_listener doveadm {
    port = 12345
    ssl = yes
  }
}

mesh_api_key = CHANGE-ME-mesh-secret
service mesh {
  inet_listener mesh-http {
    port = 8090
    ssl = yes
  }
  inet_listener mesh-http-local {
    type = http
    listen = 127.0.0.1
    port = 8091
  }
}
# Check users' replicas in the background, repair differences.
mesh_verify_interval = 1min

mesh_peer node1 {
  mesh_peer_host = node1.mesh.example
  mesh_peer_doveadm_port = 12345
  mesh_peer_http_port = 8090
  mesh_peer_lmtp_port = 24
  mesh_peer_doveadm_ssl = yes
  mesh_peer_http_ssl = yes
}
mesh_peer node2 {
  mesh_peer_host = node2.mesh.example
  mesh_peer_doveadm_port = 12345
  mesh_peer_http_port = 8090
  mesh_peer_lmtp_port = 24
  mesh_peer_doveadm_ssl = yes
  mesh_peer_http_ssl = yes
}
mesh_peer node3 {
  mesh_peer_host = node3.mesh.example
  mesh_peer_doveadm_port = 12345
  mesh_peer_http_port = 8090
  mesh_peer_lmtp_port = 24
  mesh_peer_doveadm_ssl = yes
  mesh_peer_http_ssl = yes
}

# More detail while testing:
#log_debug = category=mesh
```

`/etc/dovecot/users` (same on all nodes), e.g.
`alice@mesh.example:{PLAIN}secret`. With delivery affinity, the passdb
answering LMTP lookups must not return proxy fields itself; passwd-file
doesn't.

Notes:

- `mesh_api_key` empty makes all mesh HTTP requests fail: the nodes never
  see each other as up and nothing replicates.
- `doveadm_password` must be identical everywhere: dsync authenticates
  to the peer's doveadm port with it.
- With `mesh_peer_doveadm_ssl`/`mesh_peer_http_ssl` the peer's
  certificate must match `mesh_peer_host`.
- For a first smoke test you can drop TLS: remove `ssl = yes` from the
  listeners and the `*_ssl = yes` peer settings, and point `mesh_url` at
  port 8090.

## 5. Live test plan

Use `alice`, `bob`, ... with a few clients. Tools: `swaks` or Python
`smtplib.LMTP` to inject mail, `doveadm` inside the containers, any IMAP
client (Thunderbird, `mutt`, Python `imaplib`), and the web UI.

Useful commands (inside a node container):

```
doveadm mesh status
doveadm mesh peers
doveadm mesh users [<user>]
doveadm mesh verify <user>          # exit 0 = all nodes identical
doveadm fetch -u <user> "mailbox uid flags hdr.subject" all
```

For each test, record: what you did, whether `doveadm mesh verify` passes
for the affected users afterwards, how long until all nodes were
identical, and any warnings/errors in the logs.

1. **Startup**: all nodes up, `doveadm mesh peers` shows every peer `up`
   on every node, the web UI loads on each node.
2. **Delivery**: deliver to each node's LMTP; the mail must be on all
   nodes within a second (`doveadm fetch` on each node). With affinity,
   the "saved mail to INBOX" log line appears on the user's anchor
   (`doveadm mesh users <user>` shows it), whichever node received the
   mail; `log_debug = category=lmtp` also logs the proxying.
3. **IMAP changes on different nodes**: two clients for the same user on
   two nodes; flag, move, delete, create/rename/delete folders on both at
   the same time. Both clients must end up showing the same state; verify.
4. **Node crash**: `docker kill` a node during load; the others keep
   working (the dead node's users get new anchors, see `doveadm mesh
   users`). Restart it; it must catch up (pending pushes drain, verify).
5. **Network partition**: `docker network disconnect` one node for a
   minute while both sides get changes, then reconnect. Verify all users
   touched during the partition.
6. **Delivery while a peer is unreachable**: deliveries must still
   succeed; LMTP waits up to `mesh_delivery_sync_timeout` (2 s) and logs
   "Delivered mail isn't on another node yet" if the anchor is
   unreachable but still considered up. Nothing may be lost when it
   returns.
7. **Adding a node**: add `node4` to every node's config, `doveadm
   reload`, start node4 empty. It must receive every user's mail; verify
   a few users. Some users' anchors move to node4.
8. **Latency**: add 50-150 ms delay between nodes (`tc qdisc add dev eth0
   root netem delay 100ms`) and repeat 2-4.
9. **Volume**: a user with 10k+ mails and 50+ folders, plus 100+ users with
   some mail. Measure initial sync time to an empty node, steady-state
   CPU/memory of the `mesh` and `doveadm` processes, and how long
   `doveadm mesh verify` takes.
10. **Long run**: leave the cluster running with a trickle of deliveries
    and IMAP changes for hours, with `mesh_verify_interval` on. Watch for
    "differs" warnings and errors.
11. **Restart everything**: stop and start all nodes; sessions afterwards
    should be incremental (web UI "Recent sessions" mode column).
12. **Archiving**: give a user a few thousand mails (some MB each is
    best), then move them all from INBOX to an Archive folder with an IMAP
    client on one node, and also move a few hundred back. Measure the
    traffic between the nodes meanwhile (e.g. container network
    counters): it must stay far below the mails' size, since the other
    nodes copy the mails locally. With Maildir the moved files on the
    other nodes are hardlinks of the original ones (`stat` link count),
    and `log_debug = category=dsync` (or `mesh_dsync_debug = yes`) logs
    "Copied N mails from other local mailboxes instead of transferring
    them". Verify the user afterwards.

The harness can also run inside a single container as a regression test
before deploying (`src/mesh/harness`, see the README; needs root, a
`vmail` user, and the build installed under `/opt/dovecot` or
`DOVECOT_PREFIX`).

## 6. What to watch in the logs

Expected, harmless:

- "Log synchronization error ... Extension record update for invalid
  uid", "index reset, view is now inconsistent" and an IMAP client
  disconnect "IMAP session state is inconsistent, please relogin": once
  per folder that was created independently on two nodes (e.g. a new
  user's INBOX). Should not repeat for the same folder.
- "Duplicate file entry ... dovecot-uidlist": during UID renumbering.
- Rarely "Transaction log modseq tracking is corrupted - fixing".
- An occasional single "Syncing <user> with <peer> failed" warning
  followed by a successful retry. (A peer that's busy with the user is
  retried silently.)
- Rarely "Local mail GUID=... disappeared during sync - skipping it": a
  mail was expunged locally while dsync was about to copy it.

Report:

- `Verifying <user>: nodes differ` and especially `still differs between
  nodes after a full sync`
- any `Panic`, `Fatal`, or the `mesh` process restarting
- repeated `Syncing <user> with <peer> failed` for the same user
- `Expunging failed` or `Couldn't open for expunging` from dsync: a
  deferred expunge couldn't be applied, so the session asked for a full
  resync (deleted mails may come back)
- `Delivered mail isn't on another node yet` while all peers are up
- mails missing on some node, duplicated, or coming back after deletion
  (from `doveadm mesh verify` or client observation)
- sessions shown as "stateless" in steady state (expected only after
  anchor moves, restarts with state loss, or for the first sync)

## 7. Known limitations and open items

See also "Limitations and known issues" in the README.

- IMAP logins aren't routed to the anchor; only LMTP deliveries are.
- Moving a few mails (fewer than 8 and under 512 kB) can still transfer
  them once to each node, when the destination folder is synced before
  the source folder in that session. Larger moves are copied locally.
- Pausing a peer doesn't survive a restart.
- meson build files don't include the mesh; use autotools.
- No migration tooling beyond the README's steps (from the Syncthing
  setup: stop Syncthing, replace `new -> cur` symlinks with directories,
  start the mesh; mails existing on both nodes are matched by GUID).

## 8. Where things are

- `src/mesh/README.md`: concepts, settings, operations, API.
- `src/mesh/harness/`: `phase1.py` (mesh tests), `moves.py` (moved
  mails mustn't be transferred again), `cluster.py` (node
  setup, shows a full working config), `workload.py` (load and
  convergence checks).
- `src/mesh/*.c`: service. `mesh-queue.c` (scheduling, retries),
  `mesh-sync.c` (sessions, leases), `mesh-http.c` (API), `mesh-verify.c`
  (fingerprints), `mesh-peer.c` (heartbeats, anchors), `mesh-state.c`
  (`mesh.db`), `ui/` (web UI).
- `src/plugins/mesh/mesh-plugin.c`: plugin.
