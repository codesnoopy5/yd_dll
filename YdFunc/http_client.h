// http_client.h
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#define HTTP_API

#ifdef __cplusplus
extern "C" {
#endif

// 前向声明结构体（不透明指针）
typedef struct HttpClient HttpClient;

// 核心API（纯C接口）
HTTP_API HttpClient* http_client_create(void);
HTTP_API void http_client_destroy(HttpClient* client);
HTTP_API char* http_client_get(HttpClient* client, const char* url);
HTTP_API char* http_client_post(HttpClient* client, const char* url, const char* json_data);
HTTP_API char* http_client_parse_json(const char* json_str);
HTTP_API void http_client_free_string(char* str);

#ifdef __cplusplus
}
#endif

#endif // HTTP_CLIENT_H
