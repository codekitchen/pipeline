#---> Build Image <---
# docker build -t pipeline:latest -f Dockerfile .
#
#---> Alias Command <---
# alias pipeline='docker run -it --rm --name pipeline -v `pwd`:/root pipeline:latest'


FROM ubuntu:latest

ENV LANG "en_US.UTF-8"

WORKDIR /root/

RUN set -e; \
    apt-get update; \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        autoconf \
        build-essential \
        curl \
        tar \
        wget \
        libreadline-dev \
        libncurses5-dev \
        valgrind \
    ; \
    rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]

RUN set -e; \
    curl -s https://api.github.com/repos/codekitchen/pipeline/releases/latest | grep browser_download_url | cut -d '"' -f 4 | wget --show-progress -P /root/ -qi - && \
    tar -xzvf pipeline-*.tar.gz && \
    cd pipeline-** && \
    ./configure && \
    make install && \
    cd; rm -r pipeline-**

ENTRYPOINT pipeline
