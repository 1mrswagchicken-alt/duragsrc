#pragma once
#include <string>

// Idempotent. Creates the persistent Lua state and registers the Durag API.
void Script_Init();

// Runs Lua source through the shared state.
// Returns an IPC JSON response: {"status":"ok","msg":<output>} or {"status":"error","msg":<err>}
std::string Script_Exec(const std::string& code);
