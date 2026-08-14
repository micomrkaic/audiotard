#!/bin/sh
# This file is part of audiotard.  Copyright (C) 2026 Mico.  GPL-3.0-or-later.
#
# Deploy an audiotard tarball over the working tree, rebuild, self-test,
# and push the deltas to git.
#
# Usage:
#   ./deploy.sh [tarball] ["commit message"]
#
# Defaults: tarball = ./audiotard.tar.gz, message = "deploy <date>".
# Target directory: $AUDIOTARD_DIR or /home/mico/work/c_progs/audiotard.
#
# First time with a fresh tarball, get this script out of it with:
#   tar -xzf audiotard.tar.gz audiotard/deploy.sh
#   audiotard/deploy.sh audiotard.tar.gz

set -e

TAR="${1:-audiotard.tar.gz}"
MSG="${2:-deploy $(date +%F_%H%M)}"
DIR="${AUDIOTARD_DIR:-/home/mico/work/c_progs/audiotard}"

[ -f "$TAR" ] || { echo "tarball not found: $TAR" >&2; exit 1; }
TAR=$(realpath "$TAR")

echo "== unpacking $TAR over $DIR"
mkdir -p "$DIR"
# tarball contains a top-level audiotard/ directory: strip it
tar -xzf "$TAR" -C "$DIR" --strip-components=1

cd "$DIR"

echo "== building"
make
make gui
echo "== self-test"
make check > /dev/null && echo "   engine self-test passed"

echo "== git"
if [ ! -d .git ]; then
    git init -b main
    echo "   initialized new repository"
fi
# keep build products and audio out of the repo
if [ ! -f .gitignore ]; then
    cat > .gitignore << 'EOF'
audiotard
audiotard-gui
test_engine
*.o
*.wav
*.flac
/*.png
abx_session*
nohup.out
EOF
fi
git add -A
if git diff --cached --quiet; then
    echo "   nothing changed -- no commit"
else
    git commit -m "$MSG"
    echo "   committed: $MSG"
fi
if git remote get-url origin > /dev/null 2>&1; then
    BR=$(git rev-parse --abbrev-ref HEAD)
    git push origin "$BR"
    echo "   pushed to origin/$BR"
else
    cat << 'EOF'
   no 'origin' remote configured -- to push to GitHub:
     git remote add origin git@github.com:<you>/audiotard.git
     git push -u origin main
EOF
fi
echo "== done"
