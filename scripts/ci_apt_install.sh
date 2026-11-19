#!/usr/bin/env bash
# Installs apt packages on a CI runner without letting one stalled mirror
# eat the whole job: each attempt is bounded, network reads time out, and a
# failed attempt is retried. (2026-10-22: "Install fuzz dependencies" hung
# until the job's 20-minute limit, which cancelled the v0.2.69 release run.)
# On retry, falls back to an isolated official Ubuntu sources list in case
# the hosted runner's default apt sources are missing or broken (2026-11-19:
# v0.2.79-rc1 ASan runner only had packages.microsoft.com configured).
#
# Usage: scripts/ci_apt_install.sh <package>...
set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <package>..." >&2
  exit 2
fi

all_installed=1
for pkg in "$@"; do
  if ! dpkg-query -W -f='${Status}\n' "$pkg" 2>/dev/null | grep -qx 'install ok installed'; then
    all_installed=0
    break
  fi
done
if [ "${all_installed}" -eq 1 ]; then
  echo "apt packages already installed: $*"
  exit 0
fi

attempts="${CI_APT_ATTEMPTS:-3}"
attempt_seconds="${CI_APT_ATTEMPT_SECONDS:-150}"
apt_opts=(
  -o Acquire::Retries=3
  -o Acquire::http::Timeout=30
  -o Acquire::https::Timeout=30
  -o DPkg::Lock::Timeout=120
)

fallback_list=""
cleanup() {
  if [ -n "${fallback_list}" ] && [ -f "${fallback_list}" ]; then
    rm -f "${fallback_list}"
  fi
}
trap cleanup EXIT

prepare_ubuntu_fallback_sources() {
  if [ -n "${fallback_list}" ] && [ -f "${fallback_list}" ]; then
    return 0
  fi
  local codename=""
  if [ -r /etc/os-release ]; then
    codename="$(. /etc/os-release && printf '%s' "${VERSION_CODENAME:-}")"
  fi
  if [ -z "${codename}" ]; then
    return 1
  fi
  local arch
  arch="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
  local mirror="http://archive.ubuntu.com/ubuntu"
  local security="http://security.ubuntu.com/ubuntu"
  if [ "${arch}" != "amd64" ] && [ "${arch}" != "i386" ]; then
    mirror="http://ports.ubuntu.com/ubuntu-ports"
    security="http://ports.ubuntu.com/ubuntu-ports"
  fi
  fallback_list="$(mktemp /tmp/ci-apt-ubuntu-sources.XXXXXX.list)"
  cat > "${fallback_list}" <<EOF
deb ${mirror} ${codename} main universe
deb ${mirror} ${codename}-updates main universe
deb ${security} ${codename}-security main universe
EOF
  return 0
}

for ((i = 1; i <= attempts; i++)); do
  extra_opts=()
  if [ "${i}" -ge 2 ] && prepare_ubuntu_fallback_sources; then
    echo "apt install attempt ${i}/${attempts} (isolated Ubuntu mirror fallback): $*"
    extra_opts=(
      -o "Dir::Etc::sourcelist=${fallback_list}"
      -o "Dir::Etc::sourceparts=-"
    )
  else
    echo "apt install attempt ${i}/${attempts}: $*"
  fi
  if sudo timeout "${attempt_seconds}" apt-get "${apt_opts[@]}" ${extra_opts[@]+"${extra_opts[@]}"} update &&
     sudo DEBIAN_FRONTEND=noninteractive timeout "${attempt_seconds}" \
       apt-get "${apt_opts[@]}" ${extra_opts[@]+"${extra_opts[@]}"} install -y "$@"; then
    exit 0
  fi
  echo "apt install attempt ${i} failed or timed out" >&2
  sleep $((i * 10))
done

echo "apt install failed after ${attempts} attempts: $*" >&2
exit 1
