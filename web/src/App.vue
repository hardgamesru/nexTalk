<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from "vue";

type ProtocolMessage = {
  command: string;
  fields: string[];
};

type ChatItem = {
  peerId: string;
  peer: string;
  lastAt: string;
  lastSender: string;
  lastText: string;
  unreadCount: number;
};

type ChatMessage = {
  id: number;
  createdAt: string;
  sender: string;
  recipient: string;
  text: string;
  replyToMessageId: number | null;
  replyToSender: string;
  replyToText: string;
  forwardFromMessageId: number | null;
  forwardFromSender: string;
  forwardFromText: string;
};

type SearchUser = {
  id: string;
  username: string;
};

type AuthMode = "choice" | "login" | "register";

const bridgeUrl = computed(() => `ws://${window.location.hostname}:5174`);

const form = reactive({
  host: "127.0.0.1",
  port: 5555,
  username: "",
  password: "",
});

const auth = reactive({
  visible: true,
  mode: "choice" as AuthMode,
  inFlight: false,
  error: "",
});

const session = reactive({
  loggedIn: false,
  currentUser: "",
  selectedPeer: "",
  profileMenuOpen: false,
});

const connection = reactive({
  bridgeOpen: false,
  tcpConnected: false,
  statusLine: "Not connected",
});

const ui = reactive({
  messageDraft: "",
  chatFilter: "",
  historyLimit: 30,
  showAddChat: false,
  searchQuery: "",
  searchInFlight: false,
  logs: [] as string[],
  replyTarget: null as ChatMessage | null,
  contextMenuVisible: false,
  contextMenuX: 0,
  contextMenuY: 0,
  contextMenuMessageId: 0,
  highlightedMessageId: 0,
  forwardTarget: null as ChatMessage | null,
  showForwardPicker: false,
  forwardPreviewMessage: null as ChatMessage | null,
  forwardRecipient: "",
  forwardDraft: "",
});

const chats = ref<ChatItem[]>([]);
const chatIndex = reactive<Record<string, ChatItem>>({});
const messagesByPeer = reactive<Record<string, ChatMessage[]>>({});
const searchResults = ref<SearchUser[]>([]);
const pendingSearchQuery = ref("");
const messagesViewport = ref<HTMLElement | null>(null);
const logsViewport = ref<HTMLElement | null>(null);
const stickMessagesToBottom = ref(true);
const stickLogsToBottom = ref(true);

let ws: WebSocket | null = null;
let pendingAuth: { mode: "login" | "register"; username: string; password: string } | null = null;
let authCredentials: { username: string; password: string } | null = null;
let highlightTimer: number | null = null;

const filteredChats = computed(() => {
  const query = ui.chatFilter.trim().toLowerCase();
  if (!query) {
    return chats.value;
  }

  return chats.value.filter((chat) => {
    return (
      chat.peer.toLowerCase().includes(query) ||
      `${chat.lastSender}: ${chat.lastText}`.toLowerCase().includes(query)
    );
  });
});

const currentMessages = computed(() => {
  const peer = session.selectedPeer;
  if (!peer) {
    return [] as ChatMessage[];
  }
  return messagesByPeer[peer] ?? [];
});

const profileInitial = computed(() => {
  return (session.currentUser || "?").slice(0, 1).toUpperCase();
});

const forwardableChats = computed(() => {
  return chats.value.filter((chat) => chat.peer !== session.currentUser);
});

function pushLog(text: string) {
  const stamp = new Date().toLocaleString();
  ui.logs.push(`[${stamp}] ${text}`);
  if (ui.logs.length > 200) {
    ui.logs.splice(0, ui.logs.length - 200);
  }
  if (stickLogsToBottom.value) {
    nextTick(() => {
      if (logsViewport.value) {
        logsViewport.value.scrollTop = logsViewport.value.scrollHeight;
      }
    });
  }
}

function formatServerTime(value: string) {
  if (!value) {
    return "Pending...";
  }

  const normalized = value.replace(" ", "T");
  const parsed = new Date(normalized);
  if (Number.isNaN(parsed.getTime())) {
    return value;
  }

  return parsed.toLocaleString();
}

function parseChatMessage(fields: string[], offset = 0) {
  if (fields.length < offset + 5) {
    return null;
  }

  const id = Number.parseInt(fields[offset] ?? "", 10);
  if (!Number.isFinite(id) || id <= 0) {
    return null;
  }

  const replyToRaw = fields[offset + 5] ?? "";
  const replyToMessageId = replyToRaw ? Number.parseInt(replyToRaw, 10) : null;
  const forwardFromRaw = fields[offset + 8] ?? "";
  const forwardFromMessageId = forwardFromRaw ? Number.parseInt(forwardFromRaw, 10) : null;

  return {
    id,
    createdAt: fields[offset + 1] ?? "",
    sender: fields[offset + 2] ?? "",
    recipient: fields[offset + 3] ?? "",
    text: fields[offset + 4] ?? "",
    replyToMessageId: replyToMessageId && Number.isFinite(replyToMessageId) ? replyToMessageId : null,
    replyToSender: fields[offset + 6] ?? "",
    replyToText: fields[offset + 7] ?? "",
    forwardFromMessageId: forwardFromMessageId && Number.isFinite(forwardFromMessageId) ? forwardFromMessageId : null,
    forwardFromSender: fields[offset + 9] ?? "",
    forwardFromText: fields[offset + 10] ?? "",
  } as ChatMessage;
}

function peerForMessage(message: ChatMessage) {
  return message.sender === session.currentUser ? message.recipient : message.sender;
}

function excerpt(value: string, limit = 90) {
  const normalized = value.replace(/\s+/g, " ").trim();
  if (normalized.length <= limit) {
    return normalized;
  }
  return `${normalized.slice(0, limit - 1)}...`;
}

function escapeField(value: string) {
  return value
    .replaceAll("\\", "\\\\")
    .replaceAll("\t", "\\t")
    .replaceAll("\n", "\\n")
    .replaceAll("\r", "\\r");
}

function unescapeField(value: string) {
  let output = "";
  for (let i = 0; i < value.length; i += 1) {
    if (value[i] !== "\\") {
      output += value[i];
      continue;
    }

    if (i + 1 >= value.length) {
      return null;
    }

    i += 1;
    const marker = value[i];
    if (marker === "\\") output += "\\";
    else if (marker === "t") output += "\t";
    else if (marker === "n") output += "\n";
    else if (marker === "r") output += "\r";
    else return null;
  }
  return output;
}

function serialize(command: string, fields: string[] = []) {
  const safeFields = fields.map((field) => escapeField(field));
  return [command, ...safeFields].join("\t");
}

function parseLine(line: string): ProtocolMessage | null {
  const trimmed = line.replace(/[\r\n]+$/g, "");
  if (!trimmed) {
    return null;
  }

  const parts = trimmed.split("\t");
  const fields: string[] = [];
  for (let i = 1; i < parts.length; i += 1) {
    const value = unescapeField(parts[i]);
    if (value === null) {
      return null;
    }
    fields.push(value);
  }

  return { command: parts[0], fields };
}

function sendBridge(payload: object) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    pushLog("Bridge socket is not open");
    return;
  }
  ws.send(JSON.stringify(payload));
}

function sendCommand(command: string, fields: string[] = []) {
  sendBridge({ type: "send", line: serialize(command, fields) });
}

function clearSessionData() {
  session.selectedPeer = "";
  ui.messageDraft = "";
  ui.chatFilter = "";
  ui.replyTarget = null;
  ui.contextMenuVisible = false;
  ui.contextMenuMessageId = 0;
  ui.highlightedMessageId = 0;
  ui.forwardTarget = null;
  ui.showForwardPicker = false;
  ui.forwardPreviewMessage = null;
  ui.forwardRecipient = "";
  ui.forwardDraft = "";
  searchResults.value = [];
  Object.keys(chatIndex).forEach((key) => {
    delete chatIndex[key];
  });
  Object.keys(messagesByPeer).forEach((key) => {
    delete messagesByPeer[key];
  });
  chats.value = [];
}

function clearHighlightTimer() {
  if (highlightTimer !== null) {
    window.clearTimeout(highlightTimer);
    highlightTimer = null;
  }
}

function resetToWelcome(error = "") {
  session.loggedIn = false;
  session.currentUser = "";
  session.profileMenuOpen = false;
  auth.inFlight = false;
  auth.visible = true;
  auth.mode = "choice";
  auth.error = error;
  clearSessionData();
}

function connectBridge() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }

  connection.statusLine = "Connecting to bridge...";
  ws = new WebSocket(bridgeUrl.value);

  ws.onopen = () => {
    connection.bridgeOpen = true;
    connection.statusLine = "Bridge connected";
    sendBridge({ type: "connect", host: form.host, port: Number(form.port) });
  };

  ws.onclose = () => {
    connection.bridgeOpen = false;
    connection.tcpConnected = false;
    connection.statusLine = "Bridge disconnected";
    if (session.loggedIn || auth.inFlight) {
      resetToWelcome("Connection closed. Please sign in again.");
    }
  };

  ws.onerror = () => {
    connection.statusLine = "Bridge socket error";
  };

  ws.onmessage = (event) => {
    let payload: any;
    try {
      payload = JSON.parse(event.data);
    } catch {
      pushLog("Bridge sent invalid JSON");
      return;
    }

    if (payload.type === "ready") {
      pushLog(`Bridge ready on port ${payload.bridgePort}`);
      return;
    }

    if (payload.type === "connected") {
      connection.tcpConnected = true;
      connection.statusLine = `TCP connected to ${payload.host}:${payload.port}`;
      pushLog(connection.statusLine);
      if (pendingAuth) {
        const currentAuth = pendingAuth;
        pendingAuth = null;
        if (currentAuth.mode === "register") {
          sendCommand("register", [currentAuth.username, currentAuth.password]);
        } else {
          sendCommand("login", [currentAuth.username, currentAuth.password]);
        }
      }
      return;
    }

    if (payload.type === "disconnected") {
      connection.tcpConnected = false;
      connection.statusLine = "TCP disconnected";
      if (session.loggedIn || auth.inFlight) {
        resetToWelcome("Server disconnected. Please sign in again.");
      }
      return;
    }

    if (payload.type === "line") {
      const parsed = parseLine(payload.line);
      if (!parsed) {
        pushLog(`Protocol parse error: ${payload.line}`);
        return;
      }
      handleProtocolMessage(parsed);
      return;
    }

    if (payload.type === "bridge_error" || payload.type === "tcp_error") {
      connection.statusLine = payload.message;
      pushLog(`${payload.type}: ${payload.message}`);
    }
  };
}

function beginAuth(mode: "login" | "register") {
  const username = form.username.trim();
  const password = form.password;
  if (!username || !password) {
    auth.error = "Username and password are required.";
    return;
  }

  auth.error = "";
  auth.inFlight = true;
  authCredentials = { username, password };

  if (connection.tcpConnected && ws && ws.readyState === WebSocket.OPEN) {
    if (mode === "register") {
      sendCommand("register", [username, password]);
    } else {
      sendCommand("login", [username, password]);
    }
    return;
  }

  pendingAuth = { mode, username, password };

  if (!ws || ws.readyState !== WebSocket.OPEN) {
    connectBridge();
    return;
  }

  sendBridge({ type: "connect", host: form.host, port: Number(form.port) });
}

function selectPeer(peer: string) {
  session.selectedPeer = peer;
  ui.replyTarget = null;
  ui.contextMenuVisible = false;
  messagesByPeer[peer] = [];
  sendCommand("fetch_history", [peer, String(ui.historyLimit)]);
  nextTick(() => {
    if (messagesViewport.value) {
      messagesViewport.value.scrollTop = messagesViewport.value.scrollHeight;
    }
  });
}

function pushMessage(peer: string, message: ChatMessage) {
  const arr = messagesByPeer[peer] ?? (messagesByPeer[peer] = []);
  const existingIndex = arr.findIndex((item) => item.id === message.id);
  if (existingIndex >= 0) {
    arr.splice(existingIndex, 1, message);
  } else {
    arr.push(message);
  }
  arr.sort((left, right) => left.id - right.id);
  if (arr.length > 400) {
    arr.splice(0, arr.length - 400);
  }

  if (peer === session.selectedPeer && stickMessagesToBottom.value) {
    nextTick(() => {
      if (messagesViewport.value) {
        messagesViewport.value.scrollTop = messagesViewport.value.scrollHeight;
      }
    });
  }
}

function sendMessage() {
  const peer = session.selectedPeer;
  const text = ui.messageDraft.trim();
  if (!peer || !text) {
    return;
  }

  const fields = [peer, text];
  if (ui.replyTarget?.id) {
    fields.push(String(ui.replyTarget.id));
  }

  sendCommand("send_message", fields);
  ui.messageDraft = "";
  ui.replyTarget = null;
  ui.contextMenuVisible = false;
}

function clearChatIndex() {
  Object.keys(chatIndex).forEach((key) => {
    delete chatIndex[key];
  });
  chats.value = [];
}

function fetchChats() {
  clearChatIndex();
  sendCommand("fetch_chats");
}

function openAddChat() {
  ui.showAddChat = true;
  ui.searchQuery = "";
  searchResults.value = [];
}

function closeAddChat() {
  ui.showAddChat = false;
  ui.searchQuery = "";
  searchResults.value = [];
}

function runUserSearch() {
  const query = ui.searchQuery.trim();
  if (!query) {
    return;
  }
  pendingSearchQuery.value = query;
  searchResults.value = [];
  ui.searchInFlight = true;
  sendCommand("search_users", [query]);
}

function createChatFromSearch(user: SearchUser) {
  sendCommand("create_chat", [user.id]);
}

function deleteChat(peer: string) {
  if (!confirm(`Delete chat with ${peer}?`)) {
    return;
  }
  sendCommand("delete_chat", [peer]);
}

function closeContextMenu() {
  ui.contextMenuVisible = false;
  ui.contextMenuMessageId = 0;
}

function openContextMenu(event: MouseEvent, message: ChatMessage) {
  event.preventDefault();
  ui.contextMenuVisible = true;
  ui.contextMenuX = event.clientX;
  ui.contextMenuY = event.clientY;
  ui.contextMenuMessageId = message.id;
}

function beginReply() {
  const message = currentMessages.value.find((item) => item.id === ui.contextMenuMessageId) ?? null;
  if (message) {
    ui.replyTarget = message;
  }
  closeContextMenu();
}

function cancelReply() {
  ui.replyTarget = null;
}

function beginForward() {
  const message = currentMessages.value.find((item) => item.id === ui.contextMenuMessageId) ?? null;
  if (message) {
    ui.forwardTarget = message;
    ui.showForwardPicker = true;
    ui.forwardRecipient = "";
    ui.forwardDraft = "";
  }
  closeContextMenu();
}

function cancelForward() {
  ui.forwardTarget = null;
  ui.showForwardPicker = false;
  ui.forwardRecipient = "";
  ui.forwardDraft = "";
}

function selectForwardPeer(peer: string) {
  ui.forwardRecipient = peer;
}

function submitForward() {
  const peer = ui.forwardRecipient.trim();
  if (!ui.forwardTarget?.id) {
    return;
  }

  if (!peer) {
    return;
  }

  const fields = [peer, String(ui.forwardTarget.id)];
  if (ui.forwardDraft.trim()) {
    fields.push(ui.forwardDraft.trim());
  }

  sendCommand("forward_message", fields);
  pushLog(`Forwarding message #${ui.forwardTarget.id} to ${peer}`);
  cancelForward();
}

function openForwardPreview(message: ChatMessage) {
  if (!message.forwardFromMessageId) {
    return;
  }
  ui.forwardPreviewMessage = message;
}

function closeForwardPreview() {
  ui.forwardPreviewMessage = null;
}

function jumpToMessage(messageId: number | null) {
  if (!messageId) {
    return;
  }

  nextTick(() => {
    const target = document.querySelector<HTMLElement>(`[data-message-id="${messageId}"]`);
    if (!target) {
      pushLog(`Referenced message #${messageId} is not loaded in the current history window`);
      return;
    }

    clearHighlightTimer();
    ui.highlightedMessageId = messageId;
    target.scrollIntoView({ behavior: "smooth", block: "center" });
    highlightTimer = window.setTimeout(() => {
      ui.highlightedMessageId = 0;
      highlightTimer = null;
    }, 1200);
  });
}

function onMessagesScroll() {
  const viewport = messagesViewport.value;
  if (!viewport) {
    return;
  }
  const distance = viewport.scrollHeight - viewport.clientHeight - viewport.scrollTop;
  stickMessagesToBottom.value = distance < 28;
}

function onLogsScroll() {
  const viewport = logsViewport.value;
  if (!viewport) {
    return;
  }
  const distance = viewport.scrollHeight - viewport.clientHeight - viewport.scrollTop;
  stickLogsToBottom.value = distance < 28;
}

function signOut() {
  if (!confirm("Sign out from the current account?")) {
    return;
  }

  session.profileMenuOpen = false;
  if (connection.tcpConnected) {
    sendCommand("quit");
  }
  form.username = "";
  form.password = "";
  resetToWelcome();
}

const dismissMenus = () => {
  closeContextMenu();
};

onMounted(() => {
  window.addEventListener("click", dismissMenus);
  window.addEventListener("scroll", dismissMenus, true);
});

onBeforeUnmount(() => {
  window.removeEventListener("click", dismissMenus);
  window.removeEventListener("scroll", dismissMenus, true);
  clearHighlightTimer();
});

function upsertChat(chat: ChatItem) {
  chatIndex[chat.peer] = chat;
  chats.value = Object.values(chatIndex).sort((left, right) => {
    const leftTime = Date.parse(left.lastAt.replace(" ", "T"));
    const rightTime = Date.parse(right.lastAt.replace(" ", "T"));
    if (!Number.isNaN(leftTime) && !Number.isNaN(rightTime) && leftTime !== rightTime) {
      return rightTime - leftTime;
    }
    return left.peer.localeCompare(right.peer);
  });
}

function handleProtocolMessage(message: ProtocolMessage) {
  const [a = "", b = "", c = "", d = "", e = ""] = message.fields;
  switch (message.command) {
    case "info":
      pushLog(a);
      break;
    case "error":
      pushLog(`Error: ${a}`);
      break;
    case "register_result":
      if (a === "ok") {
        pushLog(`Register ${a}: ${b}`);
        if (authCredentials) {
          sendCommand("login", [authCredentials.username, authCredentials.password]);
        }
      } else {
        pendingAuth = null;
        auth.inFlight = false;
        auth.visible = true;
        auth.mode = "login";
        if (b.toLowerCase().includes("already exists")) {
          auth.error = "Username already exists. Please sign in.";
        } else {
          auth.error = b || "Registration failed";
        }
      }
      break;
    case "login_result":
      if (a === "ok") {
        pendingAuth = null;
        auth.inFlight = false;
        auth.error = "";
        auth.visible = false;
        clearSessionData();
        session.loggedIn = true;
        session.currentUser = authCredentials?.username || form.username.trim();
        fetchChats();
      } else {
        pendingAuth = null;
        auth.inFlight = false;
        auth.visible = true;
        auth.mode = "login";
        auth.error = b || "Login failed";
      }
      pushLog(`Login ${a}: ${b}`);
      break;
    case "send_message_result":
      if (a === "ok") {
        const sentMessage = parseChatMessage(message.fields, 1);
        if (!sentMessage) {
          pushLog("Send result parse error");
          break;
        }

        const peer = peerForMessage(sentMessage);
        pushMessage(peer, sentMessage);
        sendCommand("mark_read", [peer]);
      } else {
        pushLog(`Send error: ${b}`);
      }
      break;
    case "incoming_message": {
      const incomingMessage = parseChatMessage(message.fields);
      if (!incomingMessage) {
        pushLog("Incoming message parse error");
        break;
      }

      const peer = peerForMessage(incomingMessage);
      pushMessage(peer, incomingMessage);
      if (session.selectedPeer === peer) {
        sendCommand("mark_read", [peer]);
      }
      fetchChats();
      break;
    }
    case "history_message": {
      const historyMessage = parseChatMessage(message.fields);
      if (!historyMessage) {
        pushLog("History message parse error");
        break;
      }

      const peer = peerForMessage(historyMessage);
      pushMessage(peer, historyMessage);
      break;
    }
    case "history_result":
      pushLog(`History ${a}: ${b}`);
      if (a == "ok") {
        if (session.selectedPeer) {
          sendCommand("mark_read", [session.selectedPeer]);
        }
        fetchChats();
      }
      break;
    case "chat_item":
      upsertChat({
        peerId: a,
        peer: b,
        lastAt: c,
        lastSender: d,
        lastText: e,
        unreadCount: Number.parseInt(message.fields[5] ?? "0", 10) || 0,
      });
      break;
    case "chat_list_result":
      pushLog(`Chats ${a}: ${b}`);
      break;
    case "user_search_item":
      if (a !== pendingSearchQuery.value) {
        return;
      }
      searchResults.value.push({ id: b, username: c });
      break;
    case "user_search_result":
      ui.searchInFlight = false;
      pushLog(`User search ${a}: ${b}`);
      break;
    case "create_chat_result":
      if (a === "ok") {
        closeAddChat();
        fetchChats();
        if (c) {
          session.selectedPeer = c;
          selectPeer(c);
        }
      } else {
        pushLog(`Create chat error: ${b}`);
      }
      break;
    case "delete_chat_result":
      if (a === "ok") {
        const removedPeer = c;
        delete chatIndex[removedPeer];
        chats.value = Object.values(chatIndex);
        delete messagesByPeer[removedPeer];
        if (session.selectedPeer === removedPeer) {
          session.selectedPeer = "";
          ui.replyTarget = null;
        }
      } else {
        pushLog(`Delete chat error: ${b}`);
      }
      break;
    case "mark_read_result":
      if (a === "ok") {
        const peer = b;
        if (chatIndex[peer]) {
          chatIndex[peer].unreadCount = 0;
          chats.value = Object.values(chatIndex);
        }
      } else {
        pushLog("Mark read failed");
      }
      break;
    default:
      pushLog(`Unhandled command: ${message.command}`);
      break;
  }
}

watch(
  () => currentMessages.value.length,
  () => {
    if (stickMessagesToBottom.value) {
      nextTick(() => {
        if (messagesViewport.value) {
          messagesViewport.value.scrollTop = messagesViewport.value.scrollHeight;
        }
      });
    }
  },
);
</script>

<template>
  <div class="app-shell">
    <aside class="sidebar">
      <div class="sidebar-top">
        <div class="tcp-badge">{{ connection.statusLine }}</div>
        <button
          class="profile-btn"
          :disabled="!session.loggedIn"
          @click="session.profileMenuOpen = !session.profileMenuOpen"
        >
          {{ profileInitial }}
        </button>
      </div>
      <div v-if="session.profileMenuOpen" class="profile-menu">
        <strong>{{ session.currentUser }}</strong>
        <button class="danger small signout-btn" @click="signOut">Sign out</button>
      </div>

      <section class="panel chats-panel">
        <div class="panel-row">
          <h2>Chats</h2>
          <button class="small" @click="openAddChat" :disabled="!session.loggedIn">New</button>
        </div>
        <input v-model="ui.chatFilter" placeholder="Search chats" />
        <div class="chat-list">
          <button
            v-for="chat in filteredChats"
            :key="chat.peer"
            class="chat-row"
            :class="{ active: session.selectedPeer === chat.peer }"
            @click="selectPeer(chat.peer)"
          >
            <div class="chat-row-main">
              <strong>{{ chat.peer }}</strong>
              <span class="preview">
                {{ chat.lastText ? `${chat.lastSender}: ${chat.lastText}` : "No messages yet" }}
              </span>
            </div>
            <div class="chat-row-actions">
              <span v-if="chat.unreadCount > 0 && session.selectedPeer !== chat.peer" class="unread-dot"></span>
              <button class="chat-delete" @click.stop="deleteChat(chat.peer)">x</button>
            </div>
          </button>
        </div>
      </section>
    </aside>

    <main class="chat-main">
      <header class="topbar">
        <div>
          <h2>{{ session.selectedPeer ? session.selectedPeer : "Привет" }}</h2>
          <p>
            {{
              session.selectedPeer
                ? (session.loggedIn ? `Signed in as ${session.currentUser}` : "Not signed in")
                : "Выберите чат слева"
            }}
          </p>
        </div>
        <div class="top-actions">
          <label>History limit</label>
          <input v-model.number="ui.historyLimit" type="number" min="1" max="100" />
          <button class="small" @click="fetchChats" :disabled="!session.loggedIn">Refresh</button>
        </div>
      </header>

      <section ref="messagesViewport" class="messages" @scroll="onMessagesScroll">
        <div v-if="!session.selectedPeer" class="empty-chat">
          <p>Выберите чат в списке слева, чтобы увидеть переписку.</p>
        </div>
        <template v-for="(message, idx) in currentMessages" :key="message.id || idx">
          <article
            class="message-row"
            :class="{ own: message.sender === session.currentUser, highlighted: ui.highlightedMessageId === message.id }"
            :data-message-id="message.id"
            @contextmenu="openContextMenu($event, message)"
          >
            <div class="avatar">{{ (message.sender || "?").slice(0, 1).toUpperCase() }}</div>
            <div class="bubble">
              <div class="meta">
                <strong>{{ message.sender }}</strong>
                <small>{{ formatServerTime(message.createdAt) }}</small>
              </div>
              <button
                v-if="message.forwardFromMessageId"
                class="forward-snippet"
                type="button"
                @click="openForwardPreview(message)"
              >
                <strong>Forwarded from {{ message.forwardFromSender || "Unknown" }}</strong>
                <span>{{ excerpt(message.forwardFromText || message.text) }}</span>
              </button>
              <button
                v-if="message.replyToMessageId"
                class="reply-snippet"
                type="button"
                @click="jumpToMessage(message.replyToMessageId)"
              >
                <strong>{{ message.replyToSender || "Message" }}</strong>
                <span>{{ excerpt(message.replyToText || "Deleted message") }}</span>
              </button>
              <p v-if="message.text">{{ message.text }}</p>
            </div>
          </article>
        </template>

        <div
          v-if="ui.contextMenuVisible"
          class="message-menu"
          :style="{ left: `${ui.contextMenuX}px`, top: `${ui.contextMenuY}px` }"
        >
          <button class="message-menu-item" @click.stop="beginReply">Reply</button>
          <button class="message-menu-item" @click.stop="beginForward">Forward</button>
        </div>
      </section>

      <footer class="composer">
        <div class="composer-main">
          <div v-if="ui.replyTarget" class="reply-draft">
            <div class="reply-draft-copy">
              <strong>{{ ui.replyTarget.sender }}</strong>
              <span>{{ excerpt(ui.replyTarget.text) }}</span>
            </div>
            <button class="reply-cancel" @click="cancelReply">x</button>
          </div>
          <div class="composer-row">
            <input
              v-model="ui.messageDraft"
              type="text"
              placeholder="Write a message"
              :disabled="!session.selectedPeer || !session.loggedIn"
              @keydown.enter.prevent="sendMessage"
            />
            <button class="primary" @click="sendMessage" :disabled="!ui.messageDraft.trim() || !session.selectedPeer">
              Send
            </button>
          </div>
        </div>
      </footer>
    </main>

    <section class="log-panel">
      <h3>Session log</h3>
      <div ref="logsViewport" class="logs" @scroll="onLogsScroll">
        <p v-for="(line, idx) in ui.logs" :key="idx">{{ line }}</p>
      </div>
    </section>

    <section v-if="ui.showAddChat" class="modal-wrap">
      <div class="modal">
        <button class="modal-close" type="button" @click="closeAddChat" aria-label="Close">x</button>
        <h3>Create Chat</h3>
        <p>Find users by username and create a private chat.</p>
        <div class="modal-search">
          <input v-model="ui.searchQuery" type="text" placeholder="username" @keydown.enter.prevent="runUserSearch" />
          <button class="small" @click="runUserSearch" :disabled="ui.searchInFlight">
            {{ ui.searchInFlight ? "Searching..." : "Search" }}
          </button>
        </div>
        <div class="search-results">
          <button
            v-for="item in searchResults"
            :key="item.id"
            class="search-row"
            @click="createChatFromSearch(item)"
          >
            <strong>{{ item.username }}</strong>
            <small>#{{ item.id }}</small>
          </button>
          <p v-if="!ui.searchInFlight && searchResults.length === 0">No results</p>
        </div>
      </div>
    </section>

    <section v-if="ui.showForwardPicker && ui.forwardTarget" class="modal-wrap" @click.self="cancelForward">
      <div class="modal forward-modal">
        <button class="modal-close" type="button" @click="cancelForward" aria-label="Close">x</button>
        <h3>Forward message</h3>
        <p>Choose who should receive this message and add an optional comment.</p>
        <div class="forward-preview-card">
          <strong>{{ ui.forwardTarget.sender }}</strong>
          <span>{{ excerpt(ui.forwardTarget.text, 160) }}</span>
        </div>
        <div class="forward-section-title">Кому</div>
        <div class="forward-chat-list">
          <button
            v-for="chat in forwardableChats"
            :key="`forward-${chat.peer}`"
            class="forward-chat-row"
            :class="{ active: ui.forwardRecipient === chat.peer }"
            @click="selectForwardPeer(chat.peer)"
          >
            <strong>{{ chat.peer }}</strong>
            <span>{{ chat.lastText ? `${chat.lastSender}: ${chat.lastText}` : "No messages yet" }}</span>
          </button>
          <p v-if="forwardableChats.length === 0">Create another chat first, then forward messages there.</p>
        </div>
        <div class="forward-compose">
          <label>Add a comment</label>
          <textarea
            v-model="ui.forwardDraft"
            rows="3"
            placeholder="Optional text"
          ></textarea>
        </div>
        <div class="modal-actions">
          <button class="primary" @click="submitForward" :disabled="!ui.forwardRecipient">Send</button>
        </div>
      </div>
    </section>

    <section v-if="ui.forwardPreviewMessage" class="modal-wrap" @click.self="closeForwardPreview">
      <div class="modal forward-view-modal">
        <h3>Forwarded message</h3>
        <p>This is the original content preserved inside the forwarded message.</p>
        <div class="forward-view-card">
          <strong>{{ ui.forwardPreviewMessage.forwardFromSender || "Unknown" }}</strong>
          <span>{{ ui.forwardPreviewMessage.forwardFromText || ui.forwardPreviewMessage.text }}</span>
        </div>
        <div class="modal-actions">
          <button class="small" @click="closeForwardPreview">Close</button>
        </div>
      </div>
    </section>

    <section v-if="auth.visible" class="modal-wrap auth-modal-wrap">
      <div class="modal auth-modal">
        <h3>NexTalk</h3>
        <p v-if="auth.mode === 'choice'">Choose how you want to continue.</p>
        <p v-else>{{ auth.mode === "login" ? "Sign in to your account" : "Create a new account" }}</p>

        <div v-if="auth.mode === 'choice'" class="welcome-actions">
          <button class="primary" @click="auth.mode = 'login'; auth.error = ''">Sign in</button>
          <button class="secondary" @click="auth.mode = 'register'; auth.error = ''">Create account</button>
        </div>

        <div v-else class="auth-form">
          <label>Host</label>
          <input v-model="form.host" type="text" />
          <label>Port</label>
          <input v-model.number="form.port" type="number" min="1" max="65535" />
          <label>Username</label>
          <input v-model="form.username" type="text" />
          <label>Password</label>
          <input v-model="form.password" type="password" />

          <p v-if="auth.error" class="auth-error">{{ auth.error }}</p>

          <div class="auth-actions">
            <button class="small" @click="auth.mode = 'choice'; auth.error = ''" :disabled="auth.inFlight">Back</button>
            <button class="primary" @click="beginAuth(auth.mode)" :disabled="auth.inFlight">
              {{ auth.inFlight ? "Please wait..." : auth.mode === "login" ? "Sign in" : "Create & sign in" }}
            </button>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>
