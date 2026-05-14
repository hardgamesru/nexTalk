// Небольшие значения удобны для демонстрации пагинации истории на защите:
// быстро видно, что старые сообщения догружаются при прокрутке вверх.
//
// Для обычного режима можно поставить:
//   CACHE_LIMIT_PER_CHAT = 300
//   HISTORY_PAGE_SIZE = 50
export const CACHE_LIMIT_PER_CHAT = 20;
export const HISTORY_PAGE_SIZE = 5;
