#!/usr/bin/env python3
# Two-node smoke test of the Dovecot Mesh image.
#
# Starts two containers on a private Docker network, configured like a
# typical existing setup (passwd-file users, Maildir under /home/vmail,
# Sieve, TLS) plus docker/examples/mesh.conf, and checks that:
#   - the configuration loads and the nodes see each other
#   - SMTP AUTH for Postfix works on the auth inet listener
#   - LMTP deliveries reach the other node before they're confirmed
#   - IMAP moves and flag changes replicate
#   - Sieve filters deliveries, and Sieve scripts replicate
#   - users whose mail is on one node only reach the other at startup,
#     and users with different mails on both nodes end up with all of them
#   - deliveries don't hang while the other node is unreachable or down,
#     and the node catches up when it's back
#   - the entrypoint refuses Maildirs with the Syncthing setup's symlinks
#
# Usage (as root, on a Docker host):
#   python3 docker/test/smoke-test.py [--image dovecotmesh:test] [--keep]

import argparse
import imaplib
import os
import re
import shutil
import smtplib
import socket
import ssl
import subprocess
import sys
import tempfile
import time
from base64 import b64encode
from email.message import EmailMessage

HERE = os.path.dirname(os.path.abspath(__file__))
MESH_CONF = os.path.join(HERE, "..", "examples", "mesh.conf")
SUBNET = "10.231.0"
NODES = {"node1": SUBNET + ".11", "node2": SUBNET + ".12"}
DOMAIN = "example.test"
USERS = ["alice", "bob", "carol", "dave"]
VMAIL_UID = 5000

DOVECOT_CONF = """\
dovecot_config_version = 2.4.0
dovecot_storage_version = 2.4.0

auth_mechanisms = plain cram-md5
auth_allow_cleartext = no
base_dir = /var/run/dovecot/
info_log_path = /var/log/dovecot.info
log_path = /var/log/dovecot

mail_driver = maildir
mail_home = /home/vmail/%{user | domain}/%{user | username}
mail_index_path = /home/vmail/indexes/%{user | domain}/%{user | username}
mail_path = ~/
maildir_copy_with_hardlinks = yes

# the old --prefix install's path (a symlink to /etc/dovecot in the image)
passdb passwd-file {
  driver = passwd-file
  passwd_file_path = /usr/local/apps/dovecot/etc/dovecot/passwd
}
userdb passwd-file {
  driver = passwd-file
  passwd_file_path = /usr/local/apps/dovecot/etc/dovecot/users
}

protocols {
  imap = yes
  pop3 = yes
}
service auth {
  unix_listener auth-master {
    mode = 0666
  }
}
service imap-login {
  chroot = login
  executable = /usr/local/apps/dovecot/libexec/dovecot/imap-login
  user = dovecot
}
service pop3-login {
  chroot = login
  executable = /usr/local/apps/dovecot/libexec/dovecot/pop3-login
  user = dovecot
}
ssl = yes
ssl_server_cert_file = /etc/letsencrypt/live/mail.@DOMAIN@/fullchain.pem
ssl_server_key_file = /etc/letsencrypt/live/mail.@DOMAIN@/privkey.pem

protocol pop3 {
  pop3_uidl_format = %{uid | hex(8)}%{uidvalidity | hex(8)}
}
namespace inbox {
  inbox = yes
  mailbox Drafts {
    auto = subscribe
    special_use = \\Drafts
  }
  mailbox Sent {
    auto = subscribe
    special_use = \\Sent
  }
  mailbox Junk {
    auto = subscribe
    special_use = \\Junk
  }
  mailbox Trash {
    auto = subscribe
    special_use = \\Trash
  }
  mailbox Archive {
    auto = subscribe
    special_use = \\Archive
  }
}
protocol lda {
  mail_plugins {
    sieve = yes
  }
  lda_mailbox_autocreate = yes
  lda_mailbox_autosubscribe = yes
}
sieve_script personal {
  driver = file
  path = /home/vmail/sieve/%{user | domain}/%{user | username}
  active_path = /home/vmail/sieve/%{user | domain}/%{user | username}/.dovecot.sieve
}

!include mesh.conf
"""

SIEVE_SCRIPT = """\
require "fileinto";
if header :contains "subject" "[sorted]" {
  fileinto "Archive";
}
"""

problems = []


def check(cond, what):
    print("%s %s" % ("ok  " if cond else "FAIL", what), flush=True)
    if not cond:
        problems.append(what)
    return cond


def run(*args, check_rc=True, **kw):
    res = subprocess.run(args, capture_output=True, text=True, **kw)
    if check_rc and res.returncode != 0:
        raise RuntimeError("%s failed (%d): %s%s" % (
            " ".join(args), res.returncode, res.stdout, res.stderr))
    return res


def wait_for(what, func, timeout=60, interval=0.5):
    """Poll func() until it returns a true value; return it (or None)."""
    end = time.time() + timeout
    while True:
        value = func()
        if value or time.time() > end:
            if not value:
                print("     (timed out after %ds waiting: %s)" %
                      (timeout, what))
            return value
        time.sleep(interval)


class Node:
    def __init__(self, test, name):
        self.test = test
        self.name = name
        self.ip = NODES[name]
        self.container = "%s-%s" % (test.prefix, name)
        self.dir = os.path.join(test.workdir, name)
        self.vmail = os.path.join(self.dir, "vmail")

    def doveadm(self, *args, check_rc=True):
        return run("docker", "exec", self.container, "doveadm", *args,
                   check_rc=check_rc)

    def tab(self, *args):
        """doveadm -f tab output as a list of dicts."""
        lines = self.doveadm("-f", "tab", *args).stdout.splitlines()
        if not lines:
            return []
        header = lines[0].split("\t")
        return [dict(zip(header, line.split("\t"))) for line in lines[1:]]

    def message_ids(self, user, mailbox="INBOX"):
        rows = self.tab("fetch", "-u", "%s@%s" % (user, DOMAIN),
                        "hdr.message-id", "mailbox", mailbox)
        return sorted(r["hdr.message-id"].strip() for r in rows)

    def peer_up(self):
        rows = self.tab("mesh", "peers")
        return any(r.get("state") == "up" for r in rows)

    def log(self):
        out = ""
        for name in ("dovecot", "dovecot.info"):
            path = os.path.join(self.dir, "log", name)
            if os.path.exists(path):
                with open(path, errors="replace") as f:
                    out += f.read()
        return out

    def start(self):
        run("docker", "run", "-d", "--name", self.container,
            "--hostname", self.name, "--network", self.test.network,
            "--ip", self.ip,
            "-e", "MAILDIR_SYMLINK_CHECK=/home/vmail",
            "-v", "%s/etc:/etc/dovecot" % self.dir,
            "-v", "%s:/home/vmail" % self.vmail,
            "-v", "%s/lib:/var/lib/dovecot" % self.dir,
            "-v", "%s/log:/var/log" % self.dir,
            "-v", "%s/letsencrypt:/etc/letsencrypt:ro" % self.test.workdir,
            self.test.image)

    def stop(self):
        run("docker", "rm", "-f", self.container, check_rc=False)


class SmokeTest:
    def __init__(self, args):
        self.image = args.image
        self.keep = args.keep
        self.prefix = "dmsmoke"
        self.network = self.prefix + "-net"
        self.workdir = tempfile.mkdtemp(prefix="dovecotmesh-smoke-")
        self.nodes = [Node(self, name) for name in NODES]
        self.node1, self.node2 = self.nodes

    # --- setup ---

    def mesh_conf(self):
        with open(MESH_CONF) as f:
            conf = f.read()
        # Docker networks often have no IPv6, which the default (*, ::)
        # needs
        conf = "listen = *\n" + conf
        # the test connects from outside the containers
        conf = conf.replace("listen = 127.0.0.1", "listen = *")
        conf = re.sub(r"CHANGE-ME-same-on-all-nodes", "smoke-secret", conf)
        conf = conf.replace("#auth_mechanisms = PLAIN LOGIN",
                            "auth_mechanisms = PLAIN LOGIN")
        peers = "".join(
            "mesh_peer %s {\n  mesh_peer_host = %s\n"
            "  mesh_peer_doveadm_port = 12345\n"
            "  mesh_peer_http_port = 8090\n}\n" % (name, ip)
            for name, ip in NODES.items())
        conf, count = re.subn(r"(?ms)^mesh_peer .*?^}\n(?=\n# Check)",
                              peers, conf)
        assert count == 1, "mesh_peer blocks not found in mesh.conf"
        return conf

    def make_cert(self):
        live = os.path.join(self.workdir, "letsencrypt", "live",
                            "mail." + DOMAIN)
        os.makedirs(live)
        san = "DNS:mail.%s,%s" % (
            DOMAIN, ",".join("IP:" + ip for ip in NODES.values()))
        run("docker", "run", "--rm", "-v", "%s:/out" % live,
            "--entrypoint", "openssl", self.image, "req", "-x509",
            "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-subj", "/CN=mail." + DOMAIN, "-addext",
            "subjectAltName=" + san, "-keyout", "/out/privkey.pem",
            "-out", "/out/fullchain.pem")
        self.cafile = os.path.join(live, "fullchain.pem")

    def seed_mails(self, node, user, prefix, count):
        cur = os.path.join(node.vmail, DOMAIN, user, "cur")
        os.makedirs(cur, exist_ok=True)
        for sub in ("new", "tmp"):
            os.makedirs(os.path.join(os.path.dirname(cur), sub),
                        exist_ok=True)
        ids = []
        for i in range(count):
            msgid = "<%s-%d-%s@%s>" % (prefix, i, user, DOMAIN)
            data = ("From: seed@%s\nTo: %s@%s\nMessage-ID: %s\n"
                    "Subject: seeded %d\n\nbody\n" %
                    (DOMAIN, user, DOMAIN, msgid, i)).encode()
            name = "%d.M%dP1.%s,S=%d:2,S" % (time.time(), i, prefix,
                                             len(data))
            with open(os.path.join(cur, name), "wb") as f:
                f.write(data)
            ids.append(msgid)
        return ids

    def setup(self):
        run("docker", "rm", "-f", *[n.container for n in self.nodes],
            check_rc=False)
        run("docker", "network", "rm", self.network, check_rc=False)
        run("docker", "network", "create", "--subnet", SUBNET + ".0/24",
            self.network)
        self.make_cert()
        mesh_conf = self.mesh_conf()
        for node in self.nodes:
            etc = os.path.join(node.dir, "etc")
            for d in (etc, node.vmail, os.path.join(node.dir, "lib"),
                      os.path.join(node.dir, "log")):
                os.makedirs(d, exist_ok=True)
            with open(os.path.join(etc, "dovecot.conf"), "w") as f:
                f.write(DOVECOT_CONF.replace("@DOMAIN@", DOMAIN))
            with open(os.path.join(etc, "mesh.conf"), "w") as f:
                f.write(mesh_conf)
            with open(os.path.join(etc, "passwd"), "w") as f:
                for user in USERS:
                    f.write("%s@%s:{PLAIN}pw-%s::::::\n" %
                            (user, DOMAIN, user))
            with open(os.path.join(etc, "users"), "w") as f:
                for user in USERS:
                    f.write("%s@%s::%d:%d::::\n" %
                            (user, DOMAIN, VMAIL_UID, VMAIL_UID))

        # carol's mail is on node1 only; dave has different mails on both
        self.want = {
            "carol": self.seed_mails(self.node1, "carol", "n1", 5),
            "dave": (self.seed_mails(self.node1, "dave", "n1", 3) +
                     self.seed_mails(self.node2, "dave", "n2", 3)),
        }
        # bob has a Sieve script on node1
        sieve = os.path.join(self.node1.vmail, "sieve", DOMAIN, "bob")
        os.makedirs(sieve)
        with open(os.path.join(sieve, "sorting.sieve"), "w") as f:
            f.write(SIEVE_SCRIPT)
        os.symlink("sorting.sieve", os.path.join(sieve, ".dovecot.sieve"))
        for node in self.nodes:
            run("chown", "-R", "%d:%d" % (VMAIL_UID, VMAIL_UID), node.vmail)

    # --- helpers ---

    def lmtp_deliver(self, node, user, subject, msgid):
        msg = EmailMessage()
        msg["From"] = "sender@example.net"
        msg["To"] = "%s@%s" % (user, DOMAIN)
        msg["Subject"] = subject
        msg["Message-ID"] = msgid
        msg.set_content("test mail\n")
        start = time.time()
        with smtplib.LMTP(node.ip, 24, timeout=60) as lmtp:
            lmtp.send_message(msg)
        return time.time() - start

    def imap(self, node, user):
        ctx = ssl.create_default_context(cafile=self.cafile)
        conn = imaplib.IMAP4(node.ip, 143, timeout=30)
        conn.starttls(ssl_context=ctx)
        conn.login("%s@%s" % (user, DOMAIN), "pw-" + user)
        return conn

    def sasl(self, node, user, password):
        """Authenticate as Postfix does. Returns (accepted, mechanisms)."""
        with socket.create_connection((node.ip, 12346), timeout=10) as s:
            f = s.makefile("rwb")
            f.write(b"VERSION\t1\t2\nCPID\t%d\n" % os.getpid())
            f.flush()
            mechs = set()
            while True:
                args = f.readline().rstrip(b"\n").split(b"\t")
                if args[0] == b"DONE":
                    break
                if args[0] == b"MECH":
                    mechs.add(args[1].decode())
            resp = b64encode(b"\0%s@%s\0%s" % (
                user.encode(), DOMAIN.encode(), password.encode()))
            f.write(b"AUTH\t1\tPLAIN\tservice=smtp\tsecured\tresp=" +
                    resp + b"\n")
            f.flush()
            return f.readline().startswith(b"OK\t1"), mechs

    def verified(self, user, node=None):
        """Wait until doveadm mesh verify passes: right after the mails
        arrived, dsync may still be settling UIDs or flags."""
        node = node or self.node1
        return wait_for("%s verifies" % user, lambda: node.doveadm(
            "mesh", "verify", "%s@%s" % (user, DOMAIN),
            check_rc=False).returncode == 0, 60, 2)

    def all_have(self, user, ids, mailbox="INBOX"):
        want = sorted(ids)
        return all(n.message_ids(user, mailbox) == want for n in self.nodes)

    # --- tests ---

    def test_start(self):
        for node in self.nodes:
            node.start()
        ok = wait_for("both nodes see each other",
                      lambda: all(n.peer_up() for n in self.nodes), 60)
        check(ok, "nodes start and see each other")
        if not ok:
            for node in self.nodes:
                print(run("docker", "logs", node.container,
                          check_rc=False).stderr[-2000:])
                print(node.log()[-3000:])
            raise SystemExit(1)
        version = self.node1.doveadm("-f", "flow", "mesh", "status").stdout
        print("     %s" % version.strip()[:200])

    def test_sasl(self):
        ok, mechs = self.sasl(self.node1, "alice", "pw-alice")
        check(ok, "SMTP AUTH accepts the right password")
        check(not self.sasl(self.node1, "alice", "wrong")[0],
              "SMTP AUTH refuses a wrong password")
        check(mechs == {"PLAIN", "LOGIN"}, "the Postfix listener offers "
              "the mechanisms set on it: %s" % sorted(mechs))

    def test_lmtp(self):
        ids, slowest = [], 0.0
        for i, node in enumerate(self.nodes * 3):
            msgid = "<lmtp-%d@%s>" % (i, DOMAIN)
            slowest = max(slowest, self.lmtp_deliver(
                node, "alice", "delivery %d" % i, msgid))
            ids.append(msgid)
        # each delivery waited for the other node, so both have all now
        check(self.all_have("alice", ids),
              "LMTP deliveries on both nodes are on both nodes when "
              "confirmed (slowest %.2fs)" % slowest)
        self.alice_ids = ids

    def test_imap(self):
        conn = self.imap(self.node2, "alice")
        typ, data = conn.list()
        names = sorted(re.search(rb'"([^"]*)"$|(\S+)$', line).group(0)
                       .strip(b'"').decode() for line in data)
        check(names == ["Archive", "Drafts", "INBOX", "Junk", "Sent",
                        "Trash"],
              "IMAP LIST shows only the real folders: %s" % names)
        conn.select("INBOX")
        conn.uid("STORE", "1:2", "+FLAGS", "(\\Flagged)")
        conn.uid("MOVE", "3:4", "Archive")
        conn.logout()
        moved = self.alice_ids[2:4]
        ok = wait_for("move reaches node1", lambda: (
            self.node1.message_ids("alice", "Archive") == sorted(moved)), 30)
        check(ok, "IMAP MOVE on node2 replicates to node1")
        flagged = wait_for("flags reach node1", lambda: len(self.node1.tab(
            "search", "-u", "alice@" + DOMAIN, "mailbox", "INBOX",
            "FLAGGED")) == 2, 30)
        check(flagged, "IMAP flag changes replicate")

    def test_sieve(self):
        self.lmtp_deliver(self.node1, "bob", "[sorted] report",
                          "<sieve-1@%s>" % DOMAIN)
        self.lmtp_deliver(self.node1, "bob", "plain", "<sieve-2@%s>" % DOMAIN)
        check(self.node1.message_ids("bob", "Archive") ==
              ["<sieve-1@%s>" % DOMAIN] and
              self.node1.message_ids("bob") == ["<sieve-2@%s>" % DOMAIN],
              "Sieve files a matching delivery into Archive")
        check(wait_for("bob reaches node2", lambda: self.all_have(
                  "bob", ["<sieve-1@%s>" % DOMAIN], "Archive"), 30),
              "the filed mail replicates")
        script = os.path.join(self.node2.vmail, "sieve", DOMAIN, "bob",
                              "sorting.sieve")
        check(wait_for("Sieve script reaches node2",
                       lambda: os.path.exists(script), 30),
              "the Sieve script replicates")

    def test_discovery(self):
        for user in ("carol", "dave"):
            check(wait_for("%s on both nodes" % user, lambda: self.all_have(
                      user, self.want[user]), 60),
                  "%s: mail only on one node, or different on both, ends "
                  "up complete on both" % user)
            check(self.verified(user), "%s: doveadm mesh verify is ok" % user)

    def test_partition(self):
        # docker pause: node2's packets go unanswered, like a broken link
        run("docker", "pause", self.node2.container)
        try:
            took = self.lmtp_deliver(self.node1, "alice", "partition",
                                     "<partition-1@%s>" % DOMAIN)
            check(took < 5, "delivery while the other node is unreachable "
                  "returns quickly (%.2fs)" % took)
            ok = wait_for("node1 notices node2 is gone",
                          lambda: not self.node1.peer_up(), 30)
            check(ok, "node1 marks the unreachable node down")
            took = self.lmtp_deliver(self.node1, "alice", "partition",
                                     "<partition-2@%s>" % DOMAIN)
            check(took < 1, "delivery with the other node down doesn't "
                  "wait (%.2fs)" % took)
            conn = self.imap(self.node1, "alice")
            start = time.time()
            conn.append("INBOX", None, None,
                        b"Message-ID: <partition-3@example.test>\r\n"
                        b"Subject: appended\r\n\r\nbody\r\n")
            took = time.time() - start
            conn.logout()
            check(took < 1, "IMAP APPEND with the other node down doesn't "
                  "wait (%.2fs)" % took)
        finally:
            run("docker", "unpause", self.node2.container)
        ids = ["<partition-%d@%s>" % (i, DOMAIN) for i in (1, 2, 3)]
        ok = wait_for("node2 catches up", lambda: set(ids) <= set(
            self.node2.message_ids("alice")), 90)
        check(ok, "the node catches up when it's reachable again")

    def test_restart(self):
        run("docker", "stop", "-t", "30", self.node2.container)
        self.lmtp_deliver(self.node1, "alice", "while down",
                          "<restart-1@%s>" % DOMAIN)
        run("docker", "start", self.node2.container)
        ok = wait_for("node2 gets the mail after restart", lambda: (
            "<restart-1@%s>" % DOMAIN) in self.node2.message_ids("alice"), 90)
        check(ok, "a restarted node gets what it missed")
        check(self.verified("alice", self.node2),
              "alice: doveadm mesh verify is ok")

    def test_symlink_check(self):
        new = os.path.join(self.workdir, "symlinked", DOMAIN, "eve", "new")
        os.makedirs(os.path.join(os.path.dirname(new), "cur"))
        os.symlink("cur", new)
        res = run("docker", "run", "--rm", "-e",
                  "MAILDIR_SYMLINK_CHECK=/home/vmail",
                  "-v", "%s/etc:/etc/dovecot" % self.node1.dir,
                  "-v", "%s/symlinked:/home/vmail" % self.workdir,
                  "-v", "%s/letsencrypt:/etc/letsencrypt:ro" % self.workdir,
                  self.image, "true", check_rc=False)
        check(res.returncode != 0 and "new -> cur" in res.stderr,
              "the entrypoint refuses Maildirs with new -> cur symlinks")

    def test_logs(self, strict):
        """strict: before the tests that break connections on purpose."""
        if strict:
            bad, what = (re.compile(r"Panic:|Fatal:|Error: .*(mesh|dsync)"),
                         "panics, fatals or mesh errors")
        else:
            bad, what = re.compile(r"Panic:|Fatal:"), "panics or fatals"
        for node in self.nodes:
            lines = [l for l in node.log().splitlines() if bad.search(l)]
            check(not lines, "%s: no %s in the log%s" % (
                node.name, what, (": " + lines[0]) if lines else ""))

    def teardown(self):
        if self.keep:
            print("kept: containers %s, files in %s" % (
                ", ".join(n.container for n in self.nodes), self.workdir))
            return
        for node in self.nodes:
            node.stop()
        run("docker", "network", "rm", self.network, check_rc=False)
        shutil.rmtree(self.workdir, ignore_errors=True)

    def run(self):
        try:
            self.setup()
            self.test_start()
            self.test_sasl()
            self.test_lmtp()
            self.test_imap()
            self.test_sieve()
            self.test_discovery()
            self.test_logs(strict=True)
            self.test_partition()
            self.test_restart()
            self.test_symlink_check()
            self.test_logs(strict=False)
        finally:
            self.teardown()
        print("problems: %d" % len(problems))
        return 1 if problems else 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--image", default="dovecotmesh:test")
    p.add_argument("--keep", action="store_true",
                   help="leave the containers and files for inspection")
    sys.exit(SmokeTest(p.parse_args()).run())


if __name__ == "__main__":
    main()
