#!/bin/sh
set -e

conf=/etc/dovecot/dovecot.conf
if [ ! -f "$conf" ]; then
	echo "$conf is missing: mount your configuration into /etc/dovecot" >&2
	echo "(see docker/README.md)" >&2
	exit 1
fi

# The run and state directories may be fresh volumes.
mkdir -p /var/run/dovecot /var/lib/dovecot

# The Syncthing setup made each Maildir's new/ a symlink to cur/. Dovecot
# must see a real new/ again, so refuse to start until they're replaced.
if [ -n "${MAILDIR_SYMLINK_CHECK:-}" ]; then
	links=$(find "$MAILDIR_SYMLINK_CHECK" -mindepth 2 -maxdepth 5 \
		\( -name cur -o -name tmp \) -prune -o \
		-type l -name new -print | head -n 5)
	if [ -n "$links" ]; then
		echo "Maildirs still have new -> cur symlinks from the Syncthing setup, e.g.:" >&2
		echo "$links" >&2
		echo "Replace them with directories first (see docker/README.md)." >&2
		exit 1
	fi
fi

# Fail early, with doveconf's message, if the configuration is broken.
doveconf -n > /dev/null

exec "$@"
