# Build + run the cloud registration-plane load generator.
#
# FROM the cloud *builder* stage (toolchain + ACE + Lua + vendored
# libtinydtls.a already present at /src/apps/3rdparty/tinydtls). Build the
# builder first and tag it, then build this:
#
#   podman build --target builder -t iot-cloud-builder:bench -f apps/cloud/Dockerfile .
#   podman build -t iot-loadgen:latest -f apps/bench/Dockerfile \
#       --build-arg BUILDER=iot-cloud-builder:bench apps/bench
#
# run-bench.sh does both for you. The builder ldconfig's ACE, so the binary
# runs in this image with no extra runtime libs.
ARG BUILDER=iot-cloud-builder:bench
FROM ${BUILDER} AS bench

# Overlay the CURRENT bench sources over the builder's baked-in copy so edits
# since the builder image was built actually reach the compiler. ../src, ../inc
# and the vendored libtinydtls.a are unchanged and come from the builder.
COPY . /src/apps/bench/

RUN cmake -S /src/apps/bench -B /tmp/bbench \
        -DCMAKE_BUILD_TYPE=Release \
        -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 \
        -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
 && cmake --build /tmp/bbench -j "$(nproc)" \
 && cp /tmp/bbench/cloud_loadgen /usr/local/bin/cloud_loadgen

# bs-master-wrap (built by the builder's apps target) — run-bench.sh uses it to
# wrap a throwaway HKDF master, via `--entrypoint bs-master-wrap`.
RUN cp /tmp/blwm2m/bs-master-wrap /usr/local/bin/bs-master-wrap

ENTRYPOINT ["/usr/local/bin/cloud_loadgen"]
