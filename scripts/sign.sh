#!/bin/sh
# hibiki - ad-hoc codesign the macOS app bundle.
#
# Adapted from another project's wrapper (which signed ./build/toki). Signs the
# Hibiki .app with the debug entitlements (get-task-allow) so a debugger can
# attach. Usage: scripts/sign.sh [path/to/Hibiki.app]
#
# Ad-hoc identity ("-") is fine for local development; distribution would need a
# Developer ID and notarization.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENT="${ROOT}/debug.plist"

APP="${1:-}"
if [ -z "${APP}" ]; then
  if [ -d "${ROOT}/Hibiki.app" ]; then APP="${ROOT}/Hibiki.app";
  elif [ -d "${ROOT}/build/Hibiki.app" ]; then APP="${ROOT}/build/Hibiki.app";
  else echo "usage: $0 [path/to/Hibiki.app] (no default bundle found)" >&2; exit 1;
  fi
fi

# codesign rejects Finder info / resource forks / quarantine xattrs that cp -R
# and Finder leave behind; strip them first.
find "${APP}" -name .DS_Store -delete 2>/dev/null || true
xattr -cr "${APP}" 2>/dev/null || true

if [ ! -f "${ENT}" ]; then
  echo "warning: ${ENT} missing; signing without entitlements" >&2
  codesign -s - -f "${APP}"
else
  codesign -s - --entitlements "${ENT}" -f "${APP}"
fi
echo "signed ${APP}"
