#ifndef HTTP_AUTH_H
#define HTTP_AUTH_H

#include <esp_http_server.h>

esp_err_t handleHttpAuth(httpd_req_t *req, esp_err_t httpHandler(httpd_req_t *));
esp_err_t handleLoginPost(httpd_req_t *req);
esp_err_t handleLogoutGet(httpd_req_t *req);

#define HTTP_AUTH(httpHandlerFunc) [](httpd_req_t *req) { return handleHttpAuth(req, httpHandlerFunc); }
// Kept for handlers that still use the old name
#define HTTP_AUTH_BASIC(httpHandlerFunc) HTTP_AUTH(httpHandlerFunc)

#endif // HTTP_AUTH_H
