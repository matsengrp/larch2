# Stage 1: Build GCC trunk with -freflection support
FROM ubuntu:24.04 AS gcc-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential flex bison \
        libgmp-dev libmpfr-dev libmpc-dev \
        git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 git://gcc.gnu.org/git/gcc.git /gcc-src

RUN mkdir /gcc-build /gcc-install \
    && cd /gcc-build \
    && /gcc-src/configure \
        --prefix=/gcc-install \
        --enable-languages=c,c++ \
        --disable-multilib \
        --disable-bootstrap \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /gcc-src /gcc-build

# Stage 2: Development image with GCC trunk + project dependencies
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake make zlib1g-dev git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=gcc-builder /gcc-install /opt/gcc-trunk

ENV PATH="/opt/gcc-trunk/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/gcc-trunk/lib64:${LD_LIBRARY_PATH}"

WORKDIR /src
