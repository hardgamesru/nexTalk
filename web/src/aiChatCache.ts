export type AIAttachment = {
  sender: string;
  text: string;
  sourceChatId: string;
  sourceMessageId: number;
};

export type AIChatMessage = {
  id: string;
  role: "user" | "assistant" | "error";
  content: string;
  createdAt: string;
  attachment: AIAttachment | null;
};

type AIChatCacheRecord = {
  owner: string;
  messages: AIChatMessage[];
};

const DB_VERSION = 1;
const DB_PREFIX = "nexTalkAiCache";
const STORE_NAME = "ai_chat";
const RECORD_KEY = "history";
const dbPromises = new Map<string, Promise<IDBDatabase>>();

function getDbName(username: string) {
  // История AI-чата хранится отдельно для каждого аккаунта браузера.
  // Это не серверная история: ее можно чистить локально без влияния на других.
  return `${DB_PREFIX}:${username}`;
}

function waitForRequest<T>(request: IDBRequest<T>) {
  return new Promise<T>((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("IndexedDB request failed"));
  });
}

function waitForTransaction(transaction: IDBTransaction) {
  return new Promise<void>((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onerror = () => reject(transaction.error ?? new Error("IndexedDB transaction failed"));
    transaction.onabort = () => reject(transaction.error ?? new Error("IndexedDB transaction aborted"));
  });
}

export function openAiChatCacheDb(username: string): Promise<IDBDatabase> {
  const trimmedUsername = username.trim();
  if (!trimmedUsername) {
    return Promise.reject(new Error("Username is required for AI chat cache"));
  }

  const existing = dbPromises.get(trimmedUsername);
  if (existing) {
    return existing;
  }

  const promise = new Promise<IDBDatabase>((resolve, reject) => {
    const request = window.indexedDB.open(getDbName(trimmedUsername), DB_VERSION);

    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME, { keyPath: "owner" });
      }
    };

    request.onsuccess = () => {
      const db = request.result;
      db.onversionchange = () => {
        db.close();
        dbPromises.delete(trimmedUsername);
      };
      resolve(db);
    };

    request.onerror = () => {
      dbPromises.delete(trimmedUsername);
      reject(request.error ?? new Error("Failed to open AI chat cache"));
    };

    request.onblocked = () => {
      dbPromises.delete(trimmedUsername);
      reject(new Error("AI chat cache database is blocked"));
    };
  });

  dbPromises.set(trimmedUsername, promise);
  return promise;
}

export async function loadAiChatHistory(username: string): Promise<AIChatMessage[]> {
  const db = await openAiChatCacheDb(username);
  const transaction = db.transaction(STORE_NAME, "readonly");
  const store = transaction.objectStore(STORE_NAME);
  const record = await waitForRequest(store.get(RECORD_KEY)) as AIChatCacheRecord | undefined;
  const messages = Array.isArray(record?.messages) ? record!.messages : [];
  return messages.filter((message) => message && typeof message === "object");
}

export async function saveAiChatHistory(username: string, messages: AIChatMessage[]): Promise<void> {
  const db = await openAiChatCacheDb(username);
  const transaction = db.transaction(STORE_NAME, "readwrite");
  transaction.objectStore(STORE_NAME).put({
    owner: RECORD_KEY,
    messages,
  } satisfies AIChatCacheRecord);
  await waitForTransaction(transaction);
}

export async function clearAiChatHistory(username: string): Promise<void> {
  const db = await openAiChatCacheDb(username);
  const transaction = db.transaction(STORE_NAME, "readwrite");
  transaction.objectStore(STORE_NAME).delete(RECORD_KEY);
  await waitForTransaction(transaction);
}
