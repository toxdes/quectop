FROM oven/bun:1.4.5-debian@sha256:3dddea58e1f8f2240eca906b023fedb944f9a8352515a35309fd0c879fe00aaa AS bun

FROM debian:bookworm@sha256:1d04200c2ece1d4d75a2a6f8e11ebce7d86a2ae28b34bb2c9cbb3ee9f4953e75 AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev make python3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=bun /usr/local/bin/bun /usr/local/bin/bun
WORKDIR /src
COPY . .
ARG VERSION
RUN make -j2 VERSION="${VERSION:-$(cat VERSION)}"

FROM debian:bookworm@sha256:1d04200c2ece1d4d75a2a6f8e11ebce7d86a2ae28b34bb2c9cbb3ee9f4953e75 AS package
RUN apt-get update && apt-get install -y --no-install-recommends dpkg-dev python3 rpm tar \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY --from=build /src/quectop /src/quectop-hub ./
COPY VERSION LICENSE release.toml ./
COPY web/icons/LICENSE web/icons/LICENSE
COPY packaging/hub/release.toml hub-release.toml
COPY yesb/package.py yesb/package.py
ARG TARGETARCH
ARG VERSION
ARG GIT_SHA
ARG INCLUDE_APPIMAGE=0
ENV TARGETARCH="${TARGETARCH}" VERSION="${VERSION}" GIT_SHA="${GIT_SHA}" INCLUDE_APPIMAGE="${INCLUDE_APPIMAGE}"
RUN python3 yesb/package.py && cp hub-release.toml release.toml && python3 yesb/package.py

FROM scratch
COPY --from=package /output/ /
