# NexTalk Web Frontend

Vue + Vite client for NexTalk with a local WebSocket bridge to the existing C++ TCP server.

## Why a bridge is needed

Browsers cannot open raw TCP sockets to the C++ server directly.
`web/bridge/server.mjs` accepts WebSocket connections from the browser and forwards protocol lines to the TCP server.

## Run

1. Start the C++ server from project root:

```bash
./build/server/server 5555 127.0.0.1 server.log nextalk.db
```

2. Start the bridge:

```bash
cd web
npm run bridge
```

3. Start Vite:

```bash
cd web
npm run dev
```

Then open the URL printed by Vite (usually `http://localhost:5173`).

## Build

```bash
cd web
npm run build
```
