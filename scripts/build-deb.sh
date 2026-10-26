#!/usr/bin/env bash
# Build the Debian package and the source tarball published beside it.
#
#   scripts/build-deb.sh [outdir]     -> outdir/xnote_<version>_<arch>.deb
#                                        outdir/xnote_<version>.tar.gz
#
# Run from a clean checkout: the tarball is the tree as found, so build
# products from an earlier configure would ship in it. The package is then
# checked with scripts/check-deb.sh.
# debian/changelog is generated from configure.ac's AC_INIT, so the version
# has one home; a checkout that tracks its own debian/changelog gets it back
# when the script exits.
# Needs the build dependencies of debian/control, dpkg-buildpackage, and a Go
# toolchain at least as new as cloud-helper/go.mod's `go` line (the build
# runs with GOTOOLCHAIN=local, so an older Go fails instead of downloading).
# GNU coreutils (`date -d`, `tar --sort`) — Linux only, which is where a .deb
# is built.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(realpath -m "${1:-$root/dist}")
version=$(sed -n 's/^AC_INIT(\[XNote\],\[\([^]]*\)\],.*/\1/p' "$root/configure.ac")
test -n "$version"
arch=$(dpkg --print-architecture)
stamp=${SOURCE_DATE_EPOCH:-$(date +%s)}
export SOURCE_DATE_EPOCH="$stamp"
mkdir -p "$out"

# The source first, before the build writes anything into the tree. The
# screenshots are not source and would add megabytes to every archive release.
tar -C "$root/.." --sort=name --mtime="@$stamp" --owner=0 --group=0 --numeric-owner \
    --exclude=.git --exclude=dist --exclude=debian/changelog --exclude=screenshots \
    --transform "s|^$(basename "$root")|xnote-$version|" \
    -czf "$out/xnote_$version.tar.gz" "$(basename "$root")"

changelog="$root/debian/changelog"
saved=$(mktemp)
if [ -f "$changelog" ]; then
    cp "$changelog" "$saved"
    trap 'cp "$saved" "$changelog"; rm -f "$saved"' EXIT
else
    trap 'rm -f "$saved" "$changelog"' EXIT
fi
cat > "$changelog" <<CHANGELOG
xnote ($version) resolute; urgency=medium

  * Release $version. The release notes:
    https://github.com/spencernorton/xnote/blob/main/CHANGELOG.md

 -- Norvi <apt@globalentry.systems>  $(date -u -d "@$stamp" '+%a, %d %b %Y %H:%M:%S +0000')
CHANGELOG
(cd "$root" && dpkg-buildpackage -us -uc -b)
deb="$out/xnote_${version}_${arch}.deb"
mv "$root/../xnote_${version}_${arch}.deb" "$deb"
rm -f "$root"/../xnote_"${version}"_*.buildinfo "$root"/../xnote_"${version}"_*.changes \
    "$root"/../xnote-dbgsym_"${version}"_*.ddeb

"$root/scripts/check-deb.sh" "$deb"
ls -l "$out"
