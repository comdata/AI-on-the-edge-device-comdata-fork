#include "http_auth.h"
#include "../../include/defines.h"

#include <esp_tls_crypto.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "configClass.h"
#include "ClassLogFile.h"
#include "system.h"

static const char *TAG = "HTTPAUTH";

static const char *SESSION_COOKIE_NAME = "AIOTE_SESSION";
// Session lives in RAM, so a reboot logs everybody out. One session is shared by all clients
// shortcut: single session token, a second login logs the first client out; upgrade to a small token table if needed
static constexpr int64_t SESSION_TIMEOUT_US = 24LL * 60 * 60 * 1000000; // 24 hours
static char sessionToken[33] = {};                                       // 32 hex chars + '\0', empty = no session
static int64_t sessionCreatedUs = 0;


static bool secretEquals(const std::string &expected, const std::string &given)
{
    if (expected.size() != given.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < expected.size(); i++) {
        diff |= (unsigned char)(expected[i] ^ given[i]);
    }
    return diff == 0;
}


static bool sessionActive()
{
    return sessionToken[0] != '\0' && (esp_timer_get_time() - sessionCreatedUs) < SESSION_TIMEOUT_US;
}


static void createSession()
{
    constexpr char hexDigits[] = "0123456789abcdef";
    const uint32_t randomValues[4] = {esp_random(), esp_random(), esp_random(), esp_random()};
    for (size_t i = 0; i < sizeof(randomValues) / sizeof(randomValues[0]); i++) {
        for (size_t nibble = 0; nibble < 8; nibble++) {
            sessionToken[i * 8 + nibble] = hexDigits[(randomValues[i] >> (nibble * 4)) & 0x0F];
        }
    }
    sessionToken[32] = '\0';
    sessionCreatedUs = esp_timer_get_time();
}


static void clearSession()
{
    sessionToken[0] = '\0';
}


static std::string urlDecode(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') {
            out += ' ';
        }
        else if (in[i] == '%' && i + 2 < in.size()) {
            const auto hexValue = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = hexValue(in[i + 1]);
            const int low = hexValue(in[i + 2]);
            if (high < 0 || low < 0) {
                out += in[i];
            }
            else {
                out += (char)((high << 4) | low);
                i += 2;
            }
        }
        else {
            out += in[i];
        }
    }
    return out;
}


static std::string getFormField(const char *body, const char *name)
{
    const std::string fieldName(name);
    const char *pos = body;

    while (pos != nullptr && *pos != '\0') {
        const char *end = strchr(pos, '&');
        const std::string pair = end ? std::string(pos, end - pos) : std::string(pos);

        const size_t separator = pair.find('=');
        const std::string key = urlDecode(pair.substr(0, separator));
        if (key == fieldName) {
            return separator == std::string::npos ? std::string() : urlDecode(pair.substr(separator + 1));
        }

        pos = end ? end + 1 : nullptr;
    }
    return std::string();
}


static bool getCookieValue(httpd_req_t *req, const char *name, char *value, size_t valueSize)
{
    constexpr size_t MAX_COOKIE_HEADER_LEN = 512;
    char cookieHeader[MAX_COOKIE_HEADER_LEN + 1] = {};
    const size_t cookieLen = httpd_req_get_hdr_value_len(req, "Cookie");

    if (cookieLen == 0 || cookieLen > MAX_COOKIE_HEADER_LEN) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookieHeader, sizeof(cookieHeader)) != ESP_OK) {
        return false;
    }

    const std::string nameWithSeparator = std::string(name) + "=";
    const std::string header(cookieHeader);
    size_t searchFrom = 0;

    while (searchFrom < header.size()) {
        size_t start = searchFrom;
        if (start > 0) {
            start += 1; // skip the ';' of the previous cookie
            while (start < header.size() && header[start] == ' ') {
                start++;
            }
        }

        const size_t end = header.find(';', start);
        const std::string cookie = header.substr(start, end == std::string::npos ? std::string::npos : end - start);

        if (cookie.compare(0, nameWithSeparator.size(), nameWithSeparator) == 0) {
            const std::string cookieValue = cookie.substr(nameWithSeparator.size());
            if (cookieValue.empty() || cookieValue.size() >= valueSize) {
                return false;
            }
            strlcpy(value, cookieValue.c_str(), valueSize);
            return true;
        }

        if (end == std::string::npos) {
            break;
        }
        searchFrom = end;
    }
    return false;
}


static bool hasValidSessionCookie(httpd_req_t *req)
{
    char token[34] = {};
    if (!getCookieValue(req, SESSION_COOKIE_NAME, token, sizeof(token))) {
        return false;
    }
    return sessionActive() && secretEquals(std::string(sessionToken), std::string(token));
}


static bool isBrowserPageRequest(httpd_req_t *req)
{
    constexpr size_t MAX_ACCEPT_LEN = 256;
    char accept[MAX_ACCEPT_LEN + 1] = {};
    const size_t acceptLen = httpd_req_get_hdr_value_len(req, "Accept");

    if (acceptLen == 0 || acceptLen > MAX_ACCEPT_LEN) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept)) != ESP_OK) {
        return false;
    }
    return strstr(accept, "text/html") != nullptr;
}


static esp_err_t redirectToLogin(httpd_req_t *req)
{
    // Big enough for the maximum request URI (HTTPD_MAX_URI_LEN) plus the prefix
    char location[640] = "/login";

    // Keep the original page so the login can return to it, but only for short local paths
    if (req->uri[0] == '/' && req->uri[1] != '/' && strlen(req->uri) < 140) {
        snprintf(location, sizeof(location), "/login?next=%s", req->uri);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}


static esp_err_t sendAuthRequired(httpd_req_t *req, bool basicAuthEnabled, const char *message)
{
    if (basicAuthEnabled) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AI-on-the-Edge\"");
    }
    return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, message);
}


esp_err_t handleHttpAuth(httpd_req_t *req, esp_err_t httpHandler(httpd_req_t *))
{
    const auto *config = ConfigClass::getInstance()->get();
    const auto &auth = config->sectionWebUi.httpAuth;
    const bool authActive = auth.authMode != HTTP_AUTH_DISABLED && getSystemStatus() == 0;

    // CORS handling (permissive cross-origin requests by design)
    char originBuf[128] = {};
    const size_t originLen = httpd_req_get_hdr_value_len(req, "Origin");

    if (originLen > 0 && originLen < sizeof(originBuf) &&
        httpd_req_get_hdr_value_str(req, "Origin", originBuf, sizeof(originBuf)) == ESP_OK) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", originBuf);
        httpd_resp_set_hdr(req, "Vary", "Origin");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Credentials", "true");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", authActive ? "Content-Type, Authorization" : "Content-Type");
    }

    // CORS preflight
    if (req->method == HTTP_OPTIONS) {
        return httpd_resp_send(req, NULL, 0);
    }

    // Skip authorization check if
    // 1. HTTP authorization is disabled
    // 2. At critical system state (reduced web interface)
    if (!authActive) {
        return httpHandler(req);
    }

    // Login page itself (GET) must stay reachable without a session
    if (strncmp(req->uri, "/login", 6) == 0 && (req->uri[6] == '\0' || req->uri[6] == '?')) {
        return httpHandler(req);
    }

    if (hasValidSessionCookie(req)) {
        return httpHandler(req);
    }

    // Optional basic authentication, keeps REST clients (e.g. Home Assistant) working
    if (auth.basicAuth) {
        constexpr size_t MAX_AUTH_HEADER_LEN = 128;
        char authHeader[MAX_AUTH_HEADER_LEN + 1] = {};
        const size_t authLen = httpd_req_get_hdr_value_len(req, "Authorization");

        if (authLen > 0 && authLen <= MAX_AUTH_HEADER_LEN &&
            httpd_req_get_hdr_value_str(req, "Authorization", authHeader, sizeof(authHeader)) == ESP_OK) {
            const std::string userAuth = auth.username + ":" + auth.password;
            size_t expectedLen = 0;
            esp_crypto_base64_encode(NULL, 0, &expectedLen, (const unsigned char *)userAuth.c_str(), userAuth.length());

            std::string expectedAuth(6 + expectedLen, '\0');
            strcpy(&expectedAuth[0], "Basic ");
            size_t encodedLen = 0;
            esp_crypto_base64_encode((unsigned char *)&expectedAuth[6], expectedLen, &encodedLen,
                                     (const unsigned char *)userAuth.c_str(), userAuth.length());
            expectedAuth.resize(6 + encodedLen);

            if (secretEquals(expectedAuth, std::string(authHeader))) {
                return httpHandler(req);
            }
            LogFile.writeToFile(ESP_LOG_WARN, TAG, "Access denied. Wrong username or password");
            return sendAuthRequired(req, auth.basicAuth, "Access denied");
        }
    }

    if (isBrowserPageRequest(req)) {
        LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "Access denied. Redirecting to login page");
        return redirectToLogin(req);
    }

    LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "Access denied. No valid session");
    return sendAuthRequired(req, auth.basicAuth, "Access denied");
}


esp_err_t handleLoginPost(httpd_req_t *req)
{
    const auto *config = ConfigClass::getInstance()->get();
    const auto &auth = config->sectionWebUi.httpAuth;

    constexpr size_t MAX_BODY_LEN = 512;
    char body[MAX_BODY_LEN + 1] = {};

    if (req->content_len == 0 || req->content_len >= MAX_BODY_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    std::string next = getFormField(body, "next");
    // Only local paths, no protocol relative target and no header injection via CR/LF
    if (next.empty() || next[0] != '/' || (next.size() > 1 && next[1] == '/') || next.find_first_of("\r\n") != std::string::npos) {
        next = "/";
    }

    const bool authModeActive = auth.authMode != HTTP_AUTH_DISABLED;
    const bool credentialsValid = authModeActive && secretEquals(auth.username, getFormField(body, "username")) &&
                                  secretEquals(auth.password, getFormField(body, "password"));

    char location[640];

    if (!authModeActive) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", next.c_str());
        return httpd_resp_send(req, NULL, 0);
    }

    if (!credentialsValid) {
        LogFile.writeToFile(ESP_LOG_WARN, TAG, "Login failed. Wrong username or password");
        snprintf(location, sizeof(location), "/login?err=1&next=%s", next.c_str());
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", location);
        return httpd_resp_send(req, NULL, 0);
    }

    createSession();
    LogFile.writeToFile(ESP_LOG_INFO, TAG, "Login successful");

    char cookie[160];
    snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d", SESSION_COOKIE_NAME, sessionToken,
             (int)(SESSION_TIMEOUT_US / 1000000));

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Location", next.c_str());
    return httpd_resp_send(req, NULL, 0);
}


esp_err_t handleLogoutGet(httpd_req_t *req)
{
    clearSession();

    char cookie[96];
    snprintf(cookie, sizeof(cookie), "%s=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0", SESSION_COOKIE_NAME);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Location", "/login");
    return httpd_resp_send(req, NULL, 0);
}
