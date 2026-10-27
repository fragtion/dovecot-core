# Dovecot Mesh

Multi-master replication of mailboxes between any number of Dovecot
servers ("nodes"). Every node accepts deliveries, IMAP changes and
logins at the same time; the mesh keeps all of them in sync, normally
within a fraction of a second.

It replaces both what the replicator did for two servers (removed in
Dovecot 2.4) and the Syncthing-based setup this fork was originally made
for. The heavy lifting is done by dsync, Dovecot's own mailbox sync
engine; the mesh decides who syncs which user with whom, when, and with
which state, and shows what's going on.

## Contents

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Configuration](#configuration)
- [Settings reference](#settings-reference)
- [Operating it](#operating-it)
- [Delivery affinity](#delivery-affinity)
- [Monitoring](#monitoring)
- [Migrating from the Syncthing setup](#migrating-from-the-syncthing-setup)
- [Limitations and known issues](#limitations-and-known-issues)
- [Test harness](#test-harness)

## How it works

```
            node1                                 node2 (anchor of alice)
  +----------------------------+          +----------------------------+
  | imap / lmtp                |          | imap / lmtp                |
  |   mesh plugin --notify--+  |          |  +--notify-- mesh plugin   |
  |                         v  |  HTTP    |  v                         |
  |   mesh service <-----------+----------+-> mesh service             |
  |     | heartbeats, leases,  |          |   (relays alice's changes  |
  |     | pair states          |          |    to node3, node4, ...)   |
  |     v                      |  dsync   |                            |
  |   doveadm-server ----------+----------+-> doveadm server           |
  +----------------------------+          +----------------------------+
```

- **mesh plugin** (`lib25_mesh_plugin`, in mail processes): after every
  committed change (new mail, flags, keywords, expunge, mailbox
  create/delete/rename/subscribe) it tells the local mesh service which
  user changed. It also serializes local saves with dsync sessions for the
  same user (a lock file in the user's home), which avoids duplicate mails
  when dsync renumbers UIDs, and it makes LMTP/LDA deliveries wait until
  the mail is on another node.
- **mesh service** (`mesh`, one process per node): queues a push for the
  changed user, coalesces bursts, and runs `doveadm sync` (stateful dsync)
  through the local doveadm server to the peer's doveadm port. It talks to
  the other nodes' mesh services over HTTP: heartbeats, leases that keep a
  user's sessions from overlapping, and the pair's dsync state. It also
  serves the web UI, the HTTP API and the `doveadm mesh` commands.

**Topology.** With the default `anchor` topology each user has one
*anchor* node, chosen by rendezvous hashing over the nodes that are up.
All nodes compute the same anchor, and a node going down moves only the
users anchored to it. Other nodes (*spokes*) push a user's changes only
to its anchor, which relays them to every other node. So each change
crosses the network a minimal number of times, and every pair of nodes
that syncs a user has a well defined history. With `full`, every node
syncs every user with every other node; that's only sensible for two or
three nodes.

**Sessions and states.** A dsync session between two nodes is
bidirectional: both send each other what changed. Sessions are stateful
(only changes since the last session are compared), with one shared state
per pair of nodes. Before a session, the initiating node *leases* the user
from the peer; the peer refuses if it's syncing the user itself, and the
initiator retries a moment later. The lease reply also tells whether the
initiator's copy of the pair's state can be used: it must be the newest
one, and at least one of the two nodes must have synced the user only
with the other one since their last session. Otherwise mails could have
reached both nodes some other way, which a state can't describe, and the
session is *stateless*: a full sync, in which dsync compares all the
mails of every mailbox. (Without a state, dsync would otherwise skip
mailboxes whose UIDVALIDITY, UIDNEXT, message count and modseqs are the
same on both nodes, which copies made by other tools can have in common
while holding different mails.) That is slower but always safe. With the
anchor topology it happens only after the anchor moved, or if a node lost
its state file.

**Triggers.** Changes are pushed after `mesh_coalesce_delay`. In addition
every user is pushed every `mesh_incremental_sync_interval` (catches
changes nobody notified, e.g. a lost notification), fully every
`mesh_full_sync_interval`, at startup, and when peers come up or the
anchor moves. "Every user" includes all users the userdb lists (read at
startup and with each full sync), so users who don't change anything
still reach a new or emptied node. A userdb that can't list its users
(e.g. `static`) covers only users that changed something or that a peer
asked about; set `mesh_list_users = no` for it. Failures are retried with backoff from 1s up to 60s; after
three failures in a row the state is dropped and the next session is
stateless.

**Verification.** `doveadm mesh verify <user>` (or the web UI) compares
a fingerprint of the user's mailboxes on all nodes that are up: per
mailbox the number of mails and a hash of their UIDs, GUIDs and flags.
With `mesh_verify_interval` set, each anchor also verifies its users one
at a time in the background and repairs differences with a full sync.

## Requirements

- Dovecot built from this tree, on every node (same version on all).
- Maildir (`mail_driver = maildir`) is what's tested. sdbox/mdbox should
  work as dsync supports them, but haven't been tested.
- Mail should arrive through LMTP or LDA (`dovecot-lda`), not by an MTA
  writing into the Maildir itself: only Dovecot's own delivery notifies
  the mesh right away and waits for replication.
- Each node needs its own `hostname`: dsync and Maildir file names use it.
- Clocks in sync (NTP).
- For delivery affinity: Dovecot built with Lua (`--with-lua`).
- Network between the nodes: each node's doveadm TCP port and mesh HTTP
  port must be reachable from all other nodes. Use TLS for both if the
  network isn't trusted (see below).

## Configuration

The same file can be used on every node; only `mesh_node_name` and
`hostname` differ. A node skips the `mesh_peer` entry with its own name.

```
# --- per node ---
hostname = node1.example.com
mesh_node_name = node1

# --- the same on all nodes ---
mail_plugins {
  notify = yes
  mesh = yes
}

# relay-changes: needed with more than two nodes - forward changes that
# dsync imported. reuse-local-mails: when mails are moved or copied to
# another folder, copy them locally instead of transferring them again.
dsync_features = relay-changes reuse-local-mails

# dsync between nodes goes through the doveadm servers.
doveadm_port = 12345
doveadm_password = <shared secret, same on all nodes>
service doveadm {
  inet_listener doveadm {
    port = 12345
    #ssl = yes
  }
}

# Shared secret for the mesh HTTP API between nodes, and the web UI
# password.
mesh_api_key = <shared secret, same on all nodes>

service mesh {
  inet_listener mesh-http {
    port = 8090
    #ssl = yes
  }
}

mesh_peer node1 {
  mesh_peer_host = 10.0.0.1
  mesh_peer_doveadm_port = 12345
  mesh_peer_http_port = 8090
  #mesh_peer_lmtp_port = 24
  #mesh_peer_doveadm_ssl = yes
  #mesh_peer_http_ssl = yes
}
mesh_peer node2 {
  mesh_peer_host = 10.0.0.2
  mesh_peer_doveadm_port = 12345
  mesh_peer_http_port = 8090
}
mesh_peer node3 {
  mesh_peer_host = 10.0.0.3
  mesh_peer_doveadm_port = 12345
  mesh_peer_http_port = 8090
}

# Optional: check users' replicas in the background and repair
# differences.
#mesh_verify_interval = 1min
```

`maildir_ignore_new_dir` must stay `no` (the default) with the mesh. It
exists only for the old Syncthing setup.

### TLS between nodes

```
ssl = yes
ssl_server_cert_file = /etc/dovecot/node.pem
ssl_server_key_file = /etc/dovecot/node.key
# CA that signed the other nodes' certificates
ssl_client_ca_file = /etc/dovecot/mesh-ca.pem

service doveadm {
  inet_listener doveadm {
    port = 12345
    ssl = yes
  }
}
service mesh {
  inet_listener mesh-http {
    port = 8090
    ssl = yes
  }
}
mesh_peer node2 {
  mesh_peer_host = node2.example.com
  ...
  mesh_peer_doveadm_ssl = yes
  mesh_peer_http_ssl = yes
}
```

The certificates are verified against `mesh_peer_host`, so it must be a
name (or IP address) in the peer's certificate.

## Settings reference

Mesh service:

| Setting | Default | |
|---|---|---|
| `mesh_node_name` | hostname | This node's name, as the other nodes' `mesh_peer` calls it. |
| `mesh_peer <name> { }` | | One per node; entries with this node's own name are ignored. |
| `mesh_peer_host` | | Host name or IP of the peer. |
| `mesh_peer_doveadm_port` | | The peer's doveadm TCP port. |
| `mesh_peer_http_port` | | The peer's mesh HTTP port. |
| `mesh_peer_lmtp_port` | 24 | The peer's LMTP port, for delivery affinity. |
| `mesh_peer_doveadm_ssl` | no | dsync to the peer over TLS. |
| `mesh_peer_http_ssl` | no | Mesh HTTP to the peer over TLS. |
| `mesh_topology` | anchor | `anchor` or `full`. |
| `mesh_api_key` | | Shared secret for the HTTP API and UI. Empty: HTTP requests are refused. |
| `mesh_state_path` | mesh.db | State file, relative to `state_dir`. |
| `mesh_doveadm_socket_path` | doveadm-server | Local doveadm server socket, relative to `base_dir`. |
| `mesh_coalesce_delay` | 100ms | Wait this long after a change before pushing it. |
| `mesh_heartbeat_interval` | 2s | Heartbeat to each peer. |
| `mesh_peer_down_timeout` | 7s | A peer without heartbeat replies for this long is down. |
| `mesh_incremental_sync_interval` | 10min | Push every user this often (0 = never). |
| `mesh_full_sync_interval` | 1d | Fully sync every user this often (0 = never). |
| `mesh_sync_timeout` | 2min | Give up a session when no data has moved for this long (dsync's I/O timeout, on both nodes). |
| `mesh_max_parallel_syncs` | 4 | Sessions this node runs at the same time. |
| `mesh_verify_interval` | 0 | Verify one of this node's anchored users this often (0 = never). |
| `mesh_dsync_debug` | no | Run dsync with debug logging. |
| `mesh_list_users` | yes | Learn all users from the userdb at startup and with each full sync. |

Mesh plugin (mail processes):

| Setting | Default | |
|---|---|---|
| `mesh_notify_socket_path` | mesh-notify | Mesh service socket, relative to `base_dir`. |
| `mesh_delivery_sync_timeout` | 2s | LMTP/LDA wait at most this long for the mail to reach another node before confirming the delivery (0 = don't wait). |
| `mesh_save_lock_timeout` | 30s | Saving waits at most this long for a running dsync session of the user; then it saves anyway, with a warning. |
| `mesh_dsync_lock_timeout` | 10s | A dsync session waits at most this long for the user's lock, then fails (and is retried). |

dsync: `dsync_features = relay-changes` is needed with more than two
nodes. Without it a node never forwards flag changes, keyword changes or
expunges it received from one node to another.

`dsync_features = reuse-local-mails` makes moving and copying mails
between folders cheap to replicate. dsync syncs one folder at a time, so
a moved mail shows up as an expunge in the old folder and a new mail in
the new one, and without this feature the new one is downloaded from the
peer again. With it, the receiving node copies the mail from its own copy
in the old folder (a hardlink with Maildir), and the expunges a peer sent
are applied only at the end of the session so that the old copy is still
there when the new folder is synced. When the new folder is synced before
the old one, the node looks the mails up in its other folders by GUID; it
does that only if at least 8 mails or 512 kB would otherwise be
transferred, since it reads the GUIDs of all the user's mails (about 0.3
seconds for 100,000 mails). The node that starts a session turns it on
for both sides. If a deferred expunge can't be applied at the end, the
session asks for a full resync, so nothing is lost.

## Operating it

Web UI: `http://<node>:<mesh http port>/` (redirects to `/mesh/ui/`).
Log in with any user name and `mesh_api_key` as the password. It shows
this node's view: peers with heartbeat, RTT and clock offset; every user's
status per peer; recent sessions; and has buttons for pause/resume, sync
now, full resync and verify.

Command line (on a node):

```
doveadm mesh status              # node, peers up, users, pending/running
doveadm mesh peers               # per peer: up/down, RTT, last error, ...
doveadm mesh users [<user>]      # per user and peer: status, last sync, ...
doveadm mesh sync [-f] <user>    # push now (-f: full sync)
doveadm mesh verify <user>       # compare the user's mailboxes on all nodes
doveadm mesh pause|resume <peer> # stop/start syncing with a peer
```

User status per peer: `ok`; `via anchor` (relayed: this node syncs the
user only with its anchor); `pending`; `syncing`; `error` (failing,
retried with backoff); `never synced`.

HTTP API (all under `/mesh/v1/`, `Authorization: Bearer <mesh_api_key>`
or Basic auth; POST requests with Basic auth also need an
`X-Requested-With` header):

| Request | |
|---|---|
| `GET status` | Node and peers, JSON. |
| `GET users[?user=u\|q=substring][&problems=1][&limit=n]` | Users' status per peer, JSON. |
| `GET activity[?limit=n]` | Recent sessions of this node, newest first. |
| `GET route?user=u` | Where to deliver the user's mail (see below). |
| `POST sync?user=u[&full=1]` | Push the user now. |
| `POST verify?user=u` | Compare the user's mailboxes on all nodes, JSON. |
| `POST pause?peer=p`, `POST resume?peer=p` | |
| `GET heartbeat`, `GET fingerprint?user=u`, `POST lease`, `POST release` | Between nodes. |

Common tasks:

- **Node down**: the others notice after `mesh_peer_down_timeout`; users
  anchored to it get a new anchor. Changes made meanwhile are pushed when
  it comes back (startup sync, and the other nodes see it come up).
- **Adding a node**: add its `mesh_peer` to every node's configuration and
  `doveadm reload`. At startup the new node lists the users from its
  userdb and syncs each with its anchor; sessions go both ways, so an
  empty node gets every user's mail with these first (stateless)
  sessions. Anchors of some users move to it. The userdb must have the
  same users on all nodes.
- **Removing a node**: remove its `mesh_peer` everywhere and reload.
- **Something looks wrong with a user**: `doveadm mesh verify <user>`,
  then `doveadm mesh sync -f <user>` to repair, and verify again.
- **Maintenance on a node**: `doveadm mesh pause <node>` on the other
  nodes stops syncing with it (pause isn't persistent across restarts).

### When the network between nodes is slow or down

IMAP and POP3 sessions work on the local copy; only these wait for the
network:

- **Saving mail** (deliveries, IMAP APPEND, COPY and MOVE) waits while a
  session for the same user is running on this node, at most
  `mesh_save_lock_timeout` (30 s); then it saves anyway. On a good link a
  session takes milliseconds. On a slow one, an active user's saves can
  wait for the session that replicates their previous change. Reading
  mail, changing flags and deleting never wait.
- **Deliveries** (LMTP, LDA) also wait up to `mesh_delivery_sync_timeout`
  (2 s) for the mail to reach another node, but not when no peer is up.
- **A node that stops answering** is marked down after
  `mesh_peer_down_timeout` (7 s): no new sessions with it, and deliveries
  stop waiting. A session that was running with it keeps that user's
  mailboxes busy on both nodes until dsync gives up, after
  `mesh_sync_timeout` (2 min) without data; that user's saves wait up to
  `mesh_save_lock_timeout` each meanwhile. Lower both for links that
  break often, at the cost of more failed (and retried) sessions on slow
  ones.
- **An interrupted session** keeps what it copied as long as that's
  consistent; when it had moved mails to new UIDs (conflicts), it's rolled
  back and redone by the next session.

## Delivery affinity

Mail delivered to several nodes at the same time gets the same UIDs on
them, which dsync then has to renumber; IMAP clients see renumbered mails
as expunged and new. Delivering each user's mail on its anchor avoids
that. `mesh-route.lua` (installed into Dovecot's data directory, e.g.
`/usr/share/dovecot/mesh-route.lua`) is a Lua passdb that makes LMTP
proxy each recipient to its anchor:

```
lmtp_proxy = yes
# must be the first passdb
passdb mesh-route {
  driver = lua
  lua_file = /usr/share/dovecot/mesh-route.lua
  lua_settings {
    mesh_url = http://127.0.0.1:8090
    mesh_api_key = <mesh_api_key>
  }
}
passdb ... { (the real passdb) }
```

It only answers LMTP lookups and passes everything else on to the next
passdb. If the anchor is the local node, isn't reachable, or the mesh
service can't be asked, the mail is delivered locally. The peers'
`mesh_peer_lmtp_port` must be right. IMAP logins aren't routed; clients
may use any node.

If the mesh HTTP listener uses TLS, give the script a plaintext listener
that only listens on localhost:

```
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
# and in lua_settings: mesh_url = http://127.0.0.1:8091
```

## Monitoring

- Every session sends a `mesh_sync_finished` event with the fields
  `user`, `peer`, `result` (ok, changed, busy, failed), `exit_code`,
  `full`, `stateful`, `error` and the duration. Use it for metrics (e.g.
  sessions per peer and result, duration histogram) or
  `log_debug = event=mesh_sync_finished` to log them.
- Warnings worth alerting on: `Syncing <user> with <peer> failed`,
  `Peer <peer> is down`, `Verifying <user>: nodes differ` (with
  `mesh_verify_interval`), `<user> still differs between nodes after a
  full sync` (error), and from mail processes `mesh: Delivered mail isn't
  on another node yet after N ms`.
- `log_debug = category=mesh` logs every notification and session.

## Migrating from the Syncthing setup

1. Stop Syncthing on all nodes, and stop Dovecot.
2. Install this build everywhere. Leave `maildir_ignore_new_dir` unset
   (`no`).
3. Replace the `new -> cur` symlinks with real directories, e.g.
   `find /var/vmail -type l -name new -exec sh -c 'rm "$1" && mkdir -m 0700 "$1"' _ {} \;`
   and fix their owner.
4. Choose:
   - keep one node's data: move the other nodes' mail aside (keep it as a
     backup) and let the mesh copy everything. The safest choice; it
     costs bandwidth.
   - keep all nodes' data, but only if every node has every file: Syncthing
     showed all folders as up to date before you stopped it, and the
     number of mail files per user is the same on all nodes. The first
     session of each user and pair is stateless; mails on both nodes are
     matched by their GUID (the Maildir file name) and not copied again.
     A node that lacks some files would make that session delete them on
     the other nodes (see the limitations).
5. Configure the mesh as above, start Dovecot everywhere, watch the web UI
   until nothing is pending, and verify some users (or set
   `mesh_verify_interval`).
6. Point MX/MTA delivery at the nodes' LMTP, optionally with delivery
   affinity.

## Limitations and known issues

- When the same folder is first created on several nodes independently
  (e.g. INBOX of a new user on first login or delivery on two nodes), the
  first sync makes the folders' UIDVALIDITY and GUID equal, which resets
  the folder index on one node. IMAP clients connected to that folder at
  that moment are disconnected once ("IMAP session state is inconsistent,
  please relogin"), and the log shows "Log synchronization error ...
  Extension record update for invalid uid" errors. Delivery affinity and
  migrating existing data first make it rare.
- "Duplicate file entry" warnings about `dovecot-uidlist` appear while
  dsync renumbers mails; they're harmless. Rarely, "Transaction log modseq
  tracking is corrupted - fixing" appears; Dovecot repairs that itself.
- If a session fails halfway, and a mail it already copied is expunged
  before the next successful session, the mail can come back (a dsync
  limitation with partially applied states).
- A session without a state (the first one between two nodes, after an
  anchor moved, a full sync) takes mails that one node lacks *below* UIDs
  where both nodes have the same mails for deletions, and deletes them on
  the other node. That's right for mail deleted through Dovecot, but not
  for copies made by other means that are missing files: e.g. a node of
  the Syncthing setup that hadn't received every file yet, or a partial
  restore. Start such nodes empty instead (see the migration steps).
- A node that writes mail into Maildirs behind Dovecot's back (another
  MDA, scripts) isn't noticed until the next periodic sync, and delivery
  waits don't apply.
- With `reuse-local-mails`, moving a few mails (fewer than 8 and under
  512 kB) still transfers them once to each node when the destination
  folder happens to be synced before the source folder in that session.
- The `full` topology syncs mostly statelessly with more than two nodes.
- Pausing a peer isn't persistent.
- The mesh service runs as root (like doveadm), since it uses the
  doveadm-server socket and writes `state_dir`.

## Test harness

`harness/` runs real Dovecot nodes on one machine (as root, needs a
`vmail` user, Dovecot installed to `/opt/dovecot` or `DOVECOT_PREFIX`),
drives a workload of LMTP deliveries and IMAP flag changes, moves,
copies, expunges and appends on random nodes, and checks that all nodes
end up identical with nothing lost, duplicated or brought back:

```
cd src/mesh/harness
python3 phase1.py --nodes 5 --users 3 --duration 20
python3 phase1.py --nodes 4 --users 2 --kill-node node3   # crash a node
python3 phase1.py --nodes 3 --users 3 --lmtp-route        # delivery affinity
python3 phase1.py --nodes 3 --users 2 --tls               # TLS between nodes
python3 phase1.py --nodes 3 --users 3 --topology full
python3 phase1.py ... --keep   # leave the nodes running in /var/tmp/meshtest
python3 moves.py --nodes 3 --mails 500   # moved mails aren't transferred
python3 newmails.py   # a mail new on two nodes gets the same flags
python3 idleusers.py  # idle users reach every node, differing copies merge
python3 overlap.py    # copies sharing some mails under other UIDs merge
python3 overlap.py --same-uidlist  # different mails, copied dovecot-uidlist
python3 stall.py      # a stalled session times out on both nodes, loses nothing
```

`moves.py` seeds a user with mails, lets the mesh replicate them, moves
them all to another folder on one node, and fails if any mail body was
transferred over the network for the move (`--max-transferred`).

`newmails.py` runs plain dsync between two nodes: a mail that is new on
both under different UIDs (as after IMAP clients moved it into the same
folder on both) must end up with the same flags on both.

`phase0.py` drives dsync directly instead of the mesh service; it was
used to find out what dsync needs for multi-master replication.
