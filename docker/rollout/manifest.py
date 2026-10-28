#!/usr/bin/env python3
"""List the mail files of every Maildir under the mail root.

One line per mail, tab-separated: user@domain, folder (INBOX or the
Maildir++ folder name), cur or new, the file name without its flags (the
part before ":2,", which is the same on every copy), and the file name.
Compare the lists of several nodes with compare-manifests.py.

Usage:
  manifest.py [--vmail DIR] | gzip > manifest-$(hostname).txt.gz

Read-only. Expects mail_home = <vmail>/<domain>/<user> with the Maildir in
the home directory (mail_path = ~/).
"""

import argparse
import os
import sys

SKIP_TOP = {"indexes", "sieve", "lost+found"}


def folders(udir):
    """(folder name, directory) of INBOX and the Maildir++ folders"""
    yield "INBOX", udir
    for entry in sorted(os.listdir(udir)):
        path = os.path.join(udir, entry)
        if (entry.startswith(".") and entry not in (".", "..") and
                os.path.isdir(os.path.join(path, "cur"))):
            yield entry[1:], path


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--vmail", default="/home/vmail")
    args = p.parse_args()
    out = sys.stdout
    for domain in sorted(os.listdir(args.vmail)):
        ddir = os.path.join(args.vmail, domain)
        if (domain in SKIP_TOP or domain.startswith(".") or
                not os.path.isdir(ddir) or os.path.islink(ddir)):
            continue
        for user in sorted(os.listdir(ddir)):
            udir = os.path.join(ddir, user)
            if user.startswith(".") or not os.path.isdir(udir):
                continue
            name = "%s@%s" % (user, domain)
            for folder, fdir in folders(udir):
                for sub in ("cur", "new"):
                    sdir = os.path.join(fdir, sub)
                    if not os.path.isdir(sdir) or os.path.islink(sdir):
                        continue
                    for fname in sorted(os.listdir(sdir)):
                        if fname.startswith("."):
                            continue
                        base = fname.split(":2,", 1)[0]
                        out.write("%s\t%s\t%s\t%s\t%s\n" %
                                  (name, folder, sub, base, fname))


if __name__ == "__main__":
    main()
