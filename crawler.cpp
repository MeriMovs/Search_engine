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

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static constexpr int NUM_THREADS = 4;

// ---------------------------------------------------------------------------
// PageData
// ---------------------------------------------------------------------------
struct PageData {
    std::string url;
    std::string text;
    std::vector<std::string> links;
    int status_code;
};

// ---------------------------------------------------------------------------
// PageLoader  (one instance per thread — CURL handles are not thread-safe)
// ---------------------------------------------------------------------------
class PageLoader {
public:
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
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_perform(curl);
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            status_code = static_cast<int>(code);
            curl_easy_cleanup(curl);
        }
        return rb;
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
    pg.url = url;
    pg.status_code = status_code;

    GumboOutput* output = gumbo_parse(html.c_str());
    extract_text(output->root, pg.text);
    extract_links(output->root, pg.links);
    gumbo_destroy_output(&kGumboDefaultOptions, output);

    return pg;
}

// ---------------------------------------------------------------------------
// Thread-safe work queue
// ---------------------------------------------------------------------------
class WorkQueue {
public:
    // Push a new URL; returns false if the URL was already visited.
    bool push(const std::string& url) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (visited_.count(url)) return false;
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

    // Force shutdown (e.g., on error or early termination).
    void shutdown() {
        std::lock_guard<std::mutex> lk(mtx_);
        shutdown_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    std::set<std::string> visited_;
    int in_flight_ = 0;
    bool shutdown_ = false;
};

// ---------------------------------------------------------------------------
// Console mutex — guards all std::cout output
// ---------------------------------------------------------------------------
static std::mutex cout_mtx;

// ---------------------------------------------------------------------------
// Worker thread function
// ---------------------------------------------------------------------------
void worker(WorkQueue& wq) {
    // Each thread owns its own PageLoader (and therefore its own CURL handle).
    PageLoader loader;

    while (true) {
        std::string url;
        if (!wq.pop(url)) break;   // queue drained and shut down

        int status_code = 0;
        std::string html = loader.fetch(url, status_code);

        if (status_code != 200) {
            {
                std::lock_guard<std::mutex> lk(cout_mtx);
                std::cout << "[SKIP] " << url << " (" << status_code << ")\n";
            }
            wq.done();
            continue;
        }

        PageData pg = parse(url, html, status_code);

        {
            std::lock_guard<std::mutex> lk(cout_mtx);
            std::cout << "[OK] " << pg.url << "\n";
            std::cout << "Text: "
                      << (pg.text.size() > 100 ? pg.text.substr(0, 100) : pg.text)
                      << "...\n";
            std::cout << "Links found: " << pg.links.size() << "\n\n";
        }

        // Enqueue newly discovered links.
        for (const auto& link : pg.links)
            wq.push(link);   // push() is thread-safe; silently ignores visited URLs

        wq.done();
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    // curl_global_init is NOT thread-safe; call it once before spawning threads.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    WorkQueue wq;
    wq.push("https://rau.am/");

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; ++i)
        threads.emplace_back(worker, std::ref(wq));

    for (auto& t : threads)
        t.join();

    curl_global_cleanup();
    return 0;
}
