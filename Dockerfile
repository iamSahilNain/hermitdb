# ---- dev: everything needed to build, test, and poke the server ----------
# Also the daily driver on macOS: epoll is Linux-only, so all compile/test
# cycles happen inside this image (see Makefile shortcuts).
FROM ubuntu:24.04 AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
        python3 redis-tools clang-format gdb strace \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work

# ---- build ----------------------------------------------------------------
FROM dev AS build
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target hermitdb

# ---- runtime: single static-ish binary, tiny image ------------------------
FROM ubuntu:24.04 AS runtime
COPY --from=build /work/build/hermitdb /usr/local/bin/hermitdb
EXPOSE 6380
ENTRYPOINT ["hermitdb"]
CMD ["--port=6380"]
