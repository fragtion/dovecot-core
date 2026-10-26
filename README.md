If you are looking for the original unmodified Dovecot mailserver instead, then please go to https://github.com/dovecot/core instead.

## What is this fork?

This fork turns Dovecot into a multi-master mail cluster: any number of servers accept deliveries and IMAP changes at the same time and replicate them to each other, normally within a fraction of a second. This is **Dovecot Mesh**:

- built into Dovecot: a `mesh` service on each node plus a `mesh` mail plugin, driving Dovecot's own incremental sync engine (dsync), which transfers only what changed and recognizes renamed folders
- any number of nodes; each user has an *anchor* node that relays its changes to the others, and the anchor moves automatically when a node goes down
- deliveries are confirmed only once the mail is on a second node; optionally each user's mail is delivered on its anchor
- a web UI and `doveadm mesh` commands showing nodes, per-user sync status and recent sessions, with sync, full resync, pause and verify actions
- a verifier that compares users' mailboxes across all nodes, and can repair differences in the background
- TLS between nodes

See **[src/mesh/README.md](src/mesh/README.md)** for how it works, configuration, operation and migration from the Syncthing setup described below.

## Why the fork?

This is a modified version of dovecot, intended to work alongside https://github.com/fragtion/syncthing to allow for continuous, near-synchronous "multi-master" (Maildir-based) mail server deployments. In theory, other similar sync methods could also be used instead of Syncthing.

Dovecot's own built-in replication is limited to two nodes at max, and is not bandwidth efficient due to the lack of a delta syncing (any and all changes to the filesystem on one node must be fully re-transferred to the other node, including all contents of any renamed folders!)

Furthermore, while other third-party IMAP synchronization tools such as offlineimap, syncmaildir, etc do exist (and can also reliably keep pairs of maildirs in sync), these tools share all of the same constraints as detailed above

Syncthing is therefore used to ensure that the underlying maildir content (including dovecot-uuidlist) is kept efficiently in sync, in near-realtime

While this implementation is obviously not perfect, the relative ease of deployment, high scalability in number of server nodes, and perceived immunity to latency bottlenecks otherwise introduced by dovecot's "Director" functionality or true-synchronous NAS/NFS-based storage schemes (both of which can often impact responsiveness of the user's IMAP experience considerably), do make it a compelling alternative choice for smaller & less critical deployments

In conclusion, I determined that the current free version of dovecot simply lacks the ability to operate as an efficient and robust multi-master cluster when used standalone, and that no amount of pairing with other IMAP/Maildir synchronization utilities could reliably achieve the desired functionality either. The only solution was to modify dovecot's internal operation and maildir libraries.

Dovecot Mesh is the result of taking that conclusion one step further: instead of replicating the Maildir files underneath Dovecot, Dovecot replicates the mailboxes itself, so it always knows what changed, can resolve conflicting changes, and can show what's going on.

## Legacy: the Syncthing setup

The fork's original approach is still available, but it's off by default now. Enable it with `maildir_ignore_new_dir = yes`. Don't use it together with Dovecot Mesh.

### How does it work?

As you may know, each folder of a Maildir is comprised of three core sub-folders: 'cur/', 'new/', and 'tmp/'. Newly delivered mails are typically delivered to the 'new/' folder. When an E-mail client first becomes aware of these new mails, they are moved to the 'cur/' folder while retaining their unread status. As far as I can tell, all this does is allow the E-mail client to differentiate between UNREAD and UNSEEN mails (yes, there's a difference!).

In reality, such UNSEEN functionality is rarely needed. In our case, its presence causes serious problems when two or more IMAP clients are connected to different servers in the cluster concurrently, as each server will give that mail file a different name when it is moved from 'new/' to 'cur/', resulting in the formation of duplicates.

A calculated choice must therefore be made to sacrifice this relatively unimportant UNSEEN flag, in favour of achieving the desired replication scalability instead. In my opinion, this is a very worthy trade-off as most E-mail reading clients don't care about the RECENT flag anyway. Yes, you might no longer hear that iconic voice say "You've got mail", but since the UNREAD flag remains untouched, the vast majority of end-users would never be able to tell the difference as all newly delivered mails will still retain their unread status.

With `maildir_ignore_new_dir = yes`, dovecot will completely ignore any contents in the 'new/' folder for each Maildir folder respectively, effectively treating it as another 'tmp/' folder. This largely precludes the formation of duplicates otherwise caused by concurrent IMAP clients, but it does so at the total loss/expense of the RECENT flag, at least in the current implementation. Future revisions of this fork might re-intruduce the RECENT flag, but by means of a safer and non-conflicting workaround, such as including it in a given mail's own filename.

### Prerequisites:

All 'new/' folders should be replaced with symbolic links pointing to the 'cur/' folder for each and every Maildir folder respectively. While the 'new/' folder is no longer necessary for the operation of this dovecot fork, the symlink is necessary to retain some level of RFC complience, particularly to prevent other MTA's (such as postfix) from failing to deliver new mails.

### DISCLAIMER:

This project is still experimental, so reliability cannot be guaranteed, especially in production environments. Proceed at your own risk! This probably breaks RFC/spec, and might not scale so well! It works great for me... but YMMV - proceed at own risk.

## Coffee

Did this make you happy? I'd love to do more development like this! Please donate to show your support :)

**PayPal**: [![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate/?business=2CGE77L7BZS3S&no_recurring=0)

**BTC:** 1Q4QkBn2Rx4hxFBgHEwRJXYHJjtfusnYfy

**XMR:** 4AfeGxGR4JqDxwVGWPTZHtX5QnQ3dTzwzMWLBFvysa6FTpTbz8Juqs25XuysVfowQoSYGdMESqnvrEQ969nR9Q7mEgpA5Zm
