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

## Пример обмена

```text
C -> S: login<TAB>alice
S -> C: login_result<TAB>ok<TAB>logged in as alice
C -> S: send_message<TAB>bob<TAB>Привет
S -> bob: incoming_message<TAB>alice<TAB>Привет
S -> alice: info<TAB>delivered to bob
```
