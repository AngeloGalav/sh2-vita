# The sh2-vita packer: everything the converters need (Python with numpy, Pillow and pefile, ffmpeg for
# the Bink movies, texenc built for the image's own CPU) in one image, so the PS Vita's game data can be
# built from your own copy of the game on any computer with Docker. It holds only the port's
# converters, never game data.
#
#   docker build -f tools/packer/Dockerfile -t sh2-vita-packer .
#   docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/out:/out" sh2-vita-packer
FROM debian:bookworm-slim@sha256:7c7b2c966bc9ee8cedfeef67e0e279108992c77681fa595db4a9d65c06ccc587

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        python3 python3-numpy python3-pil python3-pefile ffmpeg g++ && \
    rm -rf /var/lib/apt/lists/*

COPY VERSION /opt/sh2-vita/VERSION
COPY tools/texenc /opt/sh2-vita/tools/texenc
RUN /opt/sh2-vita/tools/texenc/build.sh native
COPY tools/make_pack.sh /opt/sh2-vita/tools/make_pack.sh
COPY tools/sh2conv /opt/sh2-vita/tools/sh2conv
# the stage table and function list that events.py and demos.py read
COPY sh2-3ds-port-kit/data/dc /opt/sh2-vita/sh2-3ds-port-kit/data/dc
COPY tools/packer/entrypoint.sh /opt/sh2-vita/tools/packer/entrypoint.sh

# the Vita's textures (DXT from the PC's pixels) and the native encoder on every core
ENV PY="python3 -I" \
    HOME=/tmp \
    SH2_VITA_TEX=1 \
    SH2_TEXENC=native

LABEL org.opencontainers.image.title="sh2-vita packer" \
      org.opencontainers.image.description="Builds the sh2-vita game data from your own Silent Hill 2 PC (Director's Cut) install" \
      org.opencontainers.image.source="https://github.com/AngeloGalav/sh2-vita" \
      org.opencontainers.image.licenses="GPL-3.0-only"

ENTRYPOINT ["/opt/sh2-vita/tools/packer/entrypoint.sh"]
