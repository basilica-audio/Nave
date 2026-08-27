#!/usr/bin/env bash
#
# Run pluginval (strictness 10) and auval against the macOS Universal Binary's
# x86_64 slice.
#
# Usage:
#   validate-macos-x86_64-slice.sh <vst3-bundle> <component-bundle> <pluginval-binary>
#
# Why this exists
# ---------------
# ci.yml builds a Universal Binary (-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"),
# but ctest, pluginval and auval all execute in the native arm64 host process
# on GitHub's macOS runners. The test-suite side of the Intel story is covered
# by windows-latest, which runs the whole suite on genuine x86_64 hardware -
# but tests and format validation catch different classes: ctest asserts DSP
# behaviour, pluginval asserts plugin-format conformance under hostile host
# behaviour (parameter thrashing, out-of-order calls, odd block sizes, editor
# open/close cycles), and it does so against the exact Mach-O slice an Intel
# Mac loads. That slice had no format validation at all, and nothing even
# asserted it was present. That is issue #52, the twin of
# basilica-audio/Crypta#108, where this script and its evidence come from.
#
# The mechanism, established empirically rather than assumed
# ----------------------------------------------------------
# Neither tool can simply be wrapped in `arch -x86_64`, because both go
# through host plugin-scanning and the host picks the architecture. That is
# half right, and the half that is wrong is the important half.
#
# 1. pluginval, VST3. The CLI `--validate` path validates *in-process*: a run
#    shows exactly one pluginval pid and no child. (Validator.cpp does contain a
#    ChildProcessValidator, but CommandLine.cpp uses ValidationType::inProcess;
#    the child-process path is the library API, not the CLI.) The process that
#    dlopen()s the VST3 is therefore the process we launch, and `arch -x86_64`
#    does steer it. Measured in Crypta (basilica-audio/Crypta#108), against an
#    x86_64-only bundle:
#       native arm64  -> "Unable to load VST-3 plug-in file", *** FAILED
#       arch -x86_64  -> SUCCESS
#
# 2. auval, and pluginval's AU path. Neither is steerable with `arch`, and the
#    reason is structural: an AUv2 .component is not loaded into the host's
#    address space at all. macOS hosts it out-of-process in an XPC service, and
#    ships one per architecture:
#       /System/Library/Frameworks/AudioToolbox.framework/XPCServices/
#         AUHostingServiceXPC.xpc        -> x86_64 only
#         AUHostingServiceXPC_arrow.xpc  -> arm64e only
#    The system picks the one matching the *component's* slices. The caller's
#    own architecture is irrelevant. Measured (Crypta#108) against an
#    x86_64-only component, all three of these SUCCEED:
#       auval  (native arm64) | arch -x86_64 auval | pluginval (native arm64)
#    So `arch -x86_64 auval` is not a control. It looks like one, which is worse
#    than having none - it is exactly the "appears to validate Intel while
#    silently validating arm64" failure this class asks us not to ship.
#
# The artefact is the steering wheel, not the tool
# ------------------------------------------------
# What both formats do respect is which slices the bundle actually contains. So
# this validates an x86_64-*thinned* copy: `lipo -thin x86_64` of the same
# Mach-O the Universal Binary ships, ad-hoc re-signed. lipo extracts, it does
# not recompile, so the object code under test is byte-for-byte the code that
# ships in the Universal Binary - and there is no second build to pay for.
#
# That choice also makes the gate self-verifying, which asserting on `arch`
# never could. A bundle containing only x86_64 machine code cannot be validated
# as arm64 by anything, so this step cannot silently degrade into a second arm64
# pass wearing an x86_64 label. If a future macOS made `arch -x86_64` a no-op,
# the VST3 pass would fail loudly with "Unable to load VST-3 plug-in file"
# rather than quietly going green on the wrong slice.
#
# Coverage and contract are two different things (a distinction Crypta's
# PR #106 had to draw about its own demonstration): this widens *coverage* to
# the Intel slice. It does not tighten any assertion pluginval or auval
# already made.

set -euo pipefail

VST3_SRC=${1:?usage: validate-macos-x86_64-slice.sh <vst3> <component> <pluginval>}
AU_SRC=${2:?usage: validate-macos-x86_64-slice.sh <vst3> <component> <pluginval>}
PLUGINVAL=${3:?usage: validate-macos-x86_64-slice.sh <vst3> <component> <pluginval>}

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ASSERT="${SCRIPT_DIR}/assert-pluginval-passed.sh"

STRICTNESS=${PLUGINVAL_STRICTNESS:-10}
# Under build/ deliberately: that path is already gitignored, so the thinned
# bundles and logs never show up as untracked files in a working tree, and
# the artefact upload step (build/Nave_artefacts/**) does not pick them up.
WORK_DIR=${X86_VALIDATION_WORK_DIR:-build/x86_64-validation}
LOG_DIR="${WORK_DIR}/logs"
COMPONENTS_DIR="$HOME/Library/Audio/Plug-Ins/Components"

mkdir -p "$WORK_DIR" "$LOG_DIR"

fail() {
  echo "::error::macOS x86_64 validation: ${1}"
  exit 1
}

note() { echo "==> $*"; }

# --- 0. Rosetta, and a genuinely translated child. ----------------------------
# Needed for the VST3 pass (pluginval itself runs translated) and for the AU
# pass (the x86_64 AUHostingServiceXPC runs translated). Asserted rather than
# assumed, and asserted here so a missing Rosetta names itself instead of
# surfacing as "Unable to load VST-3 plug-in file" much further down.
if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  note "Rosetta 2 is not available on this image - installing it."
  sudo softwareupdate --install-rosetta --agree-to-license
fi
arch -x86_64 /usr/bin/true 2>/dev/null \
  || fail "cannot execute x86_64 code even after attempting to install Rosetta 2."

CHILD_ARCH=$(arch -x86_64 /usr/bin/uname -m)
CHILD_TRANSLATED=$(arch -x86_64 /usr/sbin/sysctl -n sysctl.proc_translated 2>/dev/null || echo "?")
note "translated child reports: uname -m=${CHILD_ARCH}, sysctl.proc_translated=${CHILD_TRANSLATED}"
[ "$CHILD_ARCH" = "x86_64" ] \
  || fail "a child started with 'arch -x86_64' reports uname -m=${CHILD_ARCH}; it is not running x86_64 code."

# --- 1. Thin a bundle to its x86_64 slice. ------------------------------------
# Copies first: the artefacts under build/ are what gets uploaded and shipped,
# and must stay Universal. Only the copies are thinned.
thin_bundle() {
  local src="$1" dest="$2" label="$3"
  local src_bin dest_bin archs

  [ -d "$src" ] || fail "no ${label} bundle at '${src}'."

  src_bin="${src}/Contents/MacOS/$(basename "${src%.*}")"
  [ -f "$src_bin" ] || src_bin=$(find "$src/Contents/MacOS" -type f -perm -u+x | head -n 1)
  [ -f "$src_bin" ] || fail "found no executable inside '${src}/Contents/MacOS'."

  archs=$(lipo -archs "$src_bin" 2>/dev/null || true)
  note "${label}: lipo -archs $(basename "$src_bin") => ${archs:-<none>}"
  case " $archs " in
    *" x86_64 "*) ;;
    *) fail "the shipped ${label} has no x86_64 slice (found: ${archs:-none}). The macOS configure step must keep -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\"." ;;
  esac

  rm -rf "$dest"
  cp -R "$src" "$dest"
  dest_bin="${dest}/Contents/MacOS/$(basename "$src_bin")"

  # lipo extracts the slice as-is - this is the same object code the Universal
  # Binary ships, not a recompilation of it.
  lipo -thin x86_64 "$src_bin" -output "${dest_bin}.x86_64"
  mv "${dest_bin}.x86_64" "$dest_bin"

  # Thinning invalidates the linker's ad-hoc signature; without a valid one the
  # bundle will not load and this would fail for a reason unrelated to the code.
  codesign --force --sign - "$dest" >/dev/null 2>&1 \
    || fail "could not ad-hoc re-sign the thinned ${label} at '${dest}'."

  # The whole gate rests on this: a bundle with only x86_64 code in it cannot be
  # validated as arm64 by pluginval, by auval, or by the XPC hosting service.
  local thinned_archs
  thinned_archs=$(lipo -archs "$dest_bin" 2>/dev/null || true)
  [ "$thinned_archs" = "x86_64" ] \
    || fail "the thinned ${label} reports architectures '${thinned_archs}', expected exactly 'x86_64'. Refusing to run a validation that could be exercising another slice."

  note "${label}: thinned to x86_64-only at ${dest} (sha256 $(shasum -a 256 "$dest_bin" | cut -d' ' -f1))"
}

VST3_X86="${WORK_DIR}/$(basename "$VST3_SRC")"
AU_X86="${WORK_DIR}/$(basename "$AU_SRC")"

thin_bundle "$VST3_SRC" "$VST3_X86" "VST3"
thin_bundle "$AU_SRC"   "$AU_X86"   "AU component"

# --- 2. pluginval, VST3, under Rosetta. ---------------------------------------
# `arch -x86_64` is load-bearing here and only here: the VST3 is dlopen()ed into
# pluginval's own address space, so an arm64 pluginval cannot open this bundle
# at all.
run_pluginval() {
  local label="$1" target="$2" log rc=0
  shift 2

  log="${LOG_DIR}/${label}.log"
  echo "::group::pluginval [${label}] (strictness ${STRICTNESS}): ${target}"
  set +e
  "$@" "$PLUGINVAL" --strictness-level "$STRICTNESS" --validate "$target" 2>&1 | tee "$log"
  rc=${PIPESTATUS[0]}
  set -e
  echo "::endgroup::"

  # Same multi-signal gate the native pass uses: exit code alone is not trusted,
  # and "Num plugins found: 0" can never read as success.
  "$ASSERT" "$log" "$rc" "$label"
}

VST3_START=$(date +%s)
run_pluginval "macos-x86_64-vst3" "$VST3_X86" arch -x86_64
VST3_ELAPSED=$(( $(date +%s) - VST3_START ))
note "pluginval x86_64 VST3 finished in ${VST3_ELAPSED}s"

# --- 3. Install the thinned AU, then pluginval and auval against it. ----------
# Both resolve the AU through the system AudioComponent registry, so it has to
# be registered rather than read off disk. This replaces the Universal component
# the native pass installed; that pass has already run by this point, and the
# uploaded artefact comes from build/, which is untouched.
[ -d "$COMPONENTS_DIR" ] || mkdir -p "$COMPONENTS_DIR"
INSTALLED="${COMPONENTS_DIR:?}/$(basename "$AU_X86")"
rm -rf "$INSTALLED"
cp -R "$AU_X86" "$COMPONENTS_DIR/"
killall -9 AudioComponentRegistrar 2>/dev/null || true
note "installed the x86_64-only component at ${INSTALLED}"

INSTALLED_ARCHS=$(lipo -archs "${INSTALLED}/Contents/MacOS/$(basename "${AU_X86%.*}")" 2>/dev/null || true)
[ "$INSTALLED_ARCHS" = "x86_64" ] \
  || fail "the installed component reports '${INSTALLED_ARCHS}', expected 'x86_64'. auval would validate the wrong slice."

# Deliberately NOT wrapped in `arch -x86_64`. The component runs in
# AUHostingServiceXPC (x86_64) out-of-process regardless of this process's
# architecture, so wrapping it would translate the validator for no gain and
# would imply the wrapper is what selects the slice. It is not; the thinned
# bundle asserted above is.
AU_START=$(date +%s)
run_pluginval "macos-x86_64-au" "$INSTALLED"
AU_ELAPSED=$(( $(date +%s) - AU_START ))
note "pluginval x86_64 AU finished in ${AU_ELAPSED}s"

# --- 4. auval. ----------------------------------------------------------------
AUVAL_LOG="${LOG_DIR}/macos-x86_64-auval.log"
AUVAL_START=$(date +%s)
echo "::group::auval (x86_64 slice): aufx Nave Yvsv"
set +e
auval -strict -v aufx Nave Yvsv 2>&1 | tee "$AUVAL_LOG"
AUVAL_RC=${PIPESTATUS[0]}
set -e
echo "::endgroup::"
AUVAL_ELAPSED=$(( $(date +%s) - AUVAL_START ))

# auval's exit code is checked, and so is its own verdict line - a truncated or
# killed run must not read as a pass just because nothing set a non-zero status.
if [ "$AUVAL_RC" -ne 0 ]; then
  fail "auval exited with code ${AUVAL_RC} against the x86_64 slice."
fi
grep -q "AU VALIDATION SUCCEEDED" "$AUVAL_LOG" \
  || fail "auval did not print 'AU VALIDATION SUCCEEDED' against the x86_64 slice, so the run did not complete cleanly."

TOTAL=$(( VST3_ELAPSED + AU_ELAPSED + AUVAL_ELAPSED ))

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### macOS x86_64 slice - format validation"
    echo
    echo "| | |"
    echo "|---|---|"
    echo "| mechanism | \`lipo -thin x86_64\` of the shipped Mach-O, ad-hoc re-signed |"
    echo "| translated | \`uname -m=${CHILD_ARCH}\`, \`sysctl.proc_translated=${CHILD_TRANSLATED}\` |"
    echo "| pluginval VST3 (strictness ${STRICTNESS}, \`arch -x86_64\`) | ${VST3_ELAPSED}s |"
    echo "| pluginval AU (strictness ${STRICTNESS}, XPC-hosted x86_64) | ${AU_ELAPSED}s |"
    echo "| auval \`-strict\` | ${AUVAL_ELAPSED}s |"
    echo "| total | ${TOTAL}s |"
  } >> "$GITHUB_STEP_SUMMARY"
fi

echo "macOS x86_64 format validation OK: pluginval VST3 ${VST3_ELAPSED}s, pluginval AU ${AU_ELAPSED}s, auval ${AUVAL_ELAPSED}s, total ${TOTAL}s."
