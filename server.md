# Scrap Mechanic: логика мультиплеера и путь к dedicated server

Статус: исследовательский документ + рабочий прототип нативного listener
(см. §15–§17). Vanilla-клиент уже входил в Game через captured bootstrap;
экспериментальные режимы добавляют Player/Character relay, ограниченные
изменения мира, физику части построек и стриминг сохранённой поверхности
Survival. Это всё ещё **не** полноценный dedicated server: bootstrap зависит
от capture, а полная физика и серверная Lua/gameplay-логика отсутствуют.
Билд: `ScrapMechanic.exe`, SHA-256
`cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`
(IDA, образ `0x140000000`, SizeOfImage `0x1C24000`).

Важно: **готового dedicated server в игре нет.** В билде есть скрытые
флаги `-dedicated_server`, `-use_null_driver` и `-connect_steam_id`
(см. §10), но `-dedicated_server` проверен запуском и **не работает**.
Отдельного серверного exe или готового bootstrap без локального игрока
по-прежнему нет. Ниже описана реальная архитектура текущего мультиплеера
(ListenServer: один из игроков является хостом) и путь к нативному
кроссплатформенному серверу.

Обозначения:

- **✅ проверено** — статически подтверждено по IDA/строкам/декомпиляции
  этого билда.
- **⚠️ гипотеза** — логичное следствие кода, но не проверено запуском.
- **❌ не реализовано** — в проекте/игре этого нет.
- **❌ проверено запуском, не работает** — режим в билде присутствует,
  но при запуске нерабочий.

> **Ревизия 2026-11-01.** §10 добавлен после дополнительного исследования
> того же билда: скрытые флаги, проверка `-dedicated_server` (❌ не работает),
> подтверждение Bullet, формат сохранений SQLite, замеры данных и решение
> строить нативный кроссплатформенный сервер. Разделы 0–9 сохранены как
> исходная карта; устаревшие выводы помечены в §10.
>
> **Ревизия 2026-11-02.** §11–§16: реализован нативный listener, подтверждён
> реальным клиентом control handshake и GameInfo, клиент входит в мир,
> два Steam-аккаунта одновременно находятся в игре на одном listener.
> Текущее состояние, ограничения и план замены replay на настоящую
> симуляцию — в §15 и §16.

> **Ревизия 2026-11-05.** §17: восстановлены высота и сглаживание карты
> Survival, поверхность подключена к Bullet и добавлен ограниченный стриминг
> ячеек рядом с активными игроками и динамическими объектами. Границы соседних
> ячеек совпали на сохранённой карте. Совместимость этой поверхности с живым
> клиентом и точная physics parity с игрой пока не подтверждены.
>
> **Ревизия 2026-11-06.** Начальная синхронизация ID11/ID13 теперь обновляет
> только распознанные BlobData-менеджеры с подтверждённым сопоставлением
> сетевого UUID и SQLite UUID. Неизвестные менеджеры остаются из захвата;
> запуск Survival с таким набором ещё не проверен живым клиентом.
>
> Физический стример также добавляет полные voxel chunks из выбранных `.tile`
> и применяет к ним сохранённые `VoxelTerrain` изменения после проверки
> региона и обновляет collision mesh. Две распознанные ID25 ScriptData-группы обновляются из SQLite
> при совпадении минимум трёх ключей; совместимость ещё не проверена клиентом.
>
> В `--native-players` добавлен явный `PaintCharacter` для активного персонажа
> того же мира: сервер сверяет runtime ID, обновляет сохранённый на время
> процесса spawn-state и рассылает NetObj state. Цвет сохраняется в копии мира
> сервера и восстанавливается при следующем запуске. CharacterSet JSON5
> загружаются по UUID: reset использует `paint.defaultColor`, а
> `paint.enable=false` запрещает операцию; нулевой UUID игрока сохраняет
> подтверждённый цвет `0xffffffff`. Таргетинг NPC и проверка владения
> инструментом всё ещё не реализованы.

---

## 0. Краткий вывод

1. Мультиплеер Scrap Mechanic — это **listen server**: хост-игрок запускает
   и клиент, и сервер в одном процессе. Отдельного процесса/режима
   «только сервер» игра не предоставляет.
2. Транспорт — **Steam Networking Sockets** (`SteamNetworkingSockets012`),
   поверх него собственный протокол: reliable/unreliable каналы, сжатие,
   чанки, версия/чек-суммы, NetObj-репликация.
3. Серверная сторона в процессе хоста — это обычные игровые классы
   `NetworkServer` / `SteamNetworkServer` / `ClientCommunication` /
   `NetObjManager` + серверный Lua-менеджер + физика. Клиентская сторона —
   `NetworkClient` / `SteamNetworkClient` / клиентский Lua-менеджер + рендер.
4. Позиции персонажей реплицируются как **unreliable NetObj-апдейты**
   (`NetObjUnreliableType::Character`, тип 3; `CharacterTumble`, тип 4).
   Хост принимает клиентские Character-апдейты и применяет их к своему
   Character (проверки владельца в найденном обработчике нет).
5. ~~Реалистичный путь к dedicated server — **headless-хост из самой
   игры**.~~ **Ревизия 2026-11-01:** скрытый `-dedicated_server` найден в
   бинаре, но проверен запуском и не работает. Актуальный путь — **нативный
   кроссплатформенный сервер**: та же физика Bullet + кастомный физический
   слой игры, протокол и серверный Lua; см. §10.

---

## 1. Что сейчас есть: ListenServer

✅ В билде присутствуют строки/классы:

- `ListenServer.cpp`
- `NetworkServer.cpp`, `SteamNetworkServer.cpp`
- `NetworkClient.cpp`, `SteamNetworkClient.cpp`
- `ClientCommunication.cpp`
- `NetObjManager.cpp`, `NetObjReceiver.cpp`, `NetworkSendInterface.cpp`
- `CharacterManager.cpp`, `CharacterManagerClient.cpp`
- `MyPlayer.cpp`, `Player.cpp`, `PlayerManager.cpp`, `CharacterController.cpp`
- `SteamNetworkingSockets012`

✅ Строки состояния:

- `Hosting game`
- `Playing game as client`
- `Connecting to <...>`
- `Failed to send reliable network packet! Result:`
- `Failed to send unreliable network packet! size:`
- `Failed to decompress network packet`
- `Processing initial packet from server tick:`
- `Unprocessed network message. Id:`

⚠️ Отсюда модель:

```
Игрок-хост:
  [локальный игрок/камера/ввод] + [сервер: мир, физика, Lua server] +
  [SteamNetworkServer] + [локальный клиент: рендер, HUD, Lua client]

Обычный клиент:
  [локальный игрок/камера/ввод] + [SteamNetworkClient] +
  [клиентский Lua] + [рендер]
```

Сервер и клиент в одном процессе хоста общаются через те же сетевые
абстракции (loopback/локальные вызовы), поэтому «сервер» не является
отдельным продуктом.

---

## 2. Проверенная карта сетевой архитектуры

### 2.1. Транспорт

✅ Игра использует Steam Networking Sockets:

- интерфейс `SteamNetworkingSockets012`;
- `SteamInternal_FindOrCreateUserInterface`,
  `SteamInternal_FindOrCreateGameServerInterface`,
  `SteamAPI_Init`, `SteamAPI_RunCallbacks`;
- собственные каналы: reliable / unreliable;
- сжатие пакетов, разбиение на чанки, проверка размера;
- версия игры/сохранения, чек-суммы файлов, список UGC.

✅ В `SteamNetworkServer.cpp`/`SteamNetworkClient.cpp` есть обработка:

- `Failed to decompress network packet! Error code:`
- `Failed to send reliable split network packet! chunk:`
- `Packet size is over maximum guaranteed limit. Size:`
- `Network channel `, `Skip sending unreliable network data to client`
- `Budget is currently:`

Это не готовый API для стороннего сервера: формат сообщений — внутренний
протокол игры, а не документированный Steam-протокол.

### 2.2. Классы и роли

| Класс/файл | Роль |
|---|---|
| `ListenServer` | хост-сессия: поднимает сервер внутри клиента |
| `NetworkServer` | общая серверная логика: клиенты, reliable/unreliable очереди |
| `SteamNetworkServer` | Steam-транспорт сервера, соединения, сжатие/чанки |
| `NetworkClient` | клиентская логика приёма пакетов, тики сервера |
| `SteamNetworkClient` | Steam-транспорт клиента, connect/handshake |
| `ClientCommunication` | server-side обработка клиентских команд (create/remove/paint/character) |
| `NetObjManager` | менеджер сетевых объектов (NetObj), initial/update/state |
| `NetObjReceiver` | разбор входящих NetObj-апдейтов, вызов apply |
| `NetworkSendInterface` | отправка reliable/unreliable, батчи, каналы |
| `CharacterManager` | серверный менеджер персонажей |
| `CharacterManagerClient` | клиентская часть менеджера персонажей |
| `MyPlayer` | локальный игрок: камера, ввод, предсказание |
| `Player` | игрок как сущность (ID, Steam, Character) |
| `Character` | персонаж: физика, сетевой state, ring-буферы |
| `CharacterController` | физический контроллер персонажа |
| `LuaManager` | Lua VM; отдельно клиентский и серверный контексты |

### 2.3. Глобалы и офсеты (RVA от базы)

✅ Проверенные глобалы:

| Назначение | RVA / путь |
|---|---|
| Registry персонажей | `0x1A69638` (sentinel `+0x10`, count `+0x18`; node key `+0x10`, Character* `+0x18`) |
| Серверный объект (в процессе хоста) | `0x1A69610`; `+0x18` — число клиентов |
| Локальный Steam ID | `0x1A69660` |
| Локальный host (MyPlayer) | `0x1A69110` → Player `+0x70` → Player ID `+0x10`, Character ID `+0x14` |
| Lua-менеджер (клиентский) | `0x1A696B0`; wrapper `+0x358` → `lua_State*`; script map `+0x258` sentinel, `+0x260` count |
| Game object | `0x1A69500` |

✅ Character (ключевое для репликации):

| Поле | Смысл |
|---|---|
| `+0x20` | Character ID |
| `+0x24` | int32 validity (`!= -1`) |
| `+0x30` | World ID (uint16) |
| `+0x38` | Steam-владелец |
| `+0x40` | isPlayer |
| `+0x44` | owner ID для Unit-скрипта |
| `+0x932` | mode: 1/2 обычные, 4 tumbling |
| `+0xB68` | proxy → `+0x40` → ghost object → `+0x40` = позиция |
| `+0x940` | ring-объект обычного сетевого state (count `+0xF00`, cursor `+0xF04`, 40×96 байт) |
| `+0x948` | ring-объект tumble-state (count `+0x1180`, cursor `+0x1184`, 40×112) |
| `+0xCE0` | 5×16-байтный блок сетевого state; первые 16 байт — сетевая позиция |

### 2.4. Поток подключения (статическая реконструкция)

⚠️ Ниже — реконструкция по классам/строкам/декомпиляции; живого packet capture
в этом исследовании не было.

1. Клиент запускает `SteamAPI_Init`, получает `SteamNetworkingSockets012`,
   подключается к Steam ID хоста.
2. `SteamNetworkClient` проходит handshake: версия игры, пароль/приглашение,
   проверка чек-сумм файлов, список UGC/модов.
3. Сервер (`SteamNetworkServer` / `NetworkServer` / `ClientCommunication`)
   регистрирует клиента, отправляет initial-пакеты:
   - `ID_C_SCRIPT_INIT_DATA_SERVER2CLIENT`
   - `ID_C_GENERIC_INIT_DATA_SERVER2CLIENT`
   - `ID_C_SCRIPT_DATA_SERVER2CLIENT`
   - `ID_C_GENERIC_DATA_SERVER2CLIENT`
4. После initial-фазы идут обычные игровые сообщения и NetObj-апдейты.
5. `NetworkClient` проверяет `m_uLastProcessedServerTick` и
   `uServerTick >= m_uLastProcessedServerTick`; initial-пакеты обрабатываются
   до обычных.
6. Входящие NetObj-апдейты разбирает `NetObjReceiver` и вызывает
   `applyUnreliableUpdate`/state-apply соответствующего NetObj.

✅ В `NetworkClient.cpp` есть проверки:

- `!bInitialization || u8PacketIdentifier == ID_C_SCRIPT_INIT_DATA_SERVER2CLIENT || ...`
- `m_uLastProcessedServerTick == 0 || uServerTick >= m_uLastProcessedServerTick`
- `Unprocessed network message. Id: `

### 2.5. NetObj и персонажи

✅ `NetObjUnreliableType`:

- `Character` = 3
- `CharacterTumble` = 4

✅ Обработчик `Character::applyUnreliableUpdate` — RVA `0x730D40`:

```
void(Character* self, char type, unsigned char* parsedState, unsigned netId)
```

- type 3: берёт `parsedState+128` (5×16 байт) и кладёт в ring `Character+0x940`
  через `0x732C10`;
- type 4: берёт `parsedState+16` и кладёт в tumble-ring `Character+0x948`
  через `0x733360`;
- в `parsedState+128` три float — сетевая мировая позиция, `+140` — ноль.

✅ Сетевой state персонажа (type 3) в `0x730A00`:

- `+128/+132/+136` — позиция;
- `+144/+148/+152` — скорость/импульс (сжатые float);
- `+160/+176/+180/+184` — дополнительные поля (если установлен бит);
- `+208` — 8 бит флагов;
- `+192` — 3 бита;
- `+193` — 1 бит.

✅ Отправка состояния персонажа — `0x735460`:

- type 3/4 в BitStream;
- позиция читается **из живого ghost**:
  `Character+0xB68 → proxy+0x40 → +0x40`;
- rotation берётся из history `Character+0x580` (это не позиция);
- на клиенте для локального игрока позиция в пакете — это ghost, а не
  отдельный «сетевой кэш».

✅ Политика отправки/коррекции `0x72FCF0` (slot 22 vtable Character):

- для Player, чей владелец совпадает с адресатом, — код 5;
- устаревший тик — 1;
- специальный режим/игрок — 2;
- расхождение позиции пакета и ghost ≥ 64 world units — 2 (принудительная
  коррекция);
- иначе 3/4.

✅ `Character update 0x728260`:

- на клиенте для локального игрока сэмплирует ring `+0x940` через
  `0x732CC0` и может писать сэмпл в `ghost+0x40`;
- на хосте для чужого игрока также сэмплирует клиентский ring и применяет
  позицию к своему ghost (со сглаживанием/веткой stall);
- local mode должен быть 1/2 для обычного движения; mode 4 — tumbling.

✅ Хост-обработчик клиентского пакета — `0x140B382E0`:

- читает Steam ID отправителя и находит клиента в серверной карте
  (`qword_141A69610 + 8`);
- разбирает 4-битный тип; для type 3 вызывает `0x730A00`, находит Character
  по ID из пакета (`0x1407FC150` + registry `0x140821A30`);
- вызывает Character slot 24 (`0x730D40`);
- **проверки владельца персонажа нет** — сверяется только ID найденного
  Character с ID из пакета. Это важный факт и для dedicated server
  (безопасность/валидация), и для читов.

### 2.6. Доверяет ли хост клиенту

✅ По коду:

- клиент передаёт позицию своего персонажа в NetObj type 3;
- хост принимает её и кладёт в ring персонажа;
- в `Character update` хост для чужого игрока применяет сэмпл к своему
  ghost, то есть движение персонажа в основном **клиент-авторитетное**,
  с сглаживанием и коррекцией при расхождении/стойле;
- политика `0x72FCF0` принудительно шлёт коррекцию при расхождении ≥ 64
  world units.

⚠️ Следствие: dedicated server должен либо доверять клиентским Character-
апдейтам (как сейчас), либо добавить собственную валидацию (скорость,
телепорты, коллизии). Иначе клиент сможет телепортироваться/летать.

### 2.7. Lua и физика

✅ Lua-контексты разделены:

- серверный Lua: `server_onCreate`, `server_onFixedUpdate`, `sv_*`,
  `self.saved.stats` (HP юнитов), серверные менеджеры;
- клиентский Lua: `client_onCreate`, `client_onFixedUpdate`, `cl_*`,
  `client_onClientDataUpdate`, камера/HUD/эффекты;
- общий fixed dispatcher `0x832500` обслуживает **оба** менеджера; нельзя
  привязываться к thread id.

✅ Серверная физика/мир:

- физический мир, создания, NPC, CharacterController;
- серверный `CharacterManager` и `Character` с ring-буферами;
- сохранения/база: `DatabaseManager`, `GameInfo`, `m_uGameTick`, seed,
  версия save, чек-суммы, UGC.

⚠️ Dedicated server обязан крутить серверный fixed update и физику. Без
рендера это возможно, но многие системы завязаны на локального игрока
(`MyPlayer`, камера, ввод), а не только на сервер.

---

## 3. Почему dedicated server отсутствует

✅ В билде нет отдельного серверного executable/режима. Причины, видимые по
архитектуре:

1. **Единый процесс.** `ListenServer` поднимает сервер внутри клиента;
   серверные и клиентские системы используют одни и те же глобалы
   (`0x1A69610`, `0x1A69638`, `0x1A69110`).
2. **Локальный игрок обязателен.** `MyPlayer`, `PlayerCamera`, ввод, HUD,
   камера и часть Lua-скриптов создаются для локального игрока. Серверный
   код местами ожидает host/Player.
3. **Рендер и Steam-клиент.** Окно, D3D11 Present, аудио, Steam overlay —
   часть клиентского bootstrap. Headless-режим требует их отключения.
4. **Нет CLI/конфига сервера.** Нет найденного `--dedicated`,
   `server.cfg`, консольного bootstrap мира/порта/слотов.
5. **Протокол внутренний.** Формат пакетов, сжатие, NetObj-схемы и порядок
   initial-сообщений не документированы; сторонний сервер должен их
   повторить.

---

## 4. Два пути к dedicated server

> **Ревизия 2026-11-01.** Путь A проверен запуском: флаг
> `-dedicated_server` в билде есть, но режим не работает. Актуальный
> путь — нативный кроссплатформенный сервер (путь B); см. §10.
> Оценка «месяцы/годы» для пути B остаётся верной.

### Путь A (устарел): headless-хост из самой игры

Идея: запускать **тот же** `ScrapMechanic.exe`, но без окна/рендера/аудио/
ввода и без локального игрока; внутри процесса поднять `NetworkServer` /
`SteamNetworkServer`, загрузить мир, принимать клиентов, крутить fixed update,
сохранять.

Плюсы:

- используется родная физика, Lua, NetObj, сохранения и протокол;
- не нужно переписывать сетевой формат;
- максимум совместимости с обычными клиентами.

Минусы/сложности:

- нужно найти и обойти клиентский bootstrap (окно, Present, аудио, ввод);
- нужно сделать локального игрока необязательным или создать «серверного»
  игрока без клиента;
- Steam: клиентский `SteamAPI_Init` vs game server interface; возможно,
  потребуется отдельный Steam GameServer login/App ID;
- защита/проверки модулей: клиенты могут сверять модули; dedicated server
  должен либо разрешать модифицированных клиентов, либо быть без модов;
- возможны скрытые зависимости от `MyPlayer`/камеры в серверных скриптах.

Этот путь ближе всего к проекту: у нас уже есть DLL-инъекция, MinHook,
перехват Present и понимание глобалов. Headless-режим можно сделать как
опцию DLL/лаунчера, не меняя файлы игры.

### Путь B: ререализация сервера по протоколу

Идея: отдельный процесс, который говорит на протоколе игры: Steam
Networking Sockets, reliable/unreliable, initial-пакеты, NetObj-схемы,
серверный Lua/физика.

Плюсы:

- настоящий отдельный сервер, не зависящий от клиентского exe;
- можно добавить валидацию, админку, масштабирование.

Минусы:

- нужно восстановить **весь** формат сообщений и схемы NetObj;
- нужна серверная физика/мир (или эмуляция) и серверный Lua;
- нужна совместимость с версиями игры/модами/чек-суммами;
- объём работы — месяцы/годы, а не дни.

Для первого рабочего dedicated server Path A практичнее.

---

## 5. Пошаговый план Path A

### Этап 1. Найти серверный bootstrap в IDA

✅ Уже есть строки/классы-ориентиры:

- `ListenServer.cpp`, `NetworkServer.cpp`, `SteamNetworkServer.cpp`;
- `Hosting game`, `Playing game as client`;
- `SteamNetworkingSockets012`;
- `ID_C_*` identifiers;
- `qword_141A69610` (серверный объект) и его запись.

Что сделать:

1. Найти xref на строку `Hosting game` → функция запуска хоста.
2. Проследить создание `NetworkServer`/`SteamNetworkServer` и запись
   `qword_141A69610`.
3. Найти загрузку мира/save: `GameInfo`, `DatabaseManager`, `m_uGameTick`,
   seed, версия save.
4. Найти создание локального `MyPlayer`/`Player`/камеры и точку, где
   серверный bootstrap зависит от локального игрока.
5. Найти `ListenServer` конструктор/`Start` и точку входа из меню/CLI.

### Этап 2. Headless-режим

Нужно отключить/заменить:

- создание окна и swapchain;
- D3D11 Present/рендер;
- аудио;
- ввод/курсор/ImGui;
- Steam overlay;
- загрузку клиентских ассетов/рендер-моделей (по возможности).

Варианты:

- патч/хук на `CreateWindowEx`/`Present`/`D3D11CreateDevice` — возвращать
  заглушки;
- отдельный «null renderer»;
- запуск с offscreen-устройством (WARP) и без показа окна.

Риск: игра может требовать реальный swapchain для инициализации графики.
Тогда проще создать минимальный D3D11 device/swapchain 1×1 и не показывать окно.

### Этап 3. Сервер без локального игрока

Самый сложный пункт. Нужно решить одну из моделей:

**A1. Скрытый host-игрок.** Создать обычного host-игрока, но без камеры/
ввода/рендера. Он существует как Player/Character, сервер считает его
хостом. Клиенты подключаются как обычные игроки. Плюс: минимум изменений
в Lua/серверной логике. Минус: host-персонаж стоит в мире; нужен idle-
контроль или невидимость.

**A2. Сервер без Player.** Убрать создание локального Player/MyPlayer,
оставить только серверные менеджеры. Плюс: настоящий dedicated. Минус:
многие проверки/скрипты ожидают host/Player; потребуется аудит и заглушки.

**A3. Host как клиент.** Запустить сервер, а «хост-игрок» подключается
отдельным клиентом (второй процесс/машина). Серверный процесс не имеет
локального игрока. Это ближе к классическому dedicated, но требует
разделения bootstrap.

Для первой версии A1 обычно быстрее; для полноценного сервера — A2/A3.

### Этап 4. Steam GameServer

✅ В импортах есть `SteamInternal_FindOrCreateGameServerInterface` и
`SteamAPI_Init`.

Нужно:

- инициализировать Steam GameServer (анонимно или с логоном);
- указать App ID Scrap Mechanic;
- открыть сокет/принять соединения через `SteamNetworkingSockets012`;
- использовать серверные Steam ID клиентов;
- проверить, не требует ли игра клиентский Steam-контекст для host.

⚠️ Юридически/технически: dedicated server для игры, у которой его нет,
может нарушать Steam App ID/лицензию. Это ответственность владельца
сервера; технически игра не предоставляет такой режим.

### Этап 5. Мир, сохранения, fixed update

- Загрузить save/world (Survival/Creative/Challenge) из CLI.
- Запустить серверный fixed update (физика, NPC, Lua server).
- Сохранять мир по таймеру/команде.
- Обрабатывать game tick, seed, версию save, UGC/моды.

✅ В билде есть `DatabaseManager`, `GameInfo`, `m_uGameTick`, seed,
версия save, чек-суммы, список UGC. Это ориентиры для IDA.

### Этап 6. Сеть и персонажи

- Оставить родные `NetworkServer`/`SteamNetworkServer` и `NetObjManager`.
- Принимать NetObj type 3/4 от клиентов.
- Решить политику доверия:
  - как сейчас: применять клиентскую позицию;
  - или валидировать скорость/телепорты/коллизии (потребует серверной
    симуляции персонажа, а не только применения сэмпла).
- Проверить, что хост без локального игрока корректно обрабатывает
  `Character update 0x728260` для чужих игроков (ветка remote player).

### Этап 7. Админка и наблюдаемость

- Консоль: status, players, kick, ban, save, stop.
- Логи: connect/disconnect, version/checksum mismatch, packet errors.
- Метрики: tick rate, players, NetObj queue, memory.
- Краш-логи/дампы.

### Этап 8. Тесты

- 1 клиент: подключение, спавн, движение, взаимодействия, выход.
- 2 клиента: видят друг друга, синхронизация персонажей/созданий.
- Рестарт сервера: save/load.
- Клиент с другой версией/модами: корректный отказ/сообщение.
- Долгий аптайм: утечки, деградация tick rate.

---

## 6. Чеклист «минимальный dedicated server»

- [ ] Headless-запуск без окна/Present/аудио/ввода.
- [ ] Серверный bootstrap без локального игрока (или скрытый host).
- [ ] Загрузка мира/save из CLI.
- [ ] Steam GameServer login + приём клиентов.
- [ ] Родной `NetworkServer`/`NetObjManager`/`CharacterManager`.
- [ ] Fixed update физики и серверного Lua.
- [ ] Сохранение мира.
- [ ] Консоль/логи/kick/ban.
- [ ] Валидация клиентских Character-апдейтов (иначе полёты/телепорты).
- [ ] Проверка совместимости версий/модов/чек-сумм.
- [ ] Тесты 1–2 клиента и рестарт.

---

## 7. Риски и ограничения

1. **Нет гарантии, что игра запустится без графики.** Возможны скрытые
   зависимости от swapchain/окна/Steam overlay.
2. **Локальный игрок глубоко в коде.** `MyPlayer`, камера, ввод и часть Lua
   могут быть обязательны; A2/A3 потребуют много правок.
3. **Steam GameServer.** Неизвестно, поддерживает ли игра анонимный
   game server login и нужный App ID без клиента. Это надо проверять
   экспериментально.
4. **Доверие клиенту.** Сейчас хост применяет клиентские Character-апдейты
   без проверки владельца. Для публичного сервера нужна валидация, иначе
   любой клиент сможет менять чужого персонажа/летать.
5. **Моды/чек-суммы.** Игра проверяет файлы/UGC; dedicated server должен
   либо требовать ванильные клиенты, либо разрешать моды.
6. **Юридические ограничения.** Запуск собственного сервера для игры без
   официального dedicated-режима — на ответственность владельца; не
   используйте чужие App ID/аккаунты без разрешения.
7. **Версионная привязка.** Все офсеты/классы — для SHA-256
   `cf403cf9...`; после обновления игры их нужно перепроверять.

---

## 8. Приложение: полезные ориентиры для IDA

### Глобалы

| RVA | Что |
|---|---|
| `0x1A69610` | серверный объект (в процессе хоста) |
| `0x1A69638` | registry персонажей |
| `0x1A69660` | локальный Steam ID |
| `0x1A69110` | MyPlayer host |
| `0x1A696B0` | клиентский LuaManager |
| `0x1A69500` | game object |
| `0x1A690D8` | указатель на объект игры (`0x3B0` байт; настройки `+0x2E8`) |
| `0x1A690D0` | Steam/app-объект, создаётся в bootstrap |
| `0x1A69193` | глобальный флаг `-dedicated_server` (см. §10.2) |

### Функции/офсеты

| RVA | Что |
|---|---|
| `0x730D40` | `Character::applyUnreliableUpdate` (type 3/4) |
| `0x730A00` | разбор Character network state |
| `0x732C10` | запись обычного сэмпла в ring `+0x940` |
| `0x733360` | запись tumble-сэмпла в ring `+0x948` |
| `0x732CC0` | сэмплер ring (Character update) |
| `0x728260` | Character per-tick update |
| `0x735460` | сериализация Character network state; позиция берётся из ghost |
| `0x72FCF0` | политика отправки/коррекции |
| `0x140B382E0` | хост-обработчик клиентского пакета |
| `0x1407FC150` | поиск Character по ID из пакета (ориентир) |
| `0x140821A30` | resolver registry |
| `0x832500` | fixed dispatcher Lua (оба менеджера) |
| `0x474BC0` | парсер командной строки (скрытые флаги, §10.2) |
| `0x2E3200` | init игры; копирование опций в объект игры |

### Классы/строки для поиска bootstrap

- `ListenServer.cpp`
- `NetworkServer.cpp`
- `SteamNetworkServer.cpp`
- `NetworkClient.cpp`
- `SteamNetworkClient.cpp`
- `ClientCommunication.cpp`
- `NetObjManager.cpp`
- `NetObjReceiver.cpp`
- `NetworkSendInterface.cpp`
- `SteamNetworkingSockets012`
- `Hosting game`
- `Playing game as client`
- `ID_C_SCRIPT_INIT_DATA_SERVER2CLIENT`
- `ID_C_GENERIC_INIT_DATA_SERVER2CLIENT`
- `ID_C_SCRIPT_DATA_SERVER2CLIENT`
- `ID_C_GENERIC_DATA_SERVER2CLIENT`

---

## 9. Статус проверки

- ✅ Статические офсеты/классы/строки — IDA по указанному билду.
- ✅ Поток NetObj/Character — декомпиляция и xrefs.
- ⚠️ Поток подключения/handshake — реконструкция по классам/строкам,
  без packet capture.
- ❌ Headless/dedicated bootstrap — скрытый `-dedicated_server` найден, но
  проверен запуском и не работает (§10); нативный сервер не реализован.
- ❌ Steam GameServer login — не проверялся.
- ✅/⚠️ Доверие хоста клиенту — обработчик найден; отсутствие проверки
  владельца видно в коде, но полнота сетевой модели не доказана.

Этот документ — карта для реализации, а не готовый сервер. Ревизия §10:
актуальный первый практический шаг — Phase 0 нативного сервера (protocol
probe, физические параметры, SQLite-читатель мира, скелет headless-цикла),
а не headless-патч клиента.

---

## 10. Ревизия 2026-11-01: скрытые флаги, проверка запуском и нативный сервер

Статус: дополнительное исследование того же билда
(`cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`).
Разделы 1–9 сохранены как исходная карта; здесь зафиксированы новые факты
и решение строить нативный кроссплатформенный сервер.

### 10.1. Итог ревизии

✅ SHA-256 `ScrapMechanic.exe` повторно совпал с документом.

✅ В бинаре найдены скрытые флаги `-dedicated_server`, `-use_null_driver`,
`-connect_steam_id`, `-open`, `-console`, `-dev`, `-builtin_mods_only`,
`--ugc` и другие.

❌ `-dedicated_server` проверен запуском: режим **не работает**. Это,
вероятно, внутренний/незавершённый dev/QA-путь, а не готовый dedicated
server. Статическое наличие флага и его чтение в коде не означает, что
режим пригоден для эксплуатации. Решение: скрытый режим больше не
рассматриваем как путь к серверу.

✅ `-connect_steam_id <SteamID64>` клиент принимает: в логе
`Contraption::start -connect_steam_id <STEAMID64>`. Это важный факт
для нативного сервера: клиенту можно дать ярлык/лаунчер с прямым
подключением к SteamID, без сервер-браузера и без мода.

✅ Официальная сборка — **Windows-only** (Steam appdetails, AppID 387990:
`windows: true, linux: false`). Нативного Linux-бинаря игры нет.

✅ Физика — статически влинкованный **Bullet** + кастомный физический
слой игры (подробнее §10.3).

✅ Сохранения мира — **SQLite** (подробнее §10.4), а не закрытый бинарный
формат.

### 10.2. Скрытые флаги: статическая карта

Все адреса — RVA от базы `0x140000000`; строки — RVA в `.rdata`.

| Флаг | Строка (RVA) | Обработчик | Что делает |
|---|---|---|---|
| `-dedicated_server` | `0x14DB4B0` | compare в `0x474FFD`; `[opt+0x66]=1` в `0x475012` | Копируется в глобал `0x1A69193`; читается в 15+ участках. Запуском проверено: ❌ не работает. |
| `-use_null_driver` | `0x14DB6C0` | compare в `0x475B8B`; `or dword [opt+0x74], 8` в `0x475BA0` | Бит 3 в поле настроек; вероятно, нулевой рендер-драйвер. |
| `-console` | `0x14DB4C8` | `[opt+0x6B]=1` в `0x474FD9` | Включает консоль. |
| `-dev` | `0x14DB4D4` | `[opt+0x65]=1` в `0x474FA0` | Dev-режим. |
| `-builtin_mods_only` | `0x14DB498` | `[opt+0x6C]=1` в `0x47504B` | Только встроенные моды. |
| `-open <path>` | `0x14DB48C` | ветка в `0x475073` | Открыть мир/файл из CLI. |
| `-connect_steam_id` | `0x14CB8E0` | клиентский bootstrap; лог `Contraption::start` | Прямое подключение к SteamID хоста. |
| `-max_threads` | `0x14DB6B0` | ветка в `0x475BC8` | Ограничение потоков. |
| `-no_profiler` | `0x14DB6A0` | ветка в `0x475D2B` | Отключить профайлер. |
| `-no_popcnt` | `0x14DB690` | ветка в `0x475D5A` | Отключить popcnt-путь. |
| `-window` | `0x14DB4E0` | — | Оконный режим. |
| `-select_custom_gpu` | `0x14DB4E8` | — | Выбор GPU. |
| `-enable_flip_discard` | `0x14DB500` | — | D3D-опция. |
| `-disable_debug_device` | `0x14DB518` | — | D3D debug device off. |
| `-disable_mem_back_trace` | `0x14DB530` | — | Отключить backtrace аллокаций. |
| `-fps` | `0x14DB548` | — | FPS-режим. |
| `--ugc` | `0x14DB550` | — | UGC-режим. |
| `-last_save` | `0x14DB558` | — | Открыть последнее сохранение. |
| `-tileeditor` | `0x14DB568` | — | TileEditor. |

Точки сборки опций:

- парсер: функция RVA `0x474BC0`, аргумент `r13` — структура опций;
- вызывающий wrapper: RVA `0x475EC0`; опции лежат по `rbp-0x10`;
- вызов парсера: `0x476280`;
- создаётся объект игры размером `0x3B0` (конструктор RVA `0x2E2900`),
  указатель хранится в `[0x1A690D8]`; опции копируются в
  `game_obj+0x2E8` (init RVA `0x2E3200`);
- `[opt+0x66]` (dedicated) читается в `0x2E335D` и пишется в глобал
  `0x1A69193` в `0x2E3361`; затем глобал читается в `0x2E3656`,
  `0x2E4049` и других местах.

Сканирование RIP-relative дало 15 участков чтения/записи глобала
`0x1A69193`; проверенные начала инструкций: `0x2E3356` (read),
`0x2E3361` (write), `0x2E3656`, `0x2E4049`. Остальные — в функциях
`wrap_Gui`, `wrap_Quat`, `wrap_Projectile`, `wrap_Shape`, network/world
init. В IDA достаточно открыть xref на `0x141A69193`.

Steam GameServer:

- IAT `SteamInternal_FindOrCreateGameServerInterface` = RVA `0x13BC950`;
- helper RVA `0x2F1290` сначала пробует `SteamNetworkingUtils004` через
  user interface, затем через game server interface. Это подтверждает,
  что Steamworks-часть умеет работать с GameServer, но не делает
  `-dedicated_server` рабочим.

Вывод: флаги — внутренние dev/QA-заготовки. Они полезны как карта того,
какие системы у разработчиков уже разделены на server/client, но
использовать их как готовый сервер нельзя.

### 10.3. Физика: что именно придётся переносить

Игра не просто использует Bullet; поверх него построен собственный
физический слой. Если делать нативный сервер, «подключить Bullet» — это
только начало.

Подтверждённые классы Bullet (RTTI в exe):

```
btDiscreteDynamicsWorld, btDynamicsWorld, btCollisionWorld,
btCollisionObject, btRigidBody, btCollisionShape, btCompoundShape,
btBoxShape, btSphereShape, btCapsuleShape, btCapsuleShapeZ,
btCylinderShapeZ, btConvexHullShape, btConvexShape, btTriangleShape,
btHeightfieldTerrainShape, btTriangleIndexVertexArray,
btStridingMeshInterface, btOptimizedBvh, btQuantizedBvh,
btDbvtBroadphase2, btCollisionDispatcher, btCollisionConfiguration,
btDefaultCollisionConfiguration, btGhostPairCallback,
btPersistentManifold, btManifoldResult, btGjkPairDetector,
btGjkEpaPenetrationDepthSolver, btConstraintSolver, btMLCPSolverInterface,
btDantzigSolver, btConeTwistConstraint, btActionInterface
```

Кастомный слой игры (часть RTTI-имён):

```
BulletLBVH, CustomCollisionConfiguration, btSlantShape,
VoxelTerrainPhysicsProxy, SimplePhysicsTerrain,
TerrainSurfacePhysicsProxy, TerrainAssetPhysicsProxy,
BodyPhysicsProxy, JointPhysicsProxy, BodyJointPhysicsProxy,
LiftPhysicsProxy, LimiterPhysicsProxy, TunnelCatcherPhysicsProxy,
VisionPhysicsProxy, HarvestablePhysicsProxy, CharacterPhysicsProxy,
RagdollRigidBody, RagdollPhysicsProxy, DebrisPhysicsProxy,
CollisionEffects, CollisionEffectsSustained, PhysicsMaterialManager,
Physics, PhysicsBase, PhysicsProxy, AssetInstancePhysicsProxy
```

Что это значит:

- `btDbvtBroadphase2`, `BulletLBVH`, `btSlantShape` — не стандартный
  upstream-набор; возможен форк/патчи. Точную версию и конфигурацию
  Bullet нужно определить отдельно.
- `VoxelTerrainPhysicsProxy`, `SimplePhysicsTerrain` — террейн с
  вокселями/heightfield и стримингом тайлов; это отдельный крупный
  модуль.
- `JointPhysicsProxy`, `BodyJointPhysicsProxy`, `LiftPhysicsProxy`,
  `LimiterPhysicsProxy` — шарниры, поршни, подъёмники, ограничители;
  поведение деталей Scrap Mechanic в значительной степени здесь, а не
  только в Lua.
- `CharacterPhysicsProxy`, `RagdollRigidBody` — персонажи, тряпичная
  кукла, character controller.
- `HarvestablePhysicsProxy`, `DebrisPhysicsProxy`,
  `TunnelCatcherPhysicsProxy`, `VisionPhysicsProxy` — ресурсы, обломки,
  защита от проваливания, видимость.
- `PhysicsMaterialManager` — материалы/трение/упругость.

Следствие: нативный сервер должен воспроизвести не только вызовы Bullet,
но и всю эту обвязку, порядок создания тел, фильтры коллизий, параметры
fixed step, solver, sleeping, CCD и материалов. Это основной объём работ
по физике.

### 10.4. Данные, сохранения и размер серверного пакета

#### 10.4.1. Размеры установки

Замеры `du` по установленной игре:

| Путь | Размер |
|---|---|
| Вся установка | 20 GB |
| `Release` | 563 MB (из них ~437 MB — файлы IDA DB; игровые бинарники ~126 MB) |
| `Data` | 2.8 GB |
| `Survival` | 11 GB |
| `Cache` | 5.0 GB |
| Lua (`Data` + `Survival`) | 5.7 MB (600 файлов) |
| JSON | 36.1 MB (2560 файлов) |
| OBJ (коллизии) | 14.5 MB (821 файл) |
| `.tile` | 115 MB |
| `.tileson` | 105 MB |
| `.mesh` | 13 MB |
| `.export` | 95 MB |
| `.shapeset` / `.physicsmaterials` | ~1–2 MB |
| TGA/FBX/DAE/PNG | несколько ГБ (графика, серверу не нужна) |

Реалистичная оценка серверного пакета: **0.4–0.7 GB** после вырезания
текстур, аудио, FBX/DAE/PNG и лишнего кэша. Итоговый размер зависит от
того, какие `.mesh`/`.rend`/`.export` требует headless-запуск и какие
физические cache-бандлы нужны. Цель <1 GB достижима, но требует
эмпирической проверки импортером.

Кэш: в логе игры видны `$CACHE_DATA/Bundle/game_shape_physics.cbo` и
`$CACHE_DATA/Bundle/survival_shape_physics.cbo`; для сервера нужны
физические бандлы, а не весь кэш.

Юридически: ассеты и скрипты игры нельзя перераспространять. Серверный
дистрибутив — это наш код + импортер, который собирает data pack из
лицензионной установки владельца. Полностью автономный сервер без данных
игры — отдельный, гораздо больший проект.

#### 10.4.2. Формат сохранений — SQLite

Сохранения мира (`Save/*.db`) — обычный SQLite (`SQLite format 3`).
Это сильно упрощает нативный сервер: мир можно читать/писать стандартным
SQLite, а не восстанавливать закрытый контейнер.

Проверенные таблицы:

```
Game(savegameversion, flags, seed, gametick, mods BLOB, uniqueIds BLOB
     [, gameversion, buildnumber])
RigidBody(id, worldId, data BLOB)
ChildShape(id, bodyId, data BLOB)
Joint(id, childShapeIdA, childShapeIdB, data BLOB)
ShapeGroup(id, csId, data BLOB)
Controller(id, hostId, hostType, data BLOB)
ScriptData(uid, key, worldId, flags, data BLOB)
ScriptDataUuid(...)
Container(id, data BLOB)
Unit(id, worldId, x, y, data BLOB)
Harvestable(id, worldId, x, y, size, data BLOB)
Tool(id, data BLOB)
VoxelTerrain(id, worldId, x, y, data BLOB)
Portal, PathNode, ScriptableObject, GenericData, ...
```

Пример реального Survival-сейва (66 MB):

| Таблица | Строк |
|---|---:|
| `ChildShape` | 640 319 |
| `Harvestable` | 100 417 |
| `ScriptData` | 31 463 |
| `Controller` | 11 149 |
| `RigidBody` | 9 256 |
| `Unit` | 7 653 |
| `Container` | 1 280 |
| `Joint` | 491 |
| `ShapeGroup` | 113 |
| `Tool` | 38 |
| `VoxelTerrain` | 0 |

Версии сейвов: 28, 29, 30; в новых есть `gameversion`/`buildnumber`
(например, `1.0.7`, build 894). BLOB-форматы (`data` у RigidBody,
ChildShape, Joint и др.) ещё нужно разобрать; схема SQLite даёт каркас,
но не заменяет реверс содержимого BLOB.

### 10.5. Решение: нативный кроссплатформенный сервер

Требования, зафиксированные по итогам обсуждения:

- Windows и Linux — нативные бинарники;
- дистрибутив сервера желательно <1 GB;
- полная физика и коллизии, поведение как у обычного хоста мира;
- без локального игрока;
- клиенты — обычная игра (Steam), желательно без мода.

Стек:

| Слой | Выбор | Примечание |
|---|---|---|
| Язык | C++20 | MSVC 2022 и GCC/Clang |
| Сборка | CMake + Ninja | CI для Windows/Linux |
| Физика | Bullet (upstream + порт кастомного слоя) | точную версию/конфиг определить в Phase 0 |
| Lua | Lua 5.1 | игра использует `lua51.dll` |
| Мир/сейвы | SQLite3 | формат подтверждён |
| Сеть | Steamworks SDK, `SteamNetworkingSockets012` | reliable/unreliable + игровой протокол |
| Steam identity | GameServer или обычный Steam-аккаунт | проверить anonymous login AppID 387990 |
| Админка | консоль + логи + метрики | кроссплатформенно |
| Пакетирование | Windows zip, Linux tar/AppImage, Docker | без рендера/аудио |

Структура проекта (целевая):

```
scrapds/
  CMakeLists.txt
  src/
    core/        лог, конфиг, консоль, tick, потоки
    data/        SQLite-мир, импортер ассетов, форматы игры
    physics/     Bullet-мир, террейн, тела, шарниры, character controller
    game/        мир, создания, детали, контейнеры, инструменты, юниты
    lua/         Lua 5.1 VM, sm.* API, серверные скрипты
    net/         протокол, NetObj, reliable/unreliable, initial sync
    steam/       SNS 012, GameServer/identity
    admin/       команды, статус, метрики
  tools/
    probe/       DLL-хуки для снятия пакетов/физики с оригинала
    importer/    сборка server data pack из установленной игры
    parity/      golden-сценарии и сравнение с оригиналом
  tests/
```

Поток данных:

```
лицензионная установка игры
        │ importer (только серверные данные)
        ▼
server data pack (<1 GB)
        │
        ▼
SQLite world .db ──► data/ (сущности, BLOB-парсеры)
        │
        ├──► physics/ (Bullet: террейн, тела, шарниры, персонажи)
        ├──► lua/ (серверные скрипты, sm.* API)
        └──► game/ (мир, создания, инструменты, юниты)
                    │
                    ▼
                 net/ (игровой протокол) ──► Steam SNS 012 ──► клиенты
```

Ключевые проектные решения:

1. **Сервер авторитетен.** В текущем хосте клиентские Character-апдейты
   применяются почти без проверок (§2.6). Нативный сервер должен
   валидировать скорость, телепорты, коллизии и владельца персонажа.
2. **Совместимость с ванильными клиентами.** Нужен точный handshake:
   версия игры, чек-суммы, UGC/моды, initial-пакеты, NetObj-схемы.
   `-connect_steam_id` даёт клиенту прямой вход, но сервер всё равно
   должен выглядеть для клиента как обычный хост.
3. **Данные не распространяем.** Импортер требует установленной игры;
   версия data pack привязана к SHA-256/билду.
4. **Без рендера.** Никаких D3D/окна/аудио: только серверный цикл,
   физика, Lua, сеть и сохранения.

### 10.6. План этапов нативного сервера

Оценки — ориентиры, не обещания; зависят от размера команды и от того,
насколько быстро удастся снять протокол.

**Phase 0. Feasibility и оракул (2–4 недели).**

- Определить версию/конфигурацию Bullet и параметры `stepSimulation`
  (dt, substeps, solver, broadphase, sleeping, CCD) хуком в оригинальном
  хосте.
- Снять игровой протокол на уровне игры (не зашифрованный SNS):
  handshake, join, initial-пакеты, NetObj type 3/4, creation sync.
  Инструмент — DLL-хук в процессе игры на Windows; клиент и хост.
- Разобрать SQLite-сейв: BLOB-форматы RigidBody/ChildShape/Joint/
  Controller/ScriptData; собрать дамп мира Creative.
- Собрать data pack импортером и замерить размер; проверить, что
  headless-цикл не требует графических ассетов.
- Скелет бинарника Windows/Linux: загрузить Creative-мир, построить
  Bullet-мир (террейн + тела), прокрутить N тиков, сравнить позы тел с
  записью из оригинала.
- Проверить Steam identity: GameServer anonymous login для AppID 387990
  или обычный аккаунт; проверить join клиента через `-connect_steam_id`.

**Phase 1. Headless-мир (2–3 месяца).**

- SQLite reader/writer мира, стабильный save/load.
- Террейн: `SimplePhysicsTerrain`/`VoxelTerrainPhysicsProxy`, тайлы,
  heightfield/voxel collision.
- Тела, составные shapes, материалы, фильтры коллизий.
- Персонаж: capsule/character controller, движение, режимы.
- Lua 5.1, серверный `sm.*` API, загрузка серверных скриптов.
- Консоль, конфиг, логи, graceful shutdown.
- Parity-тесты: падение тел, стопки, трение, террейн.

**Phase 2. Сеть и вход клиента (3–4 месяца).**

- Handshake/версия/чек-суммы/UGC.
- Initial-пакеты, world sync, NetObj type 3/4.
- Character sync, спавн, вход/выход.
- Один ванильный клиент подключается, видит мир и двигается.
- Два клиента видят друг друга.

**Phase 3. Полный Creative (3–4 месяца).**

- Создания/детали, шарниры, поршни, контроллеры, подъёмники,
  ограничители.
- Инструменты, контейнеры, инвентарь.
- Стрельба/снаряды, эффекты коллизий.
- Дифференциальные тесты на пользовательских мирах/блюпринтах.

**Phase 4. Survival (6+ месяцев).**

- Тайловый террейн/стриминг, harvestables, юниты/NPC, рейды.
- Крафт, прогрессия, серверные game mode скрипты.
- Полный Lua API, который используют `Survival/Scripts`.

**Phase 5. Упаковка и эксплуатация (1–2 месяца).**

- Windows/Linux сборки, Docker, data pack importer.
- Админка: status, players, kick/ban, save, stop.
- Метрики, краш-логи, обновление версий.
- Валидация клиентских апдейтов, античит-минимум.
- Совместимость модов/чек-сумм.

Итог: **нативный сервер с полным Creative — реалистично 8–12 месяцев
для небольшой команды; с Survival — 12–18+ месяцев.** Это не недельный
проект; основной риск — протокол и кастомный физический слой.

### 10.7. Как проверять паритет с оригиналом

- Оригинальный хост — оракул: DLL-хук снимает `stepSimulation`,
  позиции/скорости тел, порядок создания объектов, сетевые пакеты.
- Golden-сценарии: падающий ящик, стопка, наклонная плоскость, трение,
  шарнир/подшипник, поршень, подвеска, столкновение с террейном,
  character controller, ragdoll.
- Дифференциальные тесты: один и тот же Creative-мир/блюпринт
  прогоняется в оригинале и в нативном сервере; сравниваются позы,
  скорости, события контактов, порядок тиков.
- Протокол: запись реального join/игрового сеанса, затем
  conformance-тесты нашем кодека против записи.
- Save round-trip: прочитать SQLite-мир, записать обратно, проверить,
  что оригинальная игра открывает результат.
- Кроссплатформенность: один и тот же сценарий на Windows и Linux;
  побитовая детерминированность не обязательна, но расхождения не
  должны ломать клиентскую симуляцию.

### 10.8. Риски и открытые вопросы

1. **Протокол.** Формат сообщений, сжатие, чек-суммы, NetObj-схемы,
   порядок initial-пакетов. Пакетного capture ещё нет; это Phase 0.
2. **Steam identity.** Разрешит ли Valve anonymous GameServer login для
   AppID 387990; как клиент узнаёт SteamID сервера; нужен ли обычный
   Steam-аккаунт на каждый сервер.
3. **Физический паритет.** Кастомный Bullet-слой (`BulletLBVH`,
   `VoxelTerrainPhysicsProxy`, `JointPhysicsProxy` и др.) нужно
   воспроизвести точно; ошибки в solver/материалах ломают создания.
4. **Lua API.** Серверные скрипты используют большой `sm.*` API;
   его нужно реализовать по мере покрытия game mode. Клиентский Lua
   серверу не нужен.
5. **Данные и лицензия.** Ассеты нельзя перераспространять; импортер
   должен работать из установленной игры; размер и состав data pack
   проверить.
6. **Версии игры.** Офсеты, форматы BLOB, протокол и флаги привязаны к
   билду `cf403cf9…`; после обновления всё перепроверять.
7. **Производительность.** 640k ChildShape, 100k Harvestable, 9k
   RigidBody в одном Survival-сейве; headless-сервер должен держать
   fixed tick и память без рендера.
8. **Безопасность.** Текущая модель доверяет клиентским Character-
   апдейтам; для публичного сервера нужна валидация и защита от
   телепортов/полётов.
9. **Моды.** Handshake и чек-суммы; `-builtin_mods_only` показывает,
   что режимы модов у разработчиков есть, но публичный сервер должен
   выбрать политику.
10. **Скрытый режим.** `-dedicated_server` не работает; не тратить время
    на его реанимацию, но сохранить как карту server/client-разделения.

### 10.9. Рабочее окружение и инструменты

✅ Игра: `D:\Steam\steamapps\common\Scrap Mechanic`, AppID 387990,
Windows-only; текущий билд совпадает с документом по SHA-256.

✅ IDA Professional 9.4: `C:\Program Files\IDA Professional 9.4`;
рядом с exe лежит готовая база `ScrapMechanic.exe.id0/.id1/.id2/.nam`.
Пока IDA держит базу открытой, файлы `.nam`/БД залочены; для скриптового
анализа использовать открытую сессию IDA или копию базы.

✅ Компилятор: Visual Studio Build Tools 2022 с C++
(`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`);
`cl` доступен через `vcvars64.bat`. В PATH нет `cmake`, `ninja`,
`clang`, `gcc` — поставить отдельно.

✅ Python 3.12; `capstone` 5.0.7 и `pefile` 2024.8.26 доступны.

✅ SQLite: сейвы читаются стандартным `sqlite3` (Python/CLI).

⚠️ Текущая тестовая сессия: клиент запускался с
`-connect_steam_id <STEAMID64>`; в логе
`Contraption::start -connect_steam_id ...`. Хост/сервер должен быть
запущен отдельно; успешный join ещё нужно зафиксировать.

### 10.10. Ближайшие действия

1. Поставить CMake/Ninja и зафиксировать каркас `scrapds` с CI для
   Windows (MSVC) и Linux (GCC/Clang).
2. Написать probe-DLL для оригинального процесса: хуки на
   `NetworkSendInterface`/SNS send-recv, `stepSimulation`, загрузку мира;
   снять handshake/join/NetObj и параметры физики.
3. Реализовать SQLite-читатель Creative-сейва и дамп сущностей
   (RigidBody/ChildShape/Joint/ShapeGroup/Controller/ScriptData).
4. Собрать headless-скелет: загрузка мира, Bullet-террейн + тела,
   N тиков, вывод поз; сравнить с записью оригинала.
5. Сделать importer data pack и замерить фактический размер (<1 GB).
6. Проверить Steam identity и join: GameServer anonymous vs обычный
   аккаунт; `-connect_steam_id`.
7. По результатам Phase 0 уточнить оценки и обновить §10.

Главный вывод ревизии: **нативный путь — единственный, который
одновременно даёт Linux, размер <1 GB и полный контроль над физикой;
скрытый `-dedicated_server` для этого не годится.**

---

## 11. Реализация и повторная проверка 2026-11-02

Проект создан в `scrapds/`. Текущий статус — **рабочие инструменты Phase 0,
полноценный dedicated server ещё не готов**. Актуальная сводка:
`scrapds/research/status.md`, команды — `scrapds/README.md`.

### 11.1. Что уже работает

- C++20/CMake-проект собирается MSVC на Windows. Подготовлен CI для Windows
  и Linux; фактический Linux-прогон пока не выполнен.
- `scrapds inspect` читает SQLite-миры без записи. Проверены все восемь
  доступных сохранений, 11 639 RigidBody и 650 059 ChildShape.
- Частично восстановлены big-endian BLOB-поля: ID, worldId, границы тел,
  ссылки деталей на тела. Положение и quaternion ещё требуют независимой
  проверки. Есть ограниченные декодеры и тесты усечённых/некорректных данных.
- Снимки Creative и Survival сделаны через SQLite backup API с учётом WAL;
  проверены целостность, схема и количества строк. SHA-256 и время изменения
  исходных файлов сохранились.
- Импортер собрал локальный пакет: **11 653 файла, 455 342 738 байт** с
  манифестом. Файлы проверены по SHA-256. Полнота зависимостей не доказана;
  `.mesh/.export/.rend` пока включены как неопределённые зависимости.
- `scrapds simulate` выполняет отдельные тестовые сценарии Bullet с Lua-
  описанием и CSV-трассой. Это ещё не симуляция игрового сохранения.
- Анонимный GameServer login для AppID 387990 **проверен и работает** как
  из Python-прототипа, так и из нативного C++ executable. Получены отдельные
  SteamID типа AnonymousGameServer, BLoggedOn=true и callback 101.
- Есть SNS012 listener с allowlist одного тестового клиента и записью
  plaintext-сообщений. С 2026-11-02 он реализует pre-game control
  challenge/authentication/acceptance (03/04/05); initial world sync ещё нет.
- Собрана probe-DLL с проверкой SHA/прологов и пассивными хуками сетевого
  кодека и `stepSimulation`; Start/Stop проверены в оригинальном процессе.
- Подтверждён **raw LZ4 после первого байта сообщения**. Синтетический тест
  оригинального кодека: 4097 исходных байт → 2074 сжатых, native round trip
  успешен. Независимый Python LZ4 и реализованный C++ кодек дают побайтово
  тот же результат. Это проверка кодека, а не захват handshake.

### 11.2. Исправления к прежним предположениям

1. `lua51.dll` содержит **LuaJIT 2.1.1772619647**. Lua 5.1 — семейство ABI,
   точный runtime иной. В прототипе сценариев используется явно обозначенная
   reference Lua 5.1.5; игрового `sm.*` API пока нет.
2. Наличие `-connect_steam_id` в строке `Contraption::start` **не доказывает,
   что игра начала подключение**. Парсер RVA `0x2F1C90` получает строку из
   `ISteamApps::GetLaunchCommandLine`, а не сырые параметры Windows.
   Проверять нужно `Contraption::SteamCommandLine`.
3. Для следующей проверки нужен холодный запуск через Steam URI вида
   `steam://run/387990//-connect_steam_id%20STEAMID64/`. Обычный
   `steam.exe -applaunch ... -connect_steam_id ...` в проведённом тесте
   оставлял Steam-строку пустой; клиент оставался в меню.
4. Для проверки отдельного сервера второй ПК не обязателен: независимый
   анонимный GameServer ID получен на том же ПК, где работает Steam-клиент.
   Реальный vanilla-клиент подключается и проходит control 03/04/05;
   загрузка мира (initial sync) пока не подтверждена.
5. Тестовый exe с прежним SHA сообщает в runtime `1.0.6.889`; сведения
   `1.0.7/build894` относятся к метаданным одного сохранения и не должны
   подменять версию исполняемого файла.

Следующие обязательные рубежи: initial sync после клиентского `01`,
восстановление серверных initial-пакетов, геометрия коллизий и полный
разбор BLOB, паритет физики, игровой Lua API. Полный сервер не объявляется
готовым по наличию сборки, SteamID или отдельного кодека.

---

## 12. Runtime control handshake — подтверждено 2026-11-02

### 12.1. Что подтверждено

✅ В нативный listener добавлен режим `--handshake`. Реальный ванильный
клиент дважды прошёл pre-game последовательность:

| Направление | Байты | Смысл |
|---|---|---|
| сервер → клиент | `03` | challenge, reliable |
| клиент → сервер | `04 00 00 00 00 00 00 00 00 00` | auth: id 4, длина пароля 0, secret 0, reliable |
| сервер → клиент | `05` | acceptance, reliable |
| клиент → сервер | `01` | первый game-пакет после acceptance, reliable |

✅ Свидетельства: `scrapds/artifacts/steam/live-handshake-02`
(GameServer `90296327154011157`) и `live-handshake-03`
(GameServer `90296327481055251`). Auth-пакет 10 байт; пост-пакет `01`
(SHA-256 `4bf5122f344554c53bde2ebb8cd2b7e3d1600ad631c385a5d7cce23c7785459a`).

✅ Запуск клиента подтверждён через
`steam://run/387990//-connect_steam_id%20<GameServerID>/`: Steam заполняет
`GetLaunchCommandLine`, в логе появляется
`Contraption::SteamCommandLine -connect_steam_id ...`, клиент доходит до
`Connected`. Передача `-connect_steam_id` только в OS-аргументах этот буфер
не заполняет.

✅ В C++ добавлены `control.cpp` и тест `control`; CTest проходит 11/11.
Клиент после `01` ждёт initial-синхронизацию; серверный ответ ещё не
восстановлен.

### 12.2. Host-oracle и ограничение захвата

✅ `-dedicated_server -open <world.db>` запускает ListenServer и локального
клиента. Совместимый сейв — версия 28/29; версия 30 отклоняется как
«newer than game version» (игра 1.0.6.889).

⚠️ Пассивный probe на локальном loopback-соединении записал только
повторяющиеся мелкие пакеты (message IDs 30 и 50, максимум 23 байта) и не
увидел initial world data: хуки codec-обёрток не покрывают локальный
initial-sync путь. Захват:
`scrapds/artifacts/probe-host-initial-3.cap` / `.json` (2597 записей).
Это отрицательное свидетельство, а не формат initial sync.

### 12.3. GameInfo принят; следующий шлюз — initial sync

✅ Ошибка `VersionMismatchException` в `real-gameinfo-02` была вызвана
порядком байт: многобайтовые поля GameInfo идут в network byte order, а
первый минимальный пакет был записан little-endian. После перевода
сериализатора в big-endian реальный клиент принял message id 2
(`real-gameinfo-03`, GameServer `90296328266458133`,
лог `game-20261102-010413.log`).

✅ Клиент залогировал `Version: 889`, `Game mode: AlphaTerrain`,
`Seed: 0`, `Game tick: 0`, `Script key count: 0`, `Generic key count: 0`,
`Mod developer mode 0`, затем `Connected`. Исключения и разрыва нет.
Пакет: 32 байта plaintext, 16 байт wire:
`02 00000379 00000000 00000000 00000000 00000000 0000 00000000 00000000 00`.

✅ Сразу после GameInfo клиент отправил reliable-пакет id 6 размером
41 255 байт (`000002-rx.bin`, SHA-256
`0dd4a4c13a7a5682f089e02b4b360e2d81b889d44c5059531ce33834fdfcfeaa`).
LZ4-тело распаковывается в 50 764 байта. Серверный ответ должен быть
initial world sync: парсер клиента в режиме инициализации принимает
id `11`, `13`, `25`, `27`.

✅ Loopback-захват оригинального host-сервера
(`scrapds/artifacts/host-self-initial-02.cap`, 718 записей) даёт реальный
GameInfo (game mode 14, seed 834223556, game tick 7408790) и
последовательность self-join: клиент `01`, сервер `02`, клиент `09`
(265 байт, UTF-8 имя игрока), клиент `0A`, затем периодические
8-байтные `1E`. Initial world data по loopback не идёт через
захваченные codec-обёртки.

### 12.4. Следующий шлюз

Нужен серверный ответ на клиентский id 6. Практические варианты:

1. Удалённый клиент к host-серверу, чтобы initial sync пошёл через SNS и
   попал в хуки; тогда снять последовательность серверных пакетов.
2. Дополнительные хуки на split sender (`0xB5A480`) и raw compressor
   (`0x4EE990`), которые, вероятно, обслуживают initial world data.
3. Статический разбор `ClientCommunication`/`NetworkServer`: определить
   обработчик id 6 и порядок initial-пакетов, затем реализовать их в
   нативном listener.

### 12.5. Join-state probe: `Join request accepted`

✅ Добавлен экспериментальный режим `--game-ready`. После клиентского
id 6 listener отправляет однобайтовый id 7; после клиентского
player-info id 9 (265 байт plaintext, 209 байт wire) отправляет id 10.

✅ Реальный клиент принял последовательность: в
`artifacts/steam/join-probe-01` (GameServer `90296329214978068`) он
залогировал `Receiving initialization data...`,
`Received 0 chunks of script initialization data`,
`Received 0 chunks of generic initialization data`, `Joining game...`.
В `join-probe-02` (GameServer `90296329733594132`, лог
`game-20261102-011715.log`) после id 10 появилось `Join request accepted`.

⚠️ Клиент остаётся подключённым, но без world/NetObj данных выдаёт
`DynamicDataManager.cpp:1593 Failed to load Cached Const Data for '' invalid`
и ошибку JSON. Id 7/10 двигают только state machine; для gameplay нужны
initial world data (id 11/13/25/27) и NetObj/player spawn.

Текущий listener — не полноценный dedicated server: он проводит клиента
через control 03/04/05, GameInfo и state-переходы 7/10 до
`Join request accepted`, но не отправляет мир и не вводит клиента в
gameplay.

---

## 13. Native replay: клиент вошёл в мир (2026-11-02)

✅ Второй Steam-аккаунт (`<STEAMID64>`) в Sandboxie подключился к
нативному listener через
`steam://rungameid/387990//-connect_steam_id%20<GameServerID>/`. Именно
`rungameid` заполняет Steam `GetLaunchCommandLine`; передача URI напрямую в
`steam.exe` оставляла буфер пустым.

✅ С реального host снят initial sync: GameInfo 120 байт, script init
`id 11` 1014 байт, generic init `id 13` 452 байта, world `id 29` 86 905
байт, плюс post-join `id 13`/`id 29`/`id 24`. Файлы:
`scrapds/artifacts/world-sync/replay/`.

✅ `scrapds listen-probe ... --replay artifacts/world-sync/replay` отправляет
GameInfo + init 11/13 + world 29, затем `id 7` после клиентского `id 6` и
`id 10` + post-join пакеты после `id 9`. Клиент залогировал
`Received 1 chunks of script initialization data`,
`Received 3 chunks of generic initialization data`, `Creating world 0`,
`Loaded player 1/3`, `Join request accepted` и **`Start state: Game`**
(`game-20261102-014657.log`). Скриншот:
`scrapds/artifacts/native-ingame.png` — FlatTerrain-мир, имя игрока и хотбар.

⚠️ Это **статический replay** захваченного мира, а не нативная симуляция.
Listener принимает одного peer; нет физики/gameplay-симуляции, сохранения
изменений построек и relay между несколькими клиентами. Один клиент уже
заходит и играет в мир; для «как с другом» на нативном сервере следующий
шаг — multi-peer + relay character/creation-апдейтов, затем замена replay
на реальную сериализацию мира.

---

## 14. Итог ночной сессии: нативный foundation, но не 100% сервер

✅ Сделано: control handshake 03/04/05, реальный GameInfo, initial sync
(id 11/13/29), вход клиента в мир (`Start state: Game`), серверный tick
30 Hz (id `0x1E`), второй разрешённый peer и relay игровых пакетов между
клиентами.

❌ Не сделано и является причиной жалоб на геймплей: нативная физика и
коллизии, серверный Lua/game mode, строительство и создание деталей,
инвентарь/контейнеры, посадка в кресло, отображение инструмента от
третьего лица, сохранение изменений мира. Сейчас мир — статический replay
снятого host, а не симуляция. Поэтому клиент ждёт серверных ответов, а
действия, требующие серверной логики, не работают.

Реальный host игры и null-driver host проверены как oracle, но **не**
являются продуктом: по требованию проекта сервер не должен быть
перезапущенной игрой.

Для 100% нативного сервера нужен перенос физического слоя Bullet,
сериализации мира/NetObj, серверного Lua и gameplay-систем, а также
Linux-транспорта Steam. Это проект на месяцы, а не на одну ночь.

---

## 15. Два клиента на нативном сервере (2026-11-02)

✅ Добавлен `tools/probe/launch_connect.py`: игра запускается suspended,
в неё инжектится probe, ставится hook парсера `0x2F1C90`, принудительно
задаётся `-connect_steam_id`, затем процесс возобновляется. Это обходит
Steam-подтверждение и пустой `SteamCommandLine`.

✅ Долгоживущий listener `serve-03` (GameServer `90296332603514901`) принял
оба Steam-аккаунта. Оба клиента залогировали `Loaded player 1/3`,
`Join request accepted` и **`Start state: Game`**. Listener:
`game_started_count=2`, tick 30 Гц, relay игровых пакетов между клиентами.

✅ На момент записи (2026-11-02) listener проработал **18 015 секунд
(~5 часов)**: `status=listening`, 1 800 358 сообщений, 39 553 593 байта
трафика, `challenge_count=2`, `authenticated_count=2`,
`game_info_sent_count=2`, `join_accepted_count=2`, `game_started_count=2`.
Игровые клиенты к этому моменту уже были закрыты, сервер продолжил работу.
Метаданные: `scrapds/artifacts/steam/serve-03/listener.json`.

⚠️ Это по-прежнему **статический replay + relay**, а не нативная
симуляция. Строительство, инвентарь, контейнеры, посадка в кресло и
серверная логика инструментов не реализованы. Два клиента находятся в одном
мире и обмениваются character-апдейтами, но мир не авторитетен и изменения
не сохраняются.

⚠️ Дополнение 2026-11-02: этот listener позже перестал писать отчёт после
~10,3 часов работы: `listener.json` вырос до 398 МБ, последняя запись
оборвалась с `Cannot finish probe output`, процесс завершился. Причина —
полная перезапись всего отчёта каждую секунду без ограничения массивов.
Исправлено в §17.6. Replay-ветка ниже историческая; актуальная ветка
использует нативный мир из SQLite (§17).

Реальный host игры и null-driver host в продукте не используются.

---

## 16. Сохранённый прогресс проекта (2026-11-02)

Это сводка того, что реально лежит в репозитории и что проверено, чтобы
продолжать работу без повторного исследования.

### 16.1 Что реализовано в коде

`scrapds/` — CMake-проект C++20. Сборка и тесты проверены на Windows/MSVC.

- **`src/net/codec.*`** — сжатие/распаковка игровых пакетов (LZ4 block,
  байт 0 сообщения сохраняется отдельно), совместимость с форматом игры
  проверена синтетическим тестом внутри процесса игры.
- **`src/net/control.*`** — pre-game control-протокол:
  `03` challenge → клиент `04` authentication → `05` acceptance.
  Подтверждено реальным vanilla-клиентом.
- **`src/steam/probe.cpp`** — нативный listener на
  `SteamNetworkingSockets012`:
  - анонимный Steam GameServer login;
  - приём соединений только от явно разрешённых SteamID;
  - GameInfo `id 2` в big-endian, build 889;
  - отправка initial sync: `id 11`, `id 13`, `id 29` из replay;
  - переходы `id 7` и `id 10` после клиентских `id 6` и `id 9`;
  - серверный tick 30 Гц (`id 0x1E`, tick +2 каждые 33 мс);
  - второй разрешённый peer и relay post-join игровых пакетов между
    клиентами;
  - JSON-отчёт `listener.json` и `ready.json`, остановка через
    `stop.request`.
- **`src/data/world.*`, `src/data/blob.*`** — чтение SQLite-сохранений
  (таблицы `Game`, `RigidBody`, `ChildShape`, `Joint`, `ShapeGroup`,
  `Controller`, `ScriptData`, `Container`, `Unit`, `Harvestable`, `Tool`,
  `VoxelTerrain`) без изменения исходных файлов, разбор BLOB-префиксов.
- **`src/physics/simulation.*`** — независимая Bullet-симуляция для
  bring-up (падающие коробки/стек), **не** физика игры.
- **`src/lua/runtime.*`** — референсная Lua 5.1/LuaJIT-среда, **не**
  серверный Lua игры и не `sm.*` API.
- **`tools/probe/probe.cpp`** — инжектируемый probe: passive Start/Stop,
  перехват codec-функций, split-sender hook, force-connect hook парсера
  `0x2F1C90` и экспорт `ScrapdsForceConnectId`.
- **`tools/probe/launch_connect.py`** — запуск игры suspended, инжект
  probe, принудительный `-connect_steam_id`, `ResumeThread`; обходит
  Steam-подтверждение и пустой `SteamCommandLine`.
- **`tools/probe/ida_*`, `read_capture.py`, `inspect_binary.py`** —
  экспорт декомпиляций и разбор захватов.
- **`tools/world_snapshot.py`, `tools/importer/import_data.py`** —
  снимки сохранений и импорт ассетов (455 342 738 байт, зависимости
  не подтверждены).
- **`src/main.cpp`** — команды `status`, `simulate`, `steam-identity`,
  `listen-probe`, `inspect-world`, `snapshot`, `import-assets`.
  `serve` запускает экспериментальный listener только с replay bootstrap и
  world save; `--seconds 0` работает до `<output>/stop.request` и делает
  финальный checkpoint. Полноценной world-independent native-синхронизации
  пока нет.

### 16.2 Проверенные контрольные точки

| Что | Результат | Где доказательство |
|---|---|---|
| Сборка и CTest | 11/11 тестов проходят | `artifacts/build-final.log` |
| Control handshake с реальным клиентом | `03/04/05` принят | `artifacts/steam/live-handshake-03` |
| GameInfo реальным клиентом | `Version: 889`, `Connected` | `artifacts/steam/real-gameinfo-03` |
| Initial sync и вход в мир | `Loaded player 1/3`, `Start state: Game` | `artifacts/steam/replay-03`, `artifacts/native-ingame.png` |
| Два клиента одновременно | `game_started_count=2`, relay | `artifacts/steam/serve-03` |
| Долгая работа listener | ~5 часов, 1,8 млн сообщений | `artifacts/steam/serve-03/listener.json` |
| Чтение сохранений | 11 639 RigidBody, 650 059 ChildShape | `artifacts/save-layout-evidence.json` |
| Снимки сохранений | integrity/schema OK, исходники не изменены | `artifacts/snapshots/verification.json` |

### 16.3 Что не реализовано и почему жалобы на геймплей справедливы

> Дополнение: часть пунктов ниже уже закрыта в §17 (нативный мир,
> build/paint, исправленный tick). Раздел оставлен как исходный срез.

Текущий listener отдаёт клиенту **записанный** мир и пересылает пакеты
клиентов друг другу. Серверной симуляции нет, поэтому:

- **Строительство не работает** — сервер не размещает creation, не
  валидирует его и не рассылает NetObj создания.
- **Input «тормозит, будто хост в США»** — нет авторитетной серверной
  симуляции и коррекции; клиентский prediction не получает серверных
  подтверждений, relay добавляет задержку.
- **Из инвентаря ничего не берётся** — нет серверной логики
  inventory/container и соответствующих сообщений.
- **Предмет в руке не виден от третьего лица** — нет репликации
  held-tool/character state.
- **В кресло не садится** — нет обработчика seat/interact на сервере.
- **Изменения не сохраняются** — мир replay только для чтения, записи в
  SQLite нет.

Это не сетевые баги клиента, а отсутствующие серверные системы. Пока их
нет, «100% мультиплеер» недостижим.

### 16.4 Следующий порядок работ

1. **Загрузка мира из SQLite в память** и построение сцены: `Game`,
   `RigidBody`, `ChildShape`, `Joint`, `ShapeGroup`, `VoxelTerrain`,
   `Harvestable`, `Unit`, `Container`.
2. **Импорт коллизий ассетов** (меши/формы деталей) и точная конфигурация
   Bullet-форка игры; серверный шаг физики 30 Гц.
3. **Сериализация мира/NetObj** вместо replay: сервер сам формирует
   initial sync `id 11/13/29` и последующие апдейты.
4. **Авторитет персонажей**: приём Character-апдейтов, серверная
   коррекция, рассылка всем peer'ам.
5. **Интеракции**: строительство/удаление, inventory/containers,
   seat, инструменты, двери/кнопки/контроллеры.
6. **Серверный LuaJIT и `sm.*` API** для game mode и модов.
7. **Persistence**: семантическая запись изменений в копию SQLite-сейва,
   никогда в исходный файл.
8. **Linux Steam transport** (сейчас `src/steam` только Windows) и
   кроссплатформенная сборка/CI.
9. **Продуктовая обвязка**: конфиг, администрирование, логи, упаковка
   дистрибутива без ассетов игры.

### 16.5 Как воспроизвести текущий прототип

```powershell
# сборка (из окружения Visual Studio; бинарник в scrapds/build/scrapds.exe)
cmake -S scrapds -B scrapds/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build scrapds/build

# listener на 24 часа с replay-миром
scrapds/build/scrapds.exe listen-probe `
  --game "D:/Steam/steamapps/common/Scrap Mechanic" `
  --output scrapds/artifacts/steam/serve-04 `
  --allow-peer <STEAMID64> `
  --allow-peer2 <STEAMID64> `
  --seconds 86400 --handshake --game-ready `
  --replay scrapds/artifacts/world-sync/replay

# клиент: suspended launch + probe + принудительный connect id
python scrapds/tools/probe/launch_connect.py `
  --exe "D:/Steam/steamapps/common/Scrap Mechanic/Release/ScrapMechanic.exe" `
  --dll scrapds/build/scrapds_probe.dll `
  --connect-id 90296332603514901 `
  --log scrapds/artifacts/steam/client-04.log `
  --output scrapds/artifacts/steam/client-04.json
```

Точные пути к игре и SteamID зависят от машины. Replay-файлы:
`scrapds/artifacts/world-sync/replay/` (`gameinfo.bin`, `init11.bin`,
`init13.bin`, `world29.bin`, `post/*.bin`). Остановка listener — создать
файл `stop.request` в каталоге `--output`; досрочно не убивать процесс,
иначе `listener.json` не допишется.

### 16.6 Ограничения исследования

- Анализ вёл по приватной копии бинарника; база владельца в IDA не
  трогалась.
- Ассеты игры не распространяются; импортёр работает с локальной
  установкой.
- Реальный host и null-driver host использовались только как oracle и
  **не входят** в продукт.
- Дистрибутив сервера без ассетов и физики игры пока не готов; текущий
  `scrapds` — foundation и инструменты, а не готовый хостинг.

---

## 17. Нативный мир, build/paint и исправленный tick (2026-11-02)

Этот раздел новее §16: часть пунктов «не реализовано» из §16.3 уже
закрыта в коде.

### 17.1 Нативный мир вместо replay

✅ Добавлена загрузка мира из SQLite в память и его сериализация обратно:

- `src/data/state.*` — `WorldState::load`, `advance_ticks`, `save_as`,
  `create_block`, `replace_shape_blobs`, доступ к `bodies()/shapes()`,
  `metadata()`, `table_counts()`, `summary_json()`.
- `src/data/assets.*` — `AssetDefinitions::load` из локальной установки
  игры: UUID, тип блока, цвет по умолчанию, физический материал.
- `src/data/shape_geometry.*` — разбор и сборка ChildShape BLOB
  (позиция в сетке, размеры, цвет, UUID, флаги).
- `src/physics/world_collision.*` — построение коллизий из `WorldState`.
- `src/net/world_sync.*` — разбор агрегата `id 29`, каталог ассетов
  `InitialAssetCatalog`, `rebuild_initial_world` и
  `make_child_shape_state`.
- `src/net/game_commands.*` — разбор клиентских команд
  `RemoveShape`, `PaintShape`, `PaintJoint`, `PaintCharacter`,
  `BuildShape` (включая реальный 110-байтный fixture).
- `src/game/commands.*` — серверное применение `BuildShape` (kind 2/4)
  и `PaintShape`, проверки мира, ориентации, пересечений и лимитов.

✅ У `listen-probe` появился флаг `--world <save.db>`. Listener:

1. загружает **копию** сохранения в `WorldState` (исходник открывается
   read-only);
2. поднимает каталог ассетов и пересобирает начальные NetObj-пакеты из
   этого мира, а не отдаёт записанный `world29`;
3. применяет клиентские build/paint-команды к `WorldState`;
4. при остановке пишет результат в `<output>/world.db`.

Проверка: `scrapds load-world` читает Creative v30 и Survival v29
(9 256 RigidBody, 640 319 ChildShape) без изменения исходных файлов.

### 17.2 Build/paint: что уже работает и чего ещё нет

✅ Сервер применяет:

- `BuildShapeCommand` kind 2 — создание нового тела и блока;
- `BuildShapeCommand` kind 4 — добавление блока в существующее тело;
- `PaintShapeCommand` — одиночный ChildShape, целый блок или полный
  регион блоков, с цветом клиента или цветом ассета по умолчанию;
- проверки: известный блочный ассет, допустимая ориентация, границы
  объёма, пересечение с существующим блоком, принадлежность мира;
- атомарность: при отказе мир не меняется;
- сохранение: новые тела/шейпы и счётчики ID переживают `save_as` и
  повторную загрузку.

✅ CTest `gameplay` покрывает реальную клиентскую build-команду,
создание тела/шейпа, paint региона, отказы, save/load и RTree-границы.
Все 17 тестов проходят.

⚠️ Репликация созданных тел/шейпов уже реализована в `probe.cpp`:
после применения команды формируется `id 22` с
`make_body_creation`, create-записью шейпа и `make_child_shape_state`,
затем пакет уходит всем `game_started` peer'ам. Живьём это ещё **не
подтверждено**: в сессии §17.4 клиент не строил (`applied_commands=0`),
а UI-тест строительства через computer use не удалось довести из-за
фокуса игрового окна.

❌ Пока не работает:

- PaintCharacter для NPC: target сейчас должен быть активным player Character;
  полная проверка owner/tool также отсутствует;
- инвентарь и контейнеры, посадка в кресло, held-tool/третье лицо;
- серверная физика и авторитет персонажей;
- серверный Lua/game mode и persistence игрового прогресса.

### 17.3 Исправленный серверный tick (главная причина «лага» и спама ошибок)

✅ По захвату реального хоста установлен точный формат:

```
plaintext (8 байт): 1E <tick LE32> 00 00 00
wire (9 байт):      1E 70 <tick LE32> 00 00 00   (LZ4 block, byte 0 сохранён)
```

Отправляется unreliable (flags 0) с flush, 30 Гц.

❌ Старый сервер отправлял 5 байт `1E <tick LE32>` без LZ4. Клиент
воспринимал это как сжатый пакет и падал в
`Failed to decompress network packet! Error code: -2`: 3 485 ошибок
за ~2 минуты (6 970 строк лога). Это также объясняло ощущение
«сервер в США»: авторитетного времени и подтверждений не было.

✅ После исправления клиент получает tick без единой ошибки
декомпрессии; см. §17.4.

### 17.4 Проверенная 15-минутная нативная сессия

Параметры:

- listener `native-tick-04`, GameServer `90296370461787156`;
- мир: `artifacts/steam/native-world-01/world.db` — **копия** сохранения
  пользователя (237 тел, 1 880 шейпов, 230 джойнтов);
- клиент: Steam-аккаунт `<STEAMID64>` внутри Sandboxie
  `DefaultBox`;
- длительность: 900 секунд.

Результат:

| Метрика | Значение |
|---|---|
| `challenge/authenticated/game_info/join/game_started` | 1 / 1 / 1 / 1 / 1 |
| сообщений | 41 814 (захвачено 10 000) |
| `Start state: Game` | 13:25:50 |
| `Stop state: Game` | 13:37:37, затем корректный возврат в Menu |
| `Failed to decompress` | **0** |
| `world.db` на выходе | 412 КБ |
| `applied_commands` | 0 (клиент не строил и не красил) |

Лог клиента:
`C:/Sandbox/stasa/DefaultBox/drive/D/Steam/steamapps/common/Scrap Mechanic/Logs/game-20261102-132527.log`.
Сервер:
`scrapds/artifacts/steam/native-tick-04/`.

### 17.5 Как правильно запускать тестового клиента

⚠️ Steam на этой машине работает **внутри Sandboxie** (`SbieDll.dll`
в процессе Steam). Запуск игры снаружи бокса даёт
`SteamAPI_Init() failed` («Steam must be running»), даже если Steam
виден на экране. Кроме того, `Start.exe` нельзя запускать из-под
администратора — он молча не создаёт процесс.

Рабочая схема:

1. Запустить игру **внутри** `DefaultBox`:
   `Start.exe /box:DefaultBox /wait <batch>`.
2. Сам `Start.exe` запускать **без прав администратора**:
   `runas /trustlevel:0x20000 <wrapper.cmd>`.
3. Внутри бокса `launch_connect.py` делает suspended launch, инжект
   probe, `ScrapdsForceConnectId`, `ResumeThread`.
4. SteamID внутри бокса — `<STEAMID64>`; серверу нужен
   `--allow-peer` с этим ID.

Это ограничение тестовой среды, а не архитектуры сервера.

### 17.6 Устойчивость отчёта listener

❌ Старый `serve-03` умер через 10,3 часа: `listener.json` 398 МБ,
`.listener.tmp` 0 байт, ошибка `Cannot finish probe output`.
Причина — `save()` каждую секунду писал **весь** отчёт, включая все
сообщения, а массивы не ограничивались.

✅ Исправлено:

- периодический отчёт (`раз в 30 секунд`) содержит только сводку и
  счётчики, без массивов;
- полный отчёт с массивами пишется только при штатном завершении;
- захват ограничен 10 000 сообщений и 64 МБ;
- ошибка записи промежуточного отчёта печатается в stderr и **не
  завершает сервер**;
- стартовый `world.db` создаётся до приёма клиентов; затем он атомарно
  обновляется каждые 30 секунд и при штатной остановке.

Результат: 15-минутный отчёт `native-tick-04` — 2,2 МБ вместо
сотен мегабайт.

### 17.7 Следующий порядок работ (обновлён)

1. Реальный vanilla-client тест постройки и её появления у второго
   клиента; синтетический Steam peer сейчас не инициализируется через
   доступную пользовательскую Steam-сессию.
2. Проверить PaintCharacter с live-клиентом, реализовать проверку
   инструмента/владения и NPC-таргетинг; расширить collision guard на terrain,
   детали и динамические препятствия, сверить Character с Player state.
3. Survival container transfers, crafting, harvestable/character actions,
   seating и held-tool state; сверить фактические расходы/возвраты предметов
   с игрой.
4. Авторитетная физика персонажей и построек, серверная коррекция.
5. Серверный LuaJIT/`sm.*`, game mode и моды.
6. Persistence прогресса в копию сейва.
7. Linux Steam transport и кроссплатформенная сборка.
8. Продуктовая обвязка: конфиг, администрирование, упаковка.

Текущий статус уточнён 2026-11-02: реальный vanilla-клиент проходил
handshake, загружал bootstrap мира, входил в игру и оставался подключён
15 минут без ошибок LZ4. Нативная команда строительства изменяет
in-memory SQLite мир; её NetObj update отправляется игровым клиентам.
При следующем подключении bootstrap пересобирается из актуального мира и
включает новые static body/block shape. Счётчик `Game.gametick` теперь
продвигается вместе с сетевым tick и сохраняется в `world.db`.

В том же захвате сообщение `id 9` (player-info) имеет 199 байт в transport
и 256 байт после LZ4-декомпрессии. Его поля пока не сопоставлены с
серверным Player/Character ID; переход `id 10` поэтому подтверждает только
продвижение state machine, а не авторизацию игрового персонажа.

IDA-анализ join handler уточнил runtime путь: при join CharacterManager
создаёт объект с owner SteamID подключившегося peer (поле `+56`), используя
ID от `UniqueIdProvider::getNext(4)`. Конструктор этого join объекта указывает
на `Lift` vtable, поэтому его связь с movement Character ID ещё не доказана.
Клиентский `0x1E` ID всё равно должен сверяться с server-created Character;
текущий listener не имеет такого manager/allocator и не может авторизовать
первую клиентскую заявку.
Также разобран server handler PaintCharacter: он ищет активный Character по
ID, применяет RGBA через server-only setter и для color `0` берёт default из
asset UUID Character. Это runtime NetObj изменение, оно не сохраняется в
таблицах мира. Декомпиляции сохранены в `scrapds/artifacts/ida-work/`
`character-construct-query/` и `join-followup-query/`. Character movement
lease в native listener теперь индексируется полным authenticated SteamID из
SNS message; он сохраняется, пока у аккаунта остаются активные соединения,
затем освобождается. Это предотвращает повторный захват через новый
connection handle, но пока не подтверждает право SteamID на заявленный
Character ID.

Уточнение IDA 2026-11-02: настоящий movement Character создаётся через
`CharacterManager::create` (RVA `0x821220`), который берёт ID из
`UniqueIdProvider::getNext(3)` и регистрирует Character в CharacterManager.
PlayerManager передаёт owner ID из своего Player descriptor. Это отдельный
путь от join-created Lift с allocator index 4. Для реализации в listener ещё
нужно реализовать wire-запись spawn. Индекс provider 3 инициализируется
значением 1 и не хранится в `Game.uniqueIds`: сохранённый слот 3 относится
к provider index 5. Клиентский NetObj factory (`D99ED0`, case 6) создаёт
Character и вызывает reader `72FE60`, соответствующий writer `72FDB0`.
Добавлены create record type 6 и кодирование его полей; подключение к join
ещё требует данных Player descriptor и подтверждения реальным клиентом.
Файлы анализа: `scrapds/artifacts/ida-work/character-spawn-calls/821220.c`,
`character-spawn-entry/7fb060.c`, `character-ctor-calls/824f40.c`.

Разбор пакета персонажа `0x1E` продолжен по захвату и декомпиляции:
реальный compressed 31-byte transport message распаковывается в 29-byte
movement update; декодер разбирает tick, ack-list, state type 3, позицию,
скорость (signed magnitude, 13 бит, шаг `10/8192`) и character ID. Значения
скорости сверяются с captured movement fixture. Пустой heartbeat и truncated payload проверяются отдельным
тестом. Декодирование tumble state type 4 также реализовано по паре
serializer/parser: quaternion с пропущенной компонентой, позиция и шесть
компактных величин состояния. Для type 4 проверен синтетический fixture;
реального захваченного tumble fixture пока нет. Listener валидирует пакет,
сохраняет последний sample на соединение
и пересылает принятый пакет остальным вошедшим клиентам. После первого
принятого state type 3 ID закрепляется за соединением: peer не может
переключиться на другой ID, второй peer не может занять тот же ID, а lease
освобождается при отключении. Устаревшие state ticks отклоняются с
учётом uint32 wrap; ack-only heartbeat не заменяет сохранённое состояние.
Это relay клиентской позиции, а не серверное
владение Character: нет проверки соответствия ID серверному Player при
первом захвате, физической коррекции и симуляции контроллера.

Добавлен native `PaintJoint` для сохранённых Joint BLOB варианта 69 байт:
цвет хранится как RGBA32 BE в последних четырёх байтах (`data[65..68]`),
что сверено с реальными Joint и ChildShape BLOB из одной копии save.
Изменение выполняется в SQLite-транзакции и реплицируется штатной записью
NetObj state type 2 с сохранённым asset-catalog index. Нулевой цвет
разрешается по asset UUID Joint и ShapeSet default color.
Проверено тестовым BLOB и save/load; живой визуальный тест клиента ещё не
выполнен.

Сквозное подтверждение строительства реальным vanilla-клиентом пока не
получено. Синтетический peer не прошёл `SteamAPI_Init`, поэтому это не
считается проверкой применения команды. Физика мира не симулируется:
collision backend покрывает только 214 block shapes из 1 880; joint и
character/dynamic runtime остаются неполными. Инвентарь, кресла,
инструменты, Lua API, Survival и моды не реализованы. Сервер не является
100% dedicated server.

После предыдущей проверки добавлено полное удаление выбранных block
shapes (`id 0x23`), удаление пустого body и split крупного блока при
частичной покраске и удалении региона. Сохранённая геометрия делится максимум на шесть
непересекающихся прямоугольных фрагментов; выделенная часть сохраняет
старый ID, фрагменты получают новые ID. Состояние и счётчик ID записываются
транзакционно. Клиентам отправляются NetObj create/state/remove operations.
Удаление shape со ссылками на joint/controller отклоняется. Изменения
покрыты gameplay-тестом и собираются, но gameplay wire path всё ещё
нуждается в тесте через настоящий клиент. CLI `serve` теперь запускает
экспериментальный listener с явными `--replay` и `--world`; production
supervision, настройки и полноценный native bootstrap пока отсутствуют.
Короткий запуск `serve-cli-smoke-01` зарегистрировал anonymous Steam
GameServer, создал и перечитал checkpoint (237 тел, 1 880 shapes, 230
joints); исходный save остался read-only.

После этой ревизии live bootstrap удаляет из replay-шаблона create/state
записи сущностей, которых уже нет в актуальном SQLite-мире, и добавляет
create/state для новых поддерживаемых 69-байтовых Joint. Так следующий
подключившийся клиент получает актуальные удалённые блоки и Joint, а не
устаревшие записи исходного replay. Joint с неизвестным asset UUID или
неподдерживаемой схемой сервер отклоняет. Capabilities JSON отмечает
`bootstrap_live_deletes` и `bootstrap_new_known_joints`; сборка проходит,
но vanilla-клиент после такой мутации ещё не запускался.

Добавлен opt-in публичный режим `serve --public`: после успешного Steam
anonymous login включается GameServer advertisement, а admission принимает
аутентифицированных individual SteamID из public universe. По умолчанию сервер
остаётся приватным и требует `--allow-peer` (опционально `--allow-peer2`).
Статус рекламы пишется в `ready.json` и `listener.json`; при штатном shutdown
advertisement выключается до Steam logoff. Это только Steam listing/admission;
публичная маршрутизация и vanilla-client join через открытый список пока не
проверены.

Каталог сетевых индексов теперь сохраняется рядом с checkpoint как
`asset-catalog.bin` и связывается с SHA-256 `world29.bin`. При перезапуске
необходимо переносить этот файл вместе с `world.db`: он сохраняет индексы
ресурсов даже после удаления последних исходных объектов. Два коротких запуска
Steam listener подтвердили запись и повторное чтение каталога; клиент в этой
проверке не подключался. Исправлена проверка времени запуска: `--seconds 0`
теперь действительно допускает бессрочный режим.

Экспериментальный `serve --native-players` создаёт Player и Character для
аутентифицированного клиента и удаляет старые записи игроков из шаблона
загрузки. В проверке `native-players-live-04` клиент вошёл в Game, сервер
принял 920 сообщений движения и heartbeat без отклонений. Исправлен порядок
байтов в идентификаторе и координатах персонажа; тестовый пакет теперь даёт
позицию `(16, 16, 0.72)`. Этот путь использует точку появления из захваченного
шаблона и пока не симулирует физику персонажа. Добавлена экспериментальная
обработка отключения: другим клиентам отправляется отвязка Player и удаление
Character; при повторном входе сохраняется ID Player.
Проверка кода проходит, но два реальных клиента и повторный вход ещё не
проверены: последняя попытка запуска игры завершилась ошибкой SteamAPI_Init.
Записи игроков и их ID сохраняются в серверной таблице `scrapds_player_v1`
вместе с checkpoint мира; совместимость дополнительной таблицы с загрузкой
сохранения обычной игрой пока не проверена.
Последняя принятая от клиента позиция сохраняется в таблице
`scrapds_player_location_v1` и используется при повторном входе. Для новых
наблюдателей сервер также формирует создание Character в последней известной
позиции. Добавлена проверка перемещения капсулы через импортированные блоки,
но её нельзя считать полной серверной физикой: ландшафт, детали и динамические
тела в этой проверке пока отсутствуют.
Вместо необрабатываемого клиентом служебного ID30 сервер теперь формирует
пустое обновление NetObj ID22; это подтверждено чтением кода клиента, но ещё
не проверено новым входом. Передача движения другим клиентам через ID30 убрана:
клиент не умеет его принимать. Вместо неё добавлено преобразование в серверный
ID24: два захваченных пакета штатного хоста восстанавливаются побайтно, а
принятое движение отправляется другим наблюдателям. Проверка этого пути двумя
живыми клиентами ещё не выполнена; физика остаётся на стороне клиента.
Полноценный сервер ещё не готов.

Добавлен экспериментальный режим `serve --native-players --native-physics`:
сервер симулирует в Bullet поддерживаемые динамические тела из блоков, сохраняет
их позы и скорости в checkpoint и отправляет клиентам обновления ID24. Параметры
плотности и материалов приближённые; тела с неподдержанными соединениями,
контроллерами, неизвестной геометрией или неподтверждёнными сетевыми флагами исключаются.
Ландшафт и исключённые детали в столкновениях отсутствуют, поэтому режим пока
непригоден для обычной игры и не включается автоматически. Отдельная команда
`simulate-world` позволяет запустить ту же симуляцию на копии сохранения.
Трёхсекундный запуск listener без клиентов выполнил 120 шагов: из 237 тел
симулировались 2 динамических и 5 статических. Проверка с живым клиентом ещё
не проведена.
Добавлены неподвижные опоры с соединениями и повёрнутые коробчатые детали;
их размеры и положение сверены с границами пяти объектов из сохранения.
Последний запуск обрабатывает 11 статических и 2 динамических тела из 237;
86 участков ландшафта пока не поддерживаются.
Добавлены цилиндрические детали: их положение сверено с сохранением, а
столкновение блока с цилиндром проверяется отдельно. В запуске `native-physics-smoke-06`
обрабатывались 13 статических и 2 динамических тела, включая 12 коробчатых
и 2 цилиндрические детали; запуск прошёл без подключения клиента. На тот
момент детали с мешами и динамические соединения ещё не поддерживались.

Добавлен разбор сохранённых изменений воксельного ландшафта: команда
`scrapds inspect-terrain <world.db>` читает записи без изменения сохранения,
проверяет координаты и извлекает изменения типа 5. В тестовом мире обнаружено
86 участков и 1621 изменение. Автоматическая привязка воксельных участков к
сохранённому миру ещё не восстановлена.

Добавлены чтение воксельных блоков игровых файлов `.tile`, сборка общих границ
и построение поверхности по таблицам версии 889 из исполняемого файла игры.
Поверхность можно явно передать в физическую сцену Bullet. Для мира
CreativeFlatWorld сервер теперь автоматически добавляет плоскую поверхность
на высоте 0, как задано в игровом скрипте. В тестовом мире тонкий блок после
400 шагов остался на поверхности. При запуске listener выполнено 79 тиков без
подключённого клиента. Воксельный ландшафт и другие типы поверхности пока
автоматически не подключаются; сервер ещё не готов к полноценной игре.

Для Survival восстановлен формат сохранённой карты генератора. В проверенном
сохранении прочитаны 16 128 ячеек с координатами, тайлами и поворотами; всем
ячейкам найдены файлы установленной игры. Загрузчик может получить воксели
ячейки в локальных координатах тайла и построить воксельную поверхность с
учётом координат и поворота ячейки. Надземный мир проверенного сохранения
использует карты высот, а не воксельные участки; их чтение добавлено. Для
столкновений ещё нужны расчёт возвышений и сглаживания и подключение к сцене.

В экспериментальную физику добавлены свободные подшипники: точки крепления и
оси читаются из сохранения. Симулируются только целиком поддерживаемые группы
тел без неподдерживаемых соединений и контроллеров; сетевой режим дополнительно
требует подтверждённые флаги каждого динамического тела. На копии мира после
400 шагов участвовали 13 динамических тел и 6 подшипников; расстояние между
парными креплениями этих подшипников было не больше 0,0025 единицы. Повторный
запуск Steam-сервера выполнил 78 тиков с теми же 13 телами и 6 подшипниками;
клиент в этом запуске не подключался.

Добавлены выпуклые формы деталей из файлов игры. Их положение проверено по
двум объектам сохранения Survival, а столкновение — с падающим блоком. В новом
запуске сервера участвовали 15 динамических тел, 6 подшипников и 4 детали с
такими формами; прошло 119 тиков без подключённого клиента. Масса деталей по
этим формам пока приближённая, а детали с неизвестным или смешанным форматом
геометрии по-прежнему исключаются.

Добавлена экспериментальная подвеска Survival: длина берётся из определения
детали, жёсткость — из сохранённого контроллера. Выделенная копия подвески из
сохранения прошла 100 шагов; проверены ограничения хода и влияние жёсткости.
После распознавания неподключённых штатных фар в одной из сохранённых конструкций
включились пять подвесок и десять подшипников. Запуск `native-spring-group-01`
обрабатывал 24 динамических тела и выполнил 193 тика за пять секунд без клиента;
86 участков воксельного ландшафта и 189 соединений ещё не поддерживаются.
Физика остаётся приближённой: в проверке под нагрузкой одна подвеска вышла за
предел хода примерно на 0,028 единицы. Изменение настройки подвески клиентом и
работа с подключёнными игроками ещё не проверены.

## 17. Потоковая поверхность Survival в физике

Формат `terrainData` ScriptData декодируется в сохранённые ячейки, карты
возвышений и уступов. Высота собирается из сетки `.tile`, игровых смещений,
поворотов ячеек и найденных FLATTEN-узлов. Рандомизированные области выравнивания
используют восстановленный hash генератора. При вычислении края читаются данные
соседней ячейки: в сохранённой карте проверены все 33 общие точки у пары
`(0,0)` / `(1,0)`.

`OverworldTerrainStreamer` подключён к `--native-physics`: поддерживаемые клетки
вокруг активных игроков и динамических тел превращаются в треугольники и
добавляются в Bullet. Неактивные клетки выгружаются из сцены; в памяти и физике
остаются не более 512 клеток, за один серверный тик строится не более четырёх.
Текущий счётчик доступности записывается в `overworld-terrain.json` рядом с
`physics.json`.

Начальные пакеты ID11/ID13 точечно обновляют только известные менеджеры:
`terrainData` берётся из записи ScriptData с совпадающим UUID, а два известных
GenericData-менеджера используют захваченные сетевые UUID/флаги и записи
сохранения с подтверждёнными SQLite UUID. Остальные BlobData-записи сохраняются
из захвата, поскольку их преобразование пока неизвестно. Поэтому часть данных
старой карты всё ещё может оставаться в bootstrap; совместимость этого
сопоставления с живым Survival-клиентом пока не проверена.

В ID25 также обновляются две захваченные группы ScriptData: runtime UUID
`ea4a3501c8d55d13b074498f392d5d7e` и
`c5b804f568305ff7947367630e1c5984` сопоставлены с сохранённым UUID
`5130c47b0a1953fca5a1205c19264574`. Сначала используется точное соответствие
ключей захвата; для новых Survival-объектов записи маршрутизируются по корневым
полям сериализованного Lua (`responseSlider` для первой группы и известные поля
контроллеров для второй). Подстановка включается лишь при наличии трёх или
более разных сохранённых ключей в группе. Пересечение классификаторов или
неизвестная схема оставляет запись из захвата. Остальные ID25-менеджеры всё
ещё зависят от bootstrap.

Steam Inventory blob из ID9 не попадает в Player packet до проверки владельца
через GameServer Inventory API. При успешной проверке сервер пересобирает
linked/unlinked Player записи, сохраняет проверенный unlinked snapshot в
выходную базу и рассылает ID13 update уже подключённым клиентам. При ошибке
или тайм-ауте blob остаётся исключённым. Это проверка Steam Inventory result,
отдельная от Survival item inventory. Сам асинхронный update не проверен живым
клиентом.

В live bootstrap захваченные Container create/state записи теперь удаляются,
если такого ID нет в `Container` таблице выбранного сейва. Player snapshots
теперь связаны с личным Container по SteamID; новые игроки получают 30-slot
контейнер и шесть стартовых инструментов с нативным форматом. Type4 create/state
пакеты сериализуют его содержимое.
Сохранённый Container schema 1 теперь разбирает массив 22-байтовых записей:
байты 2–17 — сериализованный UUID предмета, байты 18–21 — количество BE32;
`ffffffff` наблюдалось как пустой слот. Первые два байта записи и переменный
хвост сохраняются как opaque-данные. `WorldState` умеет проверить точный слот и
атомарно списать из него количество, сохраняя остальные байты и ID контейнера.
Survival BuildShape проверяет личный слот и UUID, рассчитывает стоимость по
объёму блока, атомарно создаёт блок и списывает стек, затем рассылает изменение
слота участникам того же мира. Сборка проходит, но путь ещё нужно проверить
живым клиентом. Экспериментальное удаление блоков в Survival возвращает объём
удалённой области в личный Container с учётом `stackSize` из определения блока;
если места недостаточно, операция целиком откатывается. Этот путь не проверен
живым клиентом и не подтверждает точную игровую логику возврата. Перенос
предметов, рецепты и раскраска в Survival не реализованы. Поддерживаемые
соединения без привязанного контроллера также удаляются с возвратом предмета
в контейнер; этот путь не проверен живым клиентом.

Для нативного сейва GameInfo теперь получает режим из `Game.flags`, а seed и
tick — из выбранного мира/сервера. На локальном сейве Survival значение равно
14, Creative — 1; другие пользовательские режимы требуют отдельного
сопоставления.

Поверхность пока экспериментальна. Полные соседние voxel chunks выбранных
тайлов добавляются к collision mesh, а изменения `VoxelTerrain` применяются
только к найденным chunks в том же мире; неполные границы пропускаются.
Счётчики дельт и resident voxel-клеток записываются в
`overworld-terrain.json`. Неизвестные ID25-менеджеры по-прежнему берутся из
захвата, поэтому обновление двух ScriptData-групп не гарантирует одинаковый
terrain state или полный Survival bootstrap.
При `--native-physics` капсула персонажа теперь sweep-and-slide проверяется
против текущих Bullet-тел и resident terrain; без native physics она
проверяется против сохранённых block-shapes и resident Survival terrain.
В обоих режимах terrain подгружается рядом с персонажем из сейва; Creative
плоский мир задаёт сохранённая collision-плоскость. Это только
коррекция принятой клиентской позиции: перемещение ограничено 6 м/с по
горизонтали, 8 м/с вверх (предел выбран с запасом над `jumpImpulse: 7.0`)
и 50 м/с вниз, с допуском 0,3 м на пакет. Превышение ограничений возвращает
персонажа к последней принятой позиции и отправляет сетевую коррекцию.
Повторные пакеты одного client tick проверяются относительно позиции начала
этого тика, поэтому повторная отправка не обновляет допуск 0,3 м и не даёт
постепенно набирать дистанцию без продвижения тика.
Сервер пока не симулирует
CharacterController, гравитацию, прыжки, падение, урон и перемещение между
мирами. Проверено fixture-тестами, но ещё не живым клиентом. Точная
триангуляция и physics parity также не подтверждены, bootstrap всё ещё зависит
от захвата.
Создание/состояние активного Character и его Container отправляется только
игрокам того же сохранённого мира. Успешные ответы на изменения мира также
ограничены этим миром; офлайн Player-записи остаются в общем списке. Этот
фильтр пока не проверен с двумя живыми клиентами в разных мирах.
Запросы взаимодействия с Harvestable и Character (`0x28`/`0x29`) строго
разбираются, но отклоняются, пока серверная логика этих действий не готова;
они не пересылаются другим клиентам.

Для ограниченного пути добавлена команда клиента ID27: сервер проверяет
существование Joint, его поддерживаемый формат, мир подключённых Shape и
отсутствие ссылок на связанные контроллеры. В Creative команда атомарно
удаляет SQLite-строку и рассылает NetObj remove type2. В Survival сервер
возвращает один предмет соединения в личный Container через `stackSize` из
asset definition и объединяет возврат с удалением в транзакцию. Fixture-тесты
проверяют оба режима; Survival-команда всё ещё требует проверки живым клиентом.
