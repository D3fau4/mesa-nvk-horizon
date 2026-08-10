#!/usr/bin/env bash
# Fails early, and with the actual reason, when this machine cannot
# drive docker.
#
#   scripts/ci-require-docker.sh
#
# WHY THIS EXISTS. The build jobs do not run *inside* the toolchain
# image, they drive it from outside — because a step running inside has
# $DEVKITPRO set, which puts scripts/toolchain-env.sh into local mode,
# and scripts/package-horizon.sh then records `image: local` instead of
# the digest of the image that produced the binaries. That is the
# manifest's whole job. The cost of that choice is this requirement, so
# it is stated once, checked once, and explained where it fails rather
# than three thousand lines into a build log.
#
# MEASURED ON A REAL RUNNER: Forgejo's act_runner v12.7.1 runs job
# containers from ghcr.io/catthehacker/ubuntu:runner-latest as **uid
# 1001**, not root. So "the socket is mounted" and "this user may talk
# to it" are two different questions and this asks both.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu

die() { printf '%s\n' "$@" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die \
    "error: no docker CLI here." \
    "" \
    "       These jobs drive containers; they do not run inside one." \
    "       The runner image needs the docker client, and the daemon's" \
    "       socket has to reach it." \
    "" \
    "       With Forgejo's act_runner, that is config.yml:" \
    "         container:" \
    "           valid_volumes: ['/var/run/docker.sock']" \
    "           options: -v /var/run/docker.sock:/var/run/docker.sock"

if ! err=$(docker info 2>&1 >/dev/null); then
    case "$err" in
    *"permission denied"*)
        die "error: docker is here but this user may not use it." \
            "" \
            "       Running as uid $(id -u) ($(id -un 2>/dev/null || echo '?'))," \
            "       and /var/run/docker.sock is not readable by it." \
            "       act_runner runs job containers as uid 1001, so the" \
            "       socket's group has to be one that user is in — or" \
            "       the container has to run as root." \
            "" \
            "       docker said:" \
            "       $err"
        ;;
    *"Cannot connect"*|*"Is the docker daemon running"*|*"failed to connect"*)
        die "error: docker CLI is here but no daemon answers." \
            "" \
            "       The socket is not mounted into this container, or" \
            "       \$DOCKER_HOST points somewhere unreachable" \
            "       (DOCKER_HOST=${DOCKER_HOST:-<unset>})." \
            "" \
            "       docker said:" \
            "       $err"
        ;;
    *)
        die "error: docker is not usable here." "" "       docker said:" "       $err"
        ;;
    esac
fi

echo "ci-require-docker: OK — $(docker --version), uid $(id -u)"
