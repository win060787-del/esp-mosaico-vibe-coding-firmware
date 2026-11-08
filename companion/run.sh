#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export PYTHONPATH="$PWD/companion"
exec "$PWD/.venv/bin/python" -m mosaico_pet.main "$@"
