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
send_message<TAB>bob<TAB>Привет<TAB>15
```

Поля:

1. `chat_id`: username для личного диалога или `group:<id>` для группы;
2. текст сообщения.
3. id сообщения, на которое идет ответ, необязательное поле.

Если третье поле передано, сервер проверяет, что пользователь имеет доступ к
исходному сообщению.

### forward_message

```text
forward_message<TAB>bob<TAB>group:3<TAB>42<TAB>Комментарий
```

Пересылает существующее сообщение в другой чат.

Поля:

1. целевой `chat_id`;
2. исходный `chat_id`;
3. id пересылаемого сообщения;
4. дополнительный текст-комментарий, необязательное поле.

### fetch_history

```text
fetch_history<TAB>bob<TAB>20
fetch_history<TAB>group:3<TAB>20
```

Поля:

1. `chat_id`: username собеседника или `group:<id>`;
2. лимит сообщений, необязательное поле. Если не передан, сервер использует
   значение по умолчанию. Сервер ограничивает слишком большие значения.

### fetch_chats

```text
fetch_chats
```

Запрашивает список чатов текущего пользователя. Сейчас в ответ могут прийти и
личные диалоги, и групповые чаты.

### search_users

```text
search_users<TAB>bo<TAB>group_add<TAB>group:3
```

Ищет пользователей по части имени для создания нового диалога.

Поля:

1. поисковая строка. Разрешены те же символы, что и в имени пользователя.
2. область поиска, необязательное поле: `dm`, `group_create` или `group_add`.
3. `chat_id` группы, необязательное поле для `group_add`.

### create_chat

```text
create_chat<TAB>2
```

Создает пустой личный диалог с существующим пользователем. После успешного
ответа клиент может открыть диалог и загрузить историю.

Поля:

1. id собеседника из `user_search_item`.

### create_group

```text
create_group<TAB>Study group<TAB>alice<TAB>bob,carol
```

Создает групповой чат.

Поля:

1. название группы;
2. username администратора;
3. список участников через запятую. Текущий пользователь добавляется сервером
   автоматически, даже если его нет в списке.

### get_group_info

```text
get_group_info<TAB>group:3
```

Запрашивает название группы, администратора и список участников.

### add_group_members

```text
add_group_members<TAB>group:3<TAB>dave,erin
```

Добавляет участников в группу. Команда доступна только администратору.

### remove_group_member

```text
remove_group_member<TAB>group:3<TAB>dave
```

Удаляет участника из группы. Команда доступна только администратору.
Администратор не может удалить сам себя.

### transfer_group_admin

```text
transfer_group_admin<TAB>group:3<TAB>bob
```

Передает права администратора другому участнику группы.

### leave_group

```text
leave_group<TAB>group:3
```

Выход текущего пользователя из группы. Администратор должен сначала передать
права другому участнику.

### delete_group

```text
delete_group<TAB>group:3
```

Удаляет группу для всех участников. Команда доступна только администратору.

### delete_chat

```text
delete_chat<TAB>bob
```

Удаляет личный диалог с выбранным пользователем вместе с историей сообщений.

Поля:

1. username собеседника.

### mark_read

```text
mark_read<TAB>bob
mark_read<TAB>group:3
```

Помечает чат как прочитанный для текущего пользователя.

Поля:

1. `chat_id`: username собеседника или `group:<id>`.

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
incoming_message<TAB>42<TAB>bob<TAB>2026-05-11 14:01:05<TAB>alice<TAB>bob<TAB>Привет<TAB>15<TAB>bob<TAB>Оригинал<TAB><TAB><TAB>
```

Поля:

1. id сообщения;
2. `chat_id`;
3. время сохранения на сервере;
4. отправитель;
5. получатель или название группы;
6. текст сообщения;
7. id сообщения-ответа, если есть;
8. отправитель исходного сообщения-ответа;
9. текст исходного сообщения-ответа;
10. id пересланного сообщения, если есть;
11. отправитель пересланного сообщения;
12. текст пересланного сообщения.

Такой же формат полезной нагрузки используется в `history_message`. В
`send_message_result` перед ним добавляется поле статуса.

### send_message_result

```text
send_message_result<TAB>ok<TAB>42<TAB>bob<TAB>2026-05-11 14:01:05<TAB>alice<TAB>bob<TAB>Привет<TAB><TAB><TAB><TAB><TAB><TAB>
```

Поля:

1. статус: `ok` или `error`;
2. при `ok` - id сообщения, далее поля сообщения как в `incoming_message`;
3. при `error` - описание ошибки.

### history_message

```text
history_message<TAB>42<TAB>bob<TAB>2026-05-07 12:00:00<TAB>alice<TAB>bob<TAB>Привет<TAB><TAB><TAB><TAB><TAB><TAB>
```

Одно сообщение из истории диалога.
Поля совпадают с полезной нагрузкой `incoming_message`.

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
chat_item<TAB>bob<TAB>bob<TAB>dm<TAB>2026-05-11 14:01:05<TAB>alice<TAB>Привет<TAB>3<TAB>0
chat_item<TAB>group:3<TAB>Study group<TAB>group<TAB>2026-05-11 14:03:10<TAB>bob<TAB>Hi<TAB>1<TAB>1
```

Один диалог в списке чатов.
Для пустого созданного диалога поля последнего сообщения приходят пустыми.

Поля:

1. `peerId`: username для личного диалога или `group:<id>` для группы;
2. отображаемое имя;
3. тип: `dm` или `group`;
4. время последнего сообщения;
5. отправитель последнего сообщения;
6. текст последнего сообщения;
7. количество непрочитанных сообщений;
8. `1`, если текущий пользователь может управлять группой, иначе `0`.

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
create_chat_result<TAB>ok<TAB>bob<TAB>bob
```

Завершает ответ на `create_chat`.

Поля:

1. статус: `ok` или `error`;
2. при `ok` - `chat_id`, при `error` - описание ошибки;
3. при `ok` - отображаемое имя чата.

### create_group_result

```text
create_group_result<TAB>ok<TAB>group:3<TAB>Study group
```

Поля:

1. статус: `ok` или `error`;
2. при `ok` - `chat_id`, при `error` - описание ошибки;
3. при `ok` - название группы.

### group_member_item

```text
group_member_item<TAB>group:3<TAB>alice<TAB>1
```

Один участник группы.

Поля:

1. `chat_id` группы;
2. username участника;
3. `1`, если участник является администратором, иначе `0`.

### group_info_result

```text
group_info_result<TAB>ok<TAB>group:3<TAB>Study group<TAB>alice<TAB>1
```

Завершает ответ на `get_group_info`.

Поля:

1. статус: `ok` или `error`;
2. при `ok` - `chat_id`, при `error` - описание ошибки;
3. название группы;
4. username администратора;
5. `1`, если текущий пользователь может управлять группой.

### group_update_result

```text
group_update_result<TAB>ok<TAB>members added<TAB>group:3
```

Ответ на изменение группы: добавление или удаление участника, передачу
администратора, выход или удаление группы.

Поля:

1. статус: `ok` или `error`;
2. описание результата;
3. `chat_id` группы, если сервер смог его определить.

### chat_removed

```text
chat_removed<TAB>group:3<TAB>removed from group
```

Событие для клиента: чат должен исчезнуть из списка. Используется, когда
пользователя удалили из группы, он вышел сам или администратор удалил группу.

### delete_chat_result

```text
delete_chat_result<TAB>ok<TAB>deleted<TAB>bob
```

Завершает ответ на `delete_chat`.

Поля:

1. статус: `ok` или `error`;
2. описание результата или ошибки;
3. username удаленного собеседника, если сервер смог его определить.

### mark_read_result

```text
mark_read_result<TAB>ok<TAB>bob
```

Завершает ответ на `mark_read`.

Поля:

1. статус: `ok` или `error`;
2. username собеседника (при `ok`) или описание ошибки (при `error`).

## Пример обмена

```text
C -> S: login<TAB>alice<TAB>password
S -> C: login_result<TAB>ok<TAB>logged in as alice
C -> S: send_message<TAB>bob<TAB>Привет
S -> alice: send_message_result<TAB>ok<TAB>42<TAB>bob<TAB>2026-05-07 12:00:00<TAB>alice<TAB>bob<TAB>Привет<TAB><TAB><TAB><TAB><TAB><TAB>
S -> bob: incoming_message<TAB>42<TAB>alice<TAB>2026-05-07 12:00:00<TAB>alice<TAB>bob<TAB>Привет<TAB><TAB><TAB><TAB><TAB><TAB>
S -> alice: info<TAB>delivered to bob
C -> S: fetch_history<TAB>bob<TAB>20
S -> C: history_message<TAB>42<TAB>bob<TAB>2026-05-07 12:00:00<TAB>alice<TAB>bob<TAB>Привет<TAB><TAB><TAB><TAB><TAB><TAB>
S -> C: history_result<TAB>ok<TAB>history with bob: 1 message(s)
```
