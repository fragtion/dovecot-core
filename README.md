# Dovecot Mesh

Multi-master mail clustering built into Dovecot: any number of servers
accept deliveries and IMAP changes at the same time and replicate them to
each other, normally within a fraction of a second.

This is the `dovecotmesh` branch of
[fragtion/dovecot-core](https://github.com/fragtion/dovecot-core), the
project's main line. The repository keeps its old name so that existing
links keep working.

> **Coming from the Syncthing setup?** That approach is legacy: the
> [`legacy-syncthing`](https://github.com/fragtion/dovecot-core/tree/legacy-syncthing)
> branch of this repository and [fragtion/syncthing](https://github.com/fragtion/syncthing)
> are no longer developed. See
> [Migrating from the Syncthing setup](src/mesh/README.md#migrating-from-the-syncthing-setup).
>
> **Looking for unmodified Dovecot?** Go to
> [dovecot/core](https://github.com/dovecot/core).

## What it does

- **Built into Dovecot.** A `mesh` service on each node and a `mesh` mail
  plugin drive dsync, Dovecot's own incremental mailbox sync. Only what
  changed is sent: renamed folders aren't copied again, and mails moved or
  copied between folders are copied locally on the other nodes instead of
  being transferred.
- **Any number of nodes.** Each user has an *anchor* node that relays the
  user's changes to the others. When a node goes down, its users get new
  anchors automatically.
- **Safe deliveries.** A delivery is confirmed once the mail is on a
  second node, waiting up to `mesh_delivery_sync_timeout` (2 s). Optionally,
  LMTP delivers each user's mail on their anchor (delivery affinity).
- **Visible.** A web UI and `doveadm mesh` commands show the nodes, each
  user's sync status and recent sessions, and can sync, resync, pause and
  verify.
- **Self-checking.** A verifier compares users' mailboxes across all nodes
  and repairs differences in the background.
- **TLS between nodes.**

| | Dovecot replicator | Syncthing setup (legacy) | Dovecot Mesh |
|---|---|---|---|
| Nodes | 2 (removed in Dovecot 2.4) | any | any |
| Replicates | mailboxes, with dsync | Maildir files | mailboxes, with dsync |
| Concurrent changes on two nodes | resolved by dsync | can create duplicates | resolved by dsync |
| `\Recent` flag | works (per node) | lost (`new/` is ignored) | works (per node) |
| Delivery waits for a second copy | optional | no | yes, up to 2 s |

## Quick start

Build from git (see [AGENTS.md](AGENTS.md) for the build dependencies):

```sh
autoreconf -vif
./configure --prefix=/opt/dovecot --with-lua=yes   # Lua: delivery affinity
make -j$(nproc)
sudo make install
```

Then on every node:

1. Use Maildir and deliver mail through Dovecot's LMTP or LDA.
2. Add the mesh configuration from
   [src/mesh/README.md](src/mesh/README.md#configuration): the `mesh`
   plugin, `dsync_features = relay-changes reuse-local-mails`, the doveadm
   and mesh HTTP listeners with shared secrets, and one `mesh_peer` block
   per node. Only `hostname` and `mesh_node_name` differ between nodes.
3. Start Dovecot, then check that every peer is up:
   `doveadm mesh peers`, or the web UI at `http://<node>:8090/`.

## Status

Tested on one host with the included harness (`src/mesh/harness/`): 3 to
5 nodes under mixed load (LMTP deliveries plus IMAP flag changes, moves,
copies, expunges and appends on random nodes at once), node crashes, TLS
and delivery affinity, each run ending with all nodes identical and
nothing lost, duplicated or resurrected. Not yet tested on separate
machines, over a real network, or at scale; that's next. Read the
[limitations](src/mesh/README.md#limitations-and-known-issues) before
using it in production.

## Documentation

| Where | What |
|---|---|
| [src/mesh/README.md](src/mesh/README.md) | How it works, configuration, settings, operation, delivery affinity, monitoring, migration, limitations, test harness |
| [src/mesh/HANDOFF.md](src/mesh/HANDOFF.md) | Deploying in containers and the live test plan |
| [AGENTS.md](AGENTS.md) | Build environment and running the tests |

## Branches

| Branch | What it is |
|---|---|
| `dovecotmesh` (default) | Dovecot Mesh. Upstream changes are merged in regularly. |
| `main` | An unmodified mirror of [dovecot/core](https://github.com/dovecot/core) `main`, kept in sync with upstream. |
| `legacy-syncthing` | The last state of the Syncthing-based fork. Frozen. |

## Based on

The Dovecot 2.4 development tree ([dovecot/core](https://github.com/dovecot/core)
`main`), under Dovecot's licenses (see [COPYING](COPYING)).

## Coffee

Did this make you happy? I'd love to do more development like this! Please donate to show your support :)

**PayPal**: [![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate/?business=2CGE77L7BZS3S&no_recurring=0)

**BTC:** 1Q4QkBn2Rx4hxFBgHEwRJXYHJjtfusnYfy

**XMR:** 4AfeGxGR4JqDxwVGWPTZHtX5QnQ3dTzwzMWLBFvysa6FTpTbz8Juqs25XuysVfowQoSYGdMESqnvrEQ969nR9Q7mEgpA5Zm
