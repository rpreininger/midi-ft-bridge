#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Push clips (and config.json) into the iOS app's Documents container.
#
# WHY: the clip library is far too large to ship inside the app bundle, so the
# iOS app reads config.json and ./clips from its Documents directory
# (UIFileSharingEnabled — see ios-app/MidiFtBridgeIOS/IOSAppModel.swift).
# Dragging a folder in over Finder is unreliable: Finder will not merge into an
# existing subfolder, so a re-encoded clip cannot simply be dropped in place.
# `devicectl` writes straight into the container and skips unchanged files.
#
# Usage:
#   setup/push-clips-ios.sh                     # all clips -> every connected device
#   setup/push-clips-ios.sh Fly Magneto         # just these clips (name or file)
#   setup/push-clips-ios.sh --config            # ... and config.json too
#   setup/push-clips-ios.sh --config-only       # the rundown changed, clips did not
#   setup/push-clips-ios.sh --device iFön Fly   # one device (name, UDID or id)
#   setup/push-clips-ios.sh --list              # show connected devices, copy nothing
#   setup/push-clips-ios.sh --dry-run
#   setup/push-clips-ios.sh --config-only --token <hex>   # arm a fresh device
#
# THE RUNDOWN CHANGED (notes remapped to different clips): --config-only is the
# whole job. The config push MERGES rather than overwrites -- see below -- then
# prints what changed per note and pushes any clip the new order references but
# the device does not have yet.
#
# WHY IT MERGES: config.json in this repo carries NO shutdown_token. The repo is
# public and that token is a shared secret (setup/panel-shutdown-service.sh), so
# it lives only on the devices. Copying the repo file over a device's config
# therefore deletes the token and silently disables panel shutdown -- empty
# token means "feature off" (src/config.h). This script reads the device's
# current config first and carries shutdown_token/shutdown_port across.
# Use --token to set one on a device that has none.
#
# The phone must be UNLOCKED — devicectl mounts a developer disk image first,
# and that fails with kAMDMobileImageMounterDeviceLocked on a locked device.
#
# Run from anywhere; paths are resolved relative to the repo.
# ---------------------------------------------------------------------------
set -uo pipefail

BUNDLE_ID="de.welt.midiftbridge.ios"
DEST_DIR="Documents/clips/mp4"          # matches clips_dir "./clips" + "mp4/<name>.mp4"
DEST_CONFIG="Documents/config.json"

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CLIP_DIR="$REPO/clips/mp4"
CONFIG="$REPO/config.json"

want_device=""
want_token=""
want_config=0
config_only=0
list_only=0
dry_run=0
clips=()

while [ $# -gt 0 ]; do
	case "$1" in
		--device|-d) want_device="${2:-}"; shift 2 ;;
		--config)    want_config=1; shift ;;
		--config-only) want_config=1; config_only=1; shift ;;
		--list)      list_only=1; shift ;;
		--dry-run|-n) dry_run=1; shift ;;
		--token)     want_token="${2:-}"; shift 2 ;;
		-h|--help)   awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' \
		                 "${BASH_SOURCE[0]}"; exit 0 ;;
		-*)          echo "unknown option: $1" >&2; exit 2 ;;
		*)           clips+=("$(basename "$1" .mp4)"); shift ;;
	esac
done

command -v xcrun >/dev/null || { echo "xcrun not found — install Xcode." >&2; exit 1; }

# --- which devices -------------------------------------------------------
tmp=$(mktemp -t devicectl) || exit 1
tmpdir=$(mktemp -d -t pushclips) || exit 1
trap 'rm -f "$tmp"; rm -rf "$tmpdir"' EXIT
if ! xcrun devicectl list devices --json-output "$tmp" --quiet 2>/dev/null; then
	echo "devicectl could not list devices." >&2; exit 1
fi

# id<TAB>name<TAB>model, one paired device per line
devices=()
while IFS= read -r line; do
	[ -n "$line" ] && devices+=("$line")
done < <(python3 - "$tmp" "$want_device" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
want = sys.argv[2]
for d in data.get("result", {}).get("devices", []):
    cp = d.get("connectionProperties", {})
    if cp.get("pairingState") != "paired":
        continue
    # "unavailable" = known but not reachable. Without this a phone left at
    # home is reported as "has no config, needs all 13 clips".
    if cp.get("tunnelState") == "unavailable":
        continue
    ident = d["identifier"]
    name = d.get("deviceProperties", {}).get("name", "?")
    hw = d.get("hardwareProperties", {})
    model, udid = hw.get("marketingName", "?"), hw.get("udid", "")
    if want and want not in (name, ident, udid):
        continue
    print("\t".join((ident, name, model)))
PY
)

if [ "${#devices[@]}" -eq 0 ]; then
	if [ -n "$want_device" ]; then
		echo "No paired device matching '$want_device'." >&2
	else
		echo "No paired iOS devices connected." >&2
	fi
	exit 1
fi

if [ "$list_only" -eq 1 ]; then
	printf '%s\n' "${devices[@]}" | while IFS=$'\t' read -r id name model; do
		printf '%-12s %-16s %s\n' "$name" "$model" "$id"
	done
	exit 0
fi

# --- which files ---------------------------------------------------------
sources=()
if [ "$config_only" -eq 0 ]; then
	if [ "${#clips[@]}" -eq 0 ]; then
		while IFS= read -r f; do sources+=("$f"); done < <(find "$CLIP_DIR" -maxdepth 1 -name '*.mp4' | sort)
	else
		for c in "${clips[@]}"; do
			f="$CLIP_DIR/$c.mp4"
			[ -f "$f" ] || { echo "no such clip: $f" >&2; exit 1; }
			sources+=("$f")
		done
	fi
	[ "${#sources[@]}" -gt 0 ] || { echo "no clips found in $CLIP_DIR" >&2; exit 1; }
fi
[ "$want_config" -eq 1 ] && [ ! -f "$CONFIG" ] && { echo "no config.json at $CONFIG" >&2; exit 1; }

# Read the device's own config.json into $2.
#   0 = got it   1 = device is fine but has no config yet   2 = cannot read it
# The distinction matters: on 2 we must NOT push, because we would be writing a
# config with no shutdown_token over one that had it.
pull_config() { # id, local path
	if xcrun devicectl device copy from --device "$1" \
		--domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
		--source "$DEST_CONFIG" --destination "$2" --quiet >/dev/null 2>&1; then
		return 0
	fi
	: > "$2"
	# If the container still lists, the device is reachable and simply has no
	# config yet (a fresh install); if it does not, the device is unusable.
	if xcrun devicectl device info files --device "$1" \
		--domain-type appDataContainer --domain-identifier "$BUNDLE_ID" >/dev/null 2>&1; then
		return 1
	fi
	return 2
}

# Which clips the device already holds, one basename per line.
device_clips() { # id
	xcrun devicectl device info files --device "$1" \
		--domain-type appDataContainer --domain-identifier "$BUNDLE_ID" 2>/dev/null \
		| sed -n 's|^Documents/clips/mp4/\([^ ]*\.mp4\).*|\1|p'
}

# Merge repo config with the device's device-only keys; report the note->clip
# changes. Writes the merged file to $3; prints a human summary.
merge_config() { # device-config, repo-config, out, token-override
	python3 - "$@" <<'PY_MERGE'
import json, sys
dev_path, repo_path, out_path, token = sys.argv[1:5]

repo = json.load(open(repo_path))
try:
    dev = json.load(open(dev_path))
except Exception:
    dev = {}

# Keys that live on the device only, never in the public repo.
carried = []
if token:
    repo["shutdown_token"] = token
    carried.append("shutdown_token (from --token)")
elif dev.get("shutdown_token"):
    repo["shutdown_token"] = dev["shutdown_token"]
    carried.append("shutdown_token (kept from device)")
if "shutdown_port" in dev and "shutdown_port" not in repo:
    repo["shutdown_port"] = dev["shutdown_port"]
    carried.append("shutdown_port (kept from device)")

json.dump(repo, open(out_path, "w"), indent=2, sort_keys=True)

def order(cfg):
    return {m.get("note"): m.get("clip", "") for m in cfg.get("mappings", [])}

before, after = order(dev), order(repo)
changes = []
for note in sorted(set(before) | set(after)):
    b, a = before.get(note), after.get(note)
    if b == a:
        continue
    changes.append("      note %s: %s -> %s" % (note, b or "(none)", a or "(none)"))

if not dev:
    print("      device had no config.json yet")
elif changes:
    print("\n".join(changes))
else:
    print("      rundown unchanged")

for c in carried:
    print("      %s" % c)
if not repo.get("shutdown_token"):
    print("      WARNING no shutdown_token — panel shutdown will be disabled")

# Clips this config needs, for the caller to check against the device.
with open(out_path + ".needs", "w") as f:
    for clip in sorted({m.get("clip", "") for m in repo.get("mappings", [])}):
        if clip:
            f.write(clip.split("/")[-1] + "\n")
PY_MERGE
}

# Devices usually attach over the local network rather than USB, and that link
# drops often enough mid-transfer (NWError 54, "Connection invalid", the device
# briefly vanishing) that a single attempt is not enough. Retry those; a locked
# phone is not going to fix itself, so fail fast on that one.
copy_to() { # id, local path, remote path
	[ "$dry_run" -eq 1 ] && return 0
	local err rc attempt
	for attempt in 1 2 3; do
		err=$(xcrun devicectl device copy to --device "$1" \
			--domain-type appDataContainer --domain-identifier "$BUNDLE_ID" --user mobile \
			--source "$2" --destination "$3" --quiet 2>&1)
		rc=$?
		[ $rc -eq 0 ] && { [ $attempt -gt 1 ] && printf '(retry %d) ' "$attempt"; return 0; }
		case "$err" in
			*DeviceLocked*)
				echo "LOCKED — unlock the phone and re-run." >&2
				return 1 ;;
		esac
		[ $attempt -lt 3 ] && sleep $((attempt * 3))
	done
	echo "$err" | tail -3 >&2
	return 1
}

# --- copy ----------------------------------------------------------------
okmsg=ok
[ "$dry_run" -eq 1 ] && okmsg="dry-run"
failed=0
for row in "${devices[@]}"; do
	IFS=$'\t' read -r id name model <<<"$row"
	echo "== $name ($model)"

	for f in ${sources[@]+"${sources[@]}"}; do
		base=$(basename "$f")
		printf '   %-16s ' "$base"
		if copy_to "$id" "$f" "$DEST_DIR/$base"; then echo "$okmsg"; else echo FAILED; failed=$((failed + 1)); fi
	done

	if [ "$want_config" -eq 1 ]; then
		devcfg="$tmpdir/device.json"
		merged="$tmpdir/merged.json"
		pull_config "$id" "$devcfg"; cfg_rc=$?

		if [ "$cfg_rc" -eq 2 ] && [ -z "$want_token" ]; then
			printf '   %-16s %s\n' "config.json" "SKIPPED — cannot read this device"
			echo "                    (offline or locked) — refusing to push a config"
			echo "                    without the shutdown_token it already has."
			failed=$((failed + 1))
		elif ! report=$(merge_config "$devcfg" "$CONFIG" "$merged" "$want_token"); then
			printf '   %-16s %s\n' "config.json" "FAILED (merge)"
			failed=$((failed + 1))
		else
			printf '   %-16s ' "config.json"
			if copy_to "$id" "$merged" "$DEST_CONFIG"; then echo "$okmsg"; else echo FAILED; failed=$((failed + 1)); fi
			[ "$cfg_rc" -eq 1 ] && echo "      (device had no config.json — this is a first install)"
			[ -n "$report" ] && echo "$report"

			# A reordered rundown can point at a clip this device never got.
			# Those are not optional: without them the new order has holes.
			if [ -f "$merged.needs" ]; then
				have=$(device_clips "$id")
				while IFS= read -r want; do
					[ -n "$want" ] || continue
					printf '%s\n' "$have" | grep -qxF "$want" && continue
					if [ -f "$CLIP_DIR/$want" ]; then
						printf '   %-16s ' "$want"
						if copy_to "$id" "$CLIP_DIR/$want" "$DEST_DIR/$want"; then
							echo "$okmsg (referenced by new order)"
						else
							echo FAILED; failed=$((failed + 1))
						fi
					else
						echo "   MISSING          $want — referenced by config, not in $CLIP_DIR"
						failed=$((failed + 1))
					fi
				done < "$merged.needs"
			fi
		fi
	fi
done

if [ "$failed" -gt 0 ]; then
	echo "$failed copy/copies failed." >&2
	exit 1
fi
echo "Done. Restart the engine on each device — clips are read at start."
