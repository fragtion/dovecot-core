#!/usr/bin/env python3
# Multi-node Dovecot test cluster for mesh replication.
#
# Runs N independent Dovecot instances on one host. Each node has its own
# base_dir, Maildirs, indexes and TCP ports, so nodes share nothing except
# what replication sends between them.

import os
import pwd
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
{ssl_config}
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

{passdb_first}passdb static {{
  password = {user_password}
}}
{userdb}

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
    ssl = {doveadm_ssl}
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
        # plaintext mesh HTTP for local clients (mesh-route.lua) with --tls
        self.http_local_port = base + 81
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
                passdb_first=(mesh_route_passdb(self)
                              if self.cluster.lmtp_route else ""),
                userdb=self.userdb_config(),
                ssl_config=self.cluster.ssl_config(),
                doveadm_ssl="yes" if self.cluster.tls else "no",
                extra=self.cluster.extra_config(self)))

    def userdb_config(self):
        if self.cluster.userdb_users is None:
            # can't list its users
            return ("userdb static {\n  fields {\n    uid = vmail\n"
                    "    gid = vmail\n    home = %s/home/%%{user}\n  }\n}"
                    % self.dir)
        # a passwd-file can list its users, like a typical setup
        pw = pwd.getpwnam("vmail")
        path = os.path.join(self.dir, "users")
        with open(path, "w") as f:
            for user in self.cluster.userdb_users:
                f.write("%s::%d:%d::%s/home/%s::\n" %
                        (user, pw.pw_uid, pw.pw_gid, self.dir, user))
        return ("userdb passwd-file {\n  passwd_file_path = %s\n}" % path)

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

    def mesh_admin(self, *cmd):
        """Run a command on the mesh admin socket. Returns the rows as
        lists of fields, or raises on -ERR."""
        import socket
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(120)
        sock.connect(os.path.join(self.dir, "run", "mesh"))
        sock.sendall(("VERSION\tmesh-admin\t1\t0\n" +
                      "\t".join(cmd) + "\n").encode())
        data = b""
        while True:
            lines = data.decode(errors="replace").split("\n")
            # VERSION line, +OK/-ERR, rows, empty line
            if len(lines) >= 3 and "" in lines[2:-1]:
                break
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
        sock.close()
        lines = data.decode(errors="replace").split("\n")
        reply = lines[1]
        if not reply.startswith("+OK"):
            raise RuntimeError("%s: mesh %s: %s" % (self.name, cmd, reply))
        rows = []
        for line in lines[2:]:
            if line == "":
                break
            rows.append(line.split("\t"))
        return rows

    def log(self):
        try:
            with open(os.path.join(self.dir, "log", "dovecot.log")) as f:
                return f.read()
        except OSError:
            return ""


MESH_API_KEY = "meshtest-key"


def mesh_route_passdb(node):
    """A first passdb that proxies LMTP recipients to their anchor."""
    return """lmtp_proxy = yes
passdb mesh-route {
  driver = lua
  lua_file = %s
  lua_settings {
    mesh_url = http://127.0.0.1:%d
    mesh_api_key = %s
  }
}
""" % (os.path.join(PREFIX, "share/dovecot/mesh-route.lua"),
       node.http_local_port if node.cluster.tls else node.http_port,
       MESH_API_KEY)


def mesh_config(cluster, node, topology="anchor", extra=""):
    """dovecot.conf lines that enable mesh replication on node."""
    lines = [
        "mail_plugins {",
        "  notify = yes",
        "  mesh = yes",
        "}",
        "dsync_features = relay-changes reuse-local-mails",
        "mesh_node_name = %s" % node.name,
        "mesh_topology = %s" % topology,
        "mesh_api_key = %s" % MESH_API_KEY,
        "mesh_heartbeat_interval = 500ms",
        "mesh_peer_down_timeout = 2s",
        "service mesh {",
        "  inet_listener mesh-http {",
        "    port = %d" % node.http_port,
        "    ssl = %s" % ("yes" if cluster.tls else "no"),
        "  }",
    ]
    if cluster.tls:
        lines += [
            "  inet_listener mesh-http-local {",
            "    type = http",
            "    listen = 127.0.0.1",
            "    port = %d" % node.http_local_port,
            "  }",
        ]
    lines += [
        "}",
    ]
    for peer in cluster.nodes:
        lines += [
            "mesh_peer %s {" % peer.name,
            "  mesh_peer_host = 127.0.0.1",
            "  mesh_peer_doveadm_port = %d" % peer.doveadm_port,
            "  mesh_peer_http_port = %d" % peer.http_port,
            "  mesh_peer_lmtp_port = %d" % peer.lmtp_port,
            "  mesh_peer_doveadm_ssl = %s" % ("yes" if cluster.tls else "no"),
            "  mesh_peer_http_ssl = %s" % ("yes" if cluster.tls else "no"),
            "}",
        ]
    return "\n".join(lines) + "\n" + extra


class Cluster:
    def __init__(self, n, root=ROOT, extra=None, mesh=False,
                 topology="anchor", lmtp_route=False, tls=False,
                 userdb_users=None):
        self.root = root
        # a passwd-file userdb with these users instead of a static one
        self.userdb_users = userdb_users
        self.extra = extra or (lambda node: "")
        self.mesh = mesh
        self.topology = topology
        # proxy LMTP deliveries to the user's anchor (mesh-route.lua)
        self.lmtp_route = lmtp_route
        # TLS between the nodes, for both dsync and the mesh HTTP API
        self.tls = tls
        self.nodes = [Node(self, i + 1) for i in range(n)]

    def tls_dir(self):
        return os.path.join(self.root, "tls")

    def make_certs(self):
        """A CA and one certificate for 127.0.0.1, shared by all nodes."""
        d = self.tls_dir()
        os.makedirs(d, exist_ok=True)
        run = lambda *args: subprocess.run(
            ["openssl"] + list(args), check=True, cwd=d,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-keyout", "ca.key", "-out", "ca.pem", "-subj", "/CN=mesh test CA")
        run("req", "-newkey", "rsa:2048", "-nodes", "-keyout", "node.key",
            "-out", "node.csr", "-subj", "/CN=127.0.0.1")
        with open(os.path.join(d, "ext.cnf"), "w") as f:
            f.write("subjectAltName = IP:127.0.0.1, DNS:localhost\n")
        run("x509", "-req", "-in", "node.csr", "-CA", "ca.pem",
            "-CAkey", "ca.key", "-CAcreateserial", "-days", "2",
            "-extfile", "ext.cnf", "-out", "node.pem")

    def ssl_config(self):
        if not self.tls:
            return "ssl = no"
        d = self.tls_dir()
        return ("ssl = yes\n"
                "ssl_server_cert_file = %s/node.pem\n"
                "ssl_server_key_file = %s/node.key\n"
                "ssl_client_ca_file = %s/ca.pem" % (d, d, d))

    def extra_config(self, node):
        conf = self.extra(node)
        if self.mesh:
            conf = mesh_config(self, node, self.topology) + conf
        return conf

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
        if self.tls:
            self.make_certs()
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
