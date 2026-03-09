## TODO

## 1. Tokenization

- Lowercase all tokens
- Remove punctuation: `. , ? !`
- Split text into tokens: `"i"`, `"go"`, `"home"`
- Build inverted index:
  - `go` → `[url1, url2]`
  - `school` → `[url1, url2]`
- Data structures: **Hash Table** (fast lookup) and/or **B-Tree** (sorted/range queries)

---

## 2. Crawler (Multithreaded)
crawer multithread  -- threadpool -- timeout for loading(argument for crawler)

- **Thread pool** — fixed number of worker threads
- **Timeout** — CLI argument for page load timeout (e.g. `--timeout 10`)
- **Links queue** — shared queue for URLs to crawl (thread-safe)
- HTML parsing via **Gumbo** library

---


