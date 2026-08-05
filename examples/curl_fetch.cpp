// curl_fetch — fetch a URL through libcurl driven by a coop cooperator.
//
// Demonstrates the curl-multi-under-coop bridge (examples/curl_coop.h): curl does all the protocol
// work, coop provides the io_uring event loop. Because curl owns the protocol, this transparently
// speaks HTTP/2 when the server offers it — coop implements none of it.
//
//   ./curl_fetch https://www.google.com/
//   ./curl_fetch http://127.0.0.1:8080/plaintext
//
#include <cstdio>
#include <cstring>

#include "coop/cooperator.h"
#include "coop/cooperator_configuration.h"
#include "coop/thread.h"

#include "curl_coop.h"

using namespace coop;

static size_t CountBody(char*, size_t size, size_t nmemb, void* userp)
{
    *static_cast<size_t*>(userp) += size * nmemb;
    return size * nmemb;
}

static const char* HttpVersionName(long v)
{
    switch (v)
    {
        case CURL_HTTP_VERSION_1_0: return "HTTP/1.0";
        case CURL_HTTP_VERSION_1_1: return "HTTP/1.1";
        case CURL_HTTP_VERSION_2_0: return "HTTP/2";
#ifdef CURL_HTTP_VERSION_3
        case CURL_HTTP_VERSION_3:   return "HTTP/3";
#endif
        default:                    return "HTTP/?";
    }
}

int main(int argc, char* argv[])
{
    const char* url = (argc > 1) ? argv[1] : "http://127.0.0.1:8080/plaintext";

    curl_global_init(CURL_GLOBAL_DEFAULT);

    Cooperator co(s_defaultCooperatorConfiguration);
    Thread thread(&co);

    co.SubmitSync([&](Context* ctx)
    {
        curlcoop::Driver driver(&co);

        CURL* easy = curl_easy_init();
        size_t bodyBytes = 0;

        curl_easy_setopt(easy, CURLOPT_URL, url);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CountBody);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &bodyBytes);
        // Prefer HTTP/2 over TLS; curl negotiates down if the server won't.
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");   // allow gzip/br

        long status = 0;
        CURLcode rc = driver.Perform(ctx, easy, &status);

        if (rc != CURLE_OK)
        {
            fprintf(stderr, "fetch failed: %s\n", curl_easy_strerror(rc));
        }
        else
        {
            long ver = 0;
            curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &ver);
            char* eff = nullptr;
            curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff);
            printf("%s  %ld  %s  %zu bytes\n",
                   HttpVersionName(ver), status, eff ? eff : url, bodyBytes);
        }

        curl_easy_cleanup(easy);
        co.Shutdown();
    });

    curl_global_cleanup();
    return 0;
}
