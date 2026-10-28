# Rolling out Dovecot Mesh in Docker: runbook

Step-by-step instructions for replacing a host-installed Dovecot (built
with `--prefix=/usr/local/apps/dovecot`, started by a systemd unit, mail
replicated by Syncthing) with the Dovecot Mesh container on every node,
merging the nodes' existing mail in place. Written so that an operator or
an agent with SSH access to all nodes can follow it. Background:
[../README.md](../README.md) and [../../src/mesh/README.md](../../src/mesh/README.md).

Rules for whoever carries this out:

- Do the phases in order. Phase 0 and 1 change nothing that's running.
- **STOP** points: report what you found to the operator and wait for an
  answer before going on.
- Never delete or overwrite mail. If something doesn't match what this
  runbook expects, stop and ask.
- Keep a log of every command run on every node and its output.

The node table (fill in, one row per node):

| Node (hostname) | SSH | p2p address |
|---|---|---|
| `<host1>` | `ssh root@...` | `10.x.x.x` |
| ... | | |

The node's name in the mesh is its hostname (`hostname` on it; the
container runs with the host's network and hostname). Paths below:
`$DIR` = `/root/docker/mail/dovecotmesh`, `$OLD` = `/usr/local/apps/dovecot`.

The tools are in this directory; copy them to `$DIR/rollout/` on every
node (`node-report.sh`, `check-users.py`, `manifest.py`) and keep
`compare-manifests.py` and `fill-missing.sh` where you run SSH from.

## Phase 0: inventory (read-only)

On every node:

```sh
mkdir -p $DIR/rollout   # copy the tools there
sh $DIR/rollout/node-report.sh > $DIR/rollout/report-$(hostname).txt 2>&1
python3 $DIR/rollout/check-users.py --passwd $OLD/etc/dovecot/passwd \
	--users $OLD/etc/dovecot/users --vmail /home/vmail \
	> $DIR/rollout/users-$(hostname).txt
```

Use the paths the old configuration actually has (`passdb`/`userdb`
`passwd_file_path`, `mail_home`; see the report's `doveconf -n`). Check:

- The old config is what the report's `doveconf -n` shows, the mail is
  under `/home/vmail/<domain>/<user>` (`mail_home`), indexes under
  `/home/vmail/indexes/...` and Sieve scripts under `/home/vmail/sieve/...`.
- Docker and Docker Compose are installed and enabled (if not, ask the
  operator before installing them).
- The node's hostname is unique and is what it should be called in the
  mesh. Nodes reach each other's p2p addresses.
- Enough free disk for the mail the node is missing (Phase 2.3), and for
  the image (~150 MB).

**STOP**: give the operator the reports and `users-<host>.txt` of every
node: users in the passdb but not the userdb, the reverse, users without a
Maildir, and Maildirs without a user. The userdb file decides who each node
has: a user is replicated only between the nodes whose userdb lists it. Ask
which differences to fix (and how) and which to ignore; make only the
changes the operator asks for.

## Phase 1: prepare (nothing stops)

Generate two secrets once, for all nodes: `openssl rand -hex 32` twice
(`doveadm_password` and `mesh_api_key`). Keep them out of logs and chats.

On every node:

1. **Directory and image**

   ```sh
   mkdir -p $DIR/data
   cp compose.yaml $DIR/          # ../examples/compose.yaml
   echo 'DOVECOTMESH_IMAGE=ghcr.io/fragtion/dovecotmesh:<tag>' > $DIR/.env
   cd $DIR && docker compose pull
   ```

2. **Configuration in /etc/dovecot.** Copy, don't move: the old install
   keeps its files for a rollback. If `/etc/dovecot` exists, look first:
   a symlink to `$OLD/etc/dovecot` is replaced by a real directory; a
   directory with other contents is renamed to
   `/etc/dovecot.bak-<date>` (tell the operator).

   ```sh
   cp -a $OLD/etc/dovecot/. /etc/dovecot/
   ```

   From now on the users are edited in `/etc/dovecot/passwd` and
   `/etc/dovecot/users`. The configuration may keep referring to
   `$OLD/etc/dovecot/...`: in the container that path leads to
   `/etc/dovecot`.

3. **Edit `/etc/dovecot/dovecot.conf`**:
   - Delete the `unix_listener /var/spool/postfix/private/auth { ... }`
     block inside `service auth` (keep the rest of the block).
   - Make sure the global `auth_mechanisms` includes what SMTP clients
     use, typically `auth_mechanisms = plain login cram-md5`. (A listener
     block's `auth_mechanisms` has no effect.)
   - Leave `maildir_ignore_new_dir` unset.
   - Settings that point into the old install other than `$OLD/etc/dovecot`
     (`base_dir`, `state_dir`, `mail_plugin_dir`, a `$OLD/var/...` or
     `$OLD/lib/...` path): remove them, the image has its own. Files the
     configuration reads from anywhere else on the host (certificates
     outside `/etc/letsencrypt`, a `log_path` outside `/var/log`) need a
     volume in `compose.yaml` too.
   - Append `!include mesh.conf`.

4. **`/etc/dovecot/mesh.conf`**: start from
   [../examples/mesh.conf](../examples/mesh.conf), the same file on every
   node: set both secrets, and one `mesh_peer <hostname>` block per node
   with `mesh_peer_host = <p2p address>`. If the p2p network isn't
   encrypted (e.g. a VPN), enable TLS between the nodes (see the mesh
   README).

5. **Check the configuration** (must print the config and exit 0):

   ```sh
   docker run --rm -v /etc/dovecot:/etc/dovecot \
   	-v /etc/letsencrypt:/etc/letsencrypt:ro \
   	ghcr.io/fragtion/dovecotmesh:<tag> doveconf -n > /dev/null
   ```

6. **Firewall**: the nodes must reach each other's p2p address on TCP
   12345 (dsync) and 8090 (mesh HTTP). Open them for the other nodes' p2p
   addresses only, in whatever firewall the node uses (host firewall,
   cloud security lists). Test with a temporary listener, e.g. on node A
   `python3 -m http.server 8090 --bind <A's p2p address>` and from the
   others `curl -s -o /dev/null -w '%{http_code}\n' http://<A>:8090/`
   (200); the same for 12345. Stop the listeners afterwards.

**STOP**: report the configuration diffs (without secrets) and the
connectivity results, and agree on the time of the cutover with the
operator.

## Phase 2: cutover

Mail clients can't connect from step 2 until step 6 on each node, and
incoming mail waits in the senders' queues. Keep the steps short and do
the nodes together.

1. **Syncthing**: make sure it's idle and every folder up to date on all
   nodes (its GUI or CLI), then stop and disable it on all of them
   (`systemctl disable --now <syncthing unit>`).

2. **Stop the old services** on every node:

   ```sh
   postfix stop
   systemctl disable --now dovecot
   ```

3. **Make every node's copy complete.** Mail files that a node lacks,
   of users it shares with other nodes, can make the first mesh sessions
   delete those mails everywhere (see the mesh README's limitations). So
   copy them in first:

   ```sh
   # on every node
   python3 $DIR/rollout/manifest.py --vmail /home/vmail | gzip > /root/manifest.txt.gz
   # where you run SSH from: fetch each node's manifest.txt.gz and
   # /etc/dovecot/users, then
   python3 compare-manifests.py --node <host1>=m-host1.txt.gz,users-host1 \
   	--node <host2>=... --node <host3>=...
   ```

   It prints, per user, how many files each node misses, and writes
   `plan-<host>.txt` files. **STOP** if the numbers are large or
   unexpected and ask the operator. Otherwise, for every node with a
   non-empty plan:

   ```sh
   sh fill-missing.sh plan-<host2>.txt <host2> <host1>='ssh root@...' \
   	<host2>='ssh root@...' <host3>='ssh -p 220 root@...'
   ```

   Then create the manifests and compare again: it must report 0 files to
   copy.

4. **Clean up the Syncthing leftovers** on every node:

   ```sh
   # new -> cur symlinks back to directories (the container refuses to
   # start while any are left)
   find /home/vmail -type l -name new -exec sh -c \
   	'rm "$1" && mkdir -m 0700 "$1" && chown --reference="$(dirname "$1")" "$1"' _ {} \;
   find /home/vmail -name '*sync-conflict*'
   find /home/vmail -mindepth 2 -name '.st*' -prune
   ```

   **STOP** if either `find` prints anything: list it for the operator.
   (`dovecot-uidlist.sync-conflict-*` files can be deleted; conflict copies
   of mails are duplicates and can be moved out of the Maildir. Syncthing's
   `.stfolder`, `.stignore` and `.stversions` inside a user's Maildir would
   show up as mail folders.) The ones directly in `/home/vmail` are outside
   every Maildir: leave them.

5. **Postfix**: back up `/etc/postfix/main.cf`, then

   ```sh
   postconf -e 'virtual_transport = lmtp:inet:127.0.0.1:24' \
   	'smtpd_sasl_path = inet:127.0.0.1:12346'
   ```

   (`smtpd_sasl_type = dovecot` stays.) If the report shows other ways
   mail reaches the old Dovecot (`mailbox_command`, `mailbox_transport`,
   `transport_maps` or a `master.cf` service running `dovecot-lda`),
   point those at `lmtp:inet:127.0.0.1:24` too: the old `dovecot-lda`
   would bypass the mesh.

6. **Start the container** on every node:

   ```sh
   cd $DIR && docker compose up -d
   docker logs dovecot                         # nothing
   docker exec dovecot doveadm mesh peers      # all "up" within seconds
   ```

   Then `postfix start`.

7. **Watch the mesh settle**: `docker exec dovecot doveadm mesh status`
   on each node until `pending=0 running=0`, and
   `docker exec dovecot doveadm mesh users | grep -v -E ' (ok|relayed|absent) '`
   shows no errors. Warnings like `Duplicate file entry` in the log are
   expected while UIDs are reconciled.

## Phase 3: check

- `docker exec dovecot doveadm mesh verify <user>` for a few users on
  several nodes: `match` everywhere, exit code 0.
- Deliver a test mail through each node's Postfix to a user that several
  nodes have; it appears on those nodes within seconds
  (`docker exec dovecot doveadm search -u <user> mailbox INBOX all`).
- IMAP and POP3 logins and SMTP AUTH on each node.
- A user that only one node has keeps working on it, and no other node
  creates a Maildir for it.
- The web UI: `http://<p2p address>:8090/` (any user name, `mesh_api_key`
  as the password).

Afterwards: logrotate `postrotate` for `/var/log/dovecot*`:
`docker exec dovecot doveadm log reopen`; certbot deploy hook:
`docker exec dovecot doveadm reload`. Leave the old install, its unit and
Syncthing disabled but in place until the operator decides to remove them.

## Rollback

On every node:

```sh
cd $DIR && docker compose down
cp /etc/postfix/main.cf.<backup> /etc/postfix/main.cf && postfix reload
systemctl enable --now dovecot
```

Mail delivered or changed meanwhile is in the same Maildirs, which the old
Dovecot reads. To go back to Syncthing as well, the `new` directories have
to become `new -> cur` symlinks again (move their files into `cur/` first);
ask the operator before doing that.
