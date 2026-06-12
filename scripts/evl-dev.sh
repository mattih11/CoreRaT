#!/usr/bin/env bash
# scripts/evl-dev.sh
#
# Boot the RaTOS EVL kernel in QEMU and develop against it locally.
#
# Two complementary build paths:
#
#   --build [preset]   Build inside the QEMU guest (default preset: default).
#                      Requires nothing on the host beyond ssh/rsync/qemu.
#                      Source is rsynced into the guest and built there.
#
#   --cross [preset]   Cross-compile on the HOST using the RaTOS SDK
#                      (default preset: evl-cross), then rsync binaries to
#                      the guest.  Much faster for iterative development.
#                      Requires the SDK to be installed (see --sdk-dir).
#
# Action flags execute in order: cross -> build -> test -> run -> shell.
# With no action flags the default is --cross evl-cross --test (mirrors CI).
#
# Usage:
#   # Default - cross-compile + run tests in QEMU (mirrors CI):
#   scripts/evl-dev.sh
#
#   # Interactive shell only (nothing built):
#   scripts/evl-dev.sh --shell
#
#   # Cross-compile EVL on host, deploy binaries, open shell:
#   scripts/evl-dev.sh --cross --shell
#
#   # Cross-compile, deploy, run tests:
#   scripts/evl-dev.sh --cross --test
#
#   # Cross-compile specific preset, then run a binary:
#   scripts/evl-dev.sh --cross evl-cross --run /root/corerat/test/ping_node
#
#   # Build inside guest with EVL preset, then run tests:
#   scripts/evl-dev.sh --build evl --test
#
#   # Run multiple binaries already deployed:
#   scripts/evl-dev.sh --run /root/corerat/test/ping_node \
#                      --run /root/corerat/test/pong_node
#
#   # Use pre-downloaded artifacts:
#   scripts/evl-dev.sh [flags] --ext4 path/to/ratos.ext4 \
#                               --kernel path/to/vmlinuz \
#                               --initrd path/to/initrd.img
#
#   # Download from a specific run or release tag:
#   RATOS_RUN_ID=12345678 scripts/evl-dev.sh [flags]
#   RATOS_RELEASE_TAG=v1.0.0 scripts/evl-dev.sh [flags]
#
# SDK setup (for --cross):
#   The SDK is auto-downloaded and extracted on first use.
#   Default cache: .evl-cache/sdk  (gitignored; override via EVL_SDK_DIR in .corerat.env.local)
#   Requires 'gh' to be authenticated for auto-download.
#
# Artifact caching:
#   ext4 + vmlinuz + initrd are cached in .evl-cache/ (gitignored) and
#   reused when the resolved run ID or release tag has not changed.
#   Delete .evl-cache/ to force a fresh download.
#
# Prerequisites:
#   qemu-system-x86_64  (apt: qemu-system-x86)
#   rsync, ssh, ssh-keygen
#   gh                  (for auto-download; must be authenticated)
#
# The script modifies a COPY of the ext4 image; the cache is never mutated.

set -euo pipefail

# ---------------------------------------------------------------------------
# Load .corerat.env as defaults (exported variables take precedence)
# ---------------------------------------------------------------------------
_load_env() {
    local envfile="$1"
    [[ -f "$envfile" ]] || return 0
    local line key val
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ "$line" =~ ^[[:space:]]*(#|$) ]] && continue
        key="${line%%=*}"
        val="${line#*=}"
        key="${key//[[:space:]]/}"
        [[ -z "$key" ]] && continue
        [[ -v "$key" ]] || printf -v "$key" '%s' "$val"
    done < "$envfile"
}

_EARLY_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_load_env "${_EARLY_SCRIPT_DIR}/../.corerat.env"
_load_env "${_EARLY_SCRIPT_DIR}/../.corerat.env.local"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
EXT4_PATH=""
KERNEL_PATH=""
INITRD_PATH=""
RATOS_RELEASE_REPO="${RATOS_RELEASE_REPO:-}"
RATOS_RUN_ID="${RATOS_RUN_ID:-}"
RATOS_RELEASE_TAG="${RATOS_RELEASE_TAG:-}"
QEMU_MEMORY="${QEMU_MEMORY:-2G}"
QEMU_CPUS="${QEMU_CPUS:-4}"
SSH_PORT="${SSH_PORT:-22222}"
EVL_SDK_DIR="${EVL_SDK_DIR:-.evl-cache/sdk}"

DO_CROSS=""          # preset for host cross-compile (empty = skip)
DO_BUILD=""          # preset for in-guest build (empty = skip)
DO_TEST=0
RUN_CMDS=()
DO_SHELL=0
DO_CONSOLE=0         # attach serial console to stdio (no SSH)

WORK_DIR="$(mktemp -d /tmp/corerat-evl-XXXXXX)"
CLEANUP_WORK_DIR=1

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
usage() {
    grep '^#' "$0" | sed 's/^# \?//' | tail -n +2 | head -n 45
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ext4)    EXT4_PATH="$(realpath "$2")";    shift 2 ;;
        --kernel)  KERNEL_PATH="$(realpath "$2")";  shift 2 ;;
        --initrd)  INITRD_PATH="$(realpath "$2")";  shift 2 ;;
        --run-id)  RATOS_RUN_ID="$2";               shift 2 ;;
        --tag)     RATOS_RELEASE_TAG="$2";          shift 2 ;;
        --sdk-dir) EVL_SDK_DIR="$2";                shift 2 ;;
        --cross)
            if [[ $# -gt 1 && "$2" != --* ]]; then
                DO_CROSS="$2"; shift 2
            else
                DO_CROSS="evl-cross"; shift 1
            fi
            ;;
        --build)
            if [[ $# -gt 1 && "$2" != --* ]]; then
                DO_BUILD="$2"; shift 2
            else
                DO_BUILD="default"; shift 1
            fi
            ;;
        --test)    DO_TEST=1;          shift   ;;
        --run)     RUN_CMDS+=("$2");   shift 2 ;;
        --shell)   DO_SHELL=1;         shift   ;;
        --console) DO_CONSOLE=1;       shift   ;;
        --help|-h) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

# Default when no action flags given: cross-compile + test (mirrors CI)
if [[ -z "$DO_CROSS" && -z "$DO_BUILD" && "$DO_TEST" -eq 0 \
      && ${#RUN_CMDS[@]} -eq 0 && "$DO_SHELL" -eq 0 && "$DO_CONSOLE" -eq 0 ]]; then
    DO_CROSS="evl-cross"
    DO_TEST=1
fi

# ---------------------------------------------------------------------------
# Cleanup on exit
# ---------------------------------------------------------------------------
cleanup() {
    if [[ -f "$WORK_DIR/qemu.pid" ]]; then
        local pid
        pid="$(cat "$WORK_DIR/qemu.pid")"
        if kill -0 "$pid" 2>/dev/null; then
            echo "Stopping QEMU (pid $pid)..."
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    fi
    if [[ -f "$WORK_DIR/loop_mounted" ]]; then
        sudo umount "$WORK_DIR/mnt" 2>/dev/null || true
        rm -f "$WORK_DIR/loop_mounted"
    fi
    if [[ "$CLEANUP_WORK_DIR" -eq 1 ]]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Locate repository root (this script lives in scripts/)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

# Resolve relative EVL_SDK_DIR against REPO_ROOT
[[ "$EVL_SDK_DIR" != /* ]] && EVL_SDK_DIR="$REPO_ROOT/$EVL_SDK_DIR"

# ---------------------------------------------------------------------------
# Persistent cache directory and GitHub auth
# ---------------------------------------------------------------------------
CACHE_DIR="$REPO_ROOT/.evl-cache"

if [[ -n "${RATOS_RELEASE_TOKEN:-}" ]]; then
    export GH_TOKEN="$RATOS_RELEASE_TOKEN"
fi

# ---------------------------------------------------------------------------
# Cross-compile on host (--cross path, before QEMU boot)
# ---------------------------------------------------------------------------
if [[ -n "$DO_CROSS" ]]; then
    # Ensure the RaTOS ISAR SDK is extracted to EVL_SDK_DIR.
    # The SDK is a plain tarball (amd64 Debian sysroot with libevl, SeRTial,
    # and all CoreRaT dependencies).  We use the host compiler and add the
    # SDK to CMake search paths via cmake/isar-sdk-toolchain.cmake.
    SDK_KEY_FILE="${CACHE_DIR}/.sdk_cache_key"
    _SDK_KEY=""
    [[ -n "$RATOS_RELEASE_TAG" ]] && _SDK_KEY="tag:${RATOS_RELEASE_TAG}"

    _SDK_STALE=0
    if [[ ! -d "$EVL_SDK_DIR/usr" ]]; then
        _SDK_STALE=1
        echo "RaTOS SDK not found at ${EVL_SDK_DIR} — will download and extract..."
    elif [[ -n "$_SDK_KEY" && \
          ( ! -f "$SDK_KEY_FILE" || \
            "$(cat "$SDK_KEY_FILE" 2>/dev/null)" != "$_SDK_KEY" ) ]]; then
        _SDK_STALE=1
        echo "RaTOS SDK present but does not match ${_SDK_KEY} — re-extracting..."
    fi

    if [[ "$_SDK_STALE" -eq 1 ]]; then
        if [[ -z "$RATOS_RELEASE_REPO" || -z "$RATOS_RELEASE_TAG" ]]; then
            echo "ERROR: RaTOS SDK not found and RATOS_RELEASE_TAG/RATOS_RELEASE_REPO are not set." >&2
            echo "       Set them in .corerat.env or pass --tag <version>." >&2
            exit 1
        fi

        mkdir -p "$CACHE_DIR"
        SDK_CACHE_FILE="${CACHE_DIR}/ratos-dev-sdk-container-amd64.xz"
        if [[ ! -f "$SDK_CACHE_FILE" ]]; then
            echo "Downloading RaTOS SDK for ${RATOS_RELEASE_TAG}..."
            gh release download "$RATOS_RELEASE_TAG" \
                --repo "$RATOS_RELEASE_REPO" \
                --pattern "ratos-dev-sdk-container-amd64*" \
                --dir "$CACHE_DIR" \
                --clobber
            SDK_CACHE_FILE="$(ls "$CACHE_DIR"/ratos-dev-sdk-container-amd64* | head -1)"
        else
            echo "Using cached SDK archive: ${SDK_CACHE_FILE}"
        fi

        echo "Extracting RaTOS SDK to ${EVL_SDK_DIR} ..."
        rm -rf "$EVL_SDK_DIR"
        mkdir -p "$EVL_SDK_DIR"
        tar -xJf "$SDK_CACHE_FILE" -C "$EVL_SDK_DIR" --strip-components=1 2>/dev/null || true
        if [[ ! -f "$EVL_SDK_DIR/usr/include/evl/evl.h" ]]; then
            echo "ERROR: SDK extraction failed — evl/evl.h not found in ${EVL_SDK_DIR}" >&2
            exit 1
        fi

        # Relocate SDK (patch ELF interpreter paths + GCC sysroot wrapper).
        # Requires patchelf on the host (apt install patchelf).
        if command -v patchelf &>/dev/null; then
            "$EVL_SDK_DIR/relocate-sdk.sh"
        else
            echo "WARNING: patchelf not found — SDK ar/ranlib may fail.  Install patchelf." >&2
            sed -i "s|^GCC_SYSROOT=.*|GCC_SYSROOT=\"${EVL_SDK_DIR}\"|" \
                "$EVL_SDK_DIR/usr/bin/gcc-sysroot-wrapper.sh"
        fi

        [[ -n "$_SDK_KEY" ]] && echo "$_SDK_KEY" > "$SDK_KEY_FILE"
        echo "SDK extracted and relocated."
    else
        echo "Using RaTOS SDK at ${EVL_SDK_DIR} (${_SDK_KEY:-unversioned})"
    fi

    export EVL_SDK_DIR

    echo "Building with preset '${DO_CROSS}' against RaTOS SDK..."
    cmake --preset "${DO_CROSS}"
    cmake --build --preset "${DO_CROSS}" --parallel "$(nproc)"
    echo "Build complete."
fi

# Skip QEMU if no guest-side actions were requested (compile-only run).
if [[ -n "$DO_CROSS" && -z "$DO_BUILD" && "$DO_TEST" -eq 0 \
      && ${#RUN_CMDS[@]} -eq 0 && "$DO_SHELL" -eq 0 && "$DO_CONSOLE" -eq 0 ]]; then
    echo "Cross-compile complete.  No guest actions requested — skipping QEMU."
    exit 0
fi

# ---------------------------------------------------------------------------
# Download QEMU artifacts with persistent cache
# ---------------------------------------------------------------------------

_do_download() {
    local dl_dir="$1" run_id="$2" release_tag="$3"
    mkdir -p "$dl_dir"

    if [[ -n "$run_id" ]]; then
        echo "Downloading artifacts from workflow run ${run_id}..."
        gh run download "$run_id" \
            --repo "$RATOS_RELEASE_REPO" \
            --name ratos-evl-artifacts \
            --dir "$dl_dir"
    else
        echo "Downloading artifacts from release tag ${release_tag}..."
        gh release download "$release_tag" \
            --repo "$RATOS_RELEASE_REPO" \
            --pattern "vmlinuz" \
            --pattern "initrd.img" \
            --pattern "*container-amd64*.ext4.gz" \
            --dir "$dl_dir"
    fi

    echo "Downloaded:"
    ls -lh "$dl_dir/"

    if compgen -G "$dl_dir/*.ext4.gz" > /dev/null; then
        local gz
        gz="$(ls "$dl_dir"/*.ext4.gz | head -1)"
        echo "Decompressing ${gz} ..."
        gunzip "$gz"
    fi
}

if [[ -z "$EXT4_PATH" || -z "$KERNEL_PATH" || -z "$INITRD_PATH" ]]; then
    if [[ -z "$RATOS_RELEASE_REPO" ]]; then
        echo "ERROR: RATOS_RELEASE_REPO is not set." >&2
        echo "       Set it in .corerat.env or export it before running this script." >&2
        echo "       Or pass --ext4, --kernel, --initrd to skip downloading." >&2
        exit 1
    fi

    CACHE_KEY=""
    if [[ -n "$RATOS_RELEASE_TAG" ]]; then
        CACHE_KEY="tag:${RATOS_RELEASE_TAG}"
    elif [[ -n "$RATOS_RUN_ID" ]]; then
        CACHE_KEY="run:${RATOS_RUN_ID}"
    else
        echo "No RATOS_RELEASE_TAG set — locating latest successful run on main..."
        RATOS_RUN_ID="$(gh run list \
            --repo "$RATOS_RELEASE_REPO" \
            --workflow build-and-publish.yml \
            --branch main \
            --status success \
            --limit 1 \
            --json databaseId \
            --jq '.[0].databaseId')"
        if [[ -z "$RATOS_RUN_ID" || "$RATOS_RUN_ID" == "null" ]]; then
            echo "ERROR: No successful RaTOS build found on main branch." >&2
            echo "       Pass artifact paths, --run-id <id>, or --tag <tag>." >&2
            exit 1
        fi
        echo "Latest run ID: ${RATOS_RUN_ID}"
        CACHE_KEY="run:${RATOS_RUN_ID}"
    fi

    CACHED_KEY="${CACHE_DIR}/.cache_key"
    CACHED_EXT4="${CACHE_DIR}/ratos.ext4"
    CACHED_KERNEL="${CACHE_DIR}/vmlinuz"
    CACHED_INITRD="${CACHE_DIR}/initrd.img"

    if [[ -f "$CACHED_KEY" && "$(cat "$CACHED_KEY")" == "$CACHE_KEY" \
          && -f "$CACHED_EXT4" && -f "$CACHED_KERNEL" && -f "$CACHED_INITRD" ]]; then
        echo "Using cached artifacts ($CACHE_KEY)"
    else
        echo "Cache miss ($CACHE_KEY) — downloading..."
        rm -f "$CACHED_EXT4" "$CACHED_KERNEL" "$CACHED_INITRD" "$CACHED_KEY"
        rm -f "${CACHE_DIR}"/*.ext4.gz 2>/dev/null || true
        mkdir -p "$CACHE_DIR"

        if [[ "$CACHE_KEY" == tag:* ]]; then
            _do_download "$CACHE_DIR" "" "$RATOS_RELEASE_TAG"
        else
            _do_download "$CACHE_DIR" "$RATOS_RUN_ID" ""
        fi

        [[ -f "$CACHED_EXT4" ]]   || mv "$(ls "$CACHE_DIR"/*.ext4   | head -1)" "$CACHED_EXT4"
        [[ -f "$CACHED_KERNEL" ]] || mv "$(ls "$CACHE_DIR"/*vmlinuz | head -1)" "$CACHED_KERNEL"
        [[ -f "$CACHED_INITRD" ]] || mv "$(ls "$CACHE_DIR"/*initrd* | head -1)" "$CACHED_INITRD"

        echo "$CACHE_KEY" > "$CACHED_KEY"
        echo "Artifacts cached in ${CACHE_DIR}/"
    fi

    EXT4_PATH="$CACHED_EXT4"
    KERNEL_PATH="$CACHED_KERNEL"
    INITRD_PATH="$CACHED_INITRD"
fi

echo "Using ext4 image : $EXT4_PATH"
echo "Using kernel     : $KERNEL_PATH"
echo "Using initrd     : $INITRD_PATH"

# ---------------------------------------------------------------------------
# Copy ext4 image so the cache is never modified
# ---------------------------------------------------------------------------
EXT4_COPY="$WORK_DIR/ratos.ext4"
echo "Copying ext4 image to $EXT4_COPY ..."
cp "$EXT4_PATH" "$EXT4_COPY"
truncate -s "+256M" "$EXT4_COPY"
resize2fs "$EXT4_COPY" 2>/dev/null

# ---------------------------------------------------------------------------
# Generate ephemeral SSH keypair
# ---------------------------------------------------------------------------
SSH_KEY="$WORK_DIR/ci_key"
ssh-keygen -t ed25519 -N "" -f "$SSH_KEY" -q

# ---------------------------------------------------------------------------
# Inject public key into the image
# ---------------------------------------------------------------------------
echo "Injecting SSH public key into guest image..."
MOUNT_POINT="$WORK_DIR/mnt"
mkdir -p "$MOUNT_POINT"

sudo mount -o loop "$EXT4_COPY" "$MOUNT_POINT"
touch "$WORK_DIR/loop_mounted"
sudo mkdir -p "$MOUNT_POINT/root/.ssh"
sudo cp "${SSH_KEY}.pub" "$MOUNT_POINT/root/.ssh/authorized_keys"
sudo chmod 700 "$MOUNT_POINT/root/.ssh"
sudo chmod 600 "$MOUNT_POINT/root/.ssh/authorized_keys"
printf 'ulimit -H -t unlimited 2>/dev/null || true\nulimit -t unlimited 2>/dev/null || true\n' \
    | sudo tee "$MOUNT_POINT/etc/profile.d/no-cpu-limit.sh" > /dev/null
sudo chmod 644 "$MOUNT_POINT/etc/profile.d/no-cpu-limit.sh"
sudo umount "$MOUNT_POINT"
rm -f "$WORK_DIR/loop_mounted"

# ---------------------------------------------------------------------------
# Boot QEMU
# ---------------------------------------------------------------------------
if [[ -w /dev/kvm ]]; then
    CPU_ARGS="-cpu host -enable-kvm"
    echo "KVM acceleration enabled."
else
    CPU_ARGS="-cpu qemu64"
    echo "WARNING: /dev/kvm not accessible — running without KVM (slow)." >&2
    echo "         Add yourself to the 'kvm' group: sudo usermod -aG kvm \$USER" >&2
fi

if [[ "$DO_CONSOLE" -eq 1 ]]; then
    echo "Starting QEMU with serial console (Ctrl+A X to quit)..."
    # shellcheck disable=SC2086
    exec qemu-system-x86_64 \
        ${CPU_ARGS} \
        -smp "$QEMU_CPUS" \
        -m "$QEMU_MEMORY" \
        -machine q35 \
        -kernel "$KERNEL_PATH" \
        -initrd "$INITRD_PATH" \
        -drive "file=${EXT4_COPY},discard=unmap,if=none,id=disk,format=raw" \
        -device ide-hd,drive=disk \
        -append "root=/dev/sda rw rootwait console=ttyS0" \
        -nic "user,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22,model=e1000" \
        -device virtio-rng-pci \
        -serial mon:stdio \
        -nographic
fi

pkill -f "hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22" 2>/dev/null || true

echo "Starting QEMU..."
# shellcheck disable=SC2086
qemu-system-x86_64 \
    ${CPU_ARGS} \
    -smp "$QEMU_CPUS" \
    -m "$QEMU_MEMORY" \
    -machine q35 \
    -kernel "$KERNEL_PATH" \
    -initrd "$INITRD_PATH" \
    -drive "file=${EXT4_COPY},discard=unmap,if=none,id=disk,format=raw" \
    -device ide-hd,drive=disk \
    -append "root=/dev/sda rw rootwait console=ttyS0" \
    -nic "user,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22,model=e1000" \
    -device virtio-rng-pci \
    -serial "file:${WORK_DIR}/qemu-serial.log" \
    -monitor none \
    -nographic \
    < /dev/null \
    > "$WORK_DIR/qemu.log" 2>&1 &
echo $! > "$WORK_DIR/qemu.pid"

# ---------------------------------------------------------------------------
# Wait for SSH
# ---------------------------------------------------------------------------
echo "Waiting for guest SSH (up to 3 minutes)..."
SSH_OPTS="-i ${SSH_KEY} -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=10 -o UserKnownHostsFile=/dev/null -p ${SSH_PORT}"
for i in $(seq 1 18); do
    if ! kill -0 "$(cat "$WORK_DIR/qemu.pid")" 2>/dev/null; then
        echo "ERROR: QEMU process died unexpectedly!" >&2
        tail -20 "$WORK_DIR/qemu.log" >&2
        exit 1
    fi
    if ssh ${SSH_OPTS} root@127.0.0.1 true 2>/dev/null; then
        echo "SSH ready after ~$((i * 10))s"
        break
    fi
    if [[ "$i" -eq 18 ]]; then
        echo "ERROR: Timeout waiting for SSH after 3 minutes" >&2
        tail -20 "$WORK_DIR/qemu.log" >&2
        tail -20 "$WORK_DIR/qemu-serial.log" >&2
        exit 1
    fi
    echo "Waiting for SSH... (~$((i * 10))s)"
    sleep 10
done

# ---------------------------------------------------------------------------
# Execute requested actions: cross-deploy -> build -> test -> run -> shell
# ---------------------------------------------------------------------------

# Deploy cross-compiled binaries to guest.
if [[ -z "$DO_CROSS" && "$DO_SHELL" -eq 1 && -d "build/evl-cross" ]]; then
    echo "Note: --shell without --cross; auto-deploying existing build/evl-cross/ to guest."
    DO_CROSS="evl-cross"
fi
if [[ -n "$DO_CROSS" ]]; then
    echo "Deploying build/${DO_CROSS}/ to guest /root/corerat/..."
    ssh ${SSH_OPTS} root@127.0.0.1 mkdir -p /root/corerat/test
    rsync -az --delete \
        -e "ssh ${SSH_OPTS}" \
        "build/${DO_CROSS}/" "root@127.0.0.1:/root/corerat/"
fi

# Build inside guest (rsync source first)
if [[ -n "$DO_BUILD" ]]; then
    echo "Transferring CoreRaT source to guest..."
    rsync -az --delete \
        --exclude=build \
        --exclude=.git \
        --exclude=CommRaT \
        --exclude=tims \
        --exclude=.evl-cache \
        -e "ssh ${SSH_OPTS}" \
        ./ "root@127.0.0.1:/root/CoreRaT/"

    echo "Building with preset '${DO_BUILD}' inside EVL guest..."
    ssh ${SSH_OPTS} root@127.0.0.1 bash -lc "
        set -euo pipefail
        cd /root/CoreRaT
        cmake --preset ${DO_BUILD} 2>&1
        cmake --build --preset ${DO_BUILD} --parallel \$(nproc) 2>&1
        echo 'Build complete.'
    "
fi

# Run tests
if [[ "$DO_TEST" -eq 1 ]]; then
    if [[ -n "$DO_CROSS" ]]; then
        # Binaries cross-compiled and deployed to /root/corerat/
        echo "Running CoreRaT tests on EVL guest..."
        ssh ${SSH_OPTS} root@127.0.0.1 bash -c 'set -euo pipefail
ulimit -H -t unlimited 2>/dev/null || true; ulimit -t unlimited 2>/dev/null || true

ok=0; fail=0

# ---- Unit tests (no router needed) ----
echo "=== Unit tests ==="
while IFS= read -r -d "" t; do
    printf "  %-50s" "$(basename "$t")"
    if timeout 30 "$t" > /tmp/.test_out 2>&1; then
        echo PASS; ((ok++)) || true
    else
        echo FAIL; ((fail++)) || true; cat /tmp/.test_out
    fi
done < <(find /root/corerat/test -maxdepth 1 \
              -name "test_*" -executable -type f -print0 | sort -z)

# ---- Integration tests (router required) ----
echo "=== Integration tests ==="
/root/corerat/corerat-router-tcp --port 2000 &
ROUTER_PID=$!
trap "kill $ROUTER_PID 2>/dev/null || true; wait $ROUTER_PID 2>/dev/null || true" EXIT
sleep 0.3

# Router smoke test
printf "  %-50s" "test_router_tcp"
if timeout 10 /root/corerat/test/test_router_tcp > /tmp/.test_out 2>&1; then
    echo PASS; ((ok++)) || true
else
    echo FAIL; ((fail++)) || true; cat /tmp/.test_out
fi

# Ping-pong test
printf "  %-50s" "test_pingpong"
/root/corerat/test/pong_node --count 100 > /tmp/.pong_out 2>&1 &
PONG_PID=$!
sleep 0.3
if timeout 20 /root/corerat/test/ping_node --count 100 > /tmp/.test_out 2>&1; then
    echo PASS; ((ok++)) || true
else
    echo FAIL; ((fail++)) || true
    echo "  ping_node output:"; cat /tmp/.test_out
    echo "  pong_node output:"; cat /tmp/.pong_out
fi
kill "$PONG_PID" 2>/dev/null || true
wait "$PONG_PID" 2>/dev/null || true

echo ""
echo "Results: ${ok} passed, ${fail} failed"
[[ $fail -eq 0 ]]'
    else
        CTEST_PRESET="${DO_BUILD:-default}"
        echo "Running ctest --preset ${CTEST_PRESET} on EVL guest..."
        ssh ${SSH_OPTS} root@127.0.0.1 bash -lc "
            set -euo pipefail
            cd /root/CoreRaT
            ctest --preset ${CTEST_PRESET} 2>&1
        "
    fi
fi

# Run specific binaries
for cmd in "${RUN_CMDS[@]}"; do
    echo "Running on EVL guest: ${cmd}"
    ssh ${SSH_OPTS} root@127.0.0.1 bash -lc "
        set -euo pipefail
        ${cmd}
    "
done

# Interactive shell
if [[ "$DO_SHELL" -eq 1 ]]; then
    echo ""
    echo "Opening interactive EVL guest shell."
    if [[ -n "$DO_CROSS" ]]; then
        echo "Cross-compiled binaries are at /root/corerat/"
        echo "  Router:    /root/corerat/corerat-router-tcp"
        echo "  ping_node: /root/corerat/test/ping_node"
        echo "  pong_node: /root/corerat/test/pong_node"
        echo "  evl ps     — check EVL thread status"
        echo "  evl check  — verify RT health"
    elif [[ -n "$DO_BUILD" ]]; then
        echo "CoreRaT source + build are at /root/CoreRaT/"
        echo "Build output at /root/CoreRaT/build/${DO_BUILD}/"
    fi
    echo "Type 'exit' to stop QEMU and clean up."
    echo ""
    ssh -t ${SSH_OPTS} root@127.0.0.1 'ulimit -H -t unlimited 2>/dev/null; ulimit -t unlimited 2>/dev/null; exec bash --login'
fi
