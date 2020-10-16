FROM ubuntu:latest

RUN set -e; \
    apt-get update; \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        autoconf \
        build-essential \
        git \
        libreadline-dev \
        libncurses5-dev \
        valgrind \
    ; \
    rm -rf /var/lib/apt/lists/*

COPY . /pipeline
WORKDIR /pipeline

RUN set -e; \
    autoreconf -fi; \
    ./configure; \
    make install

ENV LANG C.UTF-8
