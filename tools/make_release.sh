#!/usr/bin/env bash
# Build the release files from this checkout:
#
#   tools/make_release.sh [--skip-tests] [--allow-dirty] [--publish]
#
# - Clean firmware build (make clean; make) with the image checks, host tests unless
#   --skip-tests, the test-song projects (make testsongs) and the preset pack (make patches).
# - dist/ gets four files:
#     PolyendTrackerOTv<NNN>.ptf          the firmware image (copy it to /Firmware)
#     OpenTracker-<VERSION>-Patches.zip   the preset pack (unzip to the card root: /Patches)
#     OpenTracker-<VERSION>-Projects.zip  the example songs (unzip to the card root: /Projects)
#     OpenTracker-<VERSION>-Themes.zip    the example theme files (themes/; unzip to the card root: /Themes)
# - --publish: a DRAFT GitHub release v<VERSION> with the four files (gh CLI, logged in);
#   review it and publish it on GitHub.
# VERSION comes from the Makefile (VERSION ?= x.y.z).
set -euo pipefail
cd "$(dirname "$0")/.."

skip_tests=0 allow_dirty=0 publish=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-tests) skip_tests=1 ;;
        --allow-dirty) allow_dirty=1 ;;
        --publish) publish=1 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

version=$(sed -n 's/^VERSION *?= *//p' Makefile)
[ -n "$version" ] || { echo "no VERSION in the Makefile" >&2; exit 1; }
if [ "$allow_dirty" = 0 ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "uncommitted changes (commit them or pass --allow-dirty)" >&2
    exit 1
fi
rev=$(git rev-parse --short HEAD)
echo "== OpenTracker $version from $rev"

make clean >/dev/null
make -j"$(nproc)"
ptf=$(ls build/*.ptf)
[ "$(basename "$ptf" | wc -c)" = 25 ] || { echo "image name is not 24 characters: $ptf" >&2; exit 1; }
if [ "$skip_tests" = 0 ]; then
    make -j"$(nproc)" host-test
fi
make testsongs >/dev/null
make patches >/dev/null

rm -rf dist
mkdir -p dist
cp "$ptf" dist/
(cd build/patches && zip -qr "../../dist/OpenTracker-$version-Patches.zip" Patches)
(cd build/testsongs && zip -qr "../../dist/OpenTracker-$version-Projects.zip" Projects)
rm -rf build/themes && mkdir -p build/themes/Themes && cp themes/*.txt build/themes/Themes/
(cd build/themes && zip -qr "../../dist/OpenTracker-$version-Themes.zip" Themes)
echo "== dist/ (from $rev)"
(cd dist && sha256sum ./*)

if [ "$publish" = 1 ]; then
    command -v gh >/dev/null || { echo "--publish needs the gh CLI" >&2; exit 1; }
    gh release create "v$version" dist/* --draft --target "$(git rev-parse HEAD)" \
        --title "OpenTracker $version" --generate-notes
    echo "== draft release v$version created: review and publish it on GitHub"
fi
