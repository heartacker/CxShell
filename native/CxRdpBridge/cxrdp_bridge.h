#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(CX_RDP_BRIDGE_EXPORTS)
#define CX_RDP_API __declspec(dllexport)
#else
#define CX_RDP_API __declspec(dllimport)
#endif
#else
#define CX_RDP_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cxrdp_frame_callback)(void* user_data, int width, int height, int stride, const uint8_t* bgra_pixels);
typedef void (*cxrdp_frame_rect_callback)(
    void* user_data,
    int width,
    int height,
    int stride,
    const uint8_t* bgra_pixels,
    int dirty_x,
    int dirty_y,
    int dirty_w,
    int dirty_h);
typedef void (*cxrdp_status_callback)(void* user_data, const char* message);
typedef void (*cxrdp_disconnect_callback)(void* user_data);
typedef void (*cxrdp_clipboard_text_callback)(void* user_data, const char* text);
typedef void (*cxrdp_clipboard_image_callback)(void* user_data, uint32_t image_format, const uint8_t* data, uint32_t length);
typedef void (*cxrdp_clipboard_files_callback)(void* user_data, const char* file_paths_newline_separated);
typedef void (*cxrdp_pointer_callback)(
    void* user_data,
    int kind,
    int width,
    int height,
    int hot_x,
    int hot_y,
    const uint8_t* bgra_pixels);

#define CX_RDP_BRIDGE_API_VERSION 15u
#define CX_RDP_CAPABILITY_CLIPBOARD 0x00000001u
#define CX_RDP_CAPABILITY_DRIVE_REDIRECTION 0x00000002u
#define CX_RDP_CAPABILITY_AUDIO_PLAYBACK 0x00000004u
#define CX_RDP_CAPABILITY_MICROPHONE 0x00000008u
#define CX_RDP_CAPABILITY_KEYBOARD_HOOK 0x00000010u
#define CX_RDP_CAPABILITY_DISPLAY_SCALE 0x00000020u
#define CX_RDP_CAPABILITY_SECURITY_MODE 0x00000040u
#define CX_RDP_CAPABILITY_CLIPBOARD_IMAGE 0x00000080u
#define CX_RDP_CAPABILITY_CLIPBOARD_FILES 0x00000100u
#define CX_RDP_CAPABILITY_DYNAMIC_DISPLAY 0x00000200u
#define CX_RDP_CAPABILITY_POINTER_SHAPE 0x00000400u
#define CX_RDP_CAPABILITY_DIRTY_RECT 0x00000800u
#define CX_RDP_CAPABILITY_CLIPBOARD_RICH_TEXT 0x00001000u
#define CX_RDP_CAPABILITY_GRAPHICS_OPTIONS 0x00002000u
#define CX_RDP_CAPABILITY_SYNC_LOCK_KEYS 0x00004000u
#define CX_RDP_CAPABILITY_CLIPBOARD_LOCK 0x00008000u

#define CX_RDP_POINTER_DEFAULT 0
#define CX_RDP_POINTER_NULL 1
#define CX_RDP_POINTER_CUSTOM 2

#define CX_RDP_CLIPBOARD_IMAGE_PNG 1u
#define CX_RDP_CLIPBOARD_IMAGE_DIB 2u

#define CX_RDP_AUDIO_PLAY_LOCAL 0
#define CX_RDP_AUDIO_PLAY_REMOTE 1
#define CX_RDP_AUDIO_DO_NOT_PLAY 2

CX_RDP_API uint32_t cxrdp_get_api_version(void);
CX_RDP_API uint32_t cxrdp_get_capabilities(void);

CX_RDP_API void* cxrdp_create(void);
CX_RDP_API void cxrdp_destroy(void* handle);
CX_RDP_API void cxrdp_set_callbacks(
    void* handle,
    cxrdp_frame_callback frame_callback,
    cxrdp_status_callback status_callback,
    cxrdp_disconnect_callback disconnect_callback,
    void* user_data);
CX_RDP_API int cxrdp_connect(
    void* handle,
    const char* host,
    int port,
    const char* username,
    const char* password,
    int width,
    int height,
    int color_depth);
CX_RDP_API void cxrdp_disconnect(void* handle);
CX_RDP_API void cxrdp_send_pointer(void* handle, uint16_t flags, uint16_t x, uint16_t y);
CX_RDP_API void cxrdp_send_key(void* handle, uint32_t key, int down);
CX_RDP_API void cxrdp_send_unicode_key(void* handle, uint16_t code, int down);
CX_RDP_API void cxrdp_sync_lock_keys(void* handle, uint32_t flags);
CX_RDP_API void cxrdp_set_clipboard_callback(void* handle, cxrdp_clipboard_text_callback clipboard_text_callback);
CX_RDP_API void cxrdp_set_clipboard_image_callback(void* handle, cxrdp_clipboard_image_callback clipboard_image_callback);
CX_RDP_API void cxrdp_set_clipboard_files_callback(void* handle, cxrdp_clipboard_files_callback clipboard_files_callback);
CX_RDP_API void cxrdp_set_pointer_callback(void* handle, cxrdp_pointer_callback pointer_callback);
CX_RDP_API void cxrdp_set_frame_rect_callback(void* handle, cxrdp_frame_rect_callback frame_rect_callback);
CX_RDP_API void cxrdp_set_clipboard_text(void* handle, const char* text);
CX_RDP_API void cxrdp_set_clipboard_rich_text(
    void* handle,
    const char* plain_text,
    const uint8_t* html_data,
    uint32_t html_length,
    const uint8_t* rtf_data,
    uint32_t rtf_length);
CX_RDP_API void cxrdp_set_clipboard_image(
    void* handle,
    const uint8_t* png_data,
    uint32_t png_length,
    const uint8_t* dib_data,
    uint32_t dib_length);
CX_RDP_API void cxrdp_set_clipboard_files(void* handle, const char* file_paths_newline_separated);
CX_RDP_API int cxrdp_set_drive_redirection(
    void* handle,
    int enabled,
    const char* drive_name,
    const char* local_path);
CX_RDP_API int cxrdp_set_audio_redirection(
    void* handle,
    int playback_mode,
    int microphone_enabled);
CX_RDP_API int cxrdp_set_keyboard_options(
    void* handle,
    int apply_key_combinations_remotely);
CX_RDP_API int cxrdp_set_display_scale(
    void* handle,
    uint32_t desktop_scale_factor,
    uint32_t device_scale_factor);
CX_RDP_API int cxrdp_resize_display(
    void* handle,
    int width,
    int height,
    uint32_t desktop_scale_factor,
    uint32_t device_scale_factor);
CX_RDP_API int cxrdp_set_security_mode(
    void* handle,
    const char* preferred_mode,
    int allow_fallback);
CX_RDP_API int cxrdp_set_graphics_options(
    void* handle,
    int enable_graphics_acceleration);
CX_RDP_API int cxrdp_set_clipboard_lock_enabled(
    void* handle,
    int enabled);
CX_RDP_API const char* cxrdp_get_last_error(void* handle);

#ifdef __cplusplus
}
#endif
