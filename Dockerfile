# ==========================================
# STAGE 1: Build Environment
# ==========================================
FROM alpine:latest AS builder

RUN apk add --no-cache \
    gcc g++ make linux-headers pkgconf \
    ncurses-dev openssl-dev util-linux-dev curl tar \
    opus-dev speex-dev speexdsp-dev libsrtp-dev

WORKDIR /usr/src/pjproject

RUN curl -L https://github.com/pjsip/pjproject/archive/refs/tags/2.17.tar.gz | tar xz --strip-components=1

COPY txt_stream.c pjmedia/src/pjmedia/txt_stream.c

RUN ./configure CFLAGS="-O2" \
        --disable-sound \
        --disable-video \
        --disable-alsa \
        --disable-oss && \
    make dep && \
    make && \
    make install

WORKDIR /app
COPY pjsua_rtt_cli.c .

RUN gcc -o pjsip_cli pjsua_rtt_cli.c \
    $(pkg-config --cflags --libs --static libpjproject) \
    -lncurses -lssl -lcrypto -luuid -lm -lpthread \
    -lopus -lspeex -lspeexdsp -lsrtp2


# ==========================================
# STAGE 2: Minimal Runtime Environment
# ==========================================
FROM alpine:latest

RUN apk add --no-cache \
    openssl libuuid libstdc++ ncurses-libs ncurses-terminfo \
    opus speex speexdsp libsrtp

WORKDIR /app

COPY --from=builder /app/pjsip_cli /app/pjsip_cli

CMD ["/bin/sh"]
