#!/usr/bin/env python3
# Multi-node Dovecot test cluster for mesh replication.
#
# Runs N independent Dovecot instances on one host. Each node has its own
# base_dir, Maildirs, indexes and TCP ports, so nodes share nothing except
# what replication sends between them.

import os
import shutil
import signal
import subprocess
import time

PREFIX = os.environ.get("DOVECOT_PREFIX", "/opt/dovecot")
# UNIX socket paths are limited to ~108 bytes, so keep the root short.
ROOT = os.environ.get("MESH_TEST_ROOT", "/var/tmp/meshtest")
DOVEADM_PASSWORD = "meshtest-secret"
USER_PASSWORD = "pass"

CONF_TEMPLATE = """\
dovecot_config_version = 0.0.0
dovecot_storage_version = 2.4.0

instance_name = mesh-{name}
# Each node needs its own hostname, as in production: dsync uses it to pick
# which side locks, and Maildir puts it in new filenames.
hostname = {name}.mesh.test
base_dir = {dir}/run
state_dir = {dir}/state
log_path = {dir}/log/dovecot.log
listen = 127.0.0.1
protocols {{
  imap = yes
  lmtp = yes
}}
ssl = no
auth_allow_cleartext = yes
auth_mechanisms = plain

mail_home = {dir}/home/%{{user}}
mail_driver = maildir
mail_path = ~/Maildir
mail_uid = vmail
mail_gid = vmail
first_valid_uid = 1000

namespace inbox {{
  inbox = yes
  separator = /
  mailbox Sent {{
    special_use = \\Sent
    auto = subscribe
  }}
  mailbox Trash {{
    special_use = \\Trash
    auto = subscribe
  }}
  mailbox Archive {{
    auto = create
  }}
}}

passdb static {{
  password = {user_password}
}}
userdb static {{
  fields {{
    uid = vmail
    gid = vmail
    home = {dir}/home/%{{user}}
  }}
}}

service imap-login {{
  inet_listener imap {{
    port = {imap_port}
  }}
  inet_listener imaps {{
    port = 0
  }}
}}
service lmtp {{
  inet_listener lmtp {{
    port = {lmtp_port}
  }}
}}
service doveadm {{
  inet_listener doveadm {{
    port = {doveadm_port}
  }}
}}
doveadm_password = {doveadm_password}
doveadm_port = {doveadm_port}

{extra}
"""


class Node:
    def __init__(self, cluster, idx):
        self.cluster = cluster
        self.idx = idx
        self.name = "node%d" % idx
        self.dir = os.path.join(cluster.root, self.name)
        base = 20000 + idx * 100
        self.imap_port = base + 43
        self.lmtp_port = base + 24
        self.doveadm_port = base + 12
        self.mesh_port = base + 15
        self.http_port = base + 80
        self.conf = os.path.join(self.dir, "dovecot.conf")
        self.extra = ""

    @property
    def doveadm_location(self):
        return "tcp:127.0.0.1:%d" % self.doveadm_port

    def write_config(self):
        for sub in ("run", "state", "log", "home"):
            os.makedirs(os.path.join(self.dir, sub), exist_ok=True)
        shutil.chown(os.path.join(self.dir, "home"), "vmail", "vmail")
        with open(self.conf, "w") as f:
            f.write(CONF_TEMPLATE.format(
                name=self.name, dir=self.dir,
                imap_port=self.imap_port, lmtp_port=self.lmtp_port,
                doveadm_port=self.doveadm_port,
                doveadm_password=DOVEADM_PASSWORD,
                user_password=USER_PASSWORD,
                extra=self.cluster.extra_config(self)))

    def master_pid(self):
        try:
            with open(os.path.join(self.dir, "run", "master.pid")) as f:
                pid = int(f.read().strip())
            os.kill(pid, 0)
            return pid
        except (OSError, ValueError):
            return None

    def is_running(self):
        return self.master_pid() is not None

    def start(self):
        if self.is_running():
            return
        # The daemonized master must not inherit our stdout/stderr, or
        # callers reading our output never see EOF.
        with open(os.path.join(self.dir, "log", "start.err"), "w") as err:
            res = subprocess.run([os.path.join(PREFIX, "sbin/dovecot"),
                                  "-c", self.conf],
                                 stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=err)
        if res.returncode != 0:
            with open(os.path.join(self.dir, "log", "start.err")) as err:
                raise RuntimeError("%s failed to start: %s" %
                                   (self.name, err.read().strip()))
        for _ in range(100):
            if os.path.exists(os.path.join(self.dir, "run", "doveadm-server")):
                return
            time.sleep(0.05)
        raise RuntimeError("%s didn't start" % self.name)

    def stop(self, hard=False):
        pid = self.master_pid()
        if pid is None:
            return
        if hard:
            # Simulate a crash: kill the master and all its children.
            os.killpg(os.getpgid(pid), signal.SIGKILL) \
                if os.getpgid(pid) == pid else os.kill(pid, signal.SIGKILL)
            subprocess.run(["pkill", "-9", "-f", "mesh-%s" % self.name],
                           check=False)
        else:
            subprocess.run([os.path.join(PREFIX, "sbin/dovecot"),
                            "-c", self.conf, "stop"], check=False)
        for _ in range(100):
            if not self.is_running():
                return
            time.sleep(0.05)

    def doveadm(self, *args, check=True, input=None, timeout=60):
        cmd = [os.path.join(PREFIX, "bin/doveadm"), "-c", self.conf] + \
            list(args)
        res = subprocess.run(cmd, capture_output=True, text=True,
                             input=input, timeout=timeout)
        if check and res.returncode != 0:
            raise RuntimeError("%s: doveadm %s failed (%d): %s" % (
                self.name, " ".join(args), res.returncode,
                res.stderr.strip()))
        return res

    def log(self):
        try:
            with open(os.path.join(self.dir, "log", "dovecot.log")) as f:
                return f.read()
        except OSError:
            return ""


class Cluster:
    def __init__(self, n, root=ROOT, extra=None):
        self.root = root
        self.extra = extra or (lambda node: "")
        self.nodes = [Node(self, i + 1) for i in range(n)]

    def extra_config(self, node):
        return self.extra(node)

    def setup(self, clean=True):
        if clean:
            self.teardown()
            # Also stop nodes left over from earlier runs with more nodes.
            if os.path.isdir(self.root):
                for name in os.listdir(self.root):
                    conf = os.path.join(self.root, name, "dovecot.conf")
                    if os.path.exists(conf):
                        subprocess.run([os.path.join(PREFIX, "sbin/dovecot"),
                                        "-c", conf, "stop"],
                                       stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
            shutil.rmtree(self.root, ignore_errors=True)
        os.makedirs(self.root, exist_ok=True)
        os.chmod(self.root, 0o755)
        for node in self.nodes:
            node.write_config()

    def start(self):
        for node in self.nodes:
            node.start()

    def teardown(self):
        for node in self.nodes:
            node.stop()

    def live_nodes(self):
        return [n for n in self.nodes if n.is_running()]

    def node(self, name_or_idx):
        if isinstance(name_or_idx, int):
            return self.nodes[name_or_idx - 1]
        for node in self.nodes:
            if node.name == name_or_idx:
                return node
        raise KeyError(name_or_idx)

    def errors(self):
        """Return error/panic/fatal lines from all node logs."""
        out = []
        for node in self.nodes:
            for line in node.log().splitlines():
                if " Error: " in line or " Panic: " in line or \
                   " Fatal: " in line:
                    out.append("%s: %s" % (node.name, line))
        return out
