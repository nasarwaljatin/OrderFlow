# ---------- Stage 1: build + test ----------
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && cd build && ctest --output-on-failure -j"$(nproc)"

# ---------- Stage 2: minimal runtime image ----------
FROM ubuntu:24.04
RUN useradd --create-home orderflow
COPY --from=build /src/build/orderflow_server \
                  /src/build/orderflow_client \
                  /src/build/orderflow_gen \
                  /src/build/orderflow_replay \
                  /src/build/orderflow_latency \
                  /src/build/orderflow_bench \
                  /usr/local/bin/
USER orderflow
WORKDIR /home/orderflow
EXPOSE 9000
ENTRYPOINT ["orderflow_server"]
CMD ["--port", "9000", "--symbols", "4"]
