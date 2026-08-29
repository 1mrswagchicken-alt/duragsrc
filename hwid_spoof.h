#pragma once
#include <string>

// Per-process HWID spoofer for Vortex only.
// Hooks registry APIs inside the injected process; no other program is affected.

// Install hooks (idempotent). Safe to call from DllMain-adjacent init.
void Spoof_Init();

// Runtime toggle
void Spoof_SetEnabled(bool on);
bool Spoof_GetEnabled();

// Generate a brand-new identity seed (persists to %APPDATA%\Durag\spoof.seed)
void Spoof_Regenerate();

// JSON status: {"status":"ok","enabled":true,"seed":"<8 hex>"}
std::string Spoof_StatusJson();
