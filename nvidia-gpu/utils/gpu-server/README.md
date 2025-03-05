# NVIDIA GPU Server Documentation

## Overview
The NVIDIA GPU Server is a system component that enables communication between system management applications and NVIDIA GPUs using the MCTP (Management Component Transport Protocol) protocol. It provides a unified interface for GPU management operations through both in-kernel and daemon-based MCTP implementations.

## Architecture

```mermaid
graph TB
    subgraph "Client Layer"
        Client["Client Applications"]
        ClientLib["GPU Server Client Library"]
    end

    subgraph "Server Layer"
        GPUServer["GPU Server Daemon<br/>(gpuserverd)"]
        MctpReactor["MCTP Reactor<br/>(Discovery Service)"]
    end

    subgraph "Transport Layer"
        InKernel["In-Kernel MCTP"]
        DaemonMCTP["MCTP Daemon"]
    end

    subgraph "Hardware Layer"
        GPU["NVIDIA GPUs"]
    end

    Client --> ClientLib
    ClientLib --> GPUServer
    GPUServer --> MctpReactor
    GPUServer --> InKernel
    GPUServer --> DaemonMCTP
    InKernel --> GPU
    DaemonMCTP --> GPU
```

## Message Flow

```mermaid
sequenceDiagram
    participant Client
    participant GPUServer
    participant MCTPHandler
    participant GPU

    Client->>GPUServer: Connect (Unix Socket)
    GPUServer->>Client: Connection Established
    
    Note over Client,GPUServer: API Message Flow
    Client->>GPUServer: Send API Message
    GPUServer->>MCTPHandler: Process Request
    MCTPHandler->>GPU: MCTP Message
    GPU->>MCTPHandler: MCTP Response
    MCTPHandler->>GPUServer: Process Response
    GPUServer->>Client: API Response

    Note over Client,GPU: Discovery Flow
    Client->>GPUServer: Discovery Request
    GPUServer->>MCTPHandler: Scan Endpoints
    MCTPHandler->>GPU: Endpoint Discovery
    GPU->>MCTPHandler: Endpoint Info
    MCTPHandler->>GPUServer: Available Endpoints
    GPUServer->>Client: Discovery Response
```

## Core Components

### 1. GPU Server Daemon (gpuserverd)
- Main server process that handles client connections
- Manages communication between clients and GPUs
- Supports multiple concurrent client connections
- Handles request routing and response management

### 2. MCTP Reactor
- Handles MCTP endpoint discovery
- Manages endpoint registration and deregistration
- Provides dynamic endpoint configuration

### 3. Transport Handlers
- **In-Kernel Handler**: Direct kernel MCTP communication
- **Daemon Handler**: MCTP daemon-based communication
- Abstracts transport layer details from upper layers

### 4. Client Library
- Provides C API for client applications
- Handles connection management
- Implements message formatting and parsing
