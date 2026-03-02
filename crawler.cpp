#include <curl/curl.h>
#include <gumbo.h>
#include <iostream>
#include <string>
#include <queue>
#include <vector>
#include <set>

struct PageData {
    std::string url;
    std::string text;
    std::vector<std::string> links;
    int status_code;
};

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
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
            curl_easy_cleanup(curl);
        }
        return rb;
    }
};

// --- Gumbo ---

void extract_text(GumboNode* node, std::string& text) {
    if (node->type == GUMBO_NODE_TEXT) {
        text += node->v.text.text;
        text += " ";
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) return;

    // пропускаем script и style
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

// --- Crawler ---

void crawler(PageLoader& loader, std::queue<std::string>& q) {
    std::set<std::string> visited;

    while (!q.empty()) {
        std::string url = q.front();
        q.pop();

        if (visited.count(url)) continue;
        visited.insert(url);

        int status_code = 0;
        std::string html = loader.fetch(url, status_code);

        if (status_code != 200) {
            std::cout << "[SKIP] " << url << " (" << status_code << ")\n";
            continue;
        }

        PageData pg = parse(url, html, status_code);

        std::cout << "[OK] " << pg.url << "\n";
        std::cout << "Text: " << pg.text.substr(0, 100) << "...\n";
        std::cout << "Links found: " << pg.links.size() << "\n\n";

        for (auto& link : pg.links)
            if (!visited.count(link))
                q.push(link);
    }
}

int main() {
    std::queue<std::string> q;
    q.push("https://rau.am/");

    PageLoader loader;
    crawler(loader, q);
}
