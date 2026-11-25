FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake pkg-config libmysqlclient-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMC_BUILD_TESTS=OFF \
 && cmake --build build -j"$(nproc)"

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends libmysqlclient21 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --no-create-home crawler
COPY --from=build /src/build/minicrawler /usr/local/bin/minicrawler
USER crawler
ENTRYPOINT ["minicrawler"]