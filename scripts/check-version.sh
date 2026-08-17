#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
version=$(cat VERSION)

case "$version" in
    ''|*[!0-9.]*|.*|*..*|*.)
        echo "invalid VERSION: $version" >&2
        exit 1
        ;;
esac

test "$(printf '%s' "$version" | awk -F. '{print NF}')" -eq 3 || {
    echo "VERSION must use major.minor.patch" >&2
    exit 1
}

grep -Eq "^Version:[[:space:]]+$version$" packaging/rpm/panfan.spec || {
    echo "RPM version does not match VERSION" >&2
    exit 1
}

debian_version=$(sed -n '1s/^panfan (\([0-9][^-]*\)-[0-9][^)]*) .*$/\1/p' debian/changelog)
test "$debian_version" = "$version" || {
    echo "Debian version does not match VERSION" >&2
    exit 1
}

if test "$#" -eq 1 && test "$1" != "v$version"; then
    echo "tag $1 does not match VERSION $version" >&2
    exit 1
fi

if test -x ./panfan; then
    test "$(./panfan --version)" = "panfan $version" || {
        echo "binary version does not match VERSION" >&2
        exit 1
    }
fi
