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

### login

```text
login<TAB>alice
```

Поля:

1. имя пользователя.

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

### quit

```text
quit
```

Завершает клиентскую сессию.

## Ответы и события сервера

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
