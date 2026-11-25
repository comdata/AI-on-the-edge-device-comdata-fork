# Parameter: Basic Authentication

|                   | WebUI               | REST API
|:---               |:---                 |:----
| Parameter Name    | Basic Authentication | basicauth
| Default Value     | `Disabled`          | `false`
| Input Options     | `Disabled`<br>`Enabled` | `false`<br>`true`


## Description

The WebUI and all REST API endpoints are protected by a form based login: `GET /login` shows the login page, the
credentials are sent as a form post and the device hands back a session cookie (`HttpOnly`, `SameSite=Lax`) which is
valid for 24 hours or until the next reboot.

Enable this parameter to additionally accept the HTTP basic authentication scheme. This is required by scripts and
REST clients that cannot use the login form or keep a cookie, e.g. Home Assistant or `curl -u user:password`.


!!! Warning
    Basic authentication transmits username and password with every request, only Base64 encoded, not encrypted
    or hashed. The device serves plain HTTP (no HTTPS), so anybody on the network path can read the credentials.


!!! Note
    Keep this enabled if the Home Assistant integration polls the device; it authenticates with HTTP basic
    authentication. Disable it only when all clients use the login form.
