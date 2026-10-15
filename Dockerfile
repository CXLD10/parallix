# Runs the Parallix demo on any machine with Docker.
#   docker build -t parallix .
#   docker run --rm -it parallix              # fast demo
#   docker run --rm -it parallix ./demo.sh    # paced demo
#   docker run --rm parallix make test        # test suite
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ gcc make cmake libgtest-dev ca-certificates git \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /parallix
COPY . .
RUN make test
CMD ["./demo.sh", "--fast"]
