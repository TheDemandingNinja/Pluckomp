#!/bin/sh
# Clone the upstream projects this port reads from into refs/ (ignored by git, read-only).
# Run from the repository root: sh tools/fetch_refs.sh
set -e
mkdir -p refs
clone() {
    if [ -d "refs/$1" ]; then
        echo "refs/$1 exists, skipping"
    else
        git clone --depth 1 "$2" "refs/$1"
    fi
}
clone totk-pc-recomp https://github.com/valentderah/totk-pc-recomp.git
clone LASRecomp      https://github.com/peiikai/LASRecomp.git
clone hactool        https://github.com/SciresM/hactool.git
