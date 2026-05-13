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
const DELETED_MESSAGE_TEXT = "Сообщение удалено";
const AI_CHAT_ID = "__ai__";
const AI_CHAT_NAME = "NexTalk AI";
const AI_CONTEXT_LIMIT = 24;
const AI_SYSTEM_PROMPT =
  "You are NexTalk AI, a helpful assistant embedded in a messenger. Answer clearly and safely. When the user forwards a message, focus on the forwarded content and the user's instruction.";

const bridgeUrl = computed(() => `ws://${window.location.hostname}:5174`);
const aiServiceUrl = computed(() => `http://${window.location.hostname}:5000`);

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
const chatIndex = reactive<Record<string, ChatItem>>({});
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

function stableAvatarColor(username: string) {
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
    return "Local assistant via Ollama";
  }
  if (lastMessage.role === "user") {
    return excerpt(lastMessage.content || (lastMessage.attachment?.text ?? "Forwarded message"), 48);
  }
  return excerpt(lastMessage.content || "Ready to help", 48);
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

  return `Forwarded message:\nSender: ${attachment.sender}\nText: ${attachment.text}\n\nUser task:\n${instruction}`;
}

async function requestAiAnswer(messages: { role: string; content: string }[]) {
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
        : "AI service is unavailable right now.";
    throw new Error(errorText);
  }

  return data.answer.trim();
}

function aiMessageToProviderMessage(message: AIChatMessage) {
  const role = message.role === "user" ? "user" : "assistant";
  return {
    role,
    content: buildAiRequestContent(message.content, message.attachment),
  };
}

function appendAiMessage(message: AIChatMessage) {
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
    pushLog("AI instruction is required for the attached message");
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
        : "AI service is unavailable. Check that Ollama and ai_service are running.",
      createdAt: new Date().toISOString(),
      attachment: null,
    });
  } finally {
    ui.aiSending = false;
  }
}

function openAiReplyModal(message: ChatMessage) {
  if (isMessageDeleted(message)) {
    pushLog("Cannot generate a reply for a deleted message");
    return;
  }

  ui.aiReplyModalOpen = true;
  ui.aiReplyLoading = false;
  ui.aiReplyTargetPeer = session.selectedPeer;
  ui.aiReplySourceMessage = message;
  ui.aiReplyInstruction = "Write a short, polite reply that fits this conversation.";
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
  const instruction = ui.aiReplyInstruction.trim() || "Write a short, polite reply that fits this conversation.";

  ui.aiReplyLoading = true;
  try {
    const answer = await requestAiAnswer([
      {
        role: "system",
        content:
          "You generate short, useful draft replies for messenger chats. Return only the suggested reply text, without explanations or quotes unless the user asks for them.",
      },
      {
        role: "user",
        content:
          `Message to reply to:\nSender: ${attachment.sender}\nText: ${attachment.text}\n\nUser instruction:\n${instruction}`,
      },
    ]);
    ui.aiReplySuggestion = answer;
  } catch (error) {
    pushLog(error instanceof Error ? `AI Reply error: ${error.message}` : "AI Reply error");
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
    return "online";
  }
  if (!profile.lastSeen) {
    return "offline";
  }
  return `last seen ${formatServerTime(profile.lastSeen)}`;
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
    return "No messages yet";
  }

  const sender = chatPreviewSenderLabel(chat);
  return sender ? `${sender}: ${chat.lastText}` : chat.lastText;
}

function parseChatMessage(fields: string[], offset = 0) {
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
      void handleProtocolMessage(parsed);
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

function requestLatestHistory(peer: string) {
  if (!session.loggedIn || !peer || syncingRecentByPeer[peer] || loadingOlderByPeer[peer]) {
    return;
  }

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
    pushLog("Group name is required");
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
    pushLog("Cannot analyze a deleted message");
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
  if (ui.forwardDraft.trim()) {
    fields.push(ui.forwardDraft.trim());
  }

  sendCommand("forward_message", fields);
  const targetChat = chats.value.find((chat) => chat.peerId === peerId);
  pushLog(`Forwarding message #${ui.forwardTarget.id} to ${targetChat?.peer ?? peerId}`);
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
  if (!confirm(`Remove ${username} from the group?`)) {
    return;
  }
  sendCommand("remove_group_member", [ui.groupSettingsChatId, username]);
}

function transferAdmin(username: string) {
  if (!ui.groupSettingsChatId || username === ui.groupSettingsAdminUsername) {
    return;
  }
  if (!confirm(`Make ${username} the new administrator?`)) {
    return;
  }
  sendCommand("transfer_group_admin", [ui.groupSettingsChatId, username]);
}

function leaveCurrentGroup() {
  if (!ui.groupSettingsChatId) {
    return;
  }
  if (!confirm("Leave this group?")) {
    return;
  }
  sendCommand("leave_group", [ui.groupSettingsChatId]);
  closeGroupSettings();
}

function deleteCurrentGroup() {
  if (!ui.groupSettingsChatId) {
    return;
  }
  if (!confirm("Delete this group for everyone?")) {
    return;
  }
  if (!confirm("Please confirm again: delete this group for everyone?")) {
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

        const peer = sentMessage.chatId || peerForMessage(sentMessage);
        pushMessage(peer, sentMessage);
        requestProfiles([sentMessage.sender]);
        await persistSingleMessage(peer, sentMessage);
        if (!updateChatPreviewFromMessage(peer, sentMessage, 0)) {
          fetchChats();
        }
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
        pushLog("History message parse error");
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
        pushLog(`Delete message error: ${b}`);
      }
      break;
    case "message_deleted": {
      const deletedMessage = parseChatMessage(message.fields);
      if (!deletedMessage) {
        pushLog("Deleted message parse error");
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
      pushLog(`History ${a}: ${b}`);
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
      pushLog(`Chats ${a}: ${b}`);
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
      pushLog(`User search ${a}: ${b}`);
      break;
    case "create_chat_result":
      if (a === "ok") {
        closeAddChat();
        fetchChats();
        if (c) {
          selectPeer(c);
        }
      } else {
        pushLog(`Create chat error: ${b}`);
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
        pushLog(`Create group error: ${b}`);
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
        pushLog(`Delete chat error: ${b}`);
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
        pushLog("Mark read failed");
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
        pushLog(`Profile error: ${b}`);
      }
      break;
    case "update_profile_result":
      if (a === "ok") {
        applyProfileFromFields(message.fields.slice(1));
        ui.profileModalOpen = false;
        session.profileMenuOpen = false;
      } else {
        pushLog(`Update profile error: ${b}`);
      }
      break;
    case "profile_item":
      applyProfileFromFields(message.fields);
      pendingProfilesBatch.delete(a);
      break;
    case "profiles_result":
      if (a !== "ok") {
        pushLog(`Profiles error: ${b}`);
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
        pushLog(`Group info error: ${b}`);
      }
      break;
    case "group_update_result":
      pushLog(`Group update ${a}: ${b}`);
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
      pushLog(b || "Chat removed");
      break;
    }
    default:
      pushLog(`Unhandled command: ${message.command}`);
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
          <button class="small" @click="openOwnProfileEditor">Edit profile</button>
          <button class="danger small signout-btn" @click="signOut">Sign out</button>
        </div>
      </div>

      <section class="panel chats-panel">
        <div class="panel-row">
          <h2>Chats</h2>
          <button class="small" @click="openAddChat" :disabled="!session.loggedIn">New</button>
        </div>
        <input v-model="ui.chatFilter" placeholder="Search chats" />
        <div class="chat-list-shell">
          <div class="chat-list">
            <div
              v-for="chat in filteredChats"
              :key="chat.peerId"
              class="chat-row"
              :class="{ active: session.selectedPeer === chat.peerId }"
              @click="selectPeer(chat.peerId)"
            >
              <div class="chat-row-main">
                <strong>{{ chatDisplayName(chat) }}</strong>
                <span class="preview">
                  {{ chatPreview(chat) }}
                </span>
              </div>
              <div class="chat-row-actions">
                <span v-if="chat.unreadCount > 0 && session.selectedPeer !== chat.peerId" class="unread-dot"></span>
                <button v-if="chat.kind === 'dm'" class="chat-delete" @click.stop="deleteChat(chat.peerId)">x</button>
              </div>
            </div>
          </div>
          <div v-if="showAiChatRow" class="ai-chat-block">
            <div class="ai-chat-block-title">Assistant</div>
            <div
              class="chat-row ai-chat-row"
              :class="{ active: session.selectedPeer === AI_CHAT_ID }"
              @click="selectPeer(AI_CHAT_ID)"
            >
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
                ? "Local assistant for analysis, summaries, translations and reply drafting"
                : session.selectedPeer
                ? (session.loggedIn ? `Signed in as ${session.currentUser}` : "Not signed in")
                : "Выберите чат слева"
            }}
          </p>
        </div>
        <div class="top-actions">
          <button v-if="isAiChatSelected" class="small" @click="clearAiChat">Clear</button>
          <button v-if="canOpenDirectProfile && selectedChat" class="small" @click="openUserProfile(selectedChat.peerId)">Profile</button>
          <button v-if="canOpenGroupSettings" class="small" @click="openGroupSettings">People</button>
        </div>
      </header>

      <section ref="messagesViewport" class="messages" @scroll="onMessagesScroll">
        <div v-if="session.selectedPeer && !isAiChatSelected && loadingOlderByPeer[session.selectedPeer]" class="history-loading">
          Loading older messages...
        </div>
        <div v-if="!session.selectedPeer" class="empty-chat">
          <p>Выберите чат в списке слева, чтобы увидеть переписку.</p>
        </div>
        <template v-else-if="isAiChatSelected">
          <div v-if="currentAiMessages.length === 0" class="empty-chat">
            <p>Open NexTalk AI and ask a question, or use Ask AI on any message.</p>
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
                  <span class="ai-attachment-title">Forwarded message</span>
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
                <strong>Forwarded from {{ message.forwardFromSender ? getProfileDisplayName(message.forwardFromSender) : "Unknown" }}</strong>
                <span>{{ excerpt(message.forwardFromText || message.text) }}</span>
              </button>
              <button
                v-if="message.replyToMessageId && !isMessageDeleted(message)"
                class="reply-snippet"
                type="button"
                @click="jumpToMessage(message.replyToMessageId)"
              >
                <strong>{{ message.replyToSender ? getProfileDisplayName(message.replyToSender) : "Message" }}</strong>
                <span>{{ excerpt(message.replyToText || "Deleted message") }}</span>
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
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginReply">Reply</button>
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginForward">Forward</button>
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginAiReply">AI Reply</button>
          <button v-if="activeContextMessage && !isMessageDeleted(activeContextMessage)" class="message-menu-item" @click.stop="beginAskAi">Ask AI</button>
          <button
            v-if="activeContextMessage && canDeleteMessage(activeContextMessage)"
            class="message-menu-item danger-text"
            @click.stop="requestDeleteMessage"
          >
            Delete
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
            <button class="reply-cancel" @click="cancelReply">x</button>
          </div>
          <div v-if="isAiChatSelected && ui.aiPendingAttachment" class="reply-draft ai-pending-draft">
            <div class="reply-draft-copy">
              <small class="ai-attachment-title">Forwarded message</small>
              <strong>{{ getProfileDisplayName(ui.aiPendingAttachment.sender) }}</strong>
              <span>{{ excerpt(ui.aiPendingAttachment.text, 220) }}</span>
            </div>
            <button class="reply-cancel" @click="cancelAiAttachment">x</button>
          </div>
          <div class="composer-row">
            <input
              v-model="ui.messageDraft"
              type="text"
              :placeholder="
                isAiChatSelected && ui.aiPendingAttachment
                  ? 'What should AI do with this message?'
                  : isAiChatSelected
                    ? 'Ask NexTalk AI anything'
                    : 'Write a message'
              "
              :disabled="(!session.selectedPeer || !session.loggedIn) || ui.aiSending"
              @keydown.enter.prevent="sendMessage"
            />
            <button
              class="primary"
              @click="sendMessage"
              :disabled="
                !session.selectedPeer ||
                (!ui.messageDraft.trim() && !(isAiChatSelected && !ui.aiPendingAttachment)) ||
                ui.aiSending
              "
            >
              {{ isAiChatSelected && ui.aiSending ? "Thinking..." : "Send" }}
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

    <section v-if="ui.showNewChatChoice" class="modal-wrap">
      <div class="modal">
        <button class="modal-close" type="button" @click="closeAddChat" aria-label="Close">x</button>
        <h3>New chat</h3>
        <p>Choose what you want to create.</p>
        <div class="welcome-actions">
          <button class="primary" @click="openDirectChatCreator">Direct chat</button>
          <button class="secondary" @click="openGroupChatCreator">Group chat</button>
        </div>
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

    <section v-if="ui.showCreateGroup" class="modal-wrap">
      <div class="modal">
        <button class="modal-close" type="button" @click="closeCreateGroup" aria-label="Close">x</button>
        <h3>Create Group</h3>
        <p>Select members and choose an administrator.</p>
        <label>Group name</label>
        <input v-model="ui.createGroupName" type="text" placeholder="Group name" />
        <label>Find users</label>
        <div class="modal-search">
          <input v-model="ui.createGroupSearchQuery" type="text" placeholder="username" @keydown.enter.prevent="runCreateGroupSearch" />
          <button class="small" @click="runCreateGroupSearch" :disabled="ui.createGroupSearchInFlight">
            {{ ui.createGroupSearchInFlight ? "Searching..." : "Search" }}
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
            <small>{{ ui.createGroupSelectedUsers.some((user) => user.username === item.username) ? "selected" : "tap to add" }}</small>
          </button>
        </div>
        <label>Administrator</label>
        <select v-model="ui.createGroupAdmin">
          <option :value="session.currentUser">{{ session.currentUser }}</option>
          <option v-for="item in ui.createGroupSelectedUsers" :key="`admin-${item.username}`" :value="item.username">
            {{ item.username }}
          </option>
        </select>
        <div class="selected-chips">
          <span class="member-chip self">{{ session.currentUser }} (you)</span>
          <span v-for="item in ui.createGroupSelectedUsers" :key="`chip-${item.username}`" class="member-chip">
            {{ item.username }}
          </span>
        </div>
        <div class="modal-actions">
          <button class="primary" @click="createGroup" :disabled="!ui.createGroupName.trim()">Create</button>
        </div>
      </div>
    </section>

    <section v-if="ui.showForwardPicker && ui.forwardTarget" class="modal-wrap" @click.self="cancelForward">
      <div class="modal forward-modal">
        <button class="modal-close" type="button" @click="cancelForward" aria-label="Close">x</button>
        <h3>Forward message</h3>
        <p>Choose who should receive this message and add an optional comment.</p>
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

    <section v-if="ui.aiReplyModalOpen && ui.aiReplySourceMessage" class="modal-wrap" @click.self="closeAiReplyModal">
      <div class="modal ai-reply-modal">
        <button class="modal-close" type="button" @click="closeAiReplyModal" aria-label="Close">x</button>
        <h3>AI Reply</h3>
        <p>Generate a reply draft for the current chat without opening the full AI conversation.</p>
        <div class="forward-preview-card">
          <span class="ai-attachment-title">Source message</span>
          <strong>{{ getProfileDisplayName(buildAiAttachment(ui.aiReplySourceMessage).sender) }}</strong>
          <span>{{ excerpt(buildAiAttachment(ui.aiReplySourceMessage).text, 220) }}</span>
        </div>
        <div class="forward-compose">
          <label>Instruction</label>
          <textarea
            v-model="ui.aiReplyInstruction"
            rows="3"
            :disabled="ui.aiReplyLoading"
            placeholder="Describe the tone or purpose of the reply"
          ></textarea>
        </div>
        <div class="modal-actions ai-reply-top-actions">
          <button class="primary ai-reply-generate-btn" @click="generateAiReply" :disabled="ui.aiReplyLoading">
            {{ ui.aiReplyLoading ? "Generating..." : "Generate" }}
          </button>
        </div>
        <div class="ai-reply-suggestion">
          <span class="ai-attachment-title">Suggested reply</span>
          <p>{{ ui.aiReplySuggestion || "Generated reply will appear here." }}</p>
        </div>
        <div class="modal-actions">
          <button class="small" @click="insertAiReplyToChat" :disabled="!ui.aiReplySuggestion.trim()">Insert to chat</button>
        </div>
      </div>
    </section>

    <section v-if="ui.forwardPreviewMessage" class="modal-wrap" @click.self="closeForwardPreview">
      <div class="modal forward-view-modal">
        <h3>Forwarded message</h3>
        <p>This is the original content preserved inside the forwarded message.</p>
        <div class="forward-view-card">
          <strong>{{ ui.forwardPreviewMessage.forwardFromSender ? getProfileDisplayName(ui.forwardPreviewMessage.forwardFromSender) : "Unknown" }}</strong>
          <span>{{ ui.forwardPreviewMessage.forwardFromText || ui.forwardPreviewMessage.text }}</span>
        </div>
        <div class="modal-actions">
          <button class="small" @click="closeForwardPreview">Close</button>
        </div>
      </div>
    </section>

    <section v-if="ui.showGroupSettings" class="modal-wrap" @click.self="closeGroupSettings">
      <div class="modal group-settings-modal">
        <button class="modal-close" type="button" @click="closeGroupSettings" aria-label="Close">x</button>
        <h3>{{ ui.groupSettingsCanManage ? "Group settings" : "Group members" }}</h3>
        <p>{{ ui.groupSettingsTitle }}</p>
        <div v-if="ui.groupSettingsLoading" class="search-results"><p>Loading...</p></div>
        <template v-else>
          <div class="search-results">
            <div v-for="member in ui.groupSettingsMembers" :key="`member-${member.username}`" class="group-member-row">
              <button class="member-profile-button" type="button" @click="openUserProfile(member.username)">
                <span class="avatar small-avatar" :style="{ background: getProfileAvatarColor(member.username) }">
                  {{ getProfileInitials(member.username) }}
                </span>
                <span class="member-copy">
                  <strong>{{ getProfileDisplayName(member.username) }}</strong>
                  <small>@{{ member.username }} · {{ member.isAdmin ? "admin" : "member" }}</small>
                </span>
              </button>
              <div class="group-member-actions">
                <button
                  v-if="ui.groupSettingsCanManage && !member.isAdmin && member.username !== session.currentUser"
                  class="small"
                  @click="removeUserFromGroup(member.username)"
                >
                  Remove
                </button>
                <button
                  v-if="ui.groupSettingsCanManage && !member.isAdmin"
                  class="small"
                  @click="transferAdmin(member.username)"
                >
                  Make admin
                </button>
              </div>
            </div>
          </div>
          <template v-if="ui.groupSettingsCanManage">
            <label class="group-add-label">Add members</label>
            <div class="modal-search">
              <input v-model="ui.groupAddSearchQuery" type="text" placeholder="username" @keydown.enter.prevent="runGroupAddSearch" />
              <button class="small" @click="runGroupAddSearch" :disabled="ui.groupAddSearchInFlight">
                {{ ui.groupAddSearchInFlight ? "Searching..." : "Search" }}
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
                <small>Add</small>
              </button>
            </div>
            <div class="modal-actions group-settings-actions">
              <button class="danger small" @click="deleteCurrentGroup">Delete group</button>
            </div>
          </template>
          <div v-else class="modal-actions">
            <button class="secondary" @click="leaveCurrentGroup">Leave group</button>
          </div>
        </template>
      </div>
    </section>

    <section v-if="ui.profileModalOpen" class="modal-wrap" @click.self="closeProfileModal">
      <div class="modal profile-modal">
        <button class="modal-close" type="button" @click="closeProfileModal" aria-label="Close">x</button>
        <div class="profile-modal-head">
          <div class="avatar profile-avatar" :style="{ background: ui.profileForm.avatarColor }">
            {{ computeInitials(ui.profileForm.displayName, ui.profileForm.username) }}
          </div>
          <div>
            <h3>{{ ui.profileViewMode === "edit" ? "Edit profile" : ui.profileForm.displayName }}</h3>
            <p>@{{ ui.profileForm.username }}</p>
          </div>
        </div>

        <div v-if="ui.profileModalLoading" class="profile-loading">Loading profile...</div>

        <template v-else-if="ui.profileViewMode === 'edit'">
          <div class="profile-form">
            <label>Username</label>
            <input :value="ui.profileForm.username" type="text" readonly />
            <label>Display name</label>
            <input v-model="ui.profileForm.displayName" type="text" maxlength="40" />
            <label>Bio</label>
            <textarea v-model="ui.profileForm.bio" rows="4" maxlength="160" placeholder="A few words about you"></textarea>
            <label>Avatar color</label>
            <div class="color-row">
              <input v-model="ui.profileForm.avatarColor" type="color" />
              <span>{{ ui.profileForm.avatarColor }}</span>
            </div>
            <div class="profile-facts">
              <span>Created: {{ ui.profileForm.createdAt ? formatServerTime(ui.profileForm.createdAt) : "unknown" }}</span>
              <span>Status: {{ formatProfileFormStatus() }}</span>
            </div>
          </div>
          <div class="modal-actions">
            <button class="small" @click="closeProfileModal">Cancel</button>
            <button class="primary profile-save-btn" @click="saveOwnProfile" :disabled="!ui.profileForm.displayName.trim()">Save</button>
          </div>
        </template>

        <template v-else>
          <div class="profile-view">
            <p class="profile-bio">{{ ui.profileForm.bio || "No bio yet." }}</p>
            <div class="profile-facts">
              <span>Created: {{ ui.profileForm.createdAt ? formatServerTime(ui.profileForm.createdAt) : "unknown" }}</span>
              <span>Status: {{ formatProfileFormStatus() }}</span>
            </div>
          </div>
          <div class="modal-actions">
            <button class="small" @click="closeProfileModal">Close</button>
          </div>
        </template>
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
