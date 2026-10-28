#!/usr/bin/env python3
"""Compare a node's passdb and userdb files with its Maildirs.

Reports users that are in one file but not the other, users without a
Maildir, and Maildirs without a user. The userdb file is what the mesh goes
by: a user is replicated only between the nodes whose userdb lists it, and
a Maildir whose user isn't listed is left alone.

Usage:
  check-users.py [--passwd FILE] [--users FILE] [--vmail DIR]

Defaults fit mail_home = /home/vmail/%{user | domain}/%{user | username}.
Read-only; exit code 0 if nothing differs, 1 otherwise.
"""

import argparse
import os
import sys

SKIP_TOP = {"indexes", "sieve", "lost+found"}


def read_passwd_file(path):
    """user names (as written) from a passwd-file, in file order"""
    names = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if line == "" or line.startswith("#"):
                continue
            names.append(line.split(":", 1)[0])
    return names


def maildir_users(vmail):
    """user@domain for every <domain>/<user> directory under vmail, and
    those that don't look like a Maildir (no cur/)"""
    found, not_maildir = [], []
    for domain in sorted(os.listdir(vmail)):
        ddir = os.path.join(vmail, domain)
        if (domain in SKIP_TOP or domain.startswith(".") or
                not os.path.isdir(ddir) or os.path.islink(ddir)):
            continue
        for user in sorted(os.listdir(ddir)):
            udir = os.path.join(ddir, user)
            if user.startswith(".") or not os.path.isdir(udir):
                continue
            name = "%s@%s" % (user, domain)
            found.append(name)
            if not os.path.isdir(os.path.join(udir, "cur")):
                not_maildir.append(name)
    return found, not_maildir


def section(title, items):
    print("%s: %d" % (title, len(items)))
    for item in items:
        print("  " + item)
    print()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--passwd", default="/etc/dovecot/passwd")
    p.add_argument("--users", default="/etc/dovecot/users")
    p.add_argument("--vmail", default="/home/vmail")
    args = p.parse_args()

    passdb = read_passwd_file(args.passwd)
    userdb = read_passwd_file(args.users)
    maildirs, not_maildir = maildir_users(args.vmail)

    lower = lambda names: {n.lower(): n for n in names}
    pl, ul, ml = lower(passdb), lower(userdb), lower(maildirs)

    print("passdb %s: %d users" % (args.passwd, len(passdb)))
    print("userdb %s: %d users" % (args.users, len(userdb)))
    print("Maildirs under %s: %d\n" % (args.vmail, len(maildirs)))

    problems = 0
    dups = sorted(n for n in set(x.lower() for x in userdb)
                  if [x.lower() for x in userdb].count(n) > 1)
    checks = [
        ("Listed twice in the userdb", dups),
        ("In the passdb but not the userdb (can log in, no mail home)",
         sorted(pl[n] for n in pl if n not in ul)),
        ("In the userdb but not the passdb (gets mail, can't log in)",
         sorted(ul[n] for n in ul if n not in pl)),
        ("In the userdb without a Maildir (none yet, or elsewhere)",
         sorted(ul[n] for n in ul if n not in ml)),
        ("Maildir without a userdb entry (not replicated by the mesh)",
         sorted(ml[n] for n in ml if n not in ul)),
        ("Directory that isn't a Maildir (no cur/)", not_maildir),
        ("Upper case in the userdb (compared case-insensitively)",
         sorted(n for n in userdb if n != n.lower())),
    ]
    for title, items in checks:
        section(title, items)
        problems += len(items)
    print("differences: %d" % problems)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
