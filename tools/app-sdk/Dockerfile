# =============================================================================
# jppd-app-sdk — self-contained build image for J++Device (JPPDOS) apps.
#
# Mount an app source dir at /app and `jppd-build` compiles it against the exact
# toolchain, headers, and include paths this firmware release was built with.
# No firmware checkout needed by the app developer.
#
# The image is VERSION-LOCKED to the firmware revision it was built from — tag
# it with the matching JPPDOS_VERSION (see main/jpp_settings_screen.h).
#
# --- Why multi-stage -------------------------------------------------------
# espressif/idf:v5.5.1 is ~12 GB (2.9 GB IDF sources, 3.6 GB of toolchains for
# every ESP target, plus gdb/qemu/cmake). Compiling an app actually needs only a
# RISC-V compiler and ~28 MB of headers. So the builder stage does a full
# firmware build, capture_sysroot.py distills the include closure, and the final
# stage starts from python-slim and copies in just the compiler + those headers.
#
# Build (from the repository ROOT, which is the build context):
#     docker build -f tools/app-sdk/Dockerfile -t jppd-app-sdk .
#
# Use (from an app source directory):
#     docker run --rm -v "$PWD:/app" jppd-app-sdk
#     docker run --rm -v "$PWD:/app" --device /dev/ttyACM0 \
#         jppd-app-sdk --upload /dev/ttyACM0
# =============================================================================

# -----------------------------------------------------------------------------
# Stage 1 — builder: full firmware build, then distill the sysroot.
# Nothing from this stage ships except sysroot.tar, sdk-flags.json, mpy-cross,
# and the RISC-V toolchain.
# -----------------------------------------------------------------------------
FROM espressif/idf:v5.5.1 AS builder

# mpy-cross 1.28.0 (MicroPython app bytecode compiler) — a single small binary.
RUN git clone --depth 1 --branch v1.28.0 \
        https://github.com/micropython/micropython.git /tmp/micropython \
    && make -C /tmp/micropython/mpy-cross \
    && cp /tmp/micropython/mpy-cross/build/mpy-cross /usr/local/bin/mpy-cross \
    && rm -rf /tmp/micropython

COPY . /project
WORKDIR /project
RUN . "$IDF_PATH/export.sh" \
    && idf.py set-target esp32c6 \
    && idf.py build

# Distill: resolved compiler + full -I list, and the include closure as a tar.
RUN python3 /project/tools/app-sdk/capture_sysroot.py \
        /project/build /project /sysroot-out

# Prune the RISC-V toolchain. Apps link with -nostdlib and resolve every libc /
# libgcc symbol at load time from the firmware's exported symbol table (see
# jpp_native_symtab.c), so the target static libraries are never linked against
# — only their headers are needed. Dropping the multilib .a archives is the
# single biggest saving; C++ and LTO backends are unused too.
RUN TC=$(ls -d /opt/esp/tools/riscv32-esp-elf/*/riscv32-esp-elf) \
    && find "$TC" -name '*.a' -delete \
    && find "$TC" -name 'cc1plus' -delete \
    && find "$TC" -name 'lto1' -delete \
    && find "$TC" -name 'lto-dump*' -delete \
    && rm -f "$TC"/bin/*g++ "$TC"/bin/*c++ \
    && du -sh "$TC"

# -----------------------------------------------------------------------------
# Stage 2 — runtime: slim base + compiler + headers only.
# -----------------------------------------------------------------------------
FROM python:3.12-slim

# pyserial powers --upload (JPPD-SMP over USB-Serial-JTAG).
RUN pip install --no-cache-dir pyserial

# The RISC-V toolchain, at its original path so sdk-flags.json resolves verbatim.
COPY --from=builder /opt/esp/tools/riscv32-esp-elf /opt/esp/tools/riscv32-esp-elf

# The include closure, restored to absolute paths (/opt/esp/idf/..., /project/...).
COPY --from=builder /sysroot-out/sysroot.tar /tmp/sysroot.tar
RUN tar -xf /tmp/sysroot.tar -C / && rm /tmp/sysroot.tar

# The distilled flags — replaces shipping the 6.6 MB compile_commands.json.
COPY --from=builder /sysroot-out/sdk-flags.json /opt/jppd-sdk/sdk-flags.json

# mpy-cross for MicroPython apps.
COPY --from=builder /usr/local/bin/mpy-cross /usr/local/bin/mpy-cross

# The builder script, the manifest validator, and the device uploader.
COPY tools/app-sdk/jppd-build /usr/local/bin/jppd-build
COPY tests/validate_manifests.py /project/tests/validate_manifests.py
COPY scripts/jppd_upload.py /project/scripts/jppd_upload.py
RUN chmod 0755 /usr/local/bin/jppd-build

ENV JPPD_SDK_ROOT=/project
ENV JPPD_SDK_FLAGS=/opt/jppd-sdk/sdk-flags.json

WORKDIR /app
ENTRYPOINT ["jppd-build"]
