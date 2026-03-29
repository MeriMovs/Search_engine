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

static constexpr int DEFAULT_THREADS   = 4;
static constexpr int DEFAULT_TIMEOUT   = 10;
static constexpr int DEFAULT_MAX_PAGES = 5;

std::string json_escape(const std::string& s) {
    std::string out;
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
                out += static_cast<char>(c);
        }
    }
    return out;
}

struct PageData {
    std::string              url;
    std::string              text;
    std::vector<std::string> links;
    int                      status_code;
};

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

class ESClient {
public:
    ESClient(std::string base_url, std::string index)
        : base_url_(std::move(base_url))
        , index_(std::move(index))
        , curl_(curl_easy_init())
    {}

    ~ESClient() { if (curl_) curl_easy_cleanup(curl_); }

    int index_page(const PageData& pg) {
        if (!curl_) return -1;

        static constexpr size_t MAX_TEXT = 32 * 1024;
        const std::string text = pg.text.size() > MAX_TEXT
                                 ? pg.text.substr(0, MAX_TEXT) : pg.text;
        std::string body = "{"
            "\"url\":\""       + json_escape(pg.url)                  + "\","
            "\"status\":"      + std::to_string(pg.status_code)       + ","
            "\"text\":\""      + json_escape(text)                    + "\","
            "}";
        std::string url  = base_url_ + "/" + index_ + "/_doc";

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
};

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

    GumboOutput* output = gumbo_parse(html.c_str());
    extract_text(output->root, pg.text);
    extract_links(output->root, pg.links);
    gumbo_destroy_output(&kGumboDefaultOptions, output);

    return pg;
}

class WorkQueue {
public:
    bool push(const std::string& url) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (shutdown_ || visited_.count(url)) return false;
        visited_.insert(url);
        queue_.push(url);
        cv_.notify_one();
        return true;
    }

    bool pop(std::string& url) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !queue_.empty() || shutdown_; });
        if (queue_.empty()) return false;
        url = queue_.front();
        queue_.pop();
        ++in_flight_;
        return true;
    }

    void done() {
        std::lock_guard<std::mutex> lk(mtx_);
        --in_flight_;
        if (in_flight_ == 0 && queue_.empty()) {
            shutdown_ = true;
            cv_.notify_all();
        }
    }

    void shutdown() {
        std::lock_guard<std::mutex> lk(mtx_);
        shutdown_ = true;
        cv_.notify_all();
    }

private:
    mutable std::mutex       mtx_;
    std::condition_variable  cv_;
    std::queue<std::string>  queue_;
    std::set<std::string>    visited_;
    int                      in_flight_ = 0;
    bool                     shutdown_  = false;
};

static std::mutex cout_mtx;

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
        if (!wq.pop(url)) break;

        int         status_code = 0;
        std::string html        = loader.fetch(url, status_code);

        if (status_code != 200) {
            std::lock_guard<std::mutex> lk(cout_mtx);
            std::cout << "[SKIP] " << url << " (" << status_code << ")\n";
            wq.done();
            continue;
        }

        if (max_pages > 0) {
            int prev = pages_crawled.fetch_add(1);
            if (prev >= max_pages) {
                wq.shutdown();
                wq.done();
                break;
            }
        }

        PageData pg       = parse(url, html, status_code);
        int      es_status = es_client.index_page(pg);

        {
            std::lock_guard<std::mutex> lk(cout_mtx);
            std::cout << (es_status == 200 || es_status == 201 ? "[OK] " : "[ES FAIL] ")
                      << pg.url
                      << " (links: " << pg.links.size()
                      << ", es: " << es_status << ")\n";
        }

        if (max_pages == 0 || pages_crawled.load() < max_pages) {
            for (const auto& link : pg.links)
                wq.push(link);
        } else {
            wq.shutdown();
        }

        wq.done();
    }
}

static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <seed_url> [--threads N] [--timeout S] [--max-pages N]"
                 " [--es-url URL] [--es-index NAME]\n"
              << "  --threads N    Worker threads      (default: " << DEFAULT_THREADS   << ")\n"
              << "  --timeout S    Per-request timeout (default: " << DEFAULT_TIMEOUT   << "s)\n"
              << "  --max-pages N  Stop after N pages  (default: unlimited)\n"
              << "  --es-url URL   ElasticSearch URL   (default: http://localhost:9200)\n"
              << "  --es-index N   ES index name       (default: pages)\n";
}

int main(int argc, char* argv[]) {
    std::string seed_url;
    int         num_threads = DEFAULT_THREADS;
    int         timeout     = DEFAULT_TIMEOUT;
    int         max_pages   = DEFAULT_MAX_PAGES;
    std::string es_url      = "http://localhost:9200";
    std::string es_index    = "pages";

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "--threads" || arg == "--timeout" || arg == "--max-pages" ||
             arg == "--es-url"  || arg == "--es-index") && i + 1 >= argc) {
            print_usage(argv[0]); return 1;
        }
        if      (arg == "--threads")  { num_threads = std::stoi(argv[++i]); if (num_threads < 1) { print_usage(argv[0]); return 1; } }
        else if (arg == "--timeout")  { timeout     = std::stoi(argv[++i]); if (timeout     < 1) { print_usage(argv[0]); return 1; } }
        else if (arg == "--max-pages"){ max_pages   = std::stoi(argv[++i]); if (max_pages   < 0) { print_usage(argv[0]); return 1; } }
        else if (arg == "--es-url")   { es_url   = argv[++i]; }
        else if (arg == "--es-index") { es_index = argv[++i]; }
        else if (seed_url.empty() && arg.substr(0, 2) != "--") { seed_url = arg; }
        else { print_usage(argv[0]); return 1; }
    }

    if (seed_url.empty()) { print_usage(argv[0]); return 1; }

    std::cout << "seed=" << seed_url
              << " threads=" << num_threads
              << " timeout=" << timeout << "s"
              << " max-pages=" << (max_pages > 0 ? std::to_string(max_pages) : "unlimited")
              << " es=" << es_url << "/" << es_index << "\n";

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

    std::cout << "Done. Pages crawled: " << pages_crawled.load() << "\n";

    curl_global_cleanup();
    return 0;
}
