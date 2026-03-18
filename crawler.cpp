#include <curl/curl.h>
#include <gumbo.h>
#include <iostream>
#include <string>
#include <queue>
#include <vector>
#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstring>
#include <sstream>

// ---------------------------------------------------------------------------
// Configuration defaults (overridden by CLI args)
// ---------------------------------------------------------------------------
static constexpr int DEFAULT_THREADS   = 4;
static constexpr int DEFAULT_TIMEOUT   = 10;   // seconds
static constexpr int DEFAULT_MAX_PAGES = 0;    // 0 = unlimited

// ---------------------------------------------------------------------------
// Utility — current UTC time as ISO 8601 string
// ---------------------------------------------------------------------------
std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// JSON string escaping
// ---------------------------------------------------------------------------
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tokenization — lowercase, keep [a-z0-9] tokens only
// ---------------------------------------------------------------------------
std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string token;
    token.reserve(32);

    for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
            token += static_cast<char>(std::tolower(ch));
        } else {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        }
    }
    if (!token.empty())
        tokens.push_back(token);

    return tokens;
}

// ---------------------------------------------------------------------------
// PageData
// ---------------------------------------------------------------------------
struct PageData {
    std::string              url;
    std::string              text;
    std::vector<std::string> links;
    int                      status_code;
    std::string              timestamp;        // UTC ISO 8601
    std::vector<std::string> tokens;
};

// ---------------------------------------------------------------------------
// PageLoader  (one instance per thread — CURL handles are not thread-safe)
// ---------------------------------------------------------------------------
class PageLoader {
public:
    explicit PageLoader(long timeout_secs) : timeout_(timeout_secs) {}

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* u) {
        ((std::string*)u)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string fetch(const std::string& url, int& status_code) {
        CURL* curl = curl_easy_init();
        std::string rb;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_);
            curl_easy_perform(curl);
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            status_code = static_cast<int>(code);
            curl_easy_cleanup(curl);
        }
        return rb;
    }

private:
    long timeout_;
};

// ---------------------------------------------------------------------------
// ESClient  (one instance per thread — owns its own CURL handle)
// ---------------------------------------------------------------------------
class ESClient {
public:
    ESClient(std::string base_url, std::string index)
        : base_url_(std::move(base_url))
        , index_(std::move(index))
        , curl_(curl_easy_init())
    {}

    ~ESClient() {
        if (curl_) curl_easy_cleanup(curl_);
    }

    ESClient(const ESClient&)            = delete;
    ESClient& operator=(const ESClient&) = delete;

    // Returns HTTP response code, or -1 on CURL error.
    int index_page(const PageData& pg) {
        if (!curl_) return -1;

        std::string url  = base_url_ + "/" + index_ + "/_doc";
        std::string body = build_json(pg);

        curl_easy_reset(curl_);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl_, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS,    body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER,    headers);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT,       10L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, discard_cb);

        CURLcode res = curl_easy_perform(curl_);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) return -1;

        long code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
        return static_cast<int>(code);
    }

private:
    std::string base_url_;
    std::string index_;
    CURL*       curl_;

    static size_t discard_cb(void*, size_t size, size_t nmemb, void*) {
        return size * nmemb;
    }

    std::string build_json(const PageData& pg) const {
        static constexpr size_t MAX_TEXT = 32 * 1024;
        const std::string text = pg.text.size() > MAX_TEXT
                                 ? pg.text.substr(0, MAX_TEXT)
                                 : pg.text;
        std::ostringstream ss;
        ss << "{"
           << "\"url\":\""       << json_escape(pg.url)       << "\","
           << "\"timestamp\":\"" << json_escape(pg.timestamp) << "\","
           << "\"status\":"      << pg.status_code            << ","
           << "\"text\":\""      << json_escape(text)         << "\","
           << "\"token_count\":" << pg.tokens.size()
           << "}";
        return ss.str();
    }
};

// ---------------------------------------------------------------------------
// Gumbo helpers
// ---------------------------------------------------------------------------
void extract_text(GumboNode* node, std::string& text) {
    if (node->type == GUMBO_NODE_TEXT) {
        text += node->v.text.text;
        text += " ";
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) return;

    GumboTag tag = node->v.element.tag;
    if (tag == GUMBO_TAG_SCRIPT || tag == GUMBO_TAG_STYLE) return;

    GumboVector* children = &node->v.element.children;
    for (size_t i = 0; i < children->length; i++)
        extract_text((GumboNode*)children->data[i], text);
}

void extract_links(GumboNode* node, std::vector<std::string>& links) {
    if (node->type != GUMBO_NODE_ELEMENT) return;

    if (node->v.element.tag == GUMBO_TAG_A) {
        GumboAttribute* href = gumbo_get_attribute(&node->v.element.attributes, "href");
        if (href && std::string(href->value).substr(0, 4) == "http")
            links.push_back(href->value);
    }

    GumboVector* children = &node->v.element.children;
    for (size_t i = 0; i < children->length; i++)
        extract_links((GumboNode*)children->data[i], links);
}

PageData parse(const std::string& url, const std::string& html, int status_code) {
    PageData pg;
    pg.url         = url;
    pg.status_code = status_code;
    pg.timestamp   = utc_timestamp();

    GumboOutput* output = gumbo_parse(html.c_str());
    extract_text(output->root, pg.text);
    extract_links(output->root, pg.links);
    gumbo_destroy_output(&kGumboDefaultOptions, output);

    pg.tokens = tokenize(pg.text);

    return pg;
}

// ---------------------------------------------------------------------------
// Thread-safe work queue
// ---------------------------------------------------------------------------
class WorkQueue {
public:
    // Push a new URL; returns false if the URL was already visited or the
    // queue is shut down.
    bool push(const std::string& url) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (shutdown_ || visited_.count(url)) return false;
        visited_.insert(url);
        queue_.push(url);
        cv_.notify_one();
        return true;
    }

    // Pop the next URL to process.
    // Blocks until work is available or the queue is shut down.
    // Returns false when shut down and the queue is empty.
    bool pop(std::string& url) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] {
            return !queue_.empty() || shutdown_;
        });
        if (queue_.empty()) return false;
        url = queue_.front();
        queue_.pop();
        ++in_flight_;
        return true;
    }

    // Call after a worker finishes processing one URL.
    // When in_flight_ reaches 0 and the queue is empty, signal shutdown.
    void done() {
        std::lock_guard<std::mutex> lk(mtx_);
        --in_flight_;
        if (in_flight_ == 0 && queue_.empty()) {
            shutdown_ = true;
            cv_.notify_all();
        }
    }

    // Force shutdown (e.g., on error, early termination, or max-pages reached).
    void shutdown() {
        std::lock_guard<std::mutex> lk(mtx_);
        shutdown_ = true;
        cv_.notify_all();
    }

    bool is_shutdown() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return shutdown_;
    }

private:
    mutable std::mutex       mtx_;
    std::condition_variable  cv_;
    std::queue<std::string>  queue_;
    std::set<std::string>    visited_;
    int                      in_flight_ = 0;
    bool                     shutdown_  = false;
};

// ---------------------------------------------------------------------------
// Console mutex — guards all std::cout output
// ---------------------------------------------------------------------------
static std::mutex cout_mtx;

// ---------------------------------------------------------------------------
// Worker thread function
// ---------------------------------------------------------------------------
void worker(WorkQueue&         wq,
            int                timeout_secs,
            int                max_pages,
            std::atomic<int>&  pages_crawled,
            const std::string& es_url,
            const std::string& es_index)
{
    PageLoader loader(static_cast<long>(timeout_secs));
    ESClient   es_client(es_url, es_index);

    while (true) {
        std::string url;
        if (!wq.pop(url)) break;   // queue drained and shut down

        int         status_code = 0;
        std::string html        = loader.fetch(url, status_code);

        if (status_code != 200) {
            {
                std::lock_guard<std::mutex> lk(cout_mtx);
                std::cout << "[SKIP] " << url << " (" << status_code << ")\n";
            }
            wq.done();
            continue;
        }

        // Check max-pages limit before counting this page.
        if (max_pages > 0) {
            int prev = pages_crawled.fetch_add(1, std::memory_order_relaxed);
            if (prev >= max_pages) {
                // We already hit the limit; don't process this page.
                wq.shutdown();
                wq.done();
                break;
            }
        }

        PageData pg = parse(url, html, status_code);

        {
            std::lock_guard<std::mutex> lk(cout_mtx);
            std::cout << "[OK] " << pg.url << "\n";
            std::cout << "Timestamp: " << pg.timestamp << "\n";
            std::cout << "Text: "
                      << (pg.text.size() > 100 ? pg.text.substr(0, 100) : pg.text)
                      << "...\n";

            // Print first 10 tokens as a preview.
            std::cout << "Tokens (first 10): [";
            size_t preview = std::min<size_t>(pg.tokens.size(), 10);
            for (size_t i = 0; i < preview; ++i) {
                std::cout << pg.tokens[i];
                if (i + 1 < preview) std::cout << ", ";
            }
            std::cout << "] (" << pg.tokens.size() << " total)\n";

            std::cout << "Links found: " << pg.links.size() << "\n\n";
        }

        // Index in ElasticSearch
        int es_status = es_client.index_page(pg);
        {
            std::lock_guard<std::mutex> lk(cout_mtx);
            if (es_status == 200 || es_status == 201)
                std::cout << "[ES OK]   " << pg.url << " -> " << es_status << "\n";
            else
                std::cout << "[ES FAIL] " << pg.url << " -> " << es_status << "\n";
        }

        // Enqueue newly discovered links only if we haven't hit the limit.
        if (max_pages == 0 ||
            pages_crawled.load(std::memory_order_relaxed) < max_pages)
        {
            for (const auto& link : pg.links)
                wq.push(link);   // push() is thread-safe; silently ignores visited URLs
        } else {
            wq.shutdown();
        }

        wq.done();
    }
}

// ---------------------------------------------------------------------------
// Usage / CLI helpers
// ---------------------------------------------------------------------------
static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <seed_url>"
                 " [--threads N]"
                 " [--timeout S]"
                 " [--max-pages N]\n"
              << "\n"
              << "  seed_url     First URL to crawl (required)\n"
              << "  --threads N  Number of worker threads  (default: " << DEFAULT_THREADS   << ")\n"
              << "  --timeout S  Per-request timeout (sec)  (default: " << DEFAULT_TIMEOUT   << ")\n"
              << "  --max-pages N  Stop after N successful pages (default: unlimited)\n"
              << "  --es-url URL   ElasticSearch base URL     (default: http://localhost:9200)\n"
              << "  --es-index N   ES index name              (default: pages)\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // ---- Parse CLI arguments ----
    std::string seed_url;
    int         num_threads = DEFAULT_THREADS;
    int         timeout     = DEFAULT_TIMEOUT;
    int         max_pages   = DEFAULT_MAX_PAGES;
    std::string es_url      = "http://localhost:9200";
    std::string es_index    = "pages";

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--threads") {
            if (i + 1 >= argc) { std::cerr << "Missing value for --threads\n"; return 1; }
            num_threads = std::stoi(argv[++i]);
            if (num_threads < 1) { std::cerr << "--threads must be >= 1\n"; return 1; }
        } else if (arg == "--timeout") {
            if (i + 1 >= argc) { std::cerr << "Missing value for --timeout\n"; return 1; }
            timeout = std::stoi(argv[++i]);
            if (timeout < 1) { std::cerr << "--timeout must be >= 1\n"; return 1; }
        } else if (arg == "--max-pages") {
            if (i + 1 >= argc) { std::cerr << "Missing value for --max-pages\n"; return 1; }
            max_pages = std::stoi(argv[++i]);
            if (max_pages < 0) { std::cerr << "--max-pages must be >= 0\n"; return 1; }
        } else if (arg == "--es-url") {
            if (i + 1 >= argc) { std::cerr << "Missing value for --es-url\n"; return 1; }
            es_url = argv[++i];
        } else if (arg == "--es-index") {
            if (i + 1 >= argc) { std::cerr << "Missing value for --es-index\n"; return 1; }
            es_index = argv[++i];
        } else if (seed_url.empty() && arg.substr(0, 2) != "--") {
            seed_url = arg;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (seed_url.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "Crawler starting\n"
              << "  seed:      " << seed_url    << "\n"
              << "  threads:   " << num_threads  << "\n"
              << "  timeout:   " << timeout      << "s\n"
              << "  max-pages: " << (max_pages > 0 ? std::to_string(max_pages) : "unlimited") << "\n"
              << "  es-url:    " << es_url    << "\n"
              << "  es-index:  " << es_index  << "\n\n";

    // ---- Set up and run ----
    // curl_global_init is NOT thread-safe; call it once before spawning threads.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::atomic<int> pages_crawled{0};
    WorkQueue wq;
    wq.push(seed_url);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i)
        threads.emplace_back(worker,
                             std::ref(wq),
                             timeout,
                             max_pages,
                             std::ref(pages_crawled),
                             std::cref(es_url),
                             std::cref(es_index));

    for (auto& t : threads)
        t.join();

    std::cout << "Crawl complete. Pages crawled: "
              << pages_crawled.load() << "\n";

    curl_global_cleanup();
    return 0;
}
