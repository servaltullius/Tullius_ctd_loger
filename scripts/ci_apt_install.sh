#!/usr/bin/env bash
# Installs apt packages on a CI runner without letting one stalled mirror
# eat the whole job: each attempt is bounded, network reads time out, and a
# failed attempt is retried. (2026-10-22: "Install fuzz dependencies" hung
# until the job's 20-minute limit, which cancelled the v0.2.69 release run.)
#
# Usage: scripts/ci_apt_install.sh <package>...
set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <package>..." >&2
  exit 2
fi

attempts="${CI_APT_ATTEMPTS:-3}"
attempt_seconds="${CI_APT_ATTEMPT_SECONDS:-150}"
apt_opts=(
  -o Acquire::Retries=3
  -o Acquire::http::Timeout=30
  -o Acquire::https::Timeout=30
  -o DPkg::Lock::Timeout=120
)

for ((i = 1; i <= attempts; i++)); do
  echo "apt install attempt ${i}/${attempts}: $*"
  if sudo timeout "${attempt_seconds}" apt-get "${apt_opts[@]}" update &&
     sudo DEBIAN_FRONTEND=noninteractive timeout "${attempt_seconds}" \
       apt-get "${apt_opts[@]}" install -y "$@"; then
    exit 0
  fi
  echo "apt install attempt ${i} failed or timed out" >&2
  sleep $((i * 10))
done

echo "apt install failed after ${attempts} attempts: $*" >&2
exit 1
