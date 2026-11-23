# Parallel Search Engine

Паралельний пошуковий рушій на C++20 для повнотекстового пошуку в наборі текстових документів. Проєкт будує інвертований індекс із кількома потоками, повертає найбільш релевантні документи та уривки навколо знайдених термінів.

Для розширення пошукового запиту синонімами використовується локальна модель Ollama. Базовий пошук працює також без LLM і реалізує fallback до вихідного запиту.

## Можливості

- Багатопотокове індексування файлів через власний пул потоків.
- Шардований інвертований індекс із читачо-записувальними м'ютексами.
- Два режими пошуку: звичайний пошуковий запит та розширений через Ollama.
- REST API для пошуку, індексування та перевірки стану.
- WebSocket API для пошукового пайплайну з поетапним результатом.
- Готовий веб-інтерфейс без зовнішніх JavaScript-залежностей.
- CLI-клієнт для пошуку з терміналу.
- Docker-оточення для сервера пошуку та Ollama.

## Архітектура

```text
Клієнт (Web UI / CLI)
        |
        +--> REST API:8080
        |       +-- /api/search
        |       +-- /api/index
        |       +-- /api/stats
        |       +-- /api/health
        |
        +--> WebSocket:8081
                +-- /ws/search

Search server
  |-- ThreadPool       Багатопотокове читання та індексування файлів
  |-- InvertedIndex    256 шардів та частоти термінів у документах
  |-- DatasetIndexer   Токенізація та побудова індексу
  +-- Ollama client    Розширення запиту синонімами

Директорія data/       Вхідний текстовий датасет
```

Пошуковий запит перетворюється на терміни довжиною від 2 до 30 символів. Документи ранжуються за сумою частот відповідних термінів; повертається максимум 10 найкращих документів із текстовим уривком.

## Структура проєкту

```text
.
├── cli/search.cpp          # CLI-клієнт пошуку
├── data/                   # Текстові документи датасету
├── src/
│   ├── main.cpp            # REST/WebSocket сервер та пошуковий пайплайн
│   ├── inverted_index.*    # Шардований інвертований індекс
│   ├── dataset_indexer.*   # Паралельне індексування датасету
│   ├── thread_pool.*       # Пул потоків
│   └── ws_server.h         # WebSocket-сервер
├── web/index.html          # Веб-інтерфейс
├── CMakeLists.txt          # Конфігурація збірки
├── Dockerfile
└── docker-compose.yml
```

## Вимоги

### Docker

- Docker
- Docker Compose
- Рекомендовано 2 ГБ вільної RAM для моделі Ollama

### Локальна збірка

- Linux або сумісне POSIX-середовище (наприклад, WSL)
- CMake 3.20+
- компілятор із підтримкою C++20
- pthread
- OpenSSL

Для Ubuntu або Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev
```

## Запуск через Docker

З кореня проєкту:

```bash
docker compose up --build -d
docker compose exec ollama ollama pull qwen2.5:1.5b
```

Після запуску відкрийте:

- Веб-інтерфейс: http://localhost:8080
- Перевірка стану: http://localhost:8080/api/health
- Статистика індексу: http://localhost:8080/api/stats
- Ollama API: http://localhost:11434

Зупинка контейнерів:

```bash
docker compose down
```

Для видалення також завантаженої моделі Ollama:

```bash
docker compose down -v
```

## Локальна збірка та запуск

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/search_server
```

Сервер очікує датасет у `./data`, HTTP-сервер слухає порт `8080`, а WebSocket-сервер — порт `8081`.

Для запуску CLI в іншому терміналі:

```bash
./build/search "search query"
./build/search --keyword "parallel computing"
./build/search --expanded "artificial intelligence"
```

За замовчуванням CLI надсилає запит до `http://localhost:8080`. Адресу сервера можна змінити змінною `SEARCH_SERVER_URL`:

```bash
SEARCH_SERVER_URL=http://localhost:8080 ./build/search "database"
```

## Конфігурація сервера

| Змінна | За замовчуванням | Призначення |
|---|---|---|
| `DATASET_PATH` | `./data` | Шлях до датасету з текстовими файлами |
| `OLLAMA_HOST` | `http://ollama:11434` | Адреса API Ollama |
| `OLLAMA_MODEL` | `qwen2.5:1.5b` | Модель для розширення запиту |

При локальному запуску з Ollama на хост-машині:

```bash
DATASET_PATH=./data \
OLLAMA_HOST=http://localhost:11434 \
OLLAMA_MODEL=qwen2.5:1.5b \
./build/search_server
```

## Підготовка датасету

Покладіть текстові файли в `data/`. Індексатор рекурсивно обходить усі файли у директорії та використовує назву кожного файла як `doc_id`.

```text
data/
├── 1.txt
├── 2.txt
└── astronomy/
    └── 3.txt
```

Індексація запускається автоматично при старті сервера. Для повторної індексації без перезапуску надішліть:

```bash
curl -X POST http://localhost:8080/api/index \
  -H "Content-Type: application/json" \
  -d '{"threads": 8}'
```

## REST API

### Перевірка стану

```http
GET /api/health
```

Приклад відповіді:

```json
{"status":"ok","ollama":true}
```

### Статистика

```http
GET /api/stats
```

Приклад відповіді:

```json
{
  "documents": 1234,
  "index_build_time_ms": 123,
  "threads_used": 6
}
```

### Пошук

```http
POST /api/search
Content-Type: application/json

{"query":"machine learning","mode":"expanded"}
```

Режими `mode`:

- `keyword` — пошук без розширення запиту.
- `expanded` або `llm` — розширення запиту через Ollama.

Приклад відповіді:

```json
{
  "results": [
    {
      "doc_id": "42.txt",
      "score": 7,
      "snippet": "...relevant text..."
    }
  ],
  "expanded_terms": ["machine", "learning", "algorithm"],
  "time_ms": 15
}
```

### Перебудова індексу

```http
POST /api/index
Content-Type: application/json

{"threads":4}
```

Параметр `threads` необов'язковий.

## WebSocket API

Підключення:

```text
ws://localhost:8081/ws/search
```

Відправте JSON-повідомлення:

```json
{"query":"space exploration"}
```

Сервер послідовно надсилає три типи повідомлень:

1. `query_expansion` — вихідний запит та розширені терміни.
2. `keyword_search` — результати пошуку та час пошуку.
3. `done` — завершення пайплайну.

## Приклади CLI

Пошук із розширенням через Ollama:

```bash
./build/search "artificial intelligence"
```

Пошук лише за точними ключовими словами:

```bash
./build/search --keyword "neural network"
```

Сценарій із CLI-клієнтом всередині Docker-контейнера потребує запуску серверного контейнера та виконання бінарника всередині образу:

```bash
docker compose up --build -d
docker compose exec search_server ./search --keyword "parallel computing"
```
