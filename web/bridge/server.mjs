import tls from "node:tls";
import { WebSocketServer } from "ws";

const BRIDGE_PORT = Number.parseInt(process.env.BRIDGE_PORT ?? "5174", 10);
const BRIDGE_HOST = process.env.BRIDGE_HOST ?? "127.0.0.1";
const DEFAULT_HOST = process.env.NEXTALK_HOST ?? "127.0.0.1";
const DEFAULT_TCP_PORT = Number.parseInt(process.env.NEXTALK_PORT ?? "5555", 10);
// Ограничение строки должно совпадать по смыслу с C++ kMaxLineLength.
// Оно защищает bridge от бесконечного буфера, если сервер или клиент сломались.
const MAX_LINE_LENGTH = Number.parseInt(process.env.BRIDGE_MAX_LINE_LENGTH ?? "8192", 10);
const ALLOWED_HOSTS = new Set(
  (process.env.BRIDGE_ALLOWED_HOSTS ?? "127.0.0.1,localhost,::1")
    .split(",")
    .map((value) => value.trim())
    .filter(Boolean),
);
const ALLOWED_ORIGINS = new Set(
  (process.env.BRIDGE_ALLOWED_ORIGINS ?? "")
    .split(",")
    .map((value) => value.trim())
    .filter(Boolean),
);
const ALLOW_PRIVATE_ORIGINS = process.env.BRIDGE_ALLOW_PRIVATE_ORIGINS === "1";

const wss = new WebSocketServer({ host: BRIDGE_HOST, port: BRIDGE_PORT });

function isPrivateIpv4(hostname) {
  const parts = hostname.split(".");
  if (parts.length !== 4) {
    return false;
  }

  const octets = parts.map((part) => Number.parseInt(part, 10));
  if (octets.some((octet) => !Number.isInteger(octet) || octet < 0 || octet > 255)) {
    return false;
  }

  return octets[0] === 10 ||
    (octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31) ||
    (octets[0] === 192 && octets[1] === 168);
}

function isLocalDevHostname(hostname) {
  return hostname === "localhost" ||
    hostname === "127.0.0.1" ||
    hostname === "::1" ||
    hostname === "[::1]";
}

function isAllowedPrivateOrigin(protocol, hostname) {
  if (!ALLOW_PRIVATE_ORIGINS) {
    return false;
  }

  return (protocol === "http:" || protocol === "https:") &&
    (isPrivateIpv4(hostname) || hostname.endsWith(".local"));
}

function isAllowedOrigin(origin) {
  // В dev-режиме браузер обычно приходит с origin вида http://localhost:5173.
  // Для сетевого dev-режима можно включить BRIDGE_ALLOW_PRIVATE_ORIGINS=1.
  if (!origin) {
    return true;
  }
  if (ALLOWED_ORIGINS.has(origin)) {
    return true;
  }

  try {
    const { hostname, protocol } = new URL(origin);
    return (protocol === "http:" || protocol === "https:") &&
      (isLocalDevHostname(hostname) || isAllowedPrivateOrigin(protocol, hostname));
  } catch {
    return false;
  }
}

function isAllowedTcpTarget(host, port) {
  // Bridge открывает TCP/TLS-соединение от имени браузера, поэтому цель
  // ограничена allowlist-ом. Иначе web-страница могла бы использовать bridge
  // как локальный TCP-proxy к произвольному адресу.
  return ALLOWED_HOSTS.has(host) && Number.isInteger(port) && port > 0 && port <= 65535;
}

function sendJson(ws, payload) {
  if (ws.readyState === ws.OPEN) {
    ws.send(JSON.stringify(payload));
  }
}

wss.on("connection", (ws, request) => {
  const origin = request.headers.origin ?? "";
  if (!isAllowedOrigin(origin)) {
    sendJson(ws, { type: "bridge_error", message: "WebSocket origin is not allowed" });
    ws.close(1008, "origin not allowed");
    return;
  }

  let tcp = null;
  let readBuffer = "";

  const closeTcp = () => {
    // destroy безопасен для уже закрытого socket и сразу будит все listeners.
    if (!tcp) {
      return;
    }

    tcp.destroy();
    tcp = null;
  };

  const ensureConnected = (host, port) => {
    // Не открываем второе TCP-соединение для одного web socket. Если UI уже
    // подключен, следующие connect-сообщения считаем идемпотентными.
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

      // C++ сервер говорит построчным TAB-протоколом поверх TCP. TCP может
      // разбить одну строку на несколько chunk-ов или склеить несколько строк,
      // поэтому bridge буферизует данные до '\n'.
      while (boundary >= 0) {
        const line = readBuffer.slice(0, boundary);
        if (line.length > MAX_LINE_LENGTH) {
          sendJson(ws, { type: "tcp_error", message: "TCP line is too large" });
          closeTcp();
          return;
        }
        readBuffer = readBuffer.slice(boundary + 1);
        sendJson(ws, { type: "line", line });
        boundary = readBuffer.indexOf("\n");
      }

      if (readBuffer.length > MAX_LINE_LENGTH) {
        sendJson(ws, { type: "tcp_error", message: "TCP line is too large" });
        closeTcp();
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
      if (!isAllowedTcpTarget(host, port)) {
        sendJson(ws, { type: "bridge_error", message: "TCP target is not allowed" });
        return;
      }
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
      if (line.length > MAX_LINE_LENGTH) {
        sendJson(ws, { type: "bridge_error", message: "Protocol line is too large" });
        return;
      }
      // Клиентский Vue-код хранит команду без перевода строки, а сервер читает
      // поток построчно. Нормализуем здесь, чтобы не дублировать это в UI.
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

wss.on("error", (error) => {
  console.error(`NexTalk bridge failed to listen on ws://${BRIDGE_HOST}:${BRIDGE_PORT}: ${error.message}`);
  process.exitCode = 1;
});

wss.on("listening", () => {
  console.log(`NexTalk bridge listening on ws://${BRIDGE_HOST}:${BRIDGE_PORT}`);
});
