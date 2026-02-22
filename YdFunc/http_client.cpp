#include "stdafx.h"
#include "http_client.h"
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <jansson.h>

// HttpClient结构体的C语言实现
struct HttpClient {
    CURL* curl;
    char* buffer;     // 动态分配的缓冲区
    size_t size;      // 当前数据大小
    size_t capacity;  // 缓冲区容量
};

// 写回调函数
static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t realsize = size * nmemb;
    HttpClient* client = (HttpClient*)userdata;

    // 检查是否需要扩容
    if (client->size + realsize + 1 > client->capacity) {
        size_t new_capacity = client->capacity * 2;
        if (new_capacity < client->size + realsize + 1) {
            new_capacity = client->size + realsize + 1;
        }

        char* new_buffer = (char*)realloc(client->buffer, new_capacity);
        if (!new_buffer) return 0;

        client->buffer = new_buffer;
        client->capacity = new_capacity;
    }

    // 追加数据
    memcpy(client->buffer + client->size, ptr, realsize);
    client->size += realsize;
    client->buffer[client->size] = '\0';

    return realsize;
}

HTTP_API HttpClient* http_client_create(void) {
    HttpClient* client = (HttpClient*)malloc(sizeof(HttpClient));
    if (!client) return NULL;

    // 初始化成员
    client->curl = curl_easy_init();
    client->buffer = NULL;
    client->size = 0;
    client->capacity = 0;

    // 初始化缓冲区(初始大小1KB)
    client->capacity = 1024;
    client->buffer = (char*)malloc(client->capacity);
    if (!client->buffer || !client->curl) {
        if (client->buffer) free(client->buffer);
        if (client->curl) curl_easy_cleanup(client->curl);
        free(client);
        return NULL;
    }

    // 设置默认HTTP头
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, headers);

    return client;
}

HTTP_API void http_client_destroy(HttpClient* client) {
    if (!client) return;

    if (client->curl) {
        curl_easy_cleanup(client->curl);
    }

    if (client->buffer) {
        free(client->buffer);
    }

    free(client);
}

HTTP_API char* http_client_get(HttpClient* client, const char* url) {
    if (!client || !url) return NULL;

    // 重置缓冲区
    client->size = 0;
    client->buffer[0] = '\0';

    curl_easy_setopt(client->curl, CURLOPT_URL, url);
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, client);

    if (curl_easy_perform(client->curl) != CURLE_OK) {
        return NULL;
    }

    // 返回数据的副本(由调用者负责释放)
    char* result = (char*)malloc(client->size + 1);
    if (result) {
        memcpy(result, client->buffer, client->size + 1);
    }
    return result;
}

HTTP_API char* http_client_post(HttpClient* client, const char* url, const char* json_data) {
    if (!client || !url || !json_data) return NULL;

    // 重置缓冲区
    client->size = 0;
    client->buffer[0] = '\0';

    curl_easy_setopt(client->curl, CURLOPT_URL, url);
    curl_easy_setopt(client->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS, json_data);
    curl_easy_setopt(client->curl, CURLOPT_POSTFIELDSIZE, strlen(json_data));
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, client);

    if (curl_easy_perform(client->curl) != CURLE_OK) {
        return NULL;
    }

    // 返回数据的副本(由调用者负责释放)
    char* result = (char*)malloc(client->size + 1);
    if (result) {
        memcpy(result, client->buffer, client->size + 1);
    }
    return result;
}

HTTP_API char* http_client_parse_json(const char* json_str) {
    if (!json_str) return NULL;

    json_error_t error;
    json_t* root = json_loads(json_str, 0, &error);
    if (!root) return NULL;

    char* result = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    json_decref(root);

    return result;
}

HTTP_API void http_client_free_string(char* str) {
    free(str);
}
