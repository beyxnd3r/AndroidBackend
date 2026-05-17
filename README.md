# AndroidBackend

Desktop'ое backend приложение для Android-проекта, который собирает GPS-координаты и параметры мобильной сети со смартфона, принимает их по ZeroMQ, сохраняет в PostgreSQL и показывает в окне ImGui.

Проект состоит из двух частей:

- `src/main.cpp` - приложение на языке C++ с ZeroMQ-сервером, подключением к PostgreSQL и desktop-интерфейсом.
- `AndroidProject/` - Android-приложение, которое может отправлять данные о локации и сетях на backend и выполнять другие функции.

## Что делает приложение

При запуске C++ приложение поднимает ZeroMQ REP-сервер на `tcp://0.0.0.0:5555` и открывает desktop-окно `Smartphone Network & GPS Monitor`.

Backend принимает JSON-сообщения от Android-клиента, записывает их в таблицу `measurements`, дублирует входящие пакеты в `location_log.json` и обновляет данные в интерфейсе. При следующем запуске история из `location_log.json` загружается обратно, поэтому графики и карта могут отображать уже накопленные измерения.

В интерфейсе есть:

- текущие GPS-данные: широта, долгота, высота, точность, время;
- список LTE/GSM/NR-сетей из последнего принятого пакета;
- графики LTE-показателей по `PCI`: `RSRP`, `RSSI`, `SINR`, активность `PCI`;
- карта на тайлах с кешированием в папку `tiles/`;
- GPS-трек;
- тепловая карта LTE-измерений поверх карты.



## Зависимости

Для C++ части нужны:

- CMake 3.14+;
- компилятор C++17;
- SDL2;
- OpenGL;
- GLEW;
- ZeroMQ;
- PostgreSQL client library (`libpq`);
- nlohmann/json;
- libcurl;
- submodules `third_party/imgui`, `third_party/implot`, `AndroidProject`.

Пример установки зависимостей в Ubuntu/WSL:

```bash
sudo apt update
sudo apt install cmake g++ libsdl2-dev libglew-dev libzmq3-dev libpq-dev nlohmann-json3-dev libcurl4-openssl-dev
```

После клонирования репозитория нужно подключить submodule и выполнить:

```bash
git submodule update --init --recursive
```

## База данных

Приложение ожидает PostgreSQL по строке подключения:

```text
host=localhost port=5432 dbname=network_monitor user=postgres password=1234
```

Таблица создается SQL-скриптом [db/init.sql](db/init.sql):

```sql
CREATE TABLE IF NOT EXISTS measurements (
    id SERIAL PRIMARY KEY,
    time TIMESTAMP,
    latitude DOUBLE PRECISION,
    longitude DOUBLE PRECISION,
    altitude DOUBLE PRECISION,
    accuracy DOUBLE PRECISION,
    pci INTEGER,
    rsrp INTEGER,
    rsrq INTEGER,
    rssi INTEGER,
    rssnr INTEGER
);
```

В [db/docker-compose.yml](db/docker-compose.yml) PostgreSQL проброшен наружу как `5433:5432`. Если запускать базу через этот compose-файл, нужно либо поменять порт в `src/main.cpp` на `5433`, либо пробросить контейнер на host-порт `5432`.

Запуск базы через Docker Compose:

```bash
cd db
docker compose up -d
```

## Сборка

Из корня репозитория:

```bash
mkdir build && cd build
cmake ..
make 
```
или:
```bash
mkdir build && cd build
cmake ..
make -j
```


## Запуск

1. Запустите PostgreSQL.
2. Соберите C++ приложение.
3. Запустите исполняемый файл:

```bash
./build/main
```


После запуска backend слушает `tcp://<ip-пк>:5555`. Android-клиент должен отправлять туда JSON с координатами и массивом `networks`.



## Полезные файлы

- [src/main.cpp](src/main.cpp) - основной backend, GUI, карта, графики и heatmap.
- [db/init.sql](db/init.sql) - схема таблицы измерений.
- [db/docker-compose.yml](db/docker-compose.yml) - PostgreSQL для локального запуска.
