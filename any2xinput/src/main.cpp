// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbt.h>

#include <hidapi.h>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

const unsigned short DONGLE_VID = 0xCAFE;
const unsigned short DONGLE_PID = 0xBAFD;
const int DONGLE_INTERFACE = 1;

struct ControllerState {
    int16_t left_stick_x = 0;
    int16_t left_stick_y = 0;
    int16_t right_stick_x = 0;
    int16_t right_stick_y = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;

    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;
    bool start = false;
    bool back = false;
    bool left_stick_click = false;
    bool right_stick_click = false;
    bool left_bumper = false;
    bool right_bumper = false;
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool guide = false;

    bool operator!=(const ControllerState& other) const {
        return left_stick_x != other.left_stick_x ||
               left_stick_y != other.left_stick_y ||
               right_stick_x != other.right_stick_x ||
               right_stick_y != other.right_stick_y ||
               left_trigger != other.left_trigger ||
               right_trigger != other.right_trigger ||
               dpad_up != other.dpad_up ||
               dpad_down != other.dpad_down ||
               dpad_left != other.dpad_left ||
               dpad_right != other.dpad_right ||
               start != other.start ||
               back != other.back ||
               left_stick_click != other.left_stick_click ||
               right_stick_click != other.right_stick_click ||
               left_bumper != other.left_bumper ||
               right_bumper != other.right_bumper ||
               a != other.a ||
               b != other.b ||
               x != other.x ||
               y != other.y ||
               guide != other.guide;
    }
};

struct ControllerSettings {
    bool enabled = true;
};

struct ControllerInfo {
    SDL_Gamepad* gamepad;
    std::string name;
    std::string key;
    bool enabled;
    ControllerState current_state;
};

std::atomic<bool> is_running{ true };
std::map<SDL_JoystickID, ControllerInfo> controllers;
std::map<std::string, ControllerSettings> controller_settings;
hid_device* dongle = nullptr;
ControllerState last_sent_state;

std::mutex imgui_mutex;
std::atomic<bool> ui_frame_requested{ true };
std::atomic<bool> dongle_needs_check{ true };

std::string GetControllerKey(SDL_Gamepad* gc) {
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(gc);
    const char* serial = SDL_GetJoystickSerial(joystick);
    if (serial && strlen(serial) > 0) {
        Uint16 vid = SDL_GetJoystickVendor(joystick);
        Uint16 pid = SDL_GetJoystickProduct(joystick);
        char buf[128];
        snprintf(buf, sizeof(buf), "%04x_%04x_%s", vid, pid, serial);
        return std::string(buf);
    } else {
        SDL_GUID guid = SDL_GetJoystickGUID(joystick);
        char buf[64];
        SDL_GUIDToString(guid, buf, sizeof(buf));
        return std::string(buf);
    }
}

void LoadSettings() {
    char* prefPath = SDL_GetPrefPath("any2xinput", "any2xinput");
    if (!prefPath) {
        return;
    }
    std::string path = std::string(prefPath) + "settings.json";
    SDL_free(prefPath);

    try {
        std::ifstream file(path);
        if (file.is_open()) {
            nlohmann::json j;
            file >> j;
            for (auto& [key, value] : j.items()) {
                if (value.is_object() && value.contains("enabled") && value["enabled"].is_boolean()) {
                    controller_settings[key].enabled = value["enabled"].get<bool>();
                }
            }
        }
    } catch (...) {
        // Ignore I/O and JSON parsing errors.
    }
}

void SaveSettings() {
    char* prefPath = SDL_GetPrefPath("any2xinput", "any2xinput");
    if (!prefPath) {
        return;
    }
    std::string path = std::string(prefPath) + "settings.json";
    SDL_free(prefPath);

    try {
        nlohmann::json j = nlohmann::json::object();
        for (auto& pair : controller_settings) {
            j[pair.first] = { { "enabled", pair.second.enabled } };
        }

        std::ofstream file(path);
        if (file.is_open()) {
            file << j.dump(4);
        }
    } catch (...) {
        // Ignore I/O and JSON serialization errors.
    }
}

void try_open_dongle() {
    if (dongle) {
        return;
    }

    struct hid_device_info *devs, *cur_dev;
    devs = hid_enumerate(DONGLE_VID, DONGLE_PID);
    cur_dev = devs;

    while (cur_dev) {
        if (cur_dev->interface_number == DONGLE_INTERFACE) {
            dongle = hid_open_path(cur_dev->path);
            if (dongle) {
                hid_set_nonblocking(dongle, 1);
                break;
            }
        }
        cur_dev = cur_dev->next;
    }
    hid_free_enumeration(devs);
}

WNDPROC OriginalWndProc = nullptr;

LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DEVICECHANGE) {
        if (!dongle) {
            dongle_needs_check.store(true);
        }
    }
    return CallWindowProc(OriginalWndProc, hwnd, msg, wParam, lParam);
}

void process_dongle_input() {
    if (!dongle) {
        return;
    }
    unsigned char buf[64];
    int res;
    while ((res = hid_read(dongle, buf, sizeof(buf))) > 0) {
        // discard for now
    }
    if (res < 0) {
        hid_close(dongle);
        dongle = nullptr;
    }
}

void send_xusb_report(const ControllerState& state) {
    if (!dongle) {
        return;
    }

    unsigned char report[20] = { 0 };
    report[0] = 0x00;  // message type
    report[1] = 0x14;  // size

    if (state.dpad_up) {
        report[2] |= 0x01;
    }
    if (state.dpad_down) {
        report[2] |= 0x02;
    }
    if (state.dpad_left) {
        report[2] |= 0x04;
    }
    if (state.dpad_right) {
        report[2] |= 0x08;
    }
    if (state.start) {
        report[2] |= 0x10;
    }
    if (state.back) {
        report[2] |= 0x20;
    }
    if (state.left_stick_click) {
        report[2] |= 0x40;
    }
    if (state.right_stick_click) {
        report[2] |= 0x80;
    }

    if (state.left_bumper) {
        report[3] |= 0x01;
    }
    if (state.right_bumper) {
        report[3] |= 0x02;
    }
    if (state.guide) {
        report[3] |= 0x04;
    }
    if (state.a) {
        report[3] |= 0x10;
    }
    if (state.b) {
        report[3] |= 0x20;
    }
    if (state.x) {
        report[3] |= 0x40;
    }
    if (state.y) {
        report[3] |= 0x80;
    }

    report[4] = state.left_trigger;
    report[5] = state.right_trigger;

    auto set_le16 = [](unsigned char* ptr, int16_t val) {
        ptr[0] = val & 0xFF;
        ptr[1] = (val >> 8) & 0xFF;
    };

    set_le16(&report[6], state.left_stick_x);

    int32_t lsy_inv = -(int32_t) state.left_stick_y;
    if (lsy_inv > 32767) {
        lsy_inv = 32767;
    } else if (lsy_inv < -32768) {
        lsy_inv = -32768;
    }
    set_le16(&report[8], (int16_t) lsy_inv);

    set_le16(&report[10], state.right_stick_x);

    int32_t rsy_inv = -(int32_t) state.right_stick_y;
    if (rsy_inv > 32767) {
        rsy_inv = 32767;
    } else if (rsy_inv < -32768) {
        rsy_inv = -32768;
    }
    set_le16(&report[12], (int16_t) rsy_inv);

    unsigned char write_buf[21] = { 0 };
    write_buf[0] = 0x00;  // Report ID
    std::memcpy(&write_buf[1], report, sizeof(report));

    int res = hid_write(dongle, write_buf, sizeof(write_buf));
    if (res < 0) {
        hid_close(dongle);
        dongle = nullptr;
    }
}

int16_t clamp_axis(int32_t val) {
    if (val < -32768) {
        return -32768;
    }
    if (val > 32767) {
        return 32767;
    }
    return (int16_t) val;
}

bool poll_and_fuse() {
    ControllerState total_state;
    int32_t lsx = 0, lsy = 0, rsx = 0, rsy = 0;

    for (auto& pair : controllers) {
        ControllerInfo& info = pair.second;
        SDL_Gamepad* gc = info.gamepad;
        ControllerState c_state;

        c_state.a = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_SOUTH);
        c_state.b = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_EAST);
        c_state.x = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_WEST);
        c_state.y = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_NORTH);
        c_state.guide = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_GUIDE);
        c_state.back = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_BACK);
        c_state.start = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_START);
        c_state.left_stick_click = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        c_state.right_stick_click = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        c_state.left_bumper = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        c_state.right_bumper = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        c_state.dpad_up = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_UP);
        c_state.dpad_down = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        c_state.dpad_left = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        c_state.dpad_right = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

        int16_t l_trig = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        int16_t r_trig = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        c_state.left_trigger = (uint8_t) ((l_trig * 255) / 32767);
        c_state.right_trigger = (uint8_t) ((r_trig * 255) / 32767);

        c_state.left_stick_x = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFTX);
        c_state.left_stick_y = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFTY);
        c_state.right_stick_x = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHTX);
        c_state.right_stick_y = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHTY);

        info.current_state = c_state;

        if (!info.enabled) {
            continue;
        }

        total_state.a |= c_state.a;
        total_state.b |= c_state.b;
        total_state.x |= c_state.x;
        total_state.y |= c_state.y;
        total_state.guide |= c_state.guide;
        total_state.back |= c_state.back;
        total_state.start |= c_state.start;
        total_state.left_stick_click |= c_state.left_stick_click;
        total_state.right_stick_click |= c_state.right_stick_click;
        total_state.left_bumper |= c_state.left_bumper;
        total_state.right_bumper |= c_state.right_bumper;
        total_state.dpad_up |= c_state.dpad_up;
        total_state.dpad_down |= c_state.dpad_down;
        total_state.dpad_left |= c_state.dpad_left;
        total_state.dpad_right |= c_state.dpad_right;

        if (c_state.left_trigger > total_state.left_trigger) {
            total_state.left_trigger = c_state.left_trigger;
        }
        if (c_state.right_trigger > total_state.right_trigger) {
            total_state.right_trigger = c_state.right_trigger;
        }

        lsx += c_state.left_stick_x;
        lsy += c_state.left_stick_y;
        rsx += c_state.right_stick_x;
        rsy += c_state.right_stick_y;
    }

    total_state.left_stick_x = clamp_axis(lsx);
    total_state.left_stick_y = clamp_axis(lsy);
    total_state.right_stick_x = clamp_axis(rsx);
    total_state.right_stick_y = clamp_axis(rsy);

    if (total_state != last_sent_state) {
        send_xusb_report(total_state);
        last_sent_state = total_state;
        return true;
    }
    return false;
}

void RenderThread(SDL_GPUDevice* device, SDL_Window* window, ImGuiContext* ctx) {
    ImGui::SetCurrentContext(ctx);
    while (is_running) {
        SDL_WindowFlags window_flags = SDL_GetWindowFlags(window);
        if (window_flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) {
            SDL_Delay(16);
            continue;
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUTexture* swapchain_tex;

        // block on V-sync
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain_tex, NULL, NULL)) {
            std::lock_guard<std::mutex> lock(imgui_mutex);
            ImDrawData* draw_data = ImGui::GetDrawData();

            if (draw_data) {
                ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);
            }

            SDL_GPUColorTargetInfo color_target = {};
            color_target.texture = swapchain_tex;
            color_target.clear_color = { 0.117f, 0.117f, 0.117f, 1.0f };
            color_target.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);

            if (draw_data) {
                ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
            }

            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        ui_frame_requested.store(true);
    }
}

const char* icon_data[32] = {
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "     @@@@@            @@@@@     ",
    "    @@@@@@@@@@@@@@@@@@@@@@@@    ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@  @@@@@@@@  @@@@@@@   ",
    "   @@@@@@ @@ @@@@@@ @@ @@@@@@   ",
    "   @@@@@ @@@@ @@@@ @@@@ @@@@@   ",
    "   @@@@@@ @@ @@@@@@ @@ @@@@@@   ",
    "   @@@@@@@  @@@@@@@@  @@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@          @@@@@@@@   ",
    "   @@@@@@@            @@@@@@@   ",
    "   @@@@@@@            @@@@@@@   ",
    "   @@@@@@@            @@@@@@@   ",
    "   @@@@@@              @@@@@@   ",
    "    @@@@                @@@@    ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                "
};

SDL_Surface* CreateIconSurface(bool connected) {
    SDL_Surface* surface = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        return nullptr;
    }

    Uint32* pixels = (Uint32*) surface->pixels;
    Uint32 fill_color = SDL_MapRGBA(SDL_GetPixelFormatDetails(surface->format), NULL,
        connected ? 50 : 255,  // R
        connected ? 200 : 50,  // G
        50,                    // B
        255);                  // A
    Uint32 trans_color = SDL_MapRGBA(SDL_GetPixelFormatDetails(surface->format), NULL, 0, 0, 0, 0);

    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            if (icon_data[y][x] == '@') {
                pixels[y * 32 + x] = fill_color;
            } else {
                pixels[y * 32 + x] = trans_color;
            }
        }
    }
    return surface;
}

int main(int argc, char* argv[]) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_VIDEO)) {
        return 1;
    }

    if (hid_init() != 0) {
        SDL_Quit();
        return 1;
    }

    LoadSettings();

    SDL_Window* window = SDL_CreateWindow("any2xinput", 800, 600, SDL_WINDOW_RESIZABLE);
    if (!window) {
        return 1;
    }

    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        true, NULL);

    SDL_ClaimWindowForGPUDevice(device, window);

    SDL_SetGPUSwapchainParameters(device, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        SDL_GPU_PRESENTMODE_VSYNC);

    IMGUI_CHECKVERSION();
    ImGuiContext* imgui_ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // Disable imgui.ini creation
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLGPU(window);

    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

    HWND hwnd = (HWND) SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (hwnd) {
        OriginalWndProc = (WNDPROC) SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR) CustomWndProc);
    }

    SDL_Surface* icon_red = CreateIconSurface(false);
    SDL_Surface* icon_green = CreateIconSurface(true);

    SDL_SetWindowIcon(window, icon_red);
    bool current_icon_connected = false;

    std::thread render_thread(RenderThread, device, window, imgui_ctx);

    SDL_Event event;
    Uint64 last_dongle_check = 0;

    while (is_running) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                is_running = false;
            } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                SDL_JoystickID jid = event.gdevice.which;
                SDL_Gamepad* gc = SDL_OpenGamepad(jid);
                if (gc) {
                    if (SDL_GetGamepadVendor(gc) == DONGLE_VID && SDL_GetGamepadProduct(gc) == DONGLE_PID) {
                        SDL_CloseGamepad(gc);
                        continue;
                    }
                    const char* name = SDL_GetGamepadName(gc);
                    std::string key = GetControllerKey(gc);
                    bool enabled = true;
                    if (controller_settings.count(key)) {
                        enabled = controller_settings[key].enabled;
                    } else {
                        controller_settings[key].enabled = true;
                    }
                    controllers[jid] = { gc, name ? name : "Unknown Controller", key, enabled, ControllerState() };
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                SDL_JoystickID jid = event.gdevice.which;
                if (controllers.count(jid)) {
                    SDL_CloseGamepad(controllers[jid].gamepad);
                    controllers.erase(jid);
                }
            }
        }

        Uint64 now = SDL_GetTicks();

        if (!dongle && (now - last_dongle_check > 1000)) {
            dongle_needs_check.store(true);
            last_dongle_check = now;
        }

        if (dongle_needs_check.exchange(false)) {
            try_open_dongle();
        }

        bool is_connected = (dongle != nullptr);
        if (is_connected != current_icon_connected) {
            SDL_SetWindowIcon(window, is_connected ? icon_green : icon_red);
            current_icon_connected = is_connected;
        }

        process_dongle_input();
        bool sent_report = poll_and_fuse();

        // UI building (synchronized to RenderThread's V-sync rate)
        if (ui_frame_requested.load()) {
            std::unique_lock<std::mutex> lock(imgui_mutex, std::try_to_lock);
            if (lock.owns_lock()) {
                ui_frame_requested.store(false);

                ImGui_ImplSDLGPU3_NewFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();

                ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(viewport->Pos);
                ImGui::SetNextWindowSize(viewport->Size);

                bool window_open = true;
                ImGui::Begin("Main Window", &window_open, flags);

                if (dongle) {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Dongle: connected");
                } else {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Dongle: not detected");
                }

                ImGui::Separator();
                ImGui::Text("Connected controllers:");
                for (auto& pair : controllers) {
                    auto& ctrl = pair.second;
                    std::string label = ctrl.name + "##" + std::to_string(pair.first);
                    if (ImGui::Checkbox(label.c_str(), &ctrl.enabled)) {
                        controller_settings[ctrl.key].enabled = ctrl.enabled;
                    }
                    if (ImGui::IsItemHovered()) {
                        SDL_Joystick* joystick = SDL_GetGamepadJoystick(ctrl.gamepad);
                        const char* path = joystick ? SDL_GetJoystickPath(joystick) : nullptr;
                        ImGui::SetTooltip("%s", path ? path : "Unknown");
                    }

                    // Total width is exactly 356 pixels
                    float start_x = ImGui::GetWindowContentRegionMax().x - 356.0f;
                    ImGui::SameLine(start_x);

                    float sz = 10.0f;
                    float spacing = 2.0f;
                    float offset_y = 6.0f;

                    auto draw_btn = [&](bool pressed) {
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        p.y += offset_y;
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), pressed ? IM_COL32(255, 255, 255, 255) : IM_COL32(100, 100, 100, 255), 2.0f);
                        ImGui::Dummy(ImVec2(sz, sz));
                        ImGui::SameLine(0, spacing);
                    };

                    auto draw_axis = [&](float fraction, float width) {
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        p.y += offset_y;
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width, p.y + sz), IM_COL32(50, 50, 50, 255), 2.0f);
                        if (fraction > 0.0f) {
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + (width * fraction), p.y + sz), IM_COL32(200, 200, 200, 255), 2.0f);
                        }
                        ImGui::Dummy(ImVec2(width, sz));
                        ImGui::SameLine(0, spacing);
                    };

                    auto draw_axis_centered = [&](float fraction, float width) {
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        p.y += offset_y;
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width, p.y + sz), IM_COL32(50, 50, 50, 255), 2.0f);

                        float center = p.x + width / 2.0f;
                        float end = p.x + (width * fraction);
                        if (end < center) {
                            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(end, p.y), ImVec2(center, p.y + sz), IM_COL32(200, 200, 200, 255), 2.0f);
                        } else {
                            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(center, p.y), ImVec2(end, p.y + sz), IM_COL32(200, 200, 200, 255), 2.0f);
                        }

                        ImGui::GetWindowDrawList()->AddLine(ImVec2(center, p.y), ImVec2(center, p.y + sz), IM_COL32(255, 50, 50, 255));

                        ImGui::Dummy(ImVec2(width, sz));
                        ImGui::SameLine(0, spacing);
                    };

                    const ControllerState& s = ctrl.current_state;

                    // Face Buttons
                    draw_btn(s.a);
                    draw_btn(s.b);
                    draw_btn(s.x);
                    draw_btn(s.y);
                    ImGui::SameLine(0, 4.0f);
                    // D-Pad
                    draw_btn(s.dpad_left);
                    draw_btn(s.dpad_right);
                    draw_btn(s.dpad_up);
                    draw_btn(s.dpad_down);
                    ImGui::SameLine(0, 4.0f);
                    // Bumpers & Stick Clicks (L1, R1, L3, R3)
                    draw_btn(s.left_bumper);
                    draw_btn(s.right_bumper);
                    draw_btn(s.left_stick_click);
                    draw_btn(s.right_stick_click);
                    ImGui::SameLine(0, 4.0f);
                    // System
                    draw_btn(s.back);
                    draw_btn(s.guide);
                    draw_btn(s.start);

                    ImGui::SameLine(0, 8.0f);

                    // Triggers
                    draw_axis(s.left_trigger / 255.0f, 20.0f);
                    draw_axis(s.right_trigger / 255.0f, 20.0f);

                    ImGui::SameLine(0, 4.0f);

                    // Left Stick
                    draw_axis_centered((s.left_stick_x + 32768.0f) / 65535.0f, 24.0f);
                    draw_axis_centered((s.left_stick_y + 32768.0f) / 65535.0f, 24.0f);

                    ImGui::SameLine(0, 4.0f);

                    // Right Stick
                    draw_axis_centered((s.right_stick_x + 32768.0f) / 65535.0f, 24.0f);
                    draw_axis_centered((s.right_stick_y + 32768.0f) / 65535.0f, 24.0f);

                    ImGui::NewLine();
                }
                ImGui::End();

                ImGui::Render();
            }
        }

        if (!sent_report) {
            SDL_DelayNS(750 * 1000);  // 750 microseconds
        }
    }

    render_thread.join();

    if (icon_red) {
        SDL_DestroySurface(icon_red);
    }
    if (icon_green) {
        SDL_DestroySurface(icon_green);
    }

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (dongle) {
        hid_close(dongle);
    }
    hid_exit();

    SaveSettings();

    for (auto& pair : controllers) {
        SDL_CloseGamepad(pair.second.gamepad);
    }
    SDL_Quit();

    return 0;
}
