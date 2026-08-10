#!/usr/bin/env bash
# Creates a Forgejo release and uploads assets to it.
#
#   scripts/ci-forgejo-release.sh <tag> <notes-file> <asset>...
#
# Reads the instance and credentials from the environment the Forgejo
# runner already sets:
#
#   GITHUB_API_URL      e.g. https://git.example.com/api/v1
#   GITHUB_REPOSITORY   owner/name
#   FORGEJO_TOKEN       or GITHUB_TOKEN — the run's automatic token
#
# WHY NOT AN ACTION, AND WHY NOT jq. A third-party action has to resolve
# on whatever instance this runs on, and `actions/*` there points at
# GitHub, so the obvious spelling fetches a repository that does not
# exist. jq is not on every runner image. python3 is already a hard
# dependency of this build — scripts/split-status.py, scripts/spv-embed.py
# and horizon_ensure_python_deps all need it — so using it here adds
# nothing new to install.
#
# Idempotent enough to re-run: a release that already exists for the tag
# is reused rather than duplicated, and an asset of the same name is
# replaced.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

[ "$#" -ge 3 ] || {
    echo "usage: $0 <tag> <notes-file> <asset>..." >&2
    exit 2
}

TAG="$1"; shift
NOTES="$1"; shift

[ -f "$NOTES" ] || { echo "error: no notes file at $NOTES" >&2; exit 1; }
for a in "$@"; do
    [ -f "$a" ] || { echo "error: no asset at $a" >&2; exit 1; }
done

: "${GITHUB_API_URL:?the Forgejo runner sets this; export it to run by hand}"
: "${GITHUB_REPOSITORY:?owner/name}"

TOKEN="${FORGEJO_TOKEN:-${GITHUB_TOKEN:-}}"
[ -n "$TOKEN" ] || {
    echo "error: neither \$FORGEJO_TOKEN nor \$GITHUB_TOKEN is set." >&2
    echo "       The workflow passes the run's automatic token; when" >&2
    echo "       running by hand, use a token with write:repository." >&2
    exit 1
}
export TOKEN TAG NOTES

exec python3 - "$@" <<'PY'
import json, os, sys, urllib.error, urllib.parse, urllib.request

api   = os.environ["GITHUB_API_URL"].rstrip("/")
repo  = os.environ["GITHUB_REPOSITORY"]
tag   = os.environ["TAG"]
token = os.environ["TOKEN"]
notes = open(os.environ["NOTES"], encoding="utf-8").read()
base  = f"{api}/repos/{repo}/releases"


def call(url, data=None, method=None, ctype=None):
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"token {token}")
    req.add_header("Accept", "application/json")
    if ctype:
        req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req) as r:
        body = r.read()
    return json.loads(body) if body else {}


# Create, or adopt the one that is already there. Re-running a release
# job must not end with two releases for one tag.
payload = json.dumps({
    "tag_name": tag,
    "name": tag,
    "body": notes,
    "draft": False,
    "prerelease": tag.startswith("v0."),
}).encode()

try:
    rel = call(base, data=payload, method="POST", ctype="application/json")
    print(f"created release {tag}")
except urllib.error.HTTPError as e:
    if e.code not in (409, 422):
        print(e.read().decode(errors="replace"), file=sys.stderr)
        raise
    rel = call(f"{base}/tags/{tag}")
    print(f"release {tag} already existed; reusing it")

rid = rel["id"]
existing = {a["name"]: a["id"] for a in (rel.get("assets") or [])}

for path in sys.argv[1:]:
    name = os.path.basename(path)
    if name in existing:
        call(f"{base}/{rid}/assets/{existing[name]}", method="DELETE")
        print(f"replaced {name}")
    # Multipart by hand: the API wants a file part called "attachment",
    # and this avoids depending on a request library the runner may not
    # have.
    boundary = "----mesa-nvk-horizon-boundary"
    with open(path, "rb") as fh:
        blob = fh.read()
    body = b"".join([
        f'--{boundary}\r\n'.encode(),
        f'Content-Disposition: form-data; name="attachment"; '
        f'filename="{name}"\r\n'.encode(),
        b"Content-Type: application/octet-stream\r\n\r\n",
        blob,
        f"\r\n--{boundary}--\r\n".encode(),
    ])
    url = f"{base}/{rid}/assets?name={urllib.parse.quote(name)}"
    call(url, data=body, method="POST",
         ctype=f"multipart/form-data; boundary={boundary}")
    print(f"uploaded {name} ({len(blob)} bytes)")

print(f"release {tag}: {rel.get('html_url', '(no url reported)')}")
PY
