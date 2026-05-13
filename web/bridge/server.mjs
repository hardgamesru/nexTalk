import tls from "node:tls";
import { WebSocketServer } from "ws";

const BRIDGE_PORT = Number.parseInt(process.env.BRIDGE_PORT ?? "5174", 10);
const DEFAULT_HOST = process.env.NEXTALK_HOST ?? "127.0.0.1";
const DEFAULT_TCP_PORT = Number.parseInt(process.env.NEXTALK_PORT ?? "5555", 10);

const wss = new WebSocketServer({ host: "127.0.0.1", port: BRIDGE_PORT });

function sendJson(ws, payload) {
  if (ws.readyState === ws.OPEN) {
    ws.send(JSON.stringify(payload));
  }
}

wss.on("connection", (ws) => {
  let tcp = null;
  let readBuffer = "";

  const closeTcp = () => {
    if (!tcp) {
      return;
    }

    tcp.destroy();
    tcp = null;
  };

  const ensureConnected = (host, port) => {
    if (tcp && !tcp.destroyed) {
      return;
    }

    tcp = tls.connect({
      host,
      port,
      // Accept the self-signed certificate in the local teaching setup only.
      rejectUnauthorized: false,
    });
    tcp.setNoDelay(true);

    tcp.on("connect", () => {
      sendJson(ws, { type: "connected", host, port });
    });

    tcp.on("data", (chunk) => {
      readBuffer += chunk.toString("utf8");
      let boundary = readBuffer.indexOf("\n");

      while (boundary >= 0) {
        const line = readBuffer.slice(0, boundary);
        readBuffer = readBuffer.slice(boundary + 1);
        sendJson(ws, { type: "line", line });
        boundary = readBuffer.indexOf("\n");
      }
    });

    tcp.on("error", (error) => {
      sendJson(ws, { type: "tcp_error", message: error.message });
    });

    tcp.on("close", () => {
      sendJson(ws, { type: "disconnected" });
      tcp = null;
    });

  };

  sendJson(ws, { type: "ready", bridgePort: BRIDGE_PORT });

  ws.on("message", (raw) => {
    let payload;
    try {
      payload = JSON.parse(raw.toString("utf8"));
    } catch {
      sendJson(ws, { type: "bridge_error", message: "invalid JSON payload" });
      return;
    }

    if (!payload || typeof payload !== "object") {
      sendJson(ws, { type: "bridge_error", message: "invalid payload type" });
      return;
    }

    if (payload.type === "connect") {
      const host = typeof payload.host === "string" && payload.host.trim() ? payload.host.trim() : DEFAULT_HOST;
      const port = Number.isInteger(payload.port) ? payload.port : DEFAULT_TCP_PORT;
      ensureConnected(host, port);
      return;
    }

    if (payload.type === "disconnect") {
      closeTcp();
      return;
    }

    if (payload.type === "send") {
      if (!tcp || tcp.destroyed) {
        sendJson(ws, { type: "bridge_error", message: "TCP socket is not connected" });
        return;
      }

      const line = String(payload.line ?? "");
      const normalized = line.endsWith("\n") ? line : `${line}\n`;
      tcp.write(normalized);
      return;
    }

    sendJson(ws, { type: "bridge_error", message: `unsupported message type: ${payload.type}` });
  });

  ws.on("close", () => {
    closeTcp();
  });

  ws.on("error", () => {
    closeTcp();
  });
});

console.log(`NexTalk bridge listening on ws://127.0.0.1:${BRIDGE_PORT}`);
