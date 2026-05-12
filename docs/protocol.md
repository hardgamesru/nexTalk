# Протокол NexTalk

## Транспорт

Клиент и сервер обмениваются сообщениями поверх TCP.
Одно протокольное сообщение занимает одну строку и завершается символом `\n`.

Формат строки:

```text
command<TAB>field1<TAB>field2\n
```

Поля экранируются:

- `\\` - обратный слеш;
- `\t` - табуляция внутри поля;
- `\n` - перевод строки внутри поля;
- `\r` - возврат каретки внутри поля.


## Команды клиента

### register

```text
register<TAB>alice<TAB>password
```

Поля:

1. имя пользователя;
2. пароль.

### login

```text
login<TAB>alice<TAB>password
```

Поля:

1. имя пользователя.
2. пароль.

### send_message

```text
send_message<TAB>bob<TAB>Привет
```

Поля:

1. получатель;
2. текст сообщения.

### fetch_history

```text
fetch_history<TAB>bob<TAB>20
```

Поля:

1. пользователь, с которым нужно загрузить историю;
2. лимит сообщений, необязательное поле. Если не передан, сервер использует
   значение по умолчанию. Сервер ограничивает слишком большие значения.

### fetch_chats

```text
fetch_chats
```

Запрашивает список личных диалогов текущего пользователя.

### search_users

```text
search_users<TAB>bo
```

Ищет пользователей по части имени для создания нового диалога.

Поля:

1. поисковая строка. Разрешены те же символы, что и в имени пользователя.

### create_chat

```text
create_chat<TAB>2
```

Создает пустой личный диалог с существующим пользователем. После успешного
ответа клиент может открыть диалог и загрузить историю.

Поля:

1. id собеседника из `user_search_item`.

### delete_chat

```text
delete_chat<TAB>bob
```

Удаляет личный диалог с выбранным пользователем вместе с историей сообщений.

Поля:

1. username собеседника.

### quit

```text
quit
```

Завершает клиентскую сессию.

## Ответы и события сервера

### register_result

```text
register_result<TAB>ok<TAB>account created for alice
```

Поля:

1. статус: `ok` или `error`;
2. описание результата.

### info

```text
info<TAB>delivered to bob
```

Информационное сообщение от сервера.

### error

```text
error<TAB>user is offline: bob
```

Ошибка обработки команды.

### login_result

```text
login_result<TAB>ok<TAB>logged in as alice
```

Поля:

1. статус: `ok` или `error`;
2. описание результата.

### incoming_message

```text
incoming_message<TAB>alice<TAB>Привет
```

Поля:

1. отправитель;
2. текст сообщения.

### history_message

```text
history_message<TAB>2026-05-07 12:00:00<TAB>alice<TAB>bob<TAB>Привет
```

Одно сообщение из истории диалога.

Поля:

1. время сохранения сообщения;
2. отправитель;
3. получатель;
4. текст сообщения.

### history_result

```text
history_result<TAB>ok<TAB>history with bob: 1 message(s)
```

Завершает ответ на `fetch_history`.

Поля:

1. статус: `ok` или `error`;
2. описание результата.

### chat_item

```text
chat_item<TAB>2<TAB>bob<TAB>2026-05-11 14:01:05<TAB>alice<TAB>Привет
```

Один диалог в списке чатов.
Для пустого созданного диалога поля последнего сообщения приходят пустыми.

Поля:

1. id собеседника;
2. username собеседника;
3. время последнего сообщения;
4. отправитель последнего сообщения;
5. текст последнего сообщения.

### chat_list_result

```text
chat_list_result<TAB>ok<TAB>1 chat(s)
```

Завершает ответ на `fetch_chats`.

### user_search_item

```text
user_search_item<TAB>bo<TAB>2<TAB>bob
```

Один найденный пользователь.
Сервер не возвращает текущего пользователя и пользователей, с которыми уже есть
диалог; исключение существующих диалогов делается через id пользователей.

Поля:

1. поисковая строка, к которой относится результат;
2. id пользователя;
3. username пользователя.

### user_search_result

```text
user_search_result<TAB>ok<TAB>1 user(s)<TAB>bo
```

Завершает ответ на `search_users`.

Поля:

1. статус: `ok` или `error`;
2. описание результата;
3. поисковая строка.

### create_chat_result

```text
create_chat_result<TAB>ok<TAB>2<TAB>bob
```

Завершает ответ на `create_chat`.

Поля:

1. статус: `ok` или `error`;
2. при `ok` - id собеседника, при `error` - описание ошибки;
3. при `ok` - username собеседника.

### delete_chat_result

```text
delete_chat_result<TAB>ok<TAB>deleted<TAB>bob
```

Завершает ответ на `delete_chat`.

Поля:

1. статус: `ok` или `error`;
2. описание результата или ошибки;
3. username удаленного собеседника, если сервер смог его определить.

## Пример обмена

```text
C -> S: login<TAB>alice
S -> C: login_result<TAB>ok<TAB>logged in as alice
C -> S: send_message<TAB>bob<TAB>Привет
S -> bob: incoming_message<TAB>alice<TAB>Привет
S -> alice: info<TAB>delivered to bob
C -> S: fetch_history<TAB>bob<TAB>20
S -> C: history_message<TAB>2026-05-07 12:00:00<TAB>alice<TAB>bob<TAB>Привет
S -> C: history_result<TAB>ok<TAB>history with bob: 1 message(s)
```
