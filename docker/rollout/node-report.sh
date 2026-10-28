#!/bin/sh
# Read-only inventory of a node before moving it to Dovecot Mesh in
# Docker: OS, Docker, the old Dovecot install and its systemd unit, where
# its configuration is, Postfix's Dovecot settings, Syncthing, listening
# ports, the firewall, disk space, and leftovers of the Syncthing setup in
# the Maildirs. Changes nothing.
#
# Usage: node-report.sh [OLD_PREFIX] [VMAIL]
#        (defaults /usr/local/apps/dovecot and /home/vmail)

prefix=${1:-/usr/local/apps/dovecot}
vmail=${2:-/home/vmail}

section() { printf '\n===== %s\n' "$1"; }
run() { printf '$ %s\n' "$*"; sh -c "$*" 2>&1 | sed 's/^/  /'; }

section "Host"
run "hostname; hostname -f"
run "uname -m; . /etc/os-release && echo \"\$PRETTY_NAME\""
run "ip -br addr"
run "date; timedatectl show -p NTPSynchronized 2>/dev/null"

section "Docker"
run "docker --version; docker compose version"
run "systemctl is-enabled docker; systemctl is-active docker"

section "Old Dovecot"
run "$prefix/sbin/dovecot --version"
run "systemctl cat dovecot.service"
run "systemctl is-enabled dovecot; systemctl is-active dovecot"
run "ls -la $prefix/etc/dovecot /etc/dovecot"
run "readlink -f /etc/dovecot"
run "$prefix/bin/doveconf -c $prefix/etc/dovecot/dovecot.conf -n | grep -v -i -E 'password|secret|key'"

section "Postfix"
run "postconf virtual_transport smtpd_sasl_type smtpd_sasl_path mailbox_command"
run "postconf -M | grep -i -E 'dovecot|lmtp'"

section "Syncthing"
run "systemctl list-units --all --no-legend 'syncthing*'"
run "ls -d $vmail/.stfolder $vmail/.stversions $vmail/.stignore"

section "Listening ports"
run "ss -ltnp | grep -E ':(24|110|143|993|995|8090|12345|12346) '"

section "Firewall"
run "ufw status"
run "firewall-cmd --list-all"
run "iptables -S | head -40"

section "Disk"
run "df -h $vmail /var/lib/docker /root"
run "du -sh $vmail"

section "Maildirs"
run "find $vmail -maxdepth 6 -type l -name new | wc -l; echo '(new -> cur symlinks)'"
run "find $vmail -maxdepth 6 -type l -name new | head -3"
run "find $vmail -name '*sync-conflict*' | wc -l; echo '(Syncthing conflict copies)'"
run "find $vmail -name '*sync-conflict*' | head -5"
run "find $vmail -mindepth 2 -name '.st*' -prune | head -5; echo '(Syncthing files below the mail root)'"
run "ls $vmail"
