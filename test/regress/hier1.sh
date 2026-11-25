set -eu

dir=$(dirname "$0")
out=$(nvc -a "$dir/hier1.v" -e hier1 glbl -r)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q PASSED
