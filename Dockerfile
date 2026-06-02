FROM espressif/idf:v5.5.1

RUN git clone --depth 1 --branch v1.28.0 \
        https://github.com/micropython/micropython.git /tmp/micropython \
    && make -C /tmp/micropython/mpy-cross \
    && cp /tmp/micropython/mpy-cross/build/mpy-cross /usr/local/bin/mpy-cross \
    && rm -rf /tmp/micropython

WORKDIR /project
EXPOSE 4000
