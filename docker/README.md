# Dovecot Mesh in Docker

One container per node, next to Postfix on the host. The image has
Dovecot with the mesh and Pigeonhole (Sieve), installed under
`/usr/local/apps/dovecot` like the Syncthing-era builds, so an existing
configuration keeps working with a few additions.

- [Get the image](#get-the-image)
- [What goes where](#what-goes-where)
- [Configuration](#configuration)
- [Postfix](#postfix)
- [Firewall](#firewall)
- [Migrating from the Syncthing setup](#migrating-from-the-syncthing-setup)
- [Quick tests](#quick-tests)
- [Adding a node, adding a user](#adding-a-node-adding-a-user)
- [Operating it](#operating-it)
- [Troubleshooting](#troubleshooting)

## Get the image

```sh
docker pull ghcr.io/fragtion/dovecotmesh:latest     # amd64 and arm64
```

Or build it from the repository root:

```sh
docker build -f docker/Dockerfile -t dovecotmesh \
	--build-arg REVISION=$(git rev-parse --short HEAD) .
```

| Build argument | Default | |
|---|---|---|
| `BASE_IMAGE` | `debian:trixie-slim` | `ubuntu:26.04` works too. |
| `PREFIX` | `/usr/local/apps/dovecot` | Where Dovecot is installed. |
| `VERSION`, `REVISION` | `0.0.0-dovecotmesh`, `unknown` | What `dovecot --version` shows. |
| `WITH_PIGEONHOLE` | `yes` | Sieve, ManageSieve, and Sieve script replication. |
| `PIGEONHOLE_REF` | a commit of Pigeonhole's main branch | Must match this tree's Dovecot API. A source tarball `docker/dovecot-pigeonhole-*.tar.gz` is built instead, if present. |
| `VMAIL_UID`, `VMAIL_GID` | 5000 | Owner of the mail (`vmail`). |

## What goes where

[`examples/compose.yaml`](examples/compose.yaml) runs the container with
the host's network, so Dovecot listens on the host's ports as before, and
the container has the host's hostname, which is the node's name in the
mesh.

| Host | In the container | |
|---|---|---|
| `/usr/local/apps/dovecot/etc/dovecot` | `/etc/dovecot` | `dovecot.conf`, `mesh.conf`, `passwd`, `users`. Also reachable as `/usr/local/apps/dovecot/etc/dovecot`, so the paths in your configuration stay valid. |
| `/home/vmail` | `/home/vmail` | Mail, indexes, Sieve scripts. |
| `/var/lib/dovecot-mesh` | `/var/lib/dovecot` | The mesh's state (`mesh.db`). Keep it: without it the next sessions compare all mails. |
| `/var/log` | `/var/log` | For `log_path` and `info_log_path` files. |
| `/etc/letsencrypt` | `/etc/letsencrypt` (read-only) | Certificates. |

The container runs as root, like Dovecot's master process on a host;
mail processes run as the users' uid (5000).

## Configuration

Starting from the `dovecot.conf` of a host install:

1. Copy [`examples/mesh.conf`](examples/mesh.conf) next to it and edit it:
   - set both secrets, e.g. to the output of `openssl rand -hex 32`;
   - add one `mesh_peer` block per node, named after the node's hostname
     (`hostname` on the host) and with its address.

   It's the same file on every node. Then add at the end of
   `dovecot.conf`:

   ```
   !include mesh.conf
   ```

2. Remove the `unix_listener /var/spool/postfix/private/auth { ... }`
   block from `service auth`: Postfix's spool directory isn't in the
   container. `mesh.conf` adds a listener on `127.0.0.1:12346` for Postfix
   instead. Postfix's clients get the global `auth_mechanisms` (an
   `auth_mechanisms` line inside a listener block has no effect), so add
   any mechanism they need there, e.g. `login`.

3. Mail is delivered over LMTP instead of `dovecot-lda`. `mesh.conf` has a
   `protocol lmtp` block with the Sieve plugin and mailbox
   autocreation, like a typical `protocol lda` block; adjust it to match
   yours. The `protocol lda` block can stay.

4. If the host has no IPv6, add `listen = *` (Dovecot listens on `*, ::`
   by default and doesn't start if `::` fails).

5. The `passwd` and `users` files must list the same users on every node.
   The mesh doesn't replicate them.

`maildir_ignore_new_dir` must not be set (it existed only for the
Syncthing setup).

Check the configuration without starting anything:

```sh
docker run --rm -v /usr/local/apps/dovecot/etc/dovecot:/etc/dovecot \
	-v /etc/letsencrypt:/etc/letsencrypt:ro \
	ghcr.io/fragtion/dovecotmesh doveconf -n | less
```

## Postfix

In `main.cf`, replace the `dovecot` pipe transport (`dovecot-lda`) and the
SASL socket:

```
virtual_transport = lmtp:inet:127.0.0.1:24
smtpd_sasl_type = dovecot
smtpd_sasl_path = inet:127.0.0.1:12346
```

and run `postfix reload`. The `dovecot` service in `master.cf` is no
longer used. Delivering through LMTP is what makes deliveries wait until
the mail is on a second node.

## Firewall

Between the nodes only (not from the internet):

| Port | |
|---|---|
| 12345/tcp | doveadm: dsync sessions between nodes |
| 8090/tcp | mesh HTTP: heartbeats, leases, the web UI and API |

Both are protected by the shared secrets but are plaintext by default.
Across an untrusted network, use a VPN or private network, or TLS (see
[TLS between nodes](../src/mesh/README.md#tls-between-nodes)).

## Migrating from the Syncthing setup

Do this in a quiet hour. Commands are for every node unless the step
says otherwise.

1. **Back up** `/home/vmail` (and the configuration) on at least one node.

2. **Stop mail flow and the old services**: `postfix stop`, stop the old
   Dovecot, stop Syncthing, and disable both so they don't come back at
   boot (e.g. `systemctl disable --now syncthing@<user> <dovecot unit>`).

3. **Decide which data to keep.** The mesh merges copies that differ, but
   a copy that is *missing* files the others have can make the first
   sessions delete those mails everywhere (see the
   [limitations](../src/mesh/README.md#limitations-and-known-issues)). So
   either:

   - **keep one node's data** (the safest): on every other node move the
     mail aside, keep it as a backup, and let the mesh copy everything:

     ```sh
     mv /home/vmail /home/vmail.syncthing
     mkdir /home/vmail && chown 5000:5000 /home/vmail
     ```

   - or **keep every node's data**, but only if all nodes have the same
     files. Compare the number of mail files per folder:

     ```sh
     cd /home/vmail && find . -path ./indexes -prune -o -type f \
     	\( -path '*/cur/*' -o -path '*/new/*' \) -print |
     	sed 's|/[^/]*$||' | sort | uniq -c > /tmp/counts.$(hostname)
     ```

     copy the files to one node and `diff` them. Any difference: use the
     first option for that node, or at least move the users that differ
     aside there.

4. **Clean up the Syncthing leftovers** in the mail that you keep:

   ```sh
   # new -> cur symlinks back to directories
   find /home/vmail -type l -name new -exec sh -c \
   	'rm "$1" && mkdir -m 0700 "$1" && chown 5000:5000 "$1"' _ {} \;
   # Syncthing's own files
   find /home/vmail \( -name .stfolder -o -name .stignore -o \
   	-name .stversions \) -prune -exec rm -rf {} +
   # conflict copies: review, then move them away
   find /home/vmail -name '*sync-conflict*'
   ```

   `dovecot-uidlist.sync-conflict-*` files can be deleted; conflict copies
   of mails are duplicates of mails that exist.

   The container refuses to start while `new -> cur` symlinks remain
   (`MAILDIR_SYMLINK_CHECK` in `compose.yaml`).

5. **Configure** as above, then start the container, first on the node
   whose data you keep, then on the others:

   ```sh
   docker compose -f /path/to/compose.yaml up -d
   docker logs dovecot     # nothing should be printed
   ```

6. **Watch the mesh come together** on any node:

   ```sh
   docker exec dovecot doveadm mesh peers    # every peer "up"
   docker exec dovecot doveadm mesh status   # pending and running drop to 0
   ```

   or open the web UI at `http://<node>:8090/` (any user name, the
   `mesh_api_key` as the password). Nodes that started empty fill up at
   the speed of the network; Sieve scripts come along.

7. **Change Postfix** (see above) and `postfix start`.

8. **Verify** some users: `docker exec dovecot doveadm mesh verify
   user@example.com` prints each folder's status on every node and
   exits non-zero if they differ. With `mesh_verify_interval` (set in the
   example) every anchor checks its users in the background too.

Keep the backup and the moved-aside mail until you're happy.

## Quick tests

On a test account:

- **Delivery**: send a mail through one node's Postfix. On every node,
  `docker exec dovecot doveadm search -u test@example.com mailbox INBOX
  all | wc -l` shows it within a second or two.
- **IMAP**: connect a mail client to one node, move a mail to another
  folder, flag one, delete one; connect to another node (or check with
  `doveadm search`) and see the same.
- **SMTP AUTH**: send a mail with authentication through each node's
  Postfix.
- **Sieve**: a filter rule of the user files a matching delivery; the
  script exists in `/home/vmail/sieve/...` on every node.
- **A node down**: `docker stop dovecot` on one node, deliver a mail
  through another (it's accepted right away), `docker start dovecot`: the
  mail arrives on the stopped node within seconds. `doveadm mesh peers`
  on the others shows it down meanwhile.
- **All of it**: `docker exec dovecot doveadm mesh verify
  test@example.com`.

[`test/smoke-test.py`](test/smoke-test.py) does all of this, and more,
with two throwaway containers on one Docker host; it doesn't touch the
host's mail or configuration (it creates a Docker network on
10.231.0.0/24 meanwhile; change `SUBNET` in it if that's in use):

```sh
sudo python3 docker/test/smoke-test.py --image ghcr.io/fragtion/dovecotmesh
```

## Adding a node, adding a user

**A node**:

1. Install Docker, copy `compose.yaml` and the configuration directory
   from another node, create an empty `/home/vmail` owned by 5000:5000.
2. Add its `mesh_peer` block to `mesh.conf` on every node, and reload the
   running ones: `docker exec dovecot doveadm reload`.
3. Start it. It lists the users from its userdb and syncs each one, so it
   receives everybody's mail; `doveadm mesh status` shows the progress.
   Some users' anchors move to it.
4. Add it to DNS/MX once nothing is pending.

**A user**: add them to `passwd` and `users` on every node. The first
delivery or login creates their mailboxes, and the mesh replicates them.

## Operating it

| Task | |
|---|---|
| doveadm | `docker exec dovecot doveadm ...` |
| Reload the configuration | `docker exec dovecot doveadm reload` |
| New certificates | certbot deploy hook: `docker exec dovecot doveadm reload` |
| Log rotation | logrotate `postrotate`: `docker exec dovecot doveadm log reopen` |
| Upgrade | `docker compose pull && docker compose up -d`, one node at a time |
| Pause syncing with a node | `docker exec dovecot doveadm mesh pause <node>` on the others |

See [Operating it](../src/mesh/README.md#operating-it) for the mesh
itself, including what slows down when the network between nodes is
slow or down.

**POP3**: a `pop3_uidl_format` based on UIDs stays the same on every
node, because the mesh keeps UIDs identical. A mail delivered to two
nodes at the same moment can get a new UID while the nodes reconcile it,
and POP3 clients then download it again;
[delivery affinity](../src/mesh/README.md#delivery-affinity) avoids
that.

## Troubleshooting

| Symptom | |
|---|---|
| `listen(::, 143) failed: Address family not supported` | No IPv6: add `listen = *`. |
| The container exits right away | `docker logs dovecot` shows the configuration error. |
| `Maildirs still have new -> cur symlinks` | Step 4 of the migration. |
| A peer stays down | Firewall, `mesh_peer_host`, the same secrets on both sides, the clocks (NTP). |
| `doveadm mesh verify` reports differences | `docker exec dovecot doveadm mesh sync -f <user>`, then verify again. |
