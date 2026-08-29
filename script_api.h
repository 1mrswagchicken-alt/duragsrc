#pragma once
// Host API exposed by durag_executor.cpp to the script runtime (durag_script.cpp).
// Function pointers are filled in during DLL init before any script runs.

struct DuragHostApi {
    // Player transform. Returns false when no player base is captured yet.
    bool (*get_self_pos)(float* x, float* y, float* z);
    bool (*set_self_pos)(float x, float y, float z);
    float (*get_self_yaw)();

    // Feature toggles: "fly" | "noclip" | "speed" | "size"
    void (*set_flag)(const char* name, bool on);
    bool (*get_flag)(const char* name);

    // Tunables: "fly_speed" | "speed_mult" | "size_scale" | "orbit_radius" | "orbit_speed"
    double (*get_mult)(const char* name);
    void (*set_mult)(const char* name, double v);

    void (*stop_all)();

    // Clients
    uint32_t (*self_pid)();
    int (*list_client_pids)(uint32_t* out, int max);
};

extern DuragHostApi g_host;
