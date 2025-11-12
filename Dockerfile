FROM ubuntu:22.04

# Build deps
RUN apt-get update && apt-get install -y \
    g++ make cmake libasio-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

# Build:
# -I. so crow/* headers resolve from the project root
# -DASIO_STANDALONE because we're using standalone Asio (libasio-dev)
# -pthread required by Crow
RUN g++ -std=c++17 -I. -DASIO_STANDALONE server.cpp airdp.cpp -O2 -pthread -o app

EXPOSE 18080
CMD ["./app"]