# Parameter: Authentication

|                   | WebUI               | REST API
|:---               |:---                 |:----
| Parameter Name    | Authentication      | authmode
| Default Value     | `Disabled`          | `0`
| Input Options     | `Disabled`<br>`Enabled` | `0`<br>`1`


## Description

Select authentication mode for WebUI / REST API authentication.


| Input Option               | Description
|:---                        |:---
| `Disabled`                 | WebUI / REST API endpoint authentication disabled, anonymous access allowed
| `Enabled`                  | WebUI / REST API endpoint authentication with username and password


!!! Warning
    All data (also security related data, e.g. password) is transmitted unencrypted (HTTP only, no HTTPS).


!!! Note
    The WebUI uses a form based login (`/login` page, session cookie valid for 24 hours or until reboot).
    HTTP basic authentication (browser popup) is only available if `Basic Authentication` is enabled as well;
    it is needed by REST clients that do not keep a cookie.


!!! Note
    The access is not only granted to WebUI, but also to any REST API endpoint, 
    because it's protected with same mechanism and linked to same account. Check REST API 
    description for more details.
