export type ChatMessage = {
  id: number;
  chatId: string;
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

type CachedMessageRecord = ChatMessage & {
  cacheKey: string;
  owner: string;
  peer: string;
};

const DB_VERSION = 1;
const STORE_NAME = "messages";
const DB_PREFIX = "nexTalkMessageCache";
const dbPromises = new Map<string, Promise<IDBDatabase>>();

function getDbName(username: string) {
  return `${DB_PREFIX}:${username}`;
}

function makeCacheKey(username: string, peer: string, message: ChatMessage) {
  return `${username}:${peer}:${message.id}`;
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

function peerRange(peer: string) {
  return IDBKeyRange.bound([peer, 0], [peer, Number.MAX_SAFE_INTEGER]);
}

function toRecord(username: string, peer: string, message: ChatMessage): CachedMessageRecord {
  return {
    ...message,
    cacheKey: makeCacheKey(username, peer, message),
    owner: username,
    peer,
  };
}

export function openMessageCacheDb(username: string): Promise<IDBDatabase> {
  const trimmedUsername = username.trim();
  if (!trimmedUsername) {
    return Promise.reject(new Error("Username is required for message cache"));
  }

  const existing = dbPromises.get(trimmedUsername);
  if (existing) {
    return existing;
  }

  const promise = new Promise<IDBDatabase>((resolve, reject) => {
    const request = window.indexedDB.open(getDbName(trimmedUsername), DB_VERSION);

    request.onupgradeneeded = () => {
      const db = request.result;
      const store = db.objectStoreNames.contains(STORE_NAME)
        ? request.transaction!.objectStore(STORE_NAME)
        : db.createObjectStore(STORE_NAME, { keyPath: "cacheKey" });

      if (!store.indexNames.contains("peerAndId")) {
        store.createIndex("peerAndId", ["peer", "id"], { unique: false });
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
      reject(request.error ?? new Error("Failed to open message cache"));
    };

    request.onblocked = () => {
      dbPromises.delete(trimmedUsername);
      reject(new Error("Message cache database is blocked"));
    };
  });

  dbPromises.set(trimmedUsername, promise);
  return promise;
}

export async function loadCachedMessages(username: string, peer: string): Promise<ChatMessage[]> {
  const db = await openMessageCacheDb(username);
  const transaction = db.transaction(STORE_NAME, "readonly");
  const store = transaction.objectStore(STORE_NAME);
  const index = store.index("peerAndId");
  const records = await waitForRequest(index.getAll(peerRange(peer))) as CachedMessageRecord[];
  return records
    .sort((left, right) => left.id - right.id)
    .map(({ cacheKey: _cacheKey, owner: _owner, peer: _peer, ...message }) => message);
}

export async function trimCachedMessages(username: string, peer: string, limit: number): Promise<void> {
  const db = await openMessageCacheDb(username);
  const transaction = db.transaction(STORE_NAME, "readwrite");
  const store = transaction.objectStore(STORE_NAME);
  const index = store.index("peerAndId");
  const request = index.openCursor(peerRange(peer), "prev");

  await new Promise<void>((resolve, reject) => {
    let kept = 0;
    request.onsuccess = () => {
      const cursor = request.result;
      if (!cursor) {
        resolve();
        return;
      }

      kept += 1;
      if (kept > limit) {
        cursor.delete();
      }
      cursor.continue();
    };
    request.onerror = () => reject(request.error ?? new Error("Failed to trim cached messages"));
  });

  await waitForTransaction(transaction);
}

export async function saveMessageToCache(username: string, peer: string, message: ChatMessage): Promise<void> {
  const db = await openMessageCacheDb(username);
  const transaction = db.transaction(STORE_NAME, "readwrite");
  transaction.objectStore(STORE_NAME).put(toRecord(username, peer, message));
  await waitForTransaction(transaction);
}

export async function saveMessagesToCache(username: string, peer: string, messages: ChatMessage[]): Promise<void> {
  if (messages.length === 0) {
    return;
  }

  const db = await openMessageCacheDb(username);
  const transaction = db.transaction(STORE_NAME, "readwrite");
  const store = transaction.objectStore(STORE_NAME);
  for (const message of messages) {
    store.put(toRecord(username, peer, message));
  }
  await waitForTransaction(transaction);
}
