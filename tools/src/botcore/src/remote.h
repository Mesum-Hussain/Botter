#ifndef BC_REMOTE_H
#define BC_REMOTE_H

#include "cJSON.h"

/*
 * What an agent reaches over HTTPS, from its config.json. Nothing is started on this
 * machine and nothing listens: every call is one request made by botcore's own client.
 *
 *   "mcp":    {"docs": {"url": "https://host/mcp", "token": "DOCS_TOKEN"}}
 *             MCP servers (Streamable HTTP; stateless 2026-07-28, or the older session
 *             handshake when the server asks for it). Their tools join the agent's own as
 *             <server>_<tool>.
 *   "agents": {"billing": {"url": "https://host", "token": "BILLING_TOKEN", "description": "..."}}
 *             Remote agents (A2A 1.0 JSON-RPC; 0.3 when the card says so), delegated to like
 *             sub-agents.
 *
 * "token" names a line of the .env file in the working directory (NAME=value): the bearer
 * token. Plain http:// is accepted for this machine only (localhost).
 */

/* NAME's value from the agent's sealed vault if it has one, else from ./.env; or NULL. */
const char *env_get(const char *name);

/* MCP: list the servers' tools once at start-up (prints one line per server). */
void  mcp_init(void);
void  mcp_schema_append(cJSON *arr);
/* Run an MCP tool; malloc'd result text ("ERROR: ..." on failure), or NULL if name is not one. */
char *mcp_call(const char *name, const char *args_json);
/* The server an MCP tool belongs to (for the status line), or NULL. */
const char *mcp_server(const char *name);

/* A2A: the remote agents of config.json. */
int         a2a_count(void);
const char *a2a_name(int i);
const char *a2a_desc(int i);
/* Send one task and wait for the answer. 0 ok, 1 interrupted, -1 failed (*reply = "ERROR: ..."), -2 no such agent. */
int         a2a_call(const char *name, const char *task, char **reply);

#endif
