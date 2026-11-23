FROM alpine:3.21

# Minimum stack: LLVM/Clang, builders, debuggers, binutils ELF and e2e curl
RUN apk add --no-cache \
    bash \
    git \
    cmake \
    ninja \
    clang \
    clang-extra-tools \
    compiler-rt \
    lld \
    lldb \
    llvm \
    binutils \
    musl-dev \
    linux-headers \
    xxd \
    gzip \
    curl

ENV CC=clang
ENV CXX=clang++

# git ls-files in check.sh: repos enter via docker cp with foreign ownership
RUN git config --global --add safe.directory '*'

# Server data: /data is a volume, survives container recreation
WORKDIR /data/work

# Future HTTP port of the server (publish lives in the server repo compose)
EXPOSE 8080

# fix colors
ENV TERM=xterm-256color
ENV COLORTERM=truecolor

# Start
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
