# Dovecot Mesh image. Build from the repository root:
#
#   docker build -f docker/Dockerfile -t dovecotmesh .
#
# Dovecot is installed under /usr/local/apps/dovecot with its configuration
# in /etc/dovecot, so a configuration written for that prefix works as is.
# Pigeonhole (Sieve) is built in by default.
#
# The base is Debian 13 slim. Ubuntu 26.04 works too, with the same
# package names: --build-arg BASE_IMAGE=ubuntu:26.04

ARG BASE_IMAGE=debian:trixie-slim

FROM ${BASE_IMAGE} AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential autoconf automake libtool pkg-config flex bison gettext \
	git wget ca-certificates perl python3 \
	libssl-dev zlib1g-dev libpcre2-dev libbz2-dev \
	liblz4-dev libzstd-dev libsodium-dev liblua5.4-dev libsqlite3-dev \
	libcap-dev libexpat1-dev \
	&& rm -rf /var/lib/apt/lists/*

ARG PREFIX=/usr/local/apps/dovecot
# Version and git revision reported by the binaries: the source has no
# .git here, e.g. --build-arg REVISION=$(git rev-parse --short HEAD)
ARG VERSION=0.0.0-dovecotmesh
ARG REVISION=unknown

WORKDIR /src/dovecot
COPY . .
# Normally generated from .git (update-version.sh), shipped in tarballs.
RUN printf '%s\n' '#ifndef DOVECOT_VERSION_H' '#define DOVECOT_VERSION_H' \
		"#define DOVECOT_REVISION \"${REVISION}\"" \
		'#define DOVECOT_VERSION_FULL VERSION" ("DOVECOT_REVISION")"' \
		'#define DOVECOT_BUILD_INFO DOVECOT_VERSION_FULL' \
		'#endif' > dovecot-version.h
# src/lib downloads Unicode data from dovecot.org unless src/lib/ucd/ has
# it already (see AGENTS.md).
RUN export VERSION="${VERSION}" \
	&& autoreconf -vif \
	&& ./configure --prefix="${PREFIX}" --sysconfdir=/etc \
		--localstatedir=/var --with-lua=yes --with-sqlite \
		--with-zstd --with-lz4 --with-bzlib \
		--with-libcap --with-sodium \
	&& make -j"$(nproc)" \
	&& make install-strip DESTDIR=/out

# Pigeonhole (Sieve, ManageSieve, and Sieve script replication by dsync).
# Built from PIGEONHOLE_REF, or from a source tarball
# (dovecot-pigeonhole-*.tar.gz) put in docker/ before building. Its API has
# to match this tree's, which follows Dovecot's main branch, so release
# tarballs of the 2.4 branch may not build.
ARG WITH_PIGEONHOLE=yes
ARG PIGEONHOLE_REPO=https://github.com/dovecot/pigeonhole
ARG PIGEONHOLE_REF=dd954b08870b0a70cf30f91daaa28312809ff8b0
RUN if [ "${WITH_PIGEONHOLE}" = yes ]; then \
		set -- /src/dovecot/docker/dovecot-pigeonhole-*.tar.gz \
		&& mkdir /src/pigeonhole && cd /src/pigeonhole \
		&& if [ -f "$1" ]; then \
			tar -xzf "$1" --strip-components=1; \
		else \
			git init -q && git remote add origin "${PIGEONHOLE_REPO}" \
			&& git fetch -q --depth 1 origin "${PIGEONHOLE_REF}" \
			&& git checkout -q FETCH_HEAD; \
		fi \
		&& { [ -x configure ] || ./autogen.sh; } \
		&& ./configure --prefix="${PREFIX}" --sysconfdir=/etc \
			--localstatedir=/var --with-dovecot=/src/dovecot \
		&& make -j"$(nproc)" \
		&& make install-strip DESTDIR=/out; \
	fi
# only needed for building plugins
RUN find /out -name '*.a' -delete -o -name '*.la' -delete \
	&& rm -rf "/out${PREFIX}/include"

FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates openssl \
	libssl3t64 zlib1g libpcre2-32-0 libbz2-1.0 liblz4-1 \
	libzstd1 libsodium23 liblua5.4-0 libsqlite3-0 libcap2 libexpat1 \
	&& rm -rf /var/lib/apt/lists/*

# dovecot and dovenull are Dovecot's default internal and login users.
# vmail (5000) owns the mail, as virtual_uid_maps in Postfix typically.
ARG VMAIL_UID=5000
ARG VMAIL_GID=5000
RUN groupadd -r dovecot && useradd -r -g dovecot -d /nonexistent \
		-s /usr/sbin/nologin dovecot \
	&& groupadd -r dovenull && useradd -r -g dovenull -d /nonexistent \
		-s /usr/sbin/nologin dovenull \
	&& groupadd -g "${VMAIL_GID}" vmail \
	&& useradd -u "${VMAIL_UID}" -g vmail -d /home/vmail \
		-s /usr/sbin/nologin vmail

ARG PREFIX=/usr/local/apps/dovecot
COPY --from=build /out/ /
# A configuration written for an install with only --prefix refers to files
# under ${PREFIX}/etc/dovecot (passdb and userdb files): the same directory.
RUN mkdir -p "${PREFIX}/etc" && ln -s /etc/dovecot "${PREFIX}/etc/dovecot"
COPY docker/entrypoint.sh /usr/local/bin/dovecot-entrypoint
ENV PATH="${PREFIX}/bin:${PREFIX}/sbin:${PATH}"

# IMAP, IMAPS, POP3, POP3S, LMTP, mesh HTTP, doveadm (dsync)
EXPOSE 143 993 110 995 24 8090 12345

ENTRYPOINT ["/usr/local/bin/dovecot-entrypoint"]
CMD ["dovecot", "-F"]
