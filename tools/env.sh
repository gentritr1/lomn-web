# Usage: source tools/env.sh   (bash or zsh)
# Exports every KEY=VALUE line of .env.local verbatim. Values may contain spaces and must NOT be quoted;
# the same unquoted file is read by make (-include .env.local), so one form serves both.
_lomn_env="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)/.env.local"
if [ -f "$_lomn_env" ]; then
  while IFS= read -r _lomn_line || [ -n "$_lomn_line" ]; do
    case "$_lomn_line" in
      [A-Z_]*=*) export "${_lomn_line%%=*}=${_lomn_line#*=}" ;;
    esac
  done < "$_lomn_env"
fi
unset _lomn_env _lomn_line
