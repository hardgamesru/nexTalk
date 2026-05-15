<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from "vue";
import { CACHE_LIMIT_PER_CHAT, HISTORY_PAGE_SIZE } from "./cacheConfig";
import {
  clearAiChatHistory,
  loadAiChatHistory,
  openAiChatCacheDb,
  saveAiChatHistory,
  type AIAttachment,
  type AIChatMessage,
} from "./aiChatCache";
import {
  loadCachedMessages,
  openMessageCacheDb,
  saveMessageToCache,
  saveMessagesToCache,
  trimCachedMessages,
  type ChatMessage,
} from "./messageCache";

type ProtocolMessage = {
  command: string;
  fields: string[];
};

// ChatItem приходит из серверной команды chat_item и используется только для
// списка диалогов слева. Полная история сообщений хранится отдельно.
type ChatItem = {
  peerId: string;
  peer: string;
  kind: "dm" | "group";
  lastAt: string;
  lastSender: string;
  lastText: string;
  unreadCount: number;
  canManage: boolean;
};

type SearchUser = {
  id: string;
  username: string;
};

type UserProfile = {
  username: string;
  displayName: string;
  bio: string;
  avatarColor: string;
  createdAt: string;
  lastSeen: string;
  online: boolean;
};

type AuthMode = "choice" | "login" | "register";

// Текст удаленного сообщения должен совпадать с серверной заглушкой, чтобы
// preview в списке чатов и пузырь в переписке выглядели одинаково.
const DELETED_MESSAGE_TEXT = "Сообщение удалено";
const AI_CHAT_ID = "__ai__";
const AI_CHAT_NAME = "NexTalk AI";
const AI_CONTEXT_LIMIT = 12;
const AI_SYSTEM_PROMPT =
  "Ты NexTalk AI, полезный ассистент внутри мессенджера. Сосредоточься на последнем запросе пользователя и прикрепленном пересланном сообщении, если оно есть. Отвечай прямо, естественно и на языке последнего сообщения пользователя, если он явно не попросил другой язык. Не добавляй вступления вроде 'Вот ответ'.";
const AI_REWRITE_PRESETS = [
  "Официальнее",
  "Дружелюбнее",
  "Короче",
  "Подробнее",
  "С юмором",
  "С извинением",
  "Увереннее",
  "Исправить текст",
  "Перевести на английский",
] as const;

const bridgeUrl = computed(() => `ws://${window.location.hostname}:5174`);
const aiServiceUrl = computed(() => {
  const configuredUrl = import.meta.env.VITE_AI_SERVICE_URL;
  return configuredUrl ? configuredUrl.replace(/\/$/, "") : `http://${window.location.hostname}:5050`;
});

// Данные подключения к C++ серверу. Web-клиент не ходит в TCP напрямую:
// сначала он подключается к Node bridge, а bridge уже открывает TLS socket.
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
  statusLine: "Нет подключения",
});

// ui содержит только состояние интерфейса: открытые модалки, черновики,
// выделения и временные флаги загрузки. Серверная модель здесь не хранится.
const ui = reactive({
  messageDraft: "",
  chatFilter: "",
  showNewChatChoice: false,
  showAddChat: false,
  showCreateGroup: false,
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
  createGroupName: "",
  createGroupAdmin: "",
  createGroupSelectedUsers: [] as SearchUser[],
  createGroupSearchQuery: "",
  createGroupSearchInFlight: false,
  createGroupSearchResults: [] as SearchUser[],
  showGroupSettings: false,
  groupSettingsLoading: false,
  groupSettingsTitle: "",
  groupSettingsChatId: "",
  groupSettingsAdminUsername: "",
  groupSettingsCanManage: false,
  groupSettingsMembers: [] as { username: string; isAdmin: boolean }[],
  groupAddSearchQuery: "",
  groupAddSearchInFlight: false,
  groupAddSearchResults: [] as SearchUser[],
  profileModalOpen: false,
  profileModalLoading: false,
  profileViewMode: "view" as "view" | "edit",
  selectedProfileUsername: "",
  aiSending: false,
  aiReplyModalOpen: false,
  aiReplyLoading: false,
  aiReplyTargetPeer: "",
  aiReplySourceMessage: null as ChatMessage | null,
  aiReplyInstruction: "",
  aiReplySuggestion: "",
  aiRewriteMenuOpen: false,
  aiRewriteLoading: false,
  aiRewriteError: "",
  aiRewriteCustomOpen: false,
  aiRewriteCustomInstruction: "",
  aiPendingAttachment: null as AIAttachment | null,
  profileForm: {
    username: "",
    displayName: "",
    bio: "",
    avatarColor: "#5C7CFA",
    createdAt: "",
    lastSeen: "",
    online: false,
  },
});

const chats = ref<ChatItem[]>([]);
// chatIndex нужен для быстрых обновлений по peerId без полного пересоздания
// списка чатов при каждом incoming_message.
const chatIndex = reactive<Record<string, ChatItem>>({});
// История сообщений хранится по peerId: username для личных чатов и group:<id>
// для групповых. Это тот же ключ, который сервер ожидает в командах истории.
const messagesByPeer = reactive<Record<string, ChatMessage[]>>({});
const profilesByUsername = reactive<Record<string, UserProfile>>({});
const aiMessages = ref<AIChatMessage[]>([]);
const searchResults = ref<SearchUser[]>([]);
const pendingSearchQuery = ref("");
const messagesViewport = ref<HTMLElement | null>(null);
const logsViewport = ref<HTMLElement | null>(null);
const stickMessagesToBottom = ref(true);
const stickLogsToBottom = ref(true);
const loadingOlderByPeer = reactive<Record<string, boolean>>({});
const reachedHistoryStartByPeer = reactive<Record<string, boolean>>({});

// Эти словари не reactive, потому что они управляют сетевыми запросами, а не
// напрямую отрисовываются в шаблоне.
const syncingRecentByPeer: Record<string, boolean> = {};
const historyRequestModeByPeer: Record<string, "latest" | "older"> = {};
const historyBatchCountByPeer: Record<string, number> = {};
const olderScrollStateByPeer: Record<string, { oldScrollHeight: number; oldScrollTop: number }> = {};
const chatFetchSeenPeerIds = new Set<string>();
let chatFetchInFlight = false;
let chatFetchQueued = false;

let ws: WebSocket | null = null;
let pendingAuth: { mode: "login" | "register"; username: string; password: string } | null = null;
let authCredentials: { username: string; password: string } | null = null;
let highlightTimer: number | null = null;
let pendingProfilesBatch = new Set<string>();

const filteredChats = computed(() => {
  const query = ui.chatFilter.trim().toLowerCase();
  if (!query) {
    return chats.value;
  }

  return chats.value.filter((chat) => {
    return (
      chatDisplayName(chat).toLowerCase().includes(query) ||
      `${chatPreviewSenderLabel(chat)}: ${chat.lastText}`.toLowerCase().includes(query)
    );
  });
});

// AI-чат не приходит с сервера как обычный chat_item, но должен жить рядом с
// остальными чатами и участвовать в поиске по списку.
const showAiChatRow = computed(() => {
  const query = ui.chatFilter.trim().toLowerCase();
  if (!query) {
    return true;
  }
  return AI_CHAT_NAME.toLowerCase().includes(query) || "assistant ai ollama".includes(query);
});

const currentMessages = computed(() => {
  const peer = session.selectedPeer;
  if (!peer || peer === AI_CHAT_ID) {
    return [] as ChatMessage[];
  }
  return messagesByPeer[peer] ?? [];
});

const isAiChatSelected = computed(() => session.selectedPeer === AI_CHAT_ID);

const currentAiMessages = computed(() => {
  return isAiChatSelected.value ? aiMessages.value : [];
});

const selectedChat = computed(() => {
  const chatId = session.selectedPeer;
  return chatId ? chatIndex[chatId] ?? null : null;
});

const selectedChatTitle = computed(() => {
  if (isAiChatSelected.value) {
    return AI_CHAT_NAME;
  }
  if (!selectedChat.value) {
    return "";
  }
  return selectedChat.value.kind === "dm"
    ? getProfileDisplayName(selectedChat.value.peerId)
    : selectedChat.value.peer;
});

const canOpenGroupSettings = computed(() => {
  return !isAiChatSelected.value && selectedChat.value?.kind === "group";
});

const canOpenDirectProfile = computed(() => {
  return !isAiChatSelected.value && selectedChat.value?.kind === "dm";
});

const selectedDirectProfile = computed(() => {
  if (!canOpenDirectProfile.value || !selectedChat.value) {
    return null;
  }
  return profilesByUsername[selectedChat.value.peerId] ?? fallbackProfile(selectedChat.value.peerId);
});

const selectedDirectPresenceText = computed(() => {
  return formatLastSeen(selectedDirectProfile.value);
});

const selectedDirectPresenceOnline = computed(() => {
  return Boolean(selectedDirectProfile.value?.online);
});

const profileInitial = computed(() => {
  return getProfileInitials(session.currentUser || "?");
});

const currentUserAvatarColor = computed(() => {
  return getProfileAvatarColor(session.currentUser || "?");
});

const forwardableChats = computed(() => {
  return chats.value.filter((chat) => !(chat.kind === "dm" && chat.peer === session.currentUser));
});

const activeContextMessage = computed(() => {
  return currentMessages.value.find((item) => item.id === ui.contextMenuMessageId) ?? null;
});

function pushLog(text: string) {
  // Журнал ограничен последними 200 строками, чтобы длительная сессия не
  // раздувала память браузера.
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
    return "Ожидается...";
  }

  const normalized = value.replace(" ", "T");
  const parsed = new Date(normalized);
  if (Number.isNaN(parsed.getTime())) {
    return value;
  }

  return parsed.toLocaleString();
}

function stableAvatarColor(username: string) {
  // Цвет fallback-аватара должен быть стабильным между перезагрузками, поэтому
  // выбираем его через простой hash от username, а не через Math.random().
  const palette = ["#5C7CFA", "#E56B6F", "#3FA37A", "#F4A261", "#7B6CF6", "#4D908E", "#577590", "#BC6C25", "#C8553D", "#8A5CF6"];
  let hash = 2166136261;
  for (let index = 0; index < username.length; index += 1) {
    hash ^= username.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return palette[Math.abs(hash) % palette.length];
}

function computeInitials(displayName: string, username: string) {
  const source = (displayName || username).trim();
  if (!source) {
    return "?";
  }

  const parts = source.split(/\s+/).filter(Boolean);
  if (parts.length >= 2) {
    return `${parts[0][0] ?? ""}${parts[1][0] ?? ""}`.toUpperCase();
  }
  return source.slice(0, 2).toUpperCase();
}

function nextAiMessageId() {
  return `ai-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

async function loadAiMessagesForUser(username: string) {
  if (!username) {
    aiMessages.value = [];
    return;
  }

  try {
    const parsed = await loadAiChatHistory(username);
    // IndexedDB хранит данные в браузере, поэтому при чтении не доверяем типам
    // полностью и нормализуем каждое поле перед показом в интерфейсе.
    aiMessages.value = parsed
      .filter((item) => item && typeof item === "object")
      .map((item) => {
        const role: AIChatMessage["role"] =
          item.role === "assistant" || item.role === "error"
            ? item.role
            : "user";

        return {
          id: typeof item.id === "string" ? item.id : nextAiMessageId(),
          role,
          content: typeof item.content === "string" ? item.content : "",
          createdAt: typeof item.createdAt === "string" ? item.createdAt : new Date().toISOString(),
          attachment:
            item.attachment &&
            typeof item.attachment === "object" &&
            typeof item.attachment.sender === "string" &&
            typeof item.attachment.text === "string"
              ? {
                  sender: item.attachment.sender,
                  text: item.attachment.text,
                  sourceChatId: typeof item.attachment.sourceChatId === "string" ? item.attachment.sourceChatId : "",
                  sourceMessageId:
                    typeof item.attachment.sourceMessageId === "number" ? item.attachment.sourceMessageId : 0,
                }
              : null,
        };
      })
      .filter((item) => item.content.trim() || item.attachment);
  } catch (error) {
    console.error("Failed to load AI chat history", error);
    aiMessages.value = [];
  }
}

async function persistAiMessages() {
  if (!session.currentUser) {
    return;
  }

  try {
    await saveAiChatHistory(session.currentUser, aiMessages.value);
  } catch (error) {
    console.error("Failed to persist AI chat history", error);
  }
}

async function clearAiChat() {
  aiMessages.value = [];
  ui.aiPendingAttachment = null;
  ui.aiSending = false;
  ui.messageDraft = "";
  if (session.currentUser) {
    try {
      await clearAiChatHistory(session.currentUser);
    } catch (error) {
      console.error("Failed to clear AI chat history", error);
    }
  }
}

function aiPreviewText() {
  const lastMessage = aiMessages.value[aiMessages.value.length - 1];
  if (!lastMessage) {
    return "AI-помощник для чатов";
  }
  if (lastMessage.role === "user") {
    return excerpt(lastMessage.content || (lastMessage.attachment?.text ?? "Пересланное сообщение"), 48);
  }
  return excerpt(lastMessage.content || "Готов помочь", 48);
}

function openAiChat() {
  if (!session.loggedIn) {
    return;
  }
  session.selectedPeer = AI_CHAT_ID;
  ui.replyTarget = null;
  ui.contextMenuVisible = false;
  nextTick(() => {
    scrollMessagesToBottom();
  });
}

function cancelAiAttachment() {
  ui.aiPendingAttachment = null;
}

function buildAiAttachment(message: ChatMessage): AIAttachment {
  // Если пользователь пересылает уже пересланное сообщение в AI, берем
  // оригинального отправителя и оригинальный текст, а не оболочку пересылки.
  const sender = message.forwardFromSender || message.sender;
  const text = message.forwardFromText || message.text;
  return {
    sender,
    text,
    sourceChatId: message.chatId || peerForMessage(message),
    sourceMessageId: message.id,
  };
}

function buildAiRequestContent(instruction: string, attachment: AIAttachment | null) {
  if (!attachment) {
    return instruction;
  }

  return `Пересланное сообщение:\nОтправитель: ${attachment.sender}\nТекст: ${attachment.text}\n\nЗадача пользователя:\n${instruction}`;
}

async function requestAiAnswer(messages: { role: string; content: string }[]) {
  // UI общается только с локальным ai_service. Выбор Ollama/GigaChat спрятан
  // за этим сервисом, поэтому фронтенд не хранит ключи провайдеров.
  const response = await fetch(`${aiServiceUrl.value}/chat`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ messages }),
  });

  const data = await response.json().catch(() => null);
  if (!response.ok || !data || data.status !== "ok" || typeof data.answer !== "string") {
    const errorText =
      data && typeof data.error === "string"
        ? data.error
        : "AI-сервис сейчас недоступен.";
    throw new Error(errorText);
  }

  return data.answer.trim();
}

function aiMessageToProviderMessage(message: AIChatMessage) {
  // Роль "error" нужна только UI. В контекст модели такие сообщения отправляем
  // как assistant, чтобы провайдер получил валидный формат.
  const role = message.role === "user" ? "user" : "assistant";
  return {
    role,
    content: buildAiRequestContent(message.content, message.attachment),
  };
}

function appendAiMessage(message: AIChatMessage) {
  // Присваиваем новый массив, а не push(), чтобы Vue гарантированно увидел
  // изменение ref и обновил список сообщений.
  aiMessages.value = [...aiMessages.value, message];
  void persistAiMessages();
  nextTick(() => {
    scrollMessagesToBottom();
  });
}

async function sendAiMessage() {
  const instruction = ui.messageDraft.trim();
  const attachment = ui.aiPendingAttachment;
  if (!session.loggedIn || ui.aiSending || (!instruction && !attachment)) {
    return;
  }

  if (attachment && !instruction) {
    pushLog("Для прикрепленного сообщения нужна инструкция для AI");
    return;
  }

  const userMessage: AIChatMessage = {
    id: nextAiMessageId(),
    role: "user",
    content: instruction,
    createdAt: new Date().toISOString(),
    attachment,
  };

  appendAiMessage(userMessage);
  ui.aiPendingAttachment = null;
  ui.messageDraft = "";
  ui.aiSending = true;

  // В провайдер отправляем только последние пары user/assistant. Полная история
  // остается в локальном кэше, но не раздувает каждый AI-запрос.
  const conversation = aiMessages.value
    .filter((item) => item.role === "user" || item.role === "assistant")
    .slice(-AI_CONTEXT_LIMIT)
    .map(aiMessageToProviderMessage);

  const requestMessages = [
    { role: "system", content: AI_SYSTEM_PROMPT },
    ...conversation,
  ];

  try {
    const answer = await requestAiAnswer(requestMessages);
    appendAiMessage({
      id: nextAiMessageId(),
      role: "assistant",
      content: answer,
      createdAt: new Date().toISOString(),
      attachment: null,
    });
  } catch (error) {
    appendAiMessage({
      id: nextAiMessageId(),
      role: "error",
      content: error instanceof Error
        ? error.message
        : "AI-сервис недоступен. Проверьте, что ai_service запущен.",
      createdAt: new Date().toISOString(),
      attachment: null,
    });
  } finally {
    ui.aiSending = false;
  }
}

function openAiReplyModal(message: ChatMessage) {
  if (isMessageDeleted(message)) {
    pushLog("Нельзя сгенерировать ответ на удаленное сообщение");
    return;
  }

  ui.aiReplyModalOpen = true;
  ui.aiReplyLoading = false;
  ui.aiReplyTargetPeer = session.selectedPeer;
  ui.aiReplySourceMessage = message;
  ui.aiReplyInstruction = "Напиши короткий вежливый ответ, который подходит к этой переписке.";
  ui.aiReplySuggestion = "";
}

function closeAiReplyModal() {
  ui.aiReplyModalOpen = false;
  ui.aiReplyLoading = false;
  ui.aiReplyTargetPeer = "";
  ui.aiReplySourceMessage = null;
  ui.aiReplyInstruction = "";
  ui.aiReplySuggestion = "";
}

async function generateAiReply() {
  const sourceMessage = ui.aiReplySourceMessage;
  if (!sourceMessage || ui.aiReplyLoading) {
    return;
  }

  const attachment = buildAiAttachment(sourceMessage);
  const instruction = ui.aiReplyInstruction.trim() || "Напиши короткий вежливый ответ, который подходит к этой переписке.";

  ui.aiReplyLoading = true;
  try {
    const answer = await requestAiAnswer([
      {
        role: "system",
        content:
          "Сгенерируй естественный черновик ответа в чате. Сохрани язык исходного сообщения, если пользователь явно не попросил другой язык. Верни только текст ответа, без пояснений и вступлений.",
      },
      {
        role: "user",
        content:
          `Сообщение, на которое нужно ответить:\nОтправитель: ${attachment.sender}\nТекст: ${attachment.text}\n\nИнструкция пользователя:\n${instruction}`,
      },
    ]);
    ui.aiReplySuggestion = answer;
  } catch (error) {
    pushLog(error instanceof Error ? `Ошибка AI-ответа: ${error.message}` : "Ошибка AI-ответа");
    ui.aiReplySuggestion = "";
  } finally {
    ui.aiReplyLoading = false;
  }
}

function insertAiReplyToChat() {
  if (!ui.aiReplySuggestion.trim()) {
    return;
  }
  ui.messageDraft = ui.aiReplySuggestion.trim();
  closeAiReplyModal();
}

function closeAiRewriteMenu() {
  ui.aiRewriteMenuOpen = false;
  ui.aiRewriteLoading = false;
  ui.aiRewriteCustomOpen = false;
  ui.aiRewriteCustomInstruction = "";
}

function toggleAiRewriteMenu() {
  if (!session.loggedIn || !session.selectedPeer || isAiChatSelected.value) {
    return;
  }

  if (!ui.messageDraft.trim()) {
    ui.aiRewriteMenuOpen = false;
    return;
  }

  ui.aiRewriteError = "";
  ui.aiRewriteMenuOpen = !ui.aiRewriteMenuOpen;
  if (!ui.aiRewriteMenuOpen) {
    ui.aiRewriteCustomOpen = false;
    ui.aiRewriteCustomInstruction = "";
  }
}

async function rewriteDraftWithAi(instruction: string) {
  const draft = ui.messageDraft.trim();
  if (!draft) {
    ui.aiRewriteError = "Сначала напишите черновик";
    return;
  }

  ui.aiRewriteLoading = true;
  ui.aiRewriteError = "";
  try {
    // AI-переписывание работает только с текущим черновиком, без истории чата:
    // так модель не утягивает личный контекст собеседника без необходимости.
    const answer = await requestAiAnswer([
      {
        role: "system",
        content:
          "Перепиши черновик сообщения для чата. Сохрани язык исходного черновика, если пользователь явно не попросил перевод. Верни только переписанный текст, без пояснений и вступлений.",
      },
      {
        role: "user",
        content: `Перепиши этот черновик по инструкции:\n${instruction}\n\nЧерновик:\n${draft}`,
      },
    ]);
    ui.messageDraft = answer;
    closeAiRewriteMenu();
  } catch (error) {
    ui.aiRewriteError = error instanceof Error ? error.message : "Не удалось переписать текст через AI";
  } finally {
    ui.aiRewriteLoading = false;
  }
}

function chooseAiRewritePreset(preset: (typeof AI_REWRITE_PRESETS)[number]) {
  void rewriteDraftWithAi(preset);
}

function openCustomAiRewrite() {
  ui.aiRewriteCustomOpen = true;
  ui.aiRewriteError = "";
}

function submitCustomAiRewrite() {
  const instruction = ui.aiRewriteCustomInstruction.trim();
  if (!instruction) {
    return;
  }
  void rewriteDraftWithAi(instruction);
}

function fallbackProfile(username: string): UserProfile {
  return {
    username,
    displayName: username,
    bio: "",
    avatarColor: stableAvatarColor(username),
    createdAt: "",
    lastSeen: "",
    online: false,
  };
}

function applyProfileFromFields(fields: string[]) {
  const username = fields[0] ?? "";
  if (!username) {
    return null;
  }

  // Порядок полей должен совпадать с server/src/MessengerServer.cpp
  // buildUserProfileFields(). Здесь превращаем строковый online-флаг в boolean.
  const profile: UserProfile = {
    username,
    displayName: fields[1] || username,
    bio: fields[2] || "",
    avatarColor: fields[3] || stableAvatarColor(username),
    createdAt: fields[4] || "",
    lastSeen: fields[5] || "",
    online: (fields[6] ?? "0") === "1",
  };

  profilesByUsername[username] = profile;

  if (ui.selectedProfileUsername === username) {
    ui.profileForm.username = profile.username;
    ui.profileForm.displayName = profile.displayName;
    ui.profileForm.bio = profile.bio;
    ui.profileForm.avatarColor = profile.avatarColor;
    ui.profileForm.createdAt = profile.createdAt;
    ui.profileForm.lastSeen = profile.lastSeen;
    ui.profileForm.online = profile.online;
    ui.profileModalLoading = false;
  }

  return profile;
}

function requestProfile(username: string) {
  const trimmedUsername = username.trim();
  if (!session.loggedIn || !trimmedUsername) {
    return;
  }
  sendCommand("get_profile", [trimmedUsername]);
}

function requestProfiles(usernames: string[]) {
  if (!session.loggedIn) {
    return;
  }

  // Профили запрашиваются пачкой и dedupe-ятся, чтобы при загрузке истории
  // не отправлять get_profile для каждого сообщения отдельно.
  const unique = Array.from(new Set(usernames.map((username) => username.trim()).filter(Boolean)));
  const missing = unique.filter((username) => !profilesByUsername[username] && !pendingProfilesBatch.has(username));
  if (missing.length === 0) {
    return;
  }

  missing.forEach((username) => pendingProfilesBatch.add(username));
  sendCommand("get_profiles", [missing.join(",")]);
}

function getProfileDisplayName(username: string) {
  return profilesByUsername[username]?.displayName || username;
}

function getProfileInitials(username: string) {
  const profile = profilesByUsername[username] ?? fallbackProfile(username);
  return computeInitials(profile.displayName, profile.username);
}

function getProfileAvatarColor(username: string) {
  return profilesByUsername[username]?.avatarColor || stableAvatarColor(username);
}

function formatLastSeen(profile: UserProfile | null) {
  if (!profile) {
    return "";
  }
  if (profile.online) {
    return "онлайн";
  }
  if (!profile.lastSeen) {
    return "офлайн";
  }
  return `был(а) ${formatServerTime(profile.lastSeen)}`;
}

function formatProfileFormStatus() {
  return formatLastSeen({
    username: ui.profileForm.username,
    displayName: ui.profileForm.displayName,
    bio: ui.profileForm.bio,
    avatarColor: ui.profileForm.avatarColor,
    createdAt: ui.profileForm.createdAt,
    lastSeen: ui.profileForm.lastSeen,
    online: ui.profileForm.online,
  });
}

function openOwnProfileEditor() {
  const username = session.currentUser;
  if (!username) {
    return;
  }

  const profile = profilesByUsername[username] ?? fallbackProfile(username);
  ui.profileViewMode = "edit";
  ui.selectedProfileUsername = username;
  ui.profileModalOpen = true;
  ui.profileModalLoading = !profilesByUsername[username];
  ui.profileForm.username = username;
  ui.profileForm.displayName = profile.displayName;
  ui.profileForm.bio = profile.bio;
  ui.profileForm.avatarColor = profile.avatarColor;
  ui.profileForm.createdAt = profile.createdAt;
  ui.profileForm.lastSeen = profile.lastSeen;
  ui.profileForm.online = profile.online;
  requestProfile(username);
}

function openUserProfile(username: string) {
  const trimmedUsername = username.trim();
  if (!trimmedUsername) {
    return;
  }

  if (trimmedUsername === session.currentUser) {
    openOwnProfileEditor();
    return;
  }

  const profile = profilesByUsername[trimmedUsername] ?? fallbackProfile(trimmedUsername);
  ui.profileViewMode = "view";
  ui.selectedProfileUsername = trimmedUsername;
  ui.profileModalOpen = true;
  ui.profileModalLoading = !profilesByUsername[trimmedUsername];
  ui.profileForm.username = trimmedUsername;
  ui.profileForm.displayName = profile.displayName;
  ui.profileForm.bio = profile.bio;
  ui.profileForm.avatarColor = profile.avatarColor;
  ui.profileForm.createdAt = profile.createdAt;
  ui.profileForm.lastSeen = profile.lastSeen;
  ui.profileForm.online = profile.online;
  requestProfile(trimmedUsername);
}

function closeProfileModal() {
  ui.profileModalOpen = false;
  ui.profileModalLoading = false;
  ui.selectedProfileUsername = "";
}

function saveOwnProfile() {
  if (!session.currentUser) {
    return;
  }
  sendCommand("update_profile", [
    ui.profileForm.displayName.trim(),
    ui.profileForm.bio.trim(),
    ui.profileForm.avatarColor.trim(),
  ]);
}

function chatDisplayName(chat: ChatItem) {
  return chat.kind === "dm" ? getProfileDisplayName(chat.peerId) : chat.peer;
}

function chatPreviewSenderLabel(chat: ChatItem) {
  if (!chat.lastSender) {
    return "";
  }
  return getProfileDisplayName(chat.lastSender);
}

function chatPreview(chat: ChatItem) {
  if (!chat.lastText) {
    return "Пока нет сообщений";
  }

  const sender = chatPreviewSenderLabel(chat);
  return sender ? `${sender}: ${chat.lastText}` : chat.lastText;
}

function parseChatMessage(fields: string[], offset = 0) {
  // offset нужен для результатов команд вида send_message_result:
  // первые поля заняты status/text, а само сообщение начинается дальше.
  if (fields.length < offset + 6) {
    return null;
  }

  const id = Number.parseInt(fields[offset] ?? "", 10);
  if (!Number.isFinite(id) || id <= 0) {
    return null;
  }

  const replyToRaw = fields[offset + 6] ?? "";
  const replyToMessageId = replyToRaw ? Number.parseInt(replyToRaw, 10) : null;
  const forwardFromRaw = fields[offset + 9] ?? "";
  const forwardFromMessageId = forwardFromRaw ? Number.parseInt(forwardFromRaw, 10) : null;

  return {
    id,
    chatId: fields[offset + 1] ?? "",
    createdAt: fields[offset + 2] ?? "",
    sender: fields[offset + 3] ?? "",
    recipient: fields[offset + 4] ?? "",
    text: fields[offset + 5] ?? "",
    replyToMessageId: replyToMessageId && Number.isFinite(replyToMessageId) ? replyToMessageId : null,
    replyToSender: fields[offset + 7] ?? "",
    replyToText: fields[offset + 8] ?? "",
    forwardFromMessageId: forwardFromMessageId && Number.isFinite(forwardFromMessageId) ? forwardFromMessageId : null,
    forwardFromSender: fields[offset + 10] ?? "",
    forwardFromText: fields[offset + 11] ?? "",
    deletedAt: fields[offset + 12] ?? "",
    deletedBy: fields[offset + 13] ?? "",
  } as ChatMessage;
}

function isMessageDeleted(message: ChatMessage) {
  return Boolean(message.deletedAt);
}

function displayedMessageText(message: ChatMessage) {
  return isMessageDeleted(message) ? DELETED_MESSAGE_TEXT : message.text;
}

function chatPreviewText(message: ChatMessage) {
  // Превью в списке чатов не должно раскрывать текст удаленного сообщения,
  // даже если старый текст все еще есть в локальном объекте.
  if (isMessageDeleted(message)) {
    return DELETED_MESSAGE_TEXT;
  }
  if (message.text) {
    return message.text;
  }
  if (message.forwardFromText) {
    return message.forwardFromText;
  }
  return "";
}

function canDeleteMessage(message: ChatMessage) {
  return message.sender === session.currentUser && !isMessageDeleted(message);
}

function peerForMessage(message: ChatMessage) {
  return message.chatId || (message.sender === session.currentUser ? message.recipient : message.sender);
}

function dedupeAndSortMessages(messages: ChatMessage[]) {
  // История может прийти из IndexedDB и с сервера одновременно. Последняя
  // версия по id побеждает, затем сообщения сортируются по возрастанию id.
  const messageMap = new Map<number, ChatMessage>();
  for (const message of messages) {
    messageMap.set(message.id, message);
  }
  return Array.from(messageMap.values()).sort((left, right) => left.id - right.id);
}

function scrollMessagesToBottom() {
  if (messagesViewport.value) {
    messagesViewport.value.scrollTop = messagesViewport.value.scrollHeight;
  }
}

function parseChatTimestamp(value: string) {
  if (!value) {
    return Number.NEGATIVE_INFINITY;
  }

  const parsed = Date.parse(value.replace(" ", "T"));
  return Number.isNaN(parsed) ? Number.NEGATIVE_INFINITY : parsed;
}

function clearRecord(record: Record<string, unknown>) {
  Object.keys(record).forEach((key) => {
    delete record[key];
  });
}

function setMessagesForPeer(peer: string, messages: ChatMessage[]) {
  messagesByPeer[peer] = dedupeAndSortMessages(messages);
}

function sortChats() {
  const nextChats = Object.values(chatIndex).sort((left, right) => {
    const rightTime = parseChatTimestamp(right.lastAt);
    const leftTime = parseChatTimestamp(left.lastAt);
    if (rightTime !== leftTime) {
      return rightTime - leftTime;
    }
    return left.peer.localeCompare(right.peer);
  });

  const current = chats.value;
  // Vue перерисовывает список при замене массива. Проверка на реальное
  // изменение порядка/превью снижает лишние обновления во время fetch_chats.
  const sameOrder =
    current.length === nextChats.length &&
    current.every((chat, index) => {
      const nextChat = nextChats[index];
      return (
        nextChat &&
        chat.peerId === nextChat.peerId &&
        chat.lastAt === nextChat.lastAt &&
        chat.lastSender === nextChat.lastSender &&
        chat.lastText === nextChat.lastText &&
        chat.unreadCount === nextChat.unreadCount &&
        chat.canManage === nextChat.canManage &&
        chat.peer === nextChat.peer &&
        chat.kind === nextChat.kind
      );
    });

  if (!sameOrder) {
    chats.value = nextChats;
  }
}

async function persistRecentMessages(peer: string) {
  if (!session.currentUser) {
    return;
  }

  const recentMessages = (messagesByPeer[peer] ?? []).slice(-CACHE_LIMIT_PER_CHAT);
  try {
    await saveMessagesToCache(session.currentUser, peer, recentMessages);
    await trimCachedMessages(session.currentUser, peer, CACHE_LIMIT_PER_CHAT);
  } catch (error) {
    console.error("Failed to persist cached messages", error);
  }
}

async function persistSingleMessage(peer: string, message: ChatMessage) {
  if (!session.currentUser) {
    return;
  }

  try {
    await saveMessageToCache(session.currentUser, peer, message);
    await trimCachedMessages(session.currentUser, peer, CACHE_LIMIT_PER_CHAT);
  } catch (error) {
    console.error("Failed to persist cached message", error);
  }
}

function excerpt(value: string, limit = 90) {
  const normalized = value.replace(/\s+/g, " ").trim();
  if (normalized.length <= limit) {
    return normalized;
  }
  return `${normalized.slice(0, limit - 1)}...`;
}

function escapeField(value: string) {
  // Клиентский web-протокол повторяет common::escapeField на C++ стороне:
  // tab/newline/backslash должны оставаться частью поля, а не ломать строку.
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
  // Bridge отдает сырую строку от C++ сервера. Здесь превращаем ее в простую
  // структуру {command, fields}, которую дальше обрабатывает handleProtocolMessage.
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
    pushLog("Bridge-сокет не открыт");
    return;
  }
  ws.send(JSON.stringify(payload));
}

function sendCommand(command: string, fields: string[] = []) {
  // Все команды на сервер идут через bridge одним типом "send"; сам bridge
  // добавит '\n' и передаст строку в TLS socket.
  sendBridge({ type: "send", line: serialize(command, fields) });
}

function clearSessionData() {
  // При logout/потере соединения чистим и серверные данные UI, и локальные
  // временные состояния, чтобы следующий пользователь не увидел чужой экран.
  session.selectedPeer = "";
  ui.messageDraft = "";
  ui.chatFilter = "";
  ui.showNewChatChoice = false;
  ui.replyTarget = null;
  ui.contextMenuVisible = false;
  ui.contextMenuMessageId = 0;
  ui.highlightedMessageId = 0;
  ui.forwardTarget = null;
  ui.showForwardPicker = false;
  ui.forwardPreviewMessage = null;
  ui.forwardRecipient = "";
  ui.forwardDraft = "";
  ui.showCreateGroup = false;
  ui.createGroupName = "";
  ui.createGroupAdmin = "";
  ui.createGroupSelectedUsers = [];
  ui.createGroupSearchQuery = "";
  ui.createGroupSearchResults = [];
  ui.showGroupSettings = false;
  ui.groupSettingsChatId = "";
  ui.groupSettingsTitle = "";
  ui.groupSettingsAdminUsername = "";
  ui.groupSettingsCanManage = false;
  ui.groupSettingsMembers = [];
  ui.groupAddSearchQuery = "";
  ui.groupAddSearchResults = [];
  ui.profileModalOpen = false;
  ui.profileModalLoading = false;
  ui.profileViewMode = "view";
  ui.selectedProfileUsername = "";
  ui.aiSending = false;
  ui.aiReplyModalOpen = false;
  ui.aiReplyLoading = false;
  ui.aiReplyTargetPeer = "";
  ui.aiReplySourceMessage = null;
  ui.aiReplyInstruction = "";
  ui.aiReplySuggestion = "";
  ui.aiRewriteMenuOpen = false;
  ui.aiRewriteLoading = false;
  ui.aiRewriteError = "";
  ui.aiRewriteCustomOpen = false;
  ui.aiRewriteCustomInstruction = "";
  ui.aiPendingAttachment = null;
  ui.profileForm.username = "";
  ui.profileForm.displayName = "";
  ui.profileForm.bio = "";
  ui.profileForm.avatarColor = "#5C7CFA";
  ui.profileForm.createdAt = "";
  ui.profileForm.lastSeen = "";
  ui.profileForm.online = false;
  searchResults.value = [];
  clearRecord(chatIndex);
  clearRecord(messagesByPeer);
  clearRecord(profilesByUsername);
  clearRecord(loadingOlderByPeer);
  clearRecord(reachedHistoryStartByPeer);
  clearRecord(syncingRecentByPeer);
  clearRecord(historyRequestModeByPeer);
  clearRecord(historyBatchCountByPeer);
  clearRecord(olderScrollStateByPeer);
  chats.value = [];
  aiMessages.value = [];
  pendingProfilesBatch.clear();
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

  connection.statusLine = "Подключение к bridge...";
  ws = new WebSocket(bridgeUrl.value);

  ws.onopen = () => {
    connection.bridgeOpen = true;
    connection.statusLine = "Bridge подключен";
    sendBridge({ type: "connect", host: form.host, port: Number(form.port) });
  };

  ws.onclose = () => {
    connection.bridgeOpen = false;
    connection.tcpConnected = false;
    connection.statusLine = "Bridge отключен";
    if (session.loggedIn || auth.inFlight) {
      resetToWelcome("Соединение закрыто. Войдите снова.");
    }
  };

  ws.onerror = () => {
    connection.statusLine = "Ошибка bridge-сокета";
  };

  ws.onmessage = (event) => {
    let payload: any;
    try {
      payload = JSON.parse(event.data);
    } catch {
      pushLog("Bridge прислал некорректный JSON");
      return;
    }

    if (payload.type === "ready") {
      pushLog(`Bridge готов на порту ${payload.bridgePort}`);
      return;
    }

    if (payload.type === "connected") {
      connection.tcpConnected = true;
      connection.statusLine = `TCP подключен к ${payload.host}:${payload.port}`;
      pushLog(connection.statusLine);
      if (pendingAuth) {
        const currentAuth = pendingAuth;
        pendingAuth = null;
        // Авторизация могла начаться до готовности TCP-соединения. Сохраняем
        // команду и отправляем ее сразу после подтверждения от bridge.
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
      connection.statusLine = "TCP отключен";
      if (session.loggedIn || auth.inFlight) {
        resetToWelcome("Сервер отключился. Войдите снова.");
      }
      return;
    }

    if (payload.type === "line") {
      const parsed = parseLine(payload.line);
      if (!parsed) {
        pushLog(`Protocol parse error: ${payload.line}`);
        return;
      }
      void handleProtocolMessage(parsed);
      return;
    }

    if (payload.type === "bridge_error" || payload.type === "tcp_error") {
      connection.statusLine = payload.message;
      pushLog(`Ошибка соединения: ${payload.message}`);
    }
  };
}

function beginAuth(mode: "login" | "register") {
  const username = form.username.trim();
  const password = form.password;
  if (!username || !password) {
    auth.error = "Введите логин и пароль.";
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

function requestLatestHistory(peer: string) {
  if (!session.loggedIn || !peer || syncingRecentByPeer[peer] || loadingOlderByPeer[peer]) {
    return;
  }

  // После открытия чата всегда просим сервер прислать свежий хвост истории:
  // локальный IndexedDB нужен только для быстрого первого отображения.
  syncingRecentByPeer[peer] = true;
  historyRequestModeByPeer[peer] = "latest";
  historyBatchCountByPeer[peer] = 0;
  sendCommand("fetch_history", [peer, String(CACHE_LIMIT_PER_CHAT)]);
}

async function selectPeer(peer: string) {
  if (peer === AI_CHAT_ID) {
    openAiChat();
    return;
  }

  session.selectedPeer = peer;
  ui.replyTarget = null;
  ui.contextMenuVisible = false;

  const selected = chatIndex[peer];
  if (selected?.kind === "dm") {
    requestProfile(peer);
  }

  const runtimeMessages = messagesByPeer[peer] ?? [];
  if (runtimeMessages.length === 0 && session.currentUser) {
    try {
      const cachedMessages = await loadCachedMessages(session.currentUser, peer);
      // Пользователь мог переключиться в другой чат, пока IndexedDB читался.
      // Не подмешиваем старый результат в новый выбранный чат.
      if (session.selectedPeer !== peer) {
        return;
      }
      if (cachedMessages.length > 0) {
        setMessagesForPeer(peer, cachedMessages);
        requestProfiles(cachedMessages.map((item) => item.sender));
      }
    } catch (error) {
      console.error("Failed to load cached messages", error);
    }
  } else if (runtimeMessages.length > 0) {
    requestProfiles(runtimeMessages.map((item) => item.sender));
  }

  await nextTick();
  scrollMessagesToBottom();
  sendCommand("mark_read", [peer]);
  requestLatestHistory(peer);
}

function pushMessage(peer: string, message: ChatMessage) {
  const arr = messagesByPeer[peer] ?? (messagesByPeer[peer] = []);
  const existingIndex = arr.findIndex((item) => item.id === message.id);
  // Сервер может сначала прислать сообщение из истории, а затем обновленную
  // версию после удаления. Поэтому по id обновляем существующую запись.
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

function updateChatPreviewFromMessage(peer: string, message: ChatMessage, unreadCount?: number) {
  const existingChat = chatIndex[peer];
  if (!existingChat) {
    return false;
  }

  // Список чатов обновляется оптимистично по последнему сообщению, чтобы UI не
  // ждал полного fetch_chats после каждого incoming_message.
  existingChat.lastAt = message.createdAt;
  existingChat.lastSender = message.sender;
  existingChat.lastText = chatPreviewText(message);
  if (typeof unreadCount === "number") {
    existingChat.unreadCount = unreadCount;
  }
  sortChats();
  return true;
}

function replaceLoadedMessage(peer: string, message: ChatMessage) {
  const arr = messagesByPeer[peer];
  if (!arr) {
    return false;
  }

  const existingIndex = arr.findIndex((item) => item.id === message.id);
  if (existingIndex < 0) {
    return false;
  }

  arr.splice(existingIndex, 1, message);
  return true;
}

function sendMessage() {
  if (session.selectedPeer === AI_CHAT_ID) {
    void sendAiMessage();
    return;
  }

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

function requestDeleteMessage() {
  const message = activeContextMessage.value;
  if (!message || !canDeleteMessage(message)) {
    closeContextMenu();
    return;
  }
  if (!confirm("Удалить сообщение?")) {
    return;
  }
  const chatId = session.selectedPeer || message.chatId || peerForMessage(message);
  sendCommand("delete_message", [chatId, String(message.id)]);
  closeContextMenu();
}

function fetchChats() {
  if (chatFetchInFlight) {
    chatFetchQueued = true;
    return;
  }

  chatFetchInFlight = true;
  chatFetchSeenPeerIds.clear();
  sendCommand("fetch_chats");
}

function openAddChat() {
  ui.showNewChatChoice = true;
}

function openDirectChatCreator() {
  ui.showNewChatChoice = false;
  ui.showAddChat = true;
  ui.searchQuery = "";
  searchResults.value = [];
}

function openGroupChatCreator() {
  ui.showNewChatChoice = false;
  ui.showCreateGroup = true;
  ui.createGroupName = "";
  ui.createGroupAdmin = session.currentUser;
  ui.createGroupSelectedUsers = [];
  ui.createGroupSearchQuery = "";
  ui.createGroupSearchResults = [];
}

function closeAddChat() {
  ui.showNewChatChoice = false;
  ui.showAddChat = false;
  ui.searchQuery = "";
  searchResults.value = [];
}

function closeCreateGroup() {
  ui.showCreateGroup = false;
  ui.createGroupName = "";
  ui.createGroupAdmin = "";
  ui.createGroupSelectedUsers = [];
  ui.createGroupSearchQuery = "";
  ui.createGroupSearchResults = [];
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

function runCreateGroupSearch() {
  const query = ui.createGroupSearchQuery.trim();
  if (!query) {
    return;
  }
  pendingSearchQuery.value = query;
  ui.createGroupSearchResults = [];
  ui.createGroupSearchInFlight = true;
  sendCommand("search_users", [query, "group_create"]);
}

function toggleCreateGroupUser(user: SearchUser) {
  const existingIndex = ui.createGroupSelectedUsers.findIndex((item) => item.username === user.username);
  if (existingIndex >= 0) {
    ui.createGroupSelectedUsers.splice(existingIndex, 1);
    if (ui.createGroupAdmin === user.username) {
      ui.createGroupAdmin = session.currentUser;
    }
    return;
  }
  ui.createGroupSelectedUsers.push(user);
}

function createGroup() {
  const name = ui.createGroupName.trim();
  if (!name) {
    pushLog("Введите название группы");
    return;
  }

  const usernames = ui.createGroupSelectedUsers.map((item) => item.username);
  const adminUsername = ui.createGroupAdmin || session.currentUser;
  sendCommand("create_group", [name, adminUsername, usernames.join(",")]);
}

function createChatFromSearch(user: SearchUser) {
  sendCommand("create_chat", [user.id]);
}

function deleteChat(peer: string) {
  if (!confirm(`Удалить чат с ${peer}?`)) {
    return;
  }
  sendCommand("delete_chat", [peer]);
}

function closeContextMenu() {
  ui.contextMenuVisible = false;
  ui.contextMenuMessageId = 0;
}

function openContextMenu(event: MouseEvent, message: ChatMessage) {
  const canReply = !isMessageDeleted(message);
  const canForward = !isMessageDeleted(message);
  const canDelete = canDeleteMessage(message);
  const canAskAi = !isMessageDeleted(message);
  if (!canReply && !canForward && !canDelete && !canAskAi) {
    return;
  }
  event.preventDefault();
  ui.contextMenuVisible = true;
  ui.contextMenuMessageId = message.id;
  ui.contextMenuX = event.clientX;
  ui.contextMenuY = event.clientY;
  nextTick(() => {
    const menu = document.querySelector<HTMLElement>(".message-menu");
    if (!menu) {
      return;
    }

    const padding = 10;
    const maxX = Math.max(padding, window.innerWidth - menu.offsetWidth - padding);
    const maxY = Math.max(padding, window.innerHeight - menu.offsetHeight - padding);
    ui.contextMenuX = Math.max(padding, Math.min(event.clientX, maxX));
    ui.contextMenuY = Math.max(padding, Math.min(event.clientY, maxY));
  });
}

function beginReply() {
  const message = activeContextMessage.value;
  if (message && !isMessageDeleted(message)) {
    ui.replyTarget = message;
  }
  closeContextMenu();
}

function cancelReply() {
  ui.replyTarget = null;
}

function beginForward() {
  const message = activeContextMessage.value;
  if (message && !isMessageDeleted(message)) {
    ui.forwardTarget = message;
    ui.showForwardPicker = true;
    ui.forwardRecipient = "";
    ui.forwardDraft = "";
  }
  closeContextMenu();
}

function beginAiReply() {
  const message = activeContextMessage.value;
  if (message) {
    openAiReplyModal(message);
  }
  closeContextMenu();
}

function beginAskAi() {
  const message = activeContextMessage.value;
  if (!message) {
    closeContextMenu();
    return;
  }

  if (isMessageDeleted(message)) {
    pushLog("Нельзя анализировать удаленное сообщение");
    closeContextMenu();
    return;
  }

  ui.aiPendingAttachment = buildAiAttachment(message);
  openAiChat();
  closeContextMenu();
}

function cancelForward() {
  ui.forwardTarget = null;
  ui.showForwardPicker = false;
  ui.forwardRecipient = "";
  ui.forwardDraft = "";
}

function selectForwardPeer(peerId: string) {
  ui.forwardRecipient = peerId;
}

function submitForward() {
  const peerId = ui.forwardRecipient.trim();
  if (!ui.forwardTarget?.id) {
    return;
  }

  if (!peerId) {
    return;
  }

  const fields = [peerId, ui.forwardTarget.chatId, String(ui.forwardTarget.id)];
  // Комментарий к пересылке опционален: если он есть, сервер отправит его как
  // новое сообщение рядом с пересланным.
  if (ui.forwardDraft.trim()) {
    fields.push(ui.forwardDraft.trim());
  }

  sendCommand("forward_message", fields);
  const targetChat = chats.value.find((chat) => chat.peerId === peerId);
  pushLog(`Пересылаем сообщение #${ui.forwardTarget.id} в ${targetChat?.peer ?? peerId}`);
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

function openGroupSettings() {
  if (!selectedChat.value || selectedChat.value.kind !== "group") {
    return;
  }
  ui.showGroupSettings = true;
  ui.groupSettingsLoading = true;
  ui.groupSettingsChatId = selectedChat.value.peerId;
  ui.groupSettingsTitle = selectedChat.value.peer;
  ui.groupSettingsMembers = [];
  ui.groupAddSearchQuery = "";
  ui.groupAddSearchResults = [];
  sendCommand("get_group_info", [selectedChat.value.peerId]);
}

function closeGroupSettings() {
  ui.showGroupSettings = false;
  ui.groupSettingsLoading = false;
  ui.groupSettingsChatId = "";
  ui.groupSettingsMembers = [];
  ui.groupAddSearchQuery = "";
  ui.groupAddSearchResults = [];
}

function runGroupAddSearch() {
  if (!ui.groupSettingsChatId) {
    return;
  }
  const query = ui.groupAddSearchQuery.trim();
  if (!query) {
    return;
  }
  pendingSearchQuery.value = query;
  ui.groupAddSearchResults = [];
  ui.groupAddSearchInFlight = true;
  sendCommand("search_users", [query, "group_add", ui.groupSettingsChatId]);
}

function addUserToGroup(user: SearchUser) {
  if (!ui.groupSettingsChatId) {
    return;
  }
  ui.groupAddSearchResults = ui.groupAddSearchResults.filter((item) => item.username !== user.username);
  sendCommand("add_group_members", [ui.groupSettingsChatId, user.username]);
}

function removeUserFromGroup(username: string) {
  if (!ui.groupSettingsChatId) {
    return;
  }
  if (!confirm(`Удалить ${username} из группы?`)) {
    return;
  }
  sendCommand("remove_group_member", [ui.groupSettingsChatId, username]);
}

function transferAdmin(username: string) {
  if (!ui.groupSettingsChatId || username === ui.groupSettingsAdminUsername) {
    return;
  }
  if (!confirm(`Сделать ${username} новым администратором?`)) {
    return;
  }
  sendCommand("transfer_group_admin", [ui.groupSettingsChatId, username]);
}

function leaveCurrentGroup() {
  if (!ui.groupSettingsChatId) {
    return;
  }
  if (!confirm("Выйти из этой группы?")) {
    return;
  }
  sendCommand("leave_group", [ui.groupSettingsChatId]);
  closeGroupSettings();
}

function deleteCurrentGroup() {
  if (!ui.groupSettingsChatId) {
    return;
  }
  if (!confirm("Удалить эту группу для всех?")) {
    return;
  }
  if (!confirm("Подтвердите еще раз: удалить эту группу для всех?")) {
    return;
  }
  sendCommand("delete_group", [ui.groupSettingsChatId]);
  closeGroupSettings();
}

function jumpToMessage(messageId: number | null) {
  if (!messageId) {
    return;
  }

  nextTick(() => {
    // Ответ может ссылаться на сообщение, которое уже не попало в текущую
    // страницу истории. В этом случае просто пишем подсказку в журнал.
    const target = document.querySelector<HTMLElement>(`[data-message-id="${messageId}"]`);
    if (!target) {
      pushLog(`Сообщение #${messageId} не загружено в текущем окне истории`);
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

function loadOlderMessages(peer: string) {
  if (
    !session.loggedIn ||
    !peer ||
    loadingOlderByPeer[peer] ||
    syncingRecentByPeer[peer] ||
    reachedHistoryStartByPeer[peer]
  ) {
    return;
  }

  const messages = messagesByPeer[peer] ?? [];
  const oldestMessage = messages[0];
  if (!oldestMessage?.id) {
    return;
  }

  const viewport = messagesViewport.value;
  if (viewport && session.selectedPeer === peer) {
    // При догрузке старых сообщений сверху сохраняем позицию прокрутки.
    // После вставки истории восстановим визуально тот же фрагмент диалога.
    olderScrollStateByPeer[peer] = {
      oldScrollHeight: viewport.scrollHeight,
      oldScrollTop: viewport.scrollTop,
    };
  }

  loadingOlderByPeer[peer] = true;
  historyRequestModeByPeer[peer] = "older";
  historyBatchCountByPeer[peer] = 0;
  sendCommand("fetch_history_before", [peer, String(oldestMessage.id), String(HISTORY_PAGE_SIZE)]);
}

function onMessagesScroll() {
  const viewport = messagesViewport.value;
  if (!viewport) {
    return;
  }
  const distance = viewport.scrollHeight - viewport.clientHeight - viewport.scrollTop;
  stickMessagesToBottom.value = distance < 28;
  if (viewport.scrollTop < 80 && session.selectedPeer && session.selectedPeer !== AI_CHAT_ID) {
    loadOlderMessages(session.selectedPeer);
  }
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
  if (!confirm("Выйти из текущего аккаунта?")) {
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

const dismissMenus = (event?: Event) => {
  const target = event?.target as HTMLElement | null;
  if (target && target.closest(".composer-ai-wrap")) {
    closeContextMenu();
    return;
  }
  closeContextMenu();
  closeAiRewriteMenu();
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
  const existingChat = chatIndex[chat.peerId];
  if (existingChat) {
    existingChat.peer = chat.peer;
    existingChat.kind = chat.kind;
    existingChat.lastAt = chat.lastAt;
    existingChat.lastSender = chat.lastSender;
    existingChat.lastText = chat.lastText;
    existingChat.unreadCount = chat.unreadCount;
    existingChat.canManage = chat.canManage;
  } else {
    chatIndex[chat.peerId] = { ...chat };
  }
  if (!chatFetchInFlight) {
    sortChats();
  }
}

function removeChatState(peerId: string) {
  delete chatIndex[peerId];
  delete messagesByPeer[peerId];
  delete loadingOlderByPeer[peerId];
  delete reachedHistoryStartByPeer[peerId];
  delete syncingRecentByPeer[peerId];
  delete historyRequestModeByPeer[peerId];
  delete historyBatchCountByPeer[peerId];
  delete olderScrollStateByPeer[peerId];
  sortChats();
}

async function handleProtocolMessage(message: ProtocolMessage) {
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
        pushLog(`Регистрация ${a}: ${b}`);
        if (authCredentials) {
          sendCommand("login", [authCredentials.username, authCredentials.password]);
        }
      } else {
        pendingAuth = null;
        auth.inFlight = false;
        auth.visible = true;
        auth.mode = "login";
        if (b.toLowerCase().includes("already exists")) {
          auth.error = "Пользователь уже существует. Попробуйте войти.";
        } else {
          auth.error = b || "Не удалось зарегистрироваться";
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
        try {
          await openMessageCacheDb(session.currentUser);
          await openAiChatCacheDb(session.currentUser);
        } catch (error) {
          console.error("Failed to open local caches", error);
        }
        await loadAiMessagesForUser(session.currentUser);
        requestProfile(session.currentUser);
        fetchChats();
      } else {
        pendingAuth = null;
        auth.inFlight = false;
        auth.visible = true;
        auth.mode = "login";
        auth.error = b || "Не удалось войти";
      }
      pushLog(`Вход ${a}: ${b}`);
      break;
    case "send_message_result":
      if (a === "ok") {
        const sentMessage = parseChatMessage(message.fields, 1);
        if (!sentMessage) {
          pushLog("Ошибка разбора результата отправки");
          break;
        }

        const peer = sentMessage.chatId || peerForMessage(sentMessage);
        pushMessage(peer, sentMessage);
        requestProfiles([sentMessage.sender]);
        await persistSingleMessage(peer, sentMessage);
        if (!updateChatPreviewFromMessage(peer, sentMessage, 0)) {
          fetchChats();
        }
        sendCommand("mark_read", [peer]);
      } else {
        pushLog(`Ошибка отправки: ${b}`);
      }
      break;
    case "incoming_message": {
      const incomingMessage = parseChatMessage(message.fields);
      if (!incomingMessage) {
        pushLog("Ошибка разбора входящего сообщения");
        break;
      }

      const peer = incomingMessage.chatId || peerForMessage(incomingMessage);
      pushMessage(peer, incomingMessage);
      requestProfiles([incomingMessage.sender]);
      await persistSingleMessage(peer, incomingMessage);
      if (session.selectedPeer === peer) {
        updateChatPreviewFromMessage(peer, incomingMessage, 0);
        sendCommand("mark_read", [peer]);
      } else if (!updateChatPreviewFromMessage(peer, incomingMessage, (chatIndex[peer]?.unreadCount ?? 0) + 1)) {
        fetchChats();
      }
      break;
    }
    case "history_message": {
      const historyMessage = parseChatMessage(message.fields);
      if (!historyMessage) {
        pushLog("Ошибка разбора сообщения из истории");
        break;
      }

      const peer = historyMessage.chatId || peerForMessage(historyMessage);
      pushMessage(peer, historyMessage);
      requestProfiles([historyMessage.sender]);
      historyBatchCountByPeer[peer] = (historyBatchCountByPeer[peer] ?? 0) + 1;
      if (historyRequestModeByPeer[peer] !== "older") {
        await persistSingleMessage(peer, historyMessage);
      }
      break;
    }
    case "delete_message_result":
      if (a !== "ok") {
        pushLog(`Ошибка удаления сообщения: ${b}`);
      }
      break;
    case "message_deleted": {
      const deletedMessage = parseChatMessage(message.fields);
      if (!deletedMessage) {
        pushLog("Ошибка разбора удаленного сообщения");
        break;
      }

      const peer = deletedMessage.chatId || peerForMessage(deletedMessage);
      const updated = replaceLoadedMessage(peer, deletedMessage);
      if (updated) {
        await persistSingleMessage(peer, deletedMessage);
      }
      if (ui.replyTarget?.id === deletedMessage.id) {
        ui.replyTarget = null;
      }
      if (ui.forwardTarget?.id === deletedMessage.id) {
        cancelForward();
      }
      break;
    }
    case "history_result": {
      const resultPeer = message.fields[2] ?? "";
      const resultMode = message.fields[3] === "older" ? "older" : "latest";
      pushLog(`История ${a}: ${b}`);
      if (resultPeer) {
        const loadedCount = historyBatchCountByPeer[resultPeer] ?? 0;
        if (resultMode === "older") {
          loadingOlderByPeer[resultPeer] = false;
          if (loadedCount < HISTORY_PAGE_SIZE) {
            reachedHistoryStartByPeer[resultPeer] = true;
          }

          if (a === "ok" && session.selectedPeer === resultPeer) {
            const scrollState = olderScrollStateByPeer[resultPeer];
            if (scrollState && messagesViewport.value) {
              await nextTick();
              const viewport = messagesViewport.value;
              if (viewport) {
                viewport.scrollTop =
                  scrollState.oldScrollTop + (viewport.scrollHeight - scrollState.oldScrollHeight);
              }
            }
          }
        } else {
          syncingRecentByPeer[resultPeer] = false;
          if (loadedCount < CACHE_LIMIT_PER_CHAT) {
            reachedHistoryStartByPeer[resultPeer] = true;
          }
          if (a === "ok" && session.selectedPeer === resultPeer) {
            sendCommand("mark_read", [resultPeer]);
          }
        }

        delete historyBatchCountByPeer[resultPeer];
        delete historyRequestModeByPeer[resultPeer];
        delete olderScrollStateByPeer[resultPeer];
      }

      if (a === "ok") {
        if (resultPeer && resultMode !== "older") {
          await persistRecentMessages(resultPeer);
        }
      } else if (resultMode === "older" && resultPeer) {
        loadingOlderByPeer[resultPeer] = false;
      } else if (resultPeer) {
        syncingRecentByPeer[resultPeer] = false;
      }
      break;
    }
    case "chat_item":
      if (chatFetchInFlight) {
        chatFetchSeenPeerIds.add(a);
      }
      if (c !== "group") {
        requestProfile(a);
      }
      if (e) {
        requestProfiles([e]);
      }
      upsertChat({
        peerId: a,
        peer: b,
        kind: (c === "group" ? "group" : "dm"),
        lastAt: d,
        lastSender: e,
        lastText: message.fields[5] ?? "",
        unreadCount: Number.parseInt(message.fields[6] ?? "0", 10) || 0,
        canManage: (message.fields[7] ?? "0") === "1",
      });
      break;
    case "chat_list_result":
      if (chatFetchInFlight) {
        Object.keys(chatIndex).forEach((peerId) => {
          if (!chatFetchSeenPeerIds.has(peerId)) {
            removeChatState(peerId);
          }
        });
        chatFetchSeenPeerIds.clear();
        sortChats();
        chatFetchInFlight = false;
        if (chatFetchQueued) {
          chatFetchQueued = false;
          fetchChats();
        }
      }
      pushLog(`Чаты ${a}: ${b}`);
      break;
    case "user_search_item":
      if (a !== pendingSearchQuery.value) {
        return;
      }
      if (ui.showAddChat) {
        searchResults.value.push({ id: b, username: c });
      } else if (ui.showCreateGroup) {
        ui.createGroupSearchResults.push({ id: b, username: c });
      } else if (ui.showGroupSettings) {
        ui.groupAddSearchResults.push({ id: b, username: c });
      }
      break;
    case "user_search_result":
      ui.searchInFlight = false;
      ui.createGroupSearchInFlight = false;
      ui.groupAddSearchInFlight = false;
      pushLog(`Поиск пользователей ${a}: ${b}`);
      break;
    case "create_chat_result":
      if (a === "ok") {
        closeAddChat();
        fetchChats();
        if (c) {
          selectPeer(c);
        }
      } else {
        pushLog(`Ошибка создания чата: ${b}`);
      }
      break;
    case "create_group_result":
      if (a === "ok") {
        closeCreateGroup();
        fetchChats();
        if (b) {
          selectPeer(b);
        }
      } else {
        pushLog(`Ошибка создания группы: ${b}`);
      }
      break;
    case "delete_chat_result":
      if (a === "ok") {
        const removedPeer = c;
        removeChatState(removedPeer);
        if (session.selectedPeer === removedPeer) {
          session.selectedPeer = "";
          ui.replyTarget = null;
        }
      } else {
        pushLog(`Ошибка удаления чата: ${b}`);
      }
      break;
    case "mark_read_result":
      if (a === "ok") {
        const peer = b;
        if (chatIndex[peer] && chatIndex[peer].unreadCount !== 0) {
          chatIndex[peer].unreadCount = 0;
          sortChats();
        }
      } else {
        pushLog("Не удалось отметить сообщения прочитанными");
      }
      break;
    case "group_member_item":
      if (a === ui.groupSettingsChatId) {
        ui.groupSettingsMembers.push({ username: b, isAdmin: c === "1" });
        requestProfile(b);
      }
      break;
    case "profile_result":
      if (a === "ok") {
        applyProfileFromFields(message.fields.slice(1));
        pendingProfilesBatch.delete(message.fields[1] ?? "");
      } else {
        ui.profileModalLoading = false;
        pushLog(`Ошибка профиля: ${b}`);
      }
      break;
    case "update_profile_result":
      if (a === "ok") {
        applyProfileFromFields(message.fields.slice(1));
        ui.profileModalOpen = false;
        session.profileMenuOpen = false;
      } else {
        pushLog(`Ошибка обновления профиля: ${b}`);
      }
      break;
    case "profile_item":
      applyProfileFromFields(message.fields);
      pendingProfilesBatch.delete(a);
      break;
    case "profiles_result":
      if (a !== "ok") {
        pushLog(`Ошибка профилей: ${b}`);
      }
      pendingProfilesBatch.clear();
      break;
    case "group_info_result":
      ui.groupSettingsLoading = false;
      if (a === "ok") {
        ui.groupSettingsChatId = b;
        ui.groupSettingsTitle = c;
        ui.groupSettingsAdminUsername = d;
        ui.groupSettingsCanManage = e === "1";
      } else {
        pushLog(`Ошибка информации о группе: ${b}`);
      }
      break;
    case "group_update_result":
      pushLog(`Обновление группы ${a}: ${b}`);
      if (a === "ok") {
        fetchChats();
        if (ui.showGroupSettings && c) {
          ui.groupSettingsMembers = [];
          ui.groupSettingsLoading = true;
          sendCommand("get_group_info", [c]);
        }
      }
      break;
    case "chat_removed": {
      const removedChatId = a;
      removeChatState(removedChatId);
      if (session.selectedPeer === removedChatId) {
        session.selectedPeer = "";
      }
      pushLog(b || "Чат удален");
      break;
    }
    default:
      pushLog(`Необработанная команда: ${message.command}`);
      break;
  }
}

watch(
  () => currentMessages.value.length,
  () => {
    if (stickMessagesToBottom.value && !isAiChatSelected.value) {
      nextTick(() => {
        if (messagesViewport.value) {
          messagesViewport.value.scrollTop = messagesViewport.value.scrollHeight;
        }
      });
    }
  },
);

watch(
  () => aiMessages.value.length,
  () => {
    if (stickMessagesToBottom.value && isAiChatSelected.value) {
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
          :style="{ background: currentUserAvatarColor }"
          @click="session.profileMenuOpen = !session.profileMenuOpen"
        >
          {{ profileInitial }}
        </button>
      </div>
      <div v-if="session.profileMenuOpen" class="profile-menu">
        <div class="profile-menu-user">
          <strong>{{ getProfileDisplayName(session.currentUser) }}</strong>
          <small>@{{ session.currentUser }}</small>
        </div>
        <div class="profile-menu-actions">
          <button class="small" @click="openOwnProfileEditor">Профиль</button>
          <button class="danger small signout-btn" @click="signOut">Выйти</button>
        </div>
      </div>

      <section class="panel chats-panel">
        <div class="panel-row">
          <h2>Чаты</h2>
          <button class="small" @click="openAddChat" :disabled="!session.loggedIn">Новый</button>
        </div>
        <input v-model="ui.chatFilter" placeholder="Поиск чатов" />
        <div class="chat-list-shell">
          <div class="chat-list">
            <div
              v-for="chat in filteredChats"
              :key="chat.peerId"
              class="chat-row"
              :class="{ active: session.selectedPeer === chat.peerId }"
              @click="selectPeer(chat.peerId)"
            >
              <div
                class="chat-avatar"
                :class="{ 'group-chat-avatar': chat.kind === 'group' }"
                :style="chat.kind === 'dm' ? { background: getProfileAvatarColor(chat.peerId) } : undefined"
              >
                {{ chat.kind === "dm" ? getProfileInitials(chat.peerId) : "ГР" }}
              </div>
              <div class="chat-row-main">
                <strong>{{ chatDisplayName(chat) }}</strong>
                <span class="preview">
                  {{ chatPreview(chat) }}
                </span>
              </div>
              <div class="chat-row-actions">
                <span v-if="chat.unreadCount > 0 && session.selectedPeer !== chat.peerId" class="unread-dot"></span>
                <button v-if="chat.kind === 'dm'" class="chat-delete" @click.stop="deleteChat(chat.peerId)">×</button>
              </div>
            </div>
          </div>
          <div v-if="showAiChatRow" class="ai-chat-block">
            <div class="ai-chat-block-title">Ассистент</div>
            <div
              class="chat-row ai-chat-row"
              :class="{ active: session.selectedPeer === AI_CHAT_ID }"
              @click="selectPeer(AI_CHAT_ID)"
            >
              <div class="chat-avatar ai-chat-avatar">AI</div>
              <div class="chat-row-main">
                <strong>{{ AI_CHAT_NAME }}</strong>
                <span class="preview">{{ aiPreviewText() }}</span>
              </div>
              <div class="chat-row-actions">
                <span class="ai-pill">AI</span>
              </div>
            </div>
          </div>
        </div>
      </section>
    </aside>

    <main class="chat-main">
      <header class="topbar">
        <div>
          <h2>{{ session.selectedPeer ? selectedChatTitle : "Привет" }}</h2>
          <p>
            {{
              isAiChatSelected
                ? "AI-помощник для анализа, пересказов, переводов и черновиков ответов"
                : session.selectedPeer
                ? (selectedChat?.kind === "dm" ? `@${selectedChat.peerId}` : "Групповой чат")
                : "Выберите чат слева"
            }}
          </p>
        </div>
        <div class="top-actions">
          <div
            v-if="canOpenDirectProfile && selectedDirectPresenceText"
            class="presence-pill"
            :class="{ online: selectedDirectPresenceOnline }"
          >
            <span class="presence-dot"></span>
            <span>{{ selectedDirectPresenceText }}</span>
          </div>
          <button v-if="isAiChatSelected" class="small" @click="clearAiChat">Очистить</button>
          <button v-if="canOpenDirectProfile && selectedChat" class="small" @click="openUserProfile(selectedChat.peerId)">Профиль</button>
          <button v-if="canOpenGroupSettings" class="small" @click="openGroupSettings">Участники</button>
        </div>
      </header>

      <section ref="messagesViewport" class="messages" @scroll="onMessagesScroll">
        <div v-if="session.selectedPeer && !isAiChatSelected && loadingOlderByPeer[session.selectedPeer]" class="history-loading">
          Загружаем старые сообщения...
        </div>
        <div v-if="!session.selectedPeer" class="empty-chat">
          <p>Выберите чат в списке слева, чтобы увидеть переписку.</p>
        </div>
        <template v-else-if="isAiChatSelected">
          <div v-if="currentAiMessages.length === 0" class="empty-chat">
            <p>Откройте NexTalk AI и задайте вопрос или используйте «Спросить AI» у любого сообщения.</p>
          </div>
          <template v-for="message in currentAiMessages" :key="message.id">
            <article
              class="message-row"
              :class="{
                own: message.role === 'user',
                'ai-assistant-row': message.role !== 'user',
                'ai-error-row': message.role === 'error',
              }"
            >
              <div
                class="avatar"
                :class="{ 'ai-avatar': message.role !== 'user' }"
                :style="message.role === 'user' ? { background: currentUserAvatarColor } : undefined"
              >
                {{ message.role === "user" ? profileInitial : "AI" }}
              </div>
              <div class="bubble" :class="{ 'ai-bubble': message.role !== 'user', 'ai-error-bubble': message.role === 'error' }">
                <div class="meta">
                  <strong>{{ message.role === "user" ? getProfileDisplayName(session.currentUser) : AI_CHAT_NAME }}</strong>
                  <small>{{ formatServerTime(message.createdAt) }}</small>
                </div>
                <div v-if="message.attachment" class="ai-attachment-card">
                  <span class="ai-attachment-title">Пересланное сообщение</span>
                  <strong>{{ getProfileDisplayName(message.attachment.sender) }}</strong>
                  <span>{{ excerpt(message.attachment.text, 220) }}</span>
                </div>
                <p v-if="message.content">{{ message.content }}</p>
              </div>
            </article>
          </template>
        </template>
        <template v-for="(message, idx) in currentMessages" :key="message.id || idx">
          <article
            class="message-row"
            :class="{ own: message.sender === session.currentUser, highlighted: ui.highlightedMessageId === message.id }"
            :data-message-id="message.id"
            @contextmenu="openContextMenu($event, message)"
          >
            <div class="avatar" :style="{ background: getProfileAvatarColor(message.sender) }">
              {{ getProfileInitials(message.sender) }}
            </div>
            <div class="bubble">
              <div class="meta">
                <strong>{{ getProfileDisplayName(message.sender) }}</strong>
                <small>{{ formatServerTime(message.createdAt) }}</small>
              </div>
              <button
                v-if="message.forwardFromMessageId && !isMessageDeleted(message)"
                class="forward-snippet"
                type="button"
                @click="openForwardPreview(message)"
              >
                <strong>Переслано от {{ message.forwardFromSender ? getProfileDisplayName(message.forwardFromSender) : "Неизвестно" }}</strong>
                <span>{{ excerpt(message.forwardFromText || message.text) }}</span>
              </button>
              <button
                v-if="message.replyToMessageId && !isMessageDeleted(message)"
                class="reply-snippet"
                type="button"
                @click="jumpToMessage(message.replyToMessageId)"
              >
                <strong>{{ message.replyToSender ? getProfileDisplayName(message.replyToSender) : "Сообщение" }}</strong>
                <span>{{ excerpt(message.replyToText || "Удаленное сообщение") }}</span>
              </button>
              <p v-if="displayedMessageText(message)" :class="{ 'deleted-copy': isMessageDeleted(message) }">
                {{ displayedMessageText(message) }}
              </p>
            </div>
          </article>
        </template>

        <div
          v-if="ui.contextMenuVisible"
          class="message-menu"
          :style="{ left: `${ui.contextMenuX}px`, top: `${ui.contextMenuY}px` }"
        >
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginReply">Ответить</button>
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginForward">Переслать</button>
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginAiReply">AI-ответ</button>
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginAskAi">Спросить AI</button>
          <button
            v-if="activeContextMessage && canDeleteMessage(activeContextMessage)"
            class="message-menu-item danger-text"
            @click.stop="requestDeleteMessage"
          >
            Удалить
          </button>
        </div>
      </section>

      <footer class="composer">
        <div class="composer-main">
          <div v-if="ui.replyTarget && !isAiChatSelected" class="reply-draft">
            <div class="reply-draft-copy">
              <strong>{{ getProfileDisplayName(ui.replyTarget.sender) }}</strong>
              <span>{{ excerpt(ui.replyTarget.text) }}</span>
            </div>
            <button class="reply-cancel" @click="cancelReply">×</button>
          </div>
          <div v-if="isAiChatSelected && ui.aiPendingAttachment" class="reply-draft ai-pending-draft">
            <div class="reply-draft-copy">
              <small class="ai-attachment-title">Пересланное сообщение</small>
              <strong>{{ getProfileDisplayName(ui.aiPendingAttachment.sender) }}</strong>
              <span>{{ excerpt(ui.aiPendingAttachment.text, 220) }}</span>
            </div>
            <button class="reply-cancel" @click="cancelAiAttachment">×</button>
          </div>
          <div class="composer-row">
            <input
              v-model="ui.messageDraft"
              type="text"
              :placeholder="
                isAiChatSelected && ui.aiPendingAttachment
                  ? 'Что AI должен сделать с этим сообщением?'
                  : isAiChatSelected
                    ? 'Спросите NexTalk AI о чем угодно'
                    : 'Напишите сообщение'
              "
              :disabled="(!session.selectedPeer || !session.loggedIn) || ui.aiSending"
              @keydown.enter.prevent="sendMessage"
            />
            <div class="composer-ai-wrap">
              <button
                class="small composer-ai-btn ai-accent-btn"
                type="button"
                @click.stop="toggleAiRewriteMenu"
                :disabled="
                  !session.selectedPeer ||
                  !session.loggedIn ||
                  isAiChatSelected ||
                  ui.aiSending ||
                  ui.aiRewriteLoading ||
                  !ui.messageDraft.trim()
                "
              >
                {{ ui.aiRewriteLoading ? "AI..." : "AI" }}
              </button>
              <div v-if="ui.aiRewriteMenuOpen" class="ai-rewrite-menu" @click.stop>
                <div class="ai-rewrite-grid">
                  <button
                    v-for="preset in AI_REWRITE_PRESETS"
                    :key="preset"
                    class="ai-rewrite-item"
                    type="button"
                    :disabled="ui.aiRewriteLoading"
                    @click="chooseAiRewritePreset(preset)"
                  >
                    {{ preset }}
                  </button>
                  <button class="ai-rewrite-item ai-rewrite-custom-toggle" type="button" :disabled="ui.aiRewriteLoading" @click="openCustomAiRewrite">
                    Свой вариант...
                  </button>
                </div>
                <div v-if="ui.aiRewriteCustomOpen" class="ai-rewrite-custom">
                  <input
                    v-model="ui.aiRewriteCustomInstruction"
                    type="text"
                    placeholder="Опишите, как переписать"
                    :disabled="ui.aiRewriteLoading"
                    @keydown.enter.prevent="submitCustomAiRewrite"
                  />
                  <button
                    class="small ai-rewrite-apply-btn ai-accent-btn"
                    type="button"
                    :disabled="ui.aiRewriteLoading || !ui.aiRewriteCustomInstruction.trim()"
                    @click="submitCustomAiRewrite"
                  >
                    Применить
                  </button>
                </div>
                <p v-if="ui.aiRewriteError" class="ai-rewrite-error">{{ ui.aiRewriteError }}</p>
              </div>
            </div>
            <button
              class="primary"
              @click="sendMessage"
              :disabled="
                !session.selectedPeer ||
                !ui.messageDraft.trim() ||
                ui.aiSending
              "
            >
              {{ isAiChatSelected && ui.aiSending ? "Думаю..." : "Отправить" }}
            </button>
          </div>
        </div>
      </footer>
    </main>

    <section class="log-panel">
      <h3>Журнал сессии</h3>
      <div ref="logsViewport" class="logs" @scroll="onLogsScroll">
        <p v-for="(line, idx) in ui.logs" :key="idx">{{ line }}</p>
      </div>
    </section>

    <section v-if="ui.showNewChatChoice" class="modal-wrap">
      <div class="modal">
        <button class="modal-close" type="button" @click="closeAddChat" aria-label="Закрыть">×</button>
        <h3>Новый чат</h3>
        <p>Выберите, что нужно создать.</p>
        <div class="welcome-actions">
          <button class="primary" @click="openDirectChatCreator">Личный чат</button>
          <button class="secondary" @click="openGroupChatCreator">Групповой чат</button>
        </div>
      </div>
    </section>

    <section v-if="ui.showAddChat" class="modal-wrap">
      <div class="modal">
        <button class="modal-close" type="button" @click="closeAddChat" aria-label="Закрыть">×</button>
        <h3>Создать чат</h3>
        <p>Найдите пользователя по логину и начните личную переписку.</p>
        <div class="modal-search">
          <input v-model="ui.searchQuery" type="text" placeholder="логин" @keydown.enter.prevent="runUserSearch" />
          <button class="small" @click="runUserSearch" :disabled="ui.searchInFlight">
            {{ ui.searchInFlight ? "Ищем..." : "Найти" }}
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
          <p v-if="!ui.searchInFlight && searchResults.length === 0">Ничего не найдено</p>
        </div>
      </div>
    </section>

    <section v-if="ui.showCreateGroup" class="modal-wrap">
      <div class="modal">
        <button class="modal-close" type="button" @click="closeCreateGroup" aria-label="Закрыть">×</button>
        <h3>Создать группу</h3>
        <p>Выберите участников и назначьте администратора.</p>
        <label>Название группы</label>
        <input v-model="ui.createGroupName" type="text" placeholder="Название группы" />
        <label>Найти пользователей</label>
        <div class="modal-search">
          <input v-model="ui.createGroupSearchQuery" type="text" placeholder="логин" @keydown.enter.prevent="runCreateGroupSearch" />
          <button class="small" @click="runCreateGroupSearch" :disabled="ui.createGroupSearchInFlight">
            {{ ui.createGroupSearchInFlight ? "Ищем..." : "Найти" }}
          </button>
        </div>
        <div class="search-results">
          <button
            v-for="item in ui.createGroupSearchResults"
            :key="`group-user-${item.id}`"
            class="search-row"
            @click="toggleCreateGroupUser(item)"
          >
            <strong>{{ item.username }}</strong>
            <small>{{ ui.createGroupSelectedUsers.some((user) => user.username === item.username) ? "выбран" : "добавить" }}</small>
          </button>
        </div>
        <label>Администратор</label>
        <select v-model="ui.createGroupAdmin">
          <option :value="session.currentUser">{{ session.currentUser }}</option>
          <option v-for="item in ui.createGroupSelectedUsers" :key="`admin-${item.username}`" :value="item.username">
            {{ item.username }}
          </option>
        </select>
        <div class="selected-chips">
          <span class="member-chip self">{{ session.currentUser }} (вы)</span>
          <span v-for="item in ui.createGroupSelectedUsers" :key="`chip-${item.username}`" class="member-chip">
            {{ item.username }}
          </span>
        </div>
        <div class="modal-actions">
          <button class="primary" @click="createGroup" :disabled="!ui.createGroupName.trim()">Создать</button>
        </div>
      </div>
    </section>

    <section v-if="ui.showForwardPicker && ui.forwardTarget" class="modal-wrap" @click.self="cancelForward">
      <div class="modal forward-modal">
        <button class="modal-close" type="button" @click="cancelForward" aria-label="Закрыть">×</button>
        <h3>Переслать сообщение</h3>
        <p>Выберите получателя и при необходимости добавьте комментарий.</p>
        <div class="forward-preview-card">
          <strong>{{ getProfileDisplayName(ui.forwardTarget.sender) }}</strong>
          <span>{{ excerpt(ui.forwardTarget.text, 160) }}</span>
        </div>
        <div class="forward-section-title">Кому</div>
        <div class="forward-chat-list">
          <button
            v-for="chat in forwardableChats"
            :key="`forward-${chat.peerId}`"
            class="forward-chat-row"
            :class="{ active: ui.forwardRecipient === chat.peerId }"
            @click="selectForwardPeer(chat.peerId)"
          >
            <strong>{{ chatDisplayName(chat) }}</strong>
            <span>{{ chatPreview(chat) }}</span>
          </button>
          <p v-if="forwardableChats.length === 0">Сначала создайте другой чат, чтобы пересылать туда сообщения.</p>
        </div>
        <div class="forward-compose">
          <label>Комментарий</label>
          <textarea
            v-model="ui.forwardDraft"
            rows="3"
            placeholder="Необязательный текст"
          ></textarea>
        </div>
        <div class="modal-actions">
          <button class="primary" @click="submitForward" :disabled="!ui.forwardRecipient">Отправить</button>
        </div>
      </div>
    </section>

    <section v-if="ui.aiReplyModalOpen && ui.aiReplySourceMessage" class="modal-wrap" @click.self="closeAiReplyModal">
      <div class="modal ai-reply-modal">
        <button class="modal-close" type="button" @click="closeAiReplyModal" aria-label="Закрыть">×</button>
        <h3>AI-ответ</h3>
        <p>Сгенерируйте черновик ответа для текущего чата, не открывая отдельный диалог с AI.</p>
        <div class="forward-preview-card">
          <span class="ai-attachment-title">Исходное сообщение</span>
          <strong>{{ getProfileDisplayName(buildAiAttachment(ui.aiReplySourceMessage).sender) }}</strong>
          <span>{{ excerpt(buildAiAttachment(ui.aiReplySourceMessage).text, 220) }}</span>
        </div>
        <div class="forward-compose">
          <label>Инструкция</label>
          <textarea
            v-model="ui.aiReplyInstruction"
            rows="3"
            :disabled="ui.aiReplyLoading"
            placeholder="Опишите тон или цель ответа"
          ></textarea>
        </div>
        <div class="modal-actions ai-reply-top-actions">
          <button class="primary ai-reply-generate-btn ai-accent-btn" @click="generateAiReply" :disabled="ui.aiReplyLoading">
            {{ ui.aiReplyLoading ? "Генерируем..." : "Сгенерировать" }}
          </button>
        </div>
        <div class="ai-reply-suggestion">
          <span class="ai-attachment-title">Предложенный ответ</span>
          <p>{{ ui.aiReplySuggestion || "Здесь появится сгенерированный ответ." }}</p>
        </div>
        <div class="modal-actions">
          <button class="small" @click="insertAiReplyToChat" :disabled="!ui.aiReplySuggestion.trim()">Вставить в чат</button>
        </div>
      </div>
    </section>

    <section v-if="ui.forwardPreviewMessage" class="modal-wrap" @click.self="closeForwardPreview">
      <div class="modal forward-view-modal">
        <h3>Пересланное сообщение</h3>
        <p>Это исходный текст, сохраненный внутри пересланного сообщения.</p>
        <div class="forward-view-card">
          <strong>{{ ui.forwardPreviewMessage.forwardFromSender ? getProfileDisplayName(ui.forwardPreviewMessage.forwardFromSender) : "Неизвестно" }}</strong>
          <span>{{ ui.forwardPreviewMessage.forwardFromText || ui.forwardPreviewMessage.text }}</span>
        </div>
        <div class="modal-actions">
          <button class="small" @click="closeForwardPreview">Закрыть</button>
        </div>
      </div>
    </section>

    <section v-if="ui.showGroupSettings" class="modal-wrap" @click.self="closeGroupSettings">
      <div class="modal group-settings-modal">
        <button class="modal-close" type="button" @click="closeGroupSettings" aria-label="Закрыть">×</button>
        <h3>{{ ui.groupSettingsCanManage ? "Настройки группы" : "Участники группы" }}</h3>
        <p>{{ ui.groupSettingsTitle }}</p>
        <div v-if="ui.groupSettingsLoading" class="search-results"><p>Загрузка...</p></div>
        <template v-else>
          <div class="search-results">
            <div v-for="member in ui.groupSettingsMembers" :key="`member-${member.username}`" class="group-member-row">
              <button class="member-profile-button" type="button" @click="openUserProfile(member.username)">
                <span class="avatar small-avatar" :style="{ background: getProfileAvatarColor(member.username) }">
                  {{ getProfileInitials(member.username) }}
                </span>
                <span class="member-copy">
                  <strong>{{ getProfileDisplayName(member.username) }}</strong>
                  <small>@{{ member.username }} · {{ member.isAdmin ? "администратор" : "участник" }}</small>
                </span>
              </button>
              <div class="group-member-actions">
                <button
                  v-if="ui.groupSettingsCanManage && !member.isAdmin && member.username !== session.currentUser"
                  class="small"
                  @click="removeUserFromGroup(member.username)"
                >
                  Удалить
                </button>
                <button
                  v-if="ui.groupSettingsCanManage && !member.isAdmin"
                  class="small"
                  @click="transferAdmin(member.username)"
                >
                  Назначить
                </button>
              </div>
            </div>
          </div>
          <template v-if="ui.groupSettingsCanManage">
            <label class="group-add-label">Добавить участников</label>
            <div class="modal-search">
              <input v-model="ui.groupAddSearchQuery" type="text" placeholder="логин" @keydown.enter.prevent="runGroupAddSearch" />
              <button class="small" @click="runGroupAddSearch" :disabled="ui.groupAddSearchInFlight">
                {{ ui.groupAddSearchInFlight ? "Ищем..." : "Найти" }}
              </button>
            </div>
            <div class="search-results">
              <button
                v-for="item in ui.groupAddSearchResults"
                :key="`group-add-${item.id}`"
                class="search-row"
                @click="addUserToGroup(item)"
              >
                <strong>{{ item.username }}</strong>
                <small>Добавить</small>
              </button>
            </div>
            <div class="modal-actions group-settings-actions">
              <button class="danger small" @click="deleteCurrentGroup">Удалить группу</button>
            </div>
          </template>
          <div v-else class="modal-actions">
            <button class="secondary" @click="leaveCurrentGroup">Выйти из группы</button>
          </div>
        </template>
      </div>
    </section>

    <section v-if="ui.profileModalOpen" class="modal-wrap" @click.self="closeProfileModal">
      <div class="modal profile-modal">
        <button class="modal-close" type="button" @click="closeProfileModal" aria-label="Закрыть">×</button>
        <div class="profile-modal-head">
          <div class="avatar profile-avatar" :style="{ background: ui.profileForm.avatarColor }">
            {{ computeInitials(ui.profileForm.displayName, ui.profileForm.username) }}
          </div>
          <div>
            <h3>{{ ui.profileViewMode === "edit" ? "Редактировать профиль" : ui.profileForm.displayName }}</h3>
            <p>@{{ ui.profileForm.username }}</p>
          </div>
        </div>

        <div v-if="ui.profileModalLoading" class="profile-loading">Загружаем профиль...</div>

        <template v-else-if="ui.profileViewMode === 'edit'">
          <div class="profile-form">
            <label>Логин</label>
            <input :value="ui.profileForm.username" type="text" readonly />
            <label>Отображаемое имя</label>
            <input v-model="ui.profileForm.displayName" type="text" maxlength="40" />
            <label>О себе</label>
            <textarea v-model="ui.profileForm.bio" rows="4" maxlength="160" placeholder="Пара слов о себе"></textarea>
            <label>Цвет аватара</label>
            <div class="color-row">
              <input v-model="ui.profileForm.avatarColor" type="color" />
              <span>{{ ui.profileForm.avatarColor }}</span>
            </div>
            <div class="profile-facts">
              <span>Создан: {{ ui.profileForm.createdAt ? formatServerTime(ui.profileForm.createdAt) : "неизвестно" }}</span>
              <span>Статус: {{ formatProfileFormStatus() }}</span>
            </div>
          </div>
          <div class="modal-actions">
            <button class="small" @click="closeProfileModal">Отмена</button>
            <button class="primary profile-save-btn" @click="saveOwnProfile" :disabled="!ui.profileForm.displayName.trim()">Сохранить</button>
          </div>
        </template>

        <template v-else>
          <div class="profile-view">
            <p class="profile-bio">{{ ui.profileForm.bio || "Описание пока не заполнено." }}</p>
            <div class="profile-facts">
              <span>Создан: {{ ui.profileForm.createdAt ? formatServerTime(ui.profileForm.createdAt) : "неизвестно" }}</span>
              <span>Статус: {{ formatProfileFormStatus() }}</span>
            </div>
          </div>
          <div class="modal-actions">
            <button class="small" @click="closeProfileModal">Закрыть</button>
          </div>
        </template>
      </div>
    </section>

    <section v-if="auth.visible" class="modal-wrap auth-modal-wrap">
      <div class="modal auth-modal">
        <h3>NexTalk</h3>
        <p v-if="auth.mode === 'choice'">Выберите, как продолжить.</p>
        <p v-else>{{ auth.mode === "login" ? "Войдите в аккаунт" : "Создайте новый аккаунт" }}</p>

        <div v-if="auth.mode === 'choice'" class="welcome-actions">
          <button class="primary" @click="auth.mode = 'login'; auth.error = ''">Войти</button>
          <button class="secondary" @click="auth.mode = 'register'; auth.error = ''">Создать аккаунт</button>
        </div>

        <div v-else class="auth-form">
          <label>Хост</label>
          <input v-model="form.host" type="text" />
          <label>Порт</label>
          <input v-model.number="form.port" type="number" min="1" max="65535" />
          <label>Логин</label>
          <input v-model="form.username" type="text" />
          <label>Пароль</label>
          <input v-model="form.password" type="password" />

          <p v-if="auth.error" class="auth-error">{{ auth.error }}</p>

          <div class="auth-actions">
            <button class="small" @click="auth.mode = 'choice'; auth.error = ''" :disabled="auth.inFlight">Назад</button>
            <button class="primary" @click="beginAuth(auth.mode)" :disabled="auth.inFlight">
              {{ auth.inFlight ? "Подождите..." : auth.mode === "login" ? "Войти" : "Создать и войти" }}
            </button>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>
