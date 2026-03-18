# Search Engine System

## Tech Stack

| Component         | Language/Tool |
|-------------------|---------------|
| Crawler/Scraper   | C++           |
| Search API        | Go            |
| Storage & Search  | ElasticSearch |

---

## Requirements

- `g++` with C++17 support
- `libcurl` and `libgumbo` (gumbo-parser)
- Go 1.22+
- Docker and Docker Compose

Install dependencies on Ubuntu/Debian:
```bash
sudo apt install g++ libcurl4-openssl-dev libgumbo-dev
```

---

## Build

```bash
make
```

This builds both the crawler (`crawler`) and the search API (`search-api/search-api`).

---

## Usage

### 1. Start ElasticSearch

```bash
docker compose up -d
```

Verify it's running:
```bash
curl http://localhost:9200
```

---

### 2. Run the Crawler

```bash
./crawler <seed_url> [options]
```

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--threads N` | 4 | Number of worker threads |
| `--timeout S` | 10 | Per-request timeout in seconds |
| `--max-pages N` | unlimited | Stop after N pages |
| `--es-url URL` | http://localhost:9200 | ElasticSearch base URL |
| `--es-index NAME` | pages | ElasticSearch index name |

**Example:**
```bash
./crawler https://example.com --max-pages 100 --threads 8
```

Check how many pages were indexed:
```bash
curl http://localhost:9200/pages/_count
```

---

### 3. Run the Search API

```bash
cd search-api && ./search-api
```

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--addr ADDR` | :8080 | Address to listen on |
| `--es-url URL` | http://localhost:9200 | ElasticSearch base URL |
| `--es-index NAME` | pages | ElasticSearch index name |
| `--size N` | 10 | Default number of results |

---

### 4. Search

**Web UI:** Open http://localhost:8080 in your browser.

**API:**
```bash
curl "http://localhost:8080/search?q=your+query"
```

Response:
```json
{
  "query": "your query",
  "total": 42,
  "results": [
    {
      "url": "https://example.com/page",
      "timestamp": "2026-03-18T10:00:00Z",
      "score": 4.25,
      "excerpt": "...matching text excerpt..."
    }
  ]
}
```

**Health check:**
```bash
curl http://localhost:8080/health
```

---

## Architecture

```
Crawler (C++) --> ElasticSearch (Docker) --> Search API (Go) --> User
```

1. The crawler starts from a seed URL, recursively follows links, extracts text, and indexes each page into ElasticSearch.
2. ElasticSearch stores documents and ranks search results using BM25.
3. The search API exposes an HTTP endpoint and web UI that queries ElasticSearch and returns ranked results.
