# Builder stage
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    zlib1g-dev \
    libircclient-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Build the server
RUN make server

# Runtime stage
FROM ubuntu:22.04

# Install runtime dependencies (zlib and libircclient libs)
RUN apt-get update && apt-get install -y \
    zlib1g \
    libircclient1 \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy the built binary from builder
COPY --from=builder /app/sauer_server .

# Expose default Sauerbraten server port
EXPOSE 28785

# Default command to run the server
CMD ["./sauer_server"]