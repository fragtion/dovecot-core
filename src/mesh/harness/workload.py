# Concurrent mail workload and convergence checks for the mesh test cluster.

import imaplib
import random
import re
import smtplib
import threading
import time

from cluster import USER_PASSWORD

FOLDERS = ["INBOX", "Archive", "Sent", "Trash"]
FLAGS = ["\\Seen", "\\Flagged", "\\Answered", "$label1", "$Junk"]

MSGID_RE = re.compile(rb"Message-ID:\s*<([^>]+)>", re.I)


def make_message(msgid, subject):
    return ("From: sender@example.com\r\n"
            "To: rcpt@example.com\r\n"
            "Subject: %s\r\n"
            "Message-ID: <%s>\r\n"
            "\r\n"
            "Body of %s\r\n" % (subject, msgid, msgid)).encode()


class History:
    """Thread-safe log of what the workload did and what clients saw."""

    def __init__(self):
        self.lock = threading.Lock()
        self.delivered = set()
        self.appended = set()
        self.copied = {}
        self.expunged = set()
        self.moved = set()
        # (node, user, mailbox, uidvalidity, uid) -> msgid seen by clients
        self.seen_uids = {}
        self.uid_reuse = []
        self.errors = []
        self.ops = 0
        # Optional per-(node, user) locks emulating the mesh plugin making
        # local saves wait while a dsync session for the user runs.
        self.serialize = False
        self._user_locks = {}

    def user_lock(self, node, user):
        import contextlib
        if not self.serialize:
            return contextlib.nullcontext()
        with self.lock:
            key = (node.name, user)
            if key not in self._user_locks:
                self._user_locks[key] = threading.Lock()
            return self._user_locks[key]

    def record_uids(self, node, user, mailbox, uidvalidity, mapping):
        with self.lock:
            for uid, msgid in mapping.items():
                key = (node, user, mailbox, uidvalidity, uid)
                old = self.seen_uids.get(key)
                if old is None:
                    self.seen_uids[key] = msgid
                elif old != msgid:
                    self.uid_reuse.append((key, old, msgid))

    def error(self, msg):
        with self.lock:
            self.errors.append(msg)


class Deliverer(threading.Thread):
    """Delivers new mail over LMTP to random nodes."""

    def __init__(self, cluster, users, history, count, rate, seed,
                 node_picker=None):
        super().__init__(daemon=True)
        self.cluster = cluster
        self.users = users
        self.history = history
        self.count = count
        self.rate = rate
        self.rnd = random.Random(seed)
        self.node_picker = node_picker
        self.tag = "d%d" % seed

    def run(self):
        for i in range(self.count):
            user = self.rnd.choice(self.users)
            if self.node_picker is not None:
                node = self.node_picker(user)
            else:
                node = self.rnd.choice(self.cluster.live_nodes())
            msgid = "%s-%d-%s@mesh" % (self.tag, i, user)
            try:
                with self.history.user_lock(node, user):
                    lmtp = smtplib.LMTP("127.0.0.1", node.lmtp_port,
                                        timeout=30)
                    lmtp.sendmail("sender@example.com", [user],
                                  make_message(msgid, "delivery %d" % i))
                    lmtp.quit()
                with self.history.lock:
                    self.history.delivered.add((user, msgid))
                    self.history.ops += 1
            except Exception as e:
                self.history.error("deliver %s to %s: %s" %
                                   (msgid, node.name, e))
            time.sleep(self.rnd.expovariate(self.rate))


class ImapActor(threading.Thread):
    """An IMAP client that keeps reconnecting to random nodes and changes
    flags, moves, copies, expunges and appends mail."""

    def __init__(self, cluster, user, history, duration, seed,
                 allow_expunge=True):
        super().__init__(daemon=True)
        self.cluster = cluster
        self.user = user
        self.history = history
        self.duration = duration
        self.rnd = random.Random(seed)
        self.tag = "a%d" % seed
        self.allow_expunge = allow_expunge
        self.appends = 0

    def connect(self, node):
        imap = imaplib.IMAP4("127.0.0.1", node.imap_port, timeout=30)
        imap.login(self.user, USER_PASSWORD)
        return imap

    def select(self, imap, node, mailbox):
        typ, _ = imap.select('"%s"' % mailbox)
        if typ != "OK":
            return None
        uidvalidity = int(imap.untagged_responses.get(
            "UIDVALIDITY", [b"0"])[-1])
        mapping = self.fetch_mapping(imap)
        self.history.record_uids(node.name, self.user, mailbox,
                                 uidvalidity, mapping)
        return mapping

    def fetch_mapping(self, imap):
        typ, data = imap.uid("FETCH", "1:*",
                             "(UID BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])")
        mapping = {}
        if typ != "OK":
            return mapping
        for item in data:
            if not isinstance(item, tuple):
                continue
            m_uid = re.search(rb"UID (\d+)", item[0])
            m_id = MSGID_RE.search(item[1])
            if m_uid and m_id:
                mapping[int(m_uid.group(1))] = m_id.group(1).decode()
        return mapping

    def one_session(self, node, deadline):
        imap = self.connect(node)
        try:
            for _ in range(self.rnd.randint(3, 10)):
                if time.time() > deadline:
                    break
                mailbox = self.rnd.choice(FOLDERS)
                with self.history.user_lock(node, self.user):
                    mapping = self.select(imap, node, mailbox)
                    if mapping is None:
                        continue
                    self.one_op(imap, mailbox, mapping)
                with self.history.lock:
                    self.history.ops += 1
                time.sleep(self.rnd.uniform(0.01, 0.2))
        finally:
            try:
                imap.logout()
            except Exception:
                pass

    def one_op(self, imap, mailbox, mapping):
        op = self.rnd.random()
        uids = sorted(mapping)
        if op < 0.10 or not uids:
            self.appends += 1
            msgid = "%s-%d-%s@mesh" % (self.tag, self.appends, self.user)
            target = self.rnd.choice(["Sent", "Archive", "INBOX"])
            typ, _ = imap.append('"%s"' % target, "(\\Seen)", None,
                                 make_message(msgid, "append"))
            if typ == "OK":
                with self.history.lock:
                    self.history.appended.add((self.user, msgid))
            return
        uid = self.rnd.choice(uids)
        msgid = mapping[uid]
        if op < 0.55:
            flag = self.rnd.choice(FLAGS)
            sign = self.rnd.choice(["+", "-"])
            imap.uid("STORE", str(uid), sign + "FLAGS.SILENT", "(%s)" % flag)
        elif op < 0.70:
            dest = self.rnd.choice([f for f in FOLDERS if f != mailbox])
            typ, _ = imap.uid("MOVE", str(uid), '"%s"' % dest)
            if typ == "OK":
                with self.history.lock:
                    self.history.moved.add((self.user, msgid))
        elif op < 0.80:
            dest = self.rnd.choice([f for f in FOLDERS if f != mailbox])
            typ, _ = imap.uid("COPY", str(uid), '"%s"' % dest)
            if typ == "OK":
                with self.history.lock:
                    key = (self.user, msgid)
                    self.history.copied[key] = \
                        self.history.copied.get(key, 0) + 1
        elif self.allow_expunge:
            imap.uid("STORE", str(uid), "+FLAGS.SILENT", "(\\Deleted)")
            typ, _ = imap.uid("EXPUNGE", str(uid))
            if typ == "OK":
                with self.history.lock:
                    self.history.expunged.add((self.user, msgid))

    def run(self):
        deadline = time.time() + self.duration
        while time.time() < deadline:
            nodes = self.cluster.live_nodes()
            node = self.rnd.choice(nodes)
            try:
                self.one_session(node, deadline)
            except Exception as e:
                self.history.error("imap %s on %s: %r" %
                                   (self.user, node.name, e))
                time.sleep(0.2)


def node_state(node, user):
    """Return {mailbox: [(uid, msgid, guid, flags)]} as seen by doveadm."""
    res = node.doveadm("-f", "tab", "fetch", "-u", user,
                       "mailbox uid guid flags hdr.message-id", "all")
    boxes = {}
    lines = res.stdout.splitlines()
    for line in lines[1:]:
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        mailbox, uid, guid, flags, msgid = parts[:5]
        msgid = msgid.strip().strip("<>")
        flags = " ".join(sorted(f for f in flags.split()
                                if f != "\\Recent"))
        boxes.setdefault(mailbox, []).append((int(uid), msgid, guid, flags))
    for v in boxes.values():
        v.sort()
    return boxes


def node_mailboxes(node, user):
    res = node.doveadm("mailbox", "list", "-u", user)
    return sorted(res.stdout.split())


def check_convergence(cluster, users, history, nodes=None):
    """Check that all nodes hold identical mail and that the invariants
    hold. Returns a list of problems (empty = converged and correct)."""
    problems = []
    nodes = nodes or cluster.live_nodes()
    for user in users:
        states = {n.name: node_state(n, user) for n in nodes}
        lists = {n.name: node_mailboxes(n, user) for n in nodes}
        ref_name = nodes[0].name
        for name, boxes in lists.items():
            if boxes != lists[ref_name]:
                problems.append("%s: mailbox list differs %s=%s %s=%s" % (
                    user, ref_name, lists[ref_name], name, boxes))
        ref = states[ref_name]
        for name, state in states.items():
            for mailbox in sorted(set(ref) | set(state)):
                a = ref.get(mailbox, [])
                b = state.get(mailbox, [])
                if a != b:
                    sa, sb = set(a), set(b)
                    problems.append(
                        "%s/%s differs %s vs %s: only-%s=%s only-%s=%s" % (
                            user, mailbox, ref_name, name,
                            ref_name, sorted(sa - sb)[:5],
                            name, sorted(sb - sa)[:5]))
        # Duplicates and losses, judged on the reference node. A client
        # that moves or copies the same message on two nodes before they
        # sync legitimately ends up with two copies, so messages that were
        # ever moved or copied are only checked for loss.
        with history.lock:
            created = {m for u, m in history.delivered | history.appended
                       if u == user}
            removed = {m for u, m in history.expunged | history.moved
                       if u == user}
            relocated = {m for u, m in history.moved if u == user} | \
                {m for (u, m) in history.copied if u == user}
        counts = {}
        for mailbox, msgs in ref.items():
            seen = set()
            for uid, msgid, guid, flags in msgs:
                if msgid in seen and msgid not in relocated:
                    problems.append("%s/%s: duplicate message %s" %
                                    (user, mailbox, msgid))
                seen.add(msgid)
                counts[msgid] = counts.get(msgid, 0) + 1
        for msgid in created:
            if msgid not in counts and msgid not in removed:
                problems.append("%s: message %s lost" % (user, msgid))
        for msgid, n in counts.items():
            if n > 1 and msgid not in relocated:
                problems.append("%s: message %s appears %d times" %
                                (user, msgid, n))
            if msgid not in created and not msgid.startswith("seed-"):
                problems.append("%s: unknown message %s" % (user, msgid))
    for key, old, new in history.uid_reuse:
        problems.append("UID reused on %s %s/%s uidvalidity=%s uid=%s: "
                        "%s then %s" % (key[0], key[1], key[2], key[3],
                                        key[4], old, new))
    return problems
