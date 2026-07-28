# =============================================================================
# jppd-docs — Dockerized MkDocs build/preview for the JPPDOS App Developer Guide.
#
# Python-only (the mkdocs-shadcn theme ships prebuilt assets, so no Node).
# Renders the docs/ tree in place; mkdocs.yml at the repo root is the config.
#
# Build (from the repository ROOT):
#     docker build -f tools/docs/Dockerfile -t jppd-docs .
#
# Live preview at http://localhost:8000 (auto-reloads on edits):
#     docker run --rm -p 8000:8000 -v "$PWD:/project" jppd-docs
#
# Produce a static site into ./site/:
#     docker run --rm -v "$PWD:/project" jppd-docs build
# =============================================================================
FROM python:3.12-slim

# git is a hard runtime dependency, not a build tool: the shadcn theme's search
# plugin imports GitPython at module load (shadcn/plugins/mixins/git.py) to
# derive per-page timestamps, and GitPython raises ImportError on import when no
# git executable is on PATH. Without this, `mkdocs serve` dies before binding
# the port. python:3.12-slim ships no git.
RUN apt-get update \
    && apt-get install -y --no-install-recommends git \
    && rm -rf /var/lib/apt/lists/*

COPY tools/docs/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt

WORKDIR /project
EXPOSE 8000

ENTRYPOINT ["mkdocs"]
# Default: live preview bound to all interfaces so the port maps out of the
# container. Override with `... jppd-docs build` for a static site.
CMD ["serve", "-a", "0.0.0.0:8000"]
