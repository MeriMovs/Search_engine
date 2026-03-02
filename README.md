# Search Engine System



## Tech Stack

|     Component     | Language/Tool |
|-------------------|---------------|
| Search Engine     | Go            |
| Scraper / Crawler | C++           |
| Storage & Search  | ElasticSearch |

---

## Components

### 1. Search Engine (Go)
Accepts a search query from the user and forwards it to ElasticSearch.

- HTTP endpoint for query input
- Passes query to ElasticSearch BM25 search
- Returns ranked document results

### 2. Scraper / Crawler (C++)
Recursively crawls the web starting from a seed URL and feeds documents into ElasticSearch.

- Takes an initial URL as entry point
- Recursively follows all links found on each page
- Extracts plain text from each page
- Outputs structured documents: `{ url, timestamp, text }`

### 3. ElasticSearch (Docker)
Stores and indexes all crawled documents, enabling fast full-text search.

- Stores documents as `{ url, timestamp, text }`
- Full-text search via BM25 ranking algorithm
- Fast keyword-based document retrieval

---

