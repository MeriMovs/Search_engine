#include <curl/curl.h>
#include <iostream>
#include <string>
#include <queue>

struct PageData {
    std::string link;
    std::string data;
    int status_code;
};

class PageLoader {
public:
    std::string rb;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* u) {
        ((std::string*)u)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    PageData save(std::string url) {
        CURL *curl = curl_easy_init();
        PageData pg;
        if (curl) {
            CURLcode result;
            int r_code;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, PageLoader::WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3);

            result = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r_code);

            std::cout << "res: " << r_code << "\n\n";

            if (r_code == 200) {
                std::cout << "Request successful!" << std::endl;
            } else if (r_code >= 400 && r_code < 500) {
                std::cout << "Client error (4xx)." << std::endl;
            } else if (r_code >= 500 && r_code < 600) {
                std::cout << "Server error (5xx)." << std::endl;
            }

            pg.link = url;
            pg.data = rb;
            pg.status_code = r_code;
        }
        return pg;
    }
};

void crawler(PageLoader& obj, std::queue<std::string>& q) {
    while (!q.empty()) {
        obj.save(q.front());
        q.pop();
    }
}

int main(void) {
    std::queue<std::string> q;
    q.push("https://www.youtube.com/");
    q.push("https://www.facebook.com/");
    q.push("https://www.instagram.com/");
    PageLoader obj;
    crawler(obj, q);
}
