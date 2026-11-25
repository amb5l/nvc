set -eu

dir=$(dirname "$0")
nvc --work=unisim -a "$dir/vlog56.v"
out=$(nvc -L . --work=work -a "$dir/vlog56.vhd" -e vlog56 -r 2>&1)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q PASSED
if printf '%s\n' "$out" | grep -q FAILED; then
  exit 1
fi
