# Third-party components

Compiled into botcore (and so into botter and every agent it builds):

| Component | Where | License |
|---|---|---|
| cJSON | tools/src/botcore/lib/cjson | MIT (see its source headers) |
| BearSSL (TLS 1.2 client), commit 7bea48e5 | tools/src/botcore/lib/bearssl | MIT, Copyright (c) 2016 Thomas Pornin (lib/bearssl/LICENSE.txt) |
| Lua 5.4.7 (core and the base, string, table, math and utf8 libraries) | tools/src/botcore/lib/lua | MIT, Copyright (C) 1994-2024 Lua.org, PUC-Rio (lib/lua/LICENSE) |
| Mozilla CA certificate data (trust anchors) | tools/src/botcore/lib/ca/ca_anchors.h | MPL-2.0, https://mozilla.org/MPL/2.0/ ; source: https://curl.se/ca/cacert.pem (Mozilla data as of 2026-09-25) |

The CA data is used unmodified (converted to BearSSL's trust-anchor format); see lib/ca/README.md to regenerate it.
