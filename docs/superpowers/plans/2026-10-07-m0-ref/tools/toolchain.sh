#!/usr/bin/env bash
# Install the pinned toolchain from deps.lock into .toolchain/ (idempotent; every download sha256-verified).
# Homebrew provides ninja, ghidra and openjdk@21; this script checks their versions but does not upgrade them.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/deps.lock"
TC="$ROOT/.toolchain"
DL="${LOMN_DOWNLOAD_CACHE:-$TC/downloads}"
mkdir -p "$TC/include" "$DL"

fetch() {  # fetch URL SHA256 DEST
  local url="$1" sha="$2" dest="$3"
  if [[ ! -f "$dest" ]] || [[ "$(shasum -a 256 "$dest" | cut -d' ' -f1)" != "$sha" ]]; then
    curl -sSfL --retry 3 --max-time 1800 -o "$dest.part" "$url"
    mv "$dest.part" "$dest"
  fi
  local got; got="$(shasum -a 256 "$dest" | cut -d' ' -f1)"
  if [[ "$got" != "$sha" ]]; then echo "sha256 mismatch for $url: $got" >&2; exit 1; fi
}

need() { command -v "$1" >/dev/null || { echo "missing $1: $2" >&2; exit 1; }; }
need ninja "brew install ninja"
need node "install Node >= $NODE_MIN_MAJOR"
[[ "$(node -p 'process.versions.node.split(".")[0]')" -ge "$NODE_MIN_MAJOR" ]] || { echo "Node >= $NODE_MIN_MAJOR required" >&2; exit 1; }
GH="$(brew --prefix ghidra 2>/dev/null || true)"
[[ -n "$GH" && "$(brew list --versions ghidra)" == *"$GHIDRA_VERSION"* ]] || { echo "need: brew install ghidra (version $GHIDRA_VERSION)" >&2; exit 1; }
[[ -d "$(brew --prefix openjdk@$JDK_MAJOR 2>/dev/null)" ]] || { echo "need: brew install openjdk@$JDK_MAJOR" >&2; exit 1; }

fetch "$DOCTEST_URL" "$DOCTEST_SHA256" "$TC/include/doctest.h"
fetch "$STB_IMAGE_WRITE_URL" "$STB_IMAGE_WRITE_SHA256" "$TC/include/stb_image_write.h"

if [[ ! -f "$TC/dawn/.ok-$DAWN_COMMIT" ]]; then
  fetch "$DAWN_MACOS_URL" "$DAWN_MACOS_SHA256" "$DL/dawn-macos.tar.gz"
  rm -rf "$TC/dawn" "$TC/dawn.tmp" && mkdir -p "$TC/dawn.tmp"
  tar xzf "$DL/dawn-macos.tar.gz" -C "$TC/dawn.tmp"
  mv "$TC/dawn.tmp/Dawn-$DAWN_COMMIT-macos-latest-Release" "$TC/dawn" && rm -rf "$TC/dawn.tmp"
  touch "$TC/dawn/.ok-$DAWN_COMMIT"
fi

if [[ ! -f "$TC/emdawnwebgpu_pkg/.ok-$DAWN_RELEASE" ]]; then
  fetch "$EMDAWNWEBGPU_URL" "$EMDAWNWEBGPU_SHA256" "$DL/emdawnwebgpu_pkg.zip"
  rm -rf "$TC/emdawnwebgpu_pkg" && unzip -q "$DL/emdawnwebgpu_pkg.zip" -d "$TC"
  touch "$TC/emdawnwebgpu_pkg/.ok-$DAWN_RELEASE"
fi

if [[ ! -f "$TC/emsdk/.ok-$EMSDK_VERSION" ]]; then
  [[ -d "$TC/emsdk/.git" ]] || git clone -q https://github.com/emscripten-core/emsdk.git "$TC/emsdk"
  git -C "$TC/emsdk" fetch -q --tags && git -C "$TC/emsdk" checkout -q "$EMSDK_VERSION"
  "$TC/emsdk/emsdk" install "$EMSDK_VERSION" >/dev/null
  "$TC/emsdk/emsdk" activate "$EMSDK_VERSION" >/dev/null
  touch "$TC/emsdk/.ok-$EMSDK_VERSION"
fi

( cd "$ROOT/web" && npm ci --no-audit --no-fund >/dev/null && npx playwright install chromium >/dev/null )
echo "toolchain ok: emsdk $EMSDK_VERSION, Dawn $DAWN_RELEASE, Playwright $PLAYWRIGHT_VERSION"
