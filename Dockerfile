#---> Build Image <---
# docker build -t pipeline:latest -f Dockerfile .
#
#---> Alias Command <---
# alias pipeline='docker run -it --rm --name pipeline -v `pwd`:/root pipeline:latest'


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
    curl -s https://api.github.com/repos/codekitchen/pipeline/releases/latest | grep browser_download_url | cut -d '"' -f 4 | wget --show-progress -P /root/ -qi - && \
    tar -xzvf pipeline-*.tar.gz && \
    cd pipeline-** && \
    ./configure && \
    make install && \
    cd; rm -r pipeline-**

ENV LANG C.UTF-8
