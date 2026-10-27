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

# Fail early, with doveconf's message, if the configuration is broken.
doveconf -n > /dev/null

exec "$@"
