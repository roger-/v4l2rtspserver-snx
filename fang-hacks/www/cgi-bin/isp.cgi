#!/bin/sh
# isp.cgi — /proc/isp wrapper with canonical key list (NO OSD)
# Ops:
#   get  : ?op=get[&keys=k1,k2,...]    # no keys => ONLY KNOWN_KEYS that exist (order preserved)
#   set  : POST op=set&pairs=k1:v1,k2:v2,...
#   save : snapshot ALL (KNOWN_KEYS first, then extras) into one shell file
#   load : apply everything from the snapshot (read back each write)
#   keys : list KNOWN_KEYS present on the device

ROOT="/proc/isp"
SAVE_DIR="/media/mmcblk0p1/bootstrap/config"
SAVE_FILE="$SAVE_DIR/isp-presets.sh"

# -------- Canonical keys (order matters) --------
KNOWN_KEYS="
filter/brightness
filter/contrast
filter/saturation
filter/sharpness
filter/hue
filter/gamma
ae/enable
ae/exp_time
ae/gain
ae/fps
ae/min_fps
ae/outdoor_mode
ae/light-freqency-hz
awb/enable
awb/temperature
drc/enable
drc/intensity
nr3d/enable
nr3d/intensity
mirror-flip/mode
sensor/fps
sensor/gain
sensor/exposure
sensor/light-hz
"

is_cgi() { [ -n "$REQUEST_METHOD" ]; }
emit_json_header(){ is_cgi && { echo "Content-Type: application/json"; echo; }; }
urldecode(){ s=${1//+/ }; printf '%b' "${s//%/\\x}"; }
read_stdin(){ [ -n "$CONTENT_LENGTH" ] || { echo ""; return; }; dd bs=1 count="$CONTENT_LENGTH" 2>/dev/null; }

parse_kv_tokens(){ for tok in "$@"; do k=${tok%%=*}; v=${tok#*=}; [ "$k" = "$v" ] && continue; eval "ARG_${k}=\$v"; done; }
parse_qs(){
  QS="$1"; OLDIFS=$IFS; IFS='&'
  for pair in $QS; do
    k=${pair%%=*}; v=${pair#*=}
    k=$(urldecode "$k"); v=$(urldecode "$v")
    case "$k" in op) ARG_op="$v";; keys) ARG_keys="$v";; pairs) ARG_pairs="$v";; esac
  done
  IFS=$OLDIFS
}

abs_for_key(){
  key="$1"
  case "$key" in ""|*..*|/*) return 1;; esac
  bad=$(printf '%s' "$key" | tr -d 'A-Za-z0-9/_-'); [ -n "$bad" ] && return 1
  path="$ROOT/$key"
  case "$path" in "$ROOT"/*) : ;; *) return 1;; esac
  [ -f "$path" ] || return 1
  echo "$path"
  return 0
}

jesc(){ sed 's/\\/\\\\/g; s/"/\\"/g'; }

var_from_key(){ echo "ISP_$(echo "$1" | sed -e 's/_/_U_/g' -e 's/-/_D_/g' -e 's#/#__#g' | tr '[:lower:]' '[:upper:]')"; }
key_from_var(){ n=${1#ISP_}; echo "$n" | tr '[:upper:]' '[:lower:]' | sed -e 's/__/\//g' -e 's/_d_/-/g' -e 's/_u_/_/g'; }

op_keys(){
  printf '{"ok":true,"keys":['
  first=1
  for k in $KNOWN_KEYS; do
    [ -f "$ROOT/$k" ] || continue
    [ $first -eq 0 ] && printf ','
    first=0
    printf '"%s"' "$k"
  done
  printf ']}'
}

op_get(){
  KEYS="$ARG_keys"
  if [ -z "$KEYS" ]; then
    keys=""
    for k in $KNOWN_KEYS; do [ -f "$ROOT/$k" ] && keys="$keys $k"; done
    KEYS="$keys"
  else
    KEYS="$(echo "$KEYS" | tr ',' ' ')"
  fi

  printf '{"ok":true,"data":{'
  first=1
  for k in $KEYS; do
    p=$(abs_for_key "$k") || continue
    if [ -r "$p" ]; then val=$(tr -d '\n' < "$p" 2>/dev/null); else val=""; fi
    [ $first -eq 0 ] && printf ','
    first=0
    esc=$(printf '%s' "$val" | jesc)
    printf '"%s":"%s"' "$k" "$esc"
  done
  printf '}}'
}

op_set(){
  PAIRS="$ARG_pairs"
  printf '{"ok":true,"results":{'
  first=1
  OLDIFS=$IFS; IFS=','
  for kv in $PAIRS; do
    IFS=':'; set -- $kv; IFS=$OLDIFS
    key="$1"; val="$2"
    p=$(abs_for_key "$key")
    [ -n "$p" ] || { [ $first -eq 0 ] && printf ','; first=0; printf '"%s":{"ok":false,"err":"bad_key"}' "$key"; continue; }

    if [ -w "$p" ]; then echo "$val" > "$p" 2>/tmp/isp.err; rc=$?; else rc=1; echo "unwritable" >/tmp/isp.err; fi
    # read back
    if [ -r "$p" ]; then rback=$(tr -d '\n' < "$p" 2>/dev/null); else rback=""; fi
    rback_esc=$(printf '%s' "$rback" | jesc)

    [ $first -eq 0 ] && printf ','
    first=0
    if [ $rc -eq 0 ]; then
      printf '"%s":{"ok":true,"val":"%s"}' "$key" "$rback_esc"
    else
      err=$(tr -d '\n' </tmp/isp.err 2>/dev/null); err_esc=$(printf '%s' "$err" | jesc)
      printf '"%s":{"ok":false,"err":"%s","val":"%s"}' "$key" "$err_esc" "$rback_esc"
    fi
  done
  printf '}}'
}

op_save(){
  mkdir -p "$SAVE_DIR" 2>/dev/null
  tmp="$SAVE_FILE.tmp.$$"
  : > "$tmp" || { printf '{"ok":false,"err":"save_open"}'; return 1; }

  total=0
  for k in $KNOWN_KEYS; do
    p="$ROOT/$k"; [ -r "$p" ] || continue
    val=$(tr -d '\n' < "$p" 2>/dev/null); var=$(var_from_key "$k")
    esc=$(printf '%s' "$val" | jesc)
    echo "$var=\"$esc\"" >> "$tmp"; total=$((total+1))
  done
  for x in $(find "$ROOT" -type f 2>/dev/null | sed "s:^$ROOT/::"); do
    echo " $KNOWN_KEYS " | grep -q " $x " && continue
    p="$ROOT/$x"; [ -r "$p" ] || continue
    val=$(tr -d '\n' < "$p" 2>/dev/null); var=$(var_from_key "$x")
    esc=$(printf '%s' "$val" | jesc)
    echo "$var=\"$esc\"" >> "$tmp"; total=$((total+1))
  done

  mv "$tmp" "$SAVE_FILE" 2>/dev/null || { printf '{"ok":false,"err":"save_mv"}'; return 1; }
  printf '{"ok":true,"file":"%s","count":%s}' "$SAVE_FILE" "$total"
}

op_load(){
  [ -f "$SAVE_FILE" ] || { printf '{"ok":false,"err":"no_save_file"}'; return 1; }
  printf '{"ok":true,"file":"%s","applied":true,"results":{' "$SAVE_FILE"
  first=1
  while IFS= read -r line; do
    case "$line" in \#*|"") continue;; esac
    var=${line%%=*}
    val=$(printf '%s' "$line" | sed 's/^[^=]*="//; s/"$//')
    key=$(key_from_var "$var")
    p=$(abs_for_key "$key") || continue
    if [ -w "$p" ]; then echo "$val" > "$p" 2>/tmp/isp.err; rc=$?; else rc=1; echo "unwritable" >/tmp/isp.err; fi
    if [ -r "$p" ]; then rback=$(tr -d '\n' < "$p" 2>/dev/null); else rback=""; fi
    rback_esc=$(printf '%s' "$rback" | jesc)

    [ $first -eq 0 ] && printf ','
    first=0
    if [ $rc -eq 0 ]; then
      printf '"%s":{"ok":true,"val":"%s"}' "$key" "$rback_esc"
    else
      err=$(tr -d '\n' </tmp/isp.err 2>/dev/null); err_esc=$(printf '%s' "$err" | jesc)
      printf '"%s":{"ok":false,"err":"%s","val":"%s"}' "$key" "$err_esc" "$rback_esc"
    fi
  done < "$SAVE_FILE"
  printf '}}'
}

run_cgi(){
  if [ "$REQUEST_METHOD" = "GET" ]; then parse_qs "$QUERY_STRING"; else BODY=$(read_stdin); parse_qs "$BODY"; fi
  emit_json_header
  case "$ARG_op" in
    ""|get)  op_get ;;
    set)     op_set ;;
    save)    op_save ;;
    load)    op_load ;;
    keys)    op_keys ;;
    *)       printf '{"ok":false,"err":"bad_op"}'; return 1 ;;
  esac
}

run_cli(){
  OP="$1"; shift 2>/dev/null
  parse_kv_tokens "$@"
  emit_json_header
  case "$OP" in
    ""|get)  op_get ;;
    set)     op_set ;;
    save)    op_save ;;
    load)    op_load ;;
    keys)    op_keys ;;
    *)       printf '{"ok":false,"err":"bad_op"}'; exit 1 ;;
  esac
}

if is_cgi; then run_cgi || exit 1; else run_cli "$@" || exit 1; fi
exit 0
