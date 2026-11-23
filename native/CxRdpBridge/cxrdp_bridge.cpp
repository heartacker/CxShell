#include "cxrdp_bridge.h"

#include <atomic>
#include <chrono>
#include <codecvt>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <openssl/provider.h>
#include <openssl/err.h>

extern "C" {
#include <freerdp/freerdp.h>
#include <freerdp/addin.h>
#include <freerdp/config.h>
#include <freerdp/version.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/disp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/audin.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/disp.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpdr.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/input.h>
#include <freerdp/event.h>
#include <freerdp/error.h>
#include <freerdp/settings.h>
#include <freerdp/settings_keys.h>
#include <freerdp/settings_types.h>
#include <winpr/synch.h>
#include <winpr/user.h>
#include <winpr/winsock.h>
}

namespace
{
struct LocalClipboardFileEntry
{
    std::string relative_name;
    std::filesystem::path full_path;
    bool is_directory = false;
    uint64_t file_size = 0;
};

struct RemoteClipboardFileEntry
{
    std::string relative_name;
    std::filesystem::path local_path;
    bool is_directory = false;
    uint64_t expected_size = 0;
    uint64_t received_bytes = 0;
    bool size_known = false;
};

constexpr uint32_t CX_CLIPRDR_FORMAT_PNG = 0xC001u;
constexpr uint32_t CX_CLIPRDR_FORMAT_FILE_GROUP_DESCRIPTOR_W = 0xC002u;
constexpr uint32_t CX_CLIPRDR_FILE_CHUNK_SIZE = 65536u;

struct CxRdpSession
{
    freerdp* instance = nullptr;
    std::thread worker;
    std::atomic_bool running = false;
    std::atomic_bool connected = false;
    std::mutex callback_mutex;
    std::string host;
    std::string username;
    std::string domain;
    std::string password;
    int port = 3389;
    int width = 1024;
    int height = 768;
    int color_depth = 32;
    bool wsa_started = false;
    bool openssl_providers_loaded = false;
    bool clipboard_channel_enabled = false;
    bool drive_redirection_enabled = false;
    std::string drive_name;
    std::string drive_path;
    int audio_playback_mode = CX_RDP_AUDIO_DO_NOT_PLAY;
    bool microphone_enabled = false;
    bool apply_key_combinations_remotely = true;
    uint32_t desktop_scale_factor = 100;
    uint32_t device_scale_factor = 100;
    std::string preferred_security_mode;
    bool allow_security_fallback = true;
    CliprdrClientContext* cliprdr = nullptr;
    DispClientContext* disp = nullptr;
    std::mutex disp_mutex;
    bool cliprdr_wire_active = false;
    bool cliprdr_ready = false;
    bool cliprdr_activation_pending = false;
    bool cliprdr_wait_logged = false;
    bool client_capabilities_sent = false;
    std::chrono::steady_clock::time_point cliprdr_connected_at{};
    uint32_t clipboard_requested_format = 0;
    uint32_t clipboard_requested_image_format = 0;
    bool clipboard_requested_files = false;
    uint64_t local_clipboard_revision = 0;
    uint64_t offered_clipboard_revision = 0;
    cxrdp_frame_callback frame_callback = nullptr;
    cxrdp_frame_rect_callback frame_rect_callback = nullptr;
    cxrdp_status_callback status_callback = nullptr;
    cxrdp_disconnect_callback disconnect_callback = nullptr;
    cxrdp_clipboard_text_callback clipboard_text_callback = nullptr;
    cxrdp_clipboard_image_callback clipboard_image_callback = nullptr;
    cxrdp_clipboard_files_callback clipboard_files_callback = nullptr;
    cxrdp_pointer_callback pointer_callback = nullptr;
    void* user_data = nullptr;
    std::string last_error;
    std::mutex clipboard_mutex;
    int local_clipboard_kind = 0; // 0=text, 1=image, 2=files
    std::string local_clipboard_text;
    std::vector<uint8_t> local_clipboard_png;
    std::vector<uint8_t> local_clipboard_dib;
    std::vector<LocalClipboardFileEntry> local_clipboard_files;
    std::vector<RemoteClipboardFileEntry> remote_clipboard_files;
    std::vector<std::string> remote_clipboard_top_level_paths;
    size_t remote_file_download_index = 0;
    uint32_t remote_file_stream_id = 1;
    bool remote_file_requesting_size = false;
};

struct CxRdpContext
{
    rdpContext context;
    CxRdpSession* session = nullptr;
};

struct SecurityProfile
{
    const char* name;
    bool nla;
    bool tls;
    bool rdp;
    bool negotiate;
    bool useRdpSecurityLayer;
};

struct ParsedCredentials
{
    std::string username;
    std::string domain;
};

std::once_flag addin_provider_once;

BOOL cx_pre_connect(freerdp* instance);
BOOL cx_post_connect(freerdp* instance);
void cx_post_disconnect(freerdp* instance);
BOOL cx_load_channels(freerdp* instance);
BOOL cx_authenticate(freerdp* instance, char** username, char** password, char** domain, rdp_auth_reason reason);
DWORD cx_verify_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* subject,
    const char* issuer,
    const char* fingerprint,
    DWORD flags);
DWORD cx_verify_changed_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* subject,
    const char* issuer,
    const char* new_fingerprint,
    const char* old_subject,
    const char* old_issuer,
    const char* old_fingerprint,
    DWORD flags);
int cx_verify_x509_certificate(
    freerdp* instance,
    const BYTE* data,
    size_t length,
    const char* hostname,
    UINT16 port,
    DWORD flags);
int cx_logon_error(freerdp* instance, UINT32 data, UINT32 type);
void cx_channel_connected(void* context, const ChannelConnectedEventArgs* e);
void cx_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e);
UINT send_client_capabilities(CliprdrClientContext* cliprdr);
UINT send_client_format_list(CliprdrClientContext* cliprdr);
void log_cliprdr_diagnostics(CxRdpSession* session, const char* reason);
void mark_cliprdr_confirmed(CxRdpSession* session, const char* reason);
UINT activate_pending_cliprdr(CxRdpSession* session);
UINT flush_local_clipboard_format_list(CxRdpSession* session);
void set_error(CxRdpSession* session, const std::string& message);

CxRdpSession* get_session(rdpContext* context)
{
    if (!context)
        return nullptr;

    return reinterpret_cast<CxRdpContext*>(context)->session;
}

std::string trim_copy(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

ParsedCredentials parse_rdp_credentials(const char* rawUsername)
{
    ParsedCredentials result;
    result.username = trim_copy(rawUsername ? rawUsername : "");

    const auto separator = result.username.find('\\');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= result.username.size())
        return result;

    result.domain = result.username.substr(0, separator);
    result.username = result.username.substr(separator + 1);
    return result;
}

rdpSettings* get_settings(CxRdpSession* session)
{
    if (!session || !session->instance || !session->instance->context)
        return nullptr;

    return session->instance->context->settings;
}

bool initialize_instance(CxRdpSession* session)
{
    if (!session)
        return false;

    session->instance = freerdp_new();
    if (!session->instance)
        return false;

    session->instance->PreConnect = cx_pre_connect;
    session->instance->PostConnect = cx_post_connect;
    session->instance->PostDisconnect = cx_post_disconnect;
    session->instance->LoadChannels = cx_load_channels;
    session->instance->AuthenticateEx = cx_authenticate;
    session->instance->VerifyCertificateEx = cx_verify_certificate;
    session->instance->VerifyChangedCertificateEx = cx_verify_changed_certificate;
    session->instance->VerifyX509Certificate = cx_verify_x509_certificate;
    session->instance->LogonErrorInfo = cx_logon_error;
    session->instance->ContextSize = sizeof(CxRdpContext);

    if (!freerdp_context_new(session->instance))
    {
        freerdp_free(session->instance);
        session->instance = nullptr;
        return false;
    }

    reinterpret_cast<CxRdpContext*>(session->instance->context)->session = session;
    return true;
}

bool configure_drive_redirection(CxRdpSession* session, rdpSettings* settings)
{
    if (!session || !settings || !session->drive_redirection_enabled)
        return true;

    const char* arguments[] = { session->drive_name.c_str(), session->drive_path.c_str() };
    auto* device = freerdp_device_new(RDPDR_DTYP_FILESYSTEM, 2, arguments);
    if (!device)
    {
        set_error(session, "Failed to create the RDP redirected drive.");
        return false;
    }

    if (!freerdp_device_collection_add(settings, device))
    {
        freerdp_device_free(device);
        set_error(session, "Failed to add the RDP redirected drive to FreeRDP settings.");
        return false;
    }

    freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, TRUE);
    return true;
}

constexpr bool has_native_audio_backend()
{
#if defined(WITH_WINMM) || defined(WITH_MACAUDIO) || defined(WITH_ALSA) || defined(WITH_PULSE) || defined(WITH_OSS)
    return true;
#else
    return false;
#endif
}

bool configure_audio_redirection(CxRdpSession* session, rdpSettings* settings)
{
    if (!session || !settings)
        return false;

    const bool playLocal = session->audio_playback_mode == CX_RDP_AUDIO_PLAY_LOCAL;
    const bool playRemote = session->audio_playback_mode == CX_RDP_AUDIO_PLAY_REMOTE;
    freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, playLocal ? TRUE : FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteConsoleAudio, playRemote ? TRUE : FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, session->microphone_enabled ? TRUE : FALSE);

    if (playLocal)
    {
        const char* soundChannel[] = { RDPSND_CHANNEL_NAME };
        if (!freerdp_client_add_static_channel(settings, 1, soundChannel))
        {
            set_error(session, "Failed to configure the RDP sound channel.");
            return false;
        }
    }

    if (session->microphone_enabled)
    {
        const char* microphoneChannel[] = { AUDIN_CHANNEL_NAME };
        if (!freerdp_client_add_dynamic_channel(settings, 1, microphoneChannel))
        {
            set_error(session, "Failed to configure the RDP microphone channel.");
            return false;
        }
    }

    return true;
}

void free_instance(CxRdpSession* session)
{
    if (!session || !session->instance)
        return;

    freerdp_context_free(session->instance);
    freerdp_free(session->instance);
    session->instance = nullptr;
}

void set_error(CxRdpSession* session, const std::string& message)
{
    if (session)
        session->last_error = message;
}

void notify_status(CxRdpSession* session, const char* message)
{
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->status_callback)
        session->status_callback(session->user_data, message);
}

void notify_clipboard_text(CxRdpSession* session, const std::string& text)
{
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->clipboard_text_callback)
        session->clipboard_text_callback(session->user_data, text.c_str());
}

void notify_clipboard_image(CxRdpSession* session, uint32_t imageFormat, const uint8_t* data, uint32_t length)
{
    if (!session || !data || length == 0)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->clipboard_image_callback)
        session->clipboard_image_callback(session->user_data, imageFormat, data, length);
}

void notify_clipboard_files(CxRdpSession* session, const std::vector<std::string>& paths)
{
    if (!session || paths.empty())
        return;

    std::ostringstream joined;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        if (i > 0)
            joined << '\n';
        joined << paths[i];
    }

    const auto payload = joined.str();
    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->clipboard_files_callback)
        session->clipboard_files_callback(session->user_data, payload.c_str());
}

std::vector<BYTE> serialize_file_group_descriptor_w(const std::vector<LocalClipboardFileEntry>& files)
{
    constexpr size_t descriptorSize = 592;
    const uint32_t count = static_cast<uint32_t>(files.size());
    std::vector<BYTE> buffer(4 + static_cast<size_t>(count) * descriptorSize, 0);

    buffer[0] = static_cast<BYTE>(count & 0xFFu);
    buffer[1] = static_cast<BYTE>((count >> 8) & 0xFFu);
    buffer[2] = static_cast<BYTE>((count >> 16) & 0xFFu);
    buffer[3] = static_cast<BYTE>((count >> 24) & 0xFFu);

    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
    for (size_t i = 0; i < files.size(); ++i)
    {
        const auto& entry = files[i];
        BYTE* dst = buffer.data() + 4 + i * descriptorSize;

        // dwFlags: FD_ATTRIBUTES (0x04) | FD_FILESIZE (0x40) | FD_PROGRESSUI (0x4000)
        const uint32_t flags = 0x00004044u;
        std::memcpy(dst + 0, &flags, 4);

        const uint32_t attrs = entry.is_directory ? 0x00000010u : 0x00000020u;
        std::memcpy(dst + 36, &attrs, 4);

        const uint32_t sizeHigh = static_cast<uint32_t>((entry.file_size >> 32) & 0xFFFFFFFFu);
        const uint32_t sizeLow = static_cast<uint32_t>(entry.file_size & 0xFFFFFFFFu);
        std::memcpy(dst + 64, &sizeHigh, 4);
        std::memcpy(dst + 68, &sizeLow, 4);

        std::u16string utf16Name;
        try
        {
            utf16Name = converter.from_bytes(entry.relative_name);
        }
        catch (const std::range_error&)
        {
            for (const unsigned char ch : entry.relative_name)
                utf16Name.push_back(ch < 0x80 ? static_cast<char16_t>(ch) : u'_');
        }

        const size_t maxChars = 259;
        const size_t charCount = utf16Name.size() < maxChars ? utf16Name.size() : maxChars;
        for (size_t c = 0; c < charCount; ++c)
        {
            const uint16_t ch = static_cast<uint16_t>(utf16Name[c]);
            dst[72 + c * 2] = static_cast<BYTE>(ch & 0xFFu);
            dst[72 + c * 2 + 1] = static_cast<BYTE>((ch >> 8) & 0xFFu);
        }
    }

    return buffer;
}

bool parse_file_group_descriptor_w(
    const BYTE* data,
    uint32_t length,
    const std::filesystem::path& baseDir,
    std::vector<RemoteClipboardFileEntry>& outFiles,
    std::vector<std::string>& outTopLevelPaths)
{
    outFiles.clear();
    outTopLevelPaths.clear();
    if (!data || length < 4)
        return false;

    uint32_t count = 0;
    std::memcpy(&count, data, 4);
    constexpr size_t descriptorSize = 592;
    if (count == 0 || 4 + static_cast<size_t>(count) * descriptorSize > length)
        return false;

    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
    for (uint32_t i = 0; i < count; ++i)
    {
        const BYTE* src = data + 4 + static_cast<size_t>(i) * descriptorSize;
        uint32_t attrs = 0;
        uint32_t sizeHigh = 0;
        uint32_t sizeLow = 0;
        std::memcpy(&attrs, src + 36, 4);
        std::memcpy(&sizeHigh, src + 64, 4);
        std::memcpy(&sizeLow, src + 68, 4);

        std::u16string utf16Name;
        for (size_t c = 0; c < 260; ++c)
        {
            const uint16_t ch = static_cast<uint16_t>(src[72 + c * 2] | (static_cast<uint16_t>(src[72 + c * 2 + 1]) << 8));
            if (ch == 0)
                break;
            utf16Name.push_back(static_cast<char16_t>(ch));
        }

        std::string relName;
        try
        {
            relName = converter.to_bytes(utf16Name);
        }
        catch (const std::range_error&)
        {
            continue;
        }

        if (relName.empty() || relName.find("..") != std::string::npos)
            continue;

        std::filesystem::path relPath = std::filesystem::u8path(relName);
        RemoteClipboardFileEntry entry{};
        entry.relative_name = relName;
        entry.local_path = baseDir / relPath;
        entry.is_directory = (attrs & 0x00000010u) != 0;
        entry.expected_size = (static_cast<uint64_t>(sizeHigh) << 32) | sizeLow;
        entry.size_known = entry.expected_size > 0;
        outFiles.push_back(entry);

        if (relName.find('\\') == std::string::npos && relName.find('/') == std::string::npos)
            outTopLevelPaths.push_back(entry.local_path.u8string());
    }

    return !outFiles.empty();
}

UINT request_next_remote_file_chunk(CliprdrClientContext* cliprdr, CxRdpSession* session)
{
    if (!cliprdr || !session || !cliprdr->ClientFileContentsRequest)
        return CHANNEL_RC_OK;

    CLIPRDR_FILE_CONTENTS_REQUEST req{};
    bool hasWork = false;
    std::vector<std::string> completedPaths;

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        while (session->remote_file_download_index < session->remote_clipboard_files.size())
        {
            auto& entry = session->remote_clipboard_files[session->remote_file_download_index];
            if (entry.is_directory)
            {
                std::error_code ec;
                std::filesystem::create_directories(entry.local_path, ec);
                ++session->remote_file_download_index;
                continue;
            }

            if (entry.size_known && entry.received_bytes >= entry.expected_size)
            {
                if (entry.expected_size == 0)
                {
                    std::error_code ec;
                    std::filesystem::create_directories(entry.local_path.parent_path(), ec);
                    std::ofstream emptyFile(entry.local_path, std::ios::binary | std::ios::trunc);
                }
                ++session->remote_file_download_index;
                continue;
            }

            req.common.msgType = CB_FILECONTENTS_REQUEST;
            req.streamId = ++session->remote_file_stream_id;
            req.listIndex = static_cast<UINT32>(session->remote_file_download_index);
            if (!entry.size_known)
            {
                req.dwFlags = FILECONTENTS_SIZE;
                req.cbRequested = 8;
                session->remote_file_requesting_size = true;
            }
            else
            {
                req.dwFlags = FILECONTENTS_RANGE;
                req.nPositionLow = static_cast<UINT32>(entry.received_bytes & 0xFFFFFFFFu);
                req.nPositionHigh = static_cast<UINT32>((entry.received_bytes >> 32) & 0xFFFFFFFFu);
                const uint64_t remaining = entry.expected_size - entry.received_bytes;
                req.cbRequested = static_cast<UINT32>(remaining < CX_CLIPRDR_FILE_CHUNK_SIZE ? remaining : CX_CLIPRDR_FILE_CHUNK_SIZE);
                session->remote_file_requesting_size = false;
            }
            hasWork = true;
            break;
        }

        if (!hasWork && !session->remote_clipboard_top_level_paths.empty())
        {
            completedPaths = session->remote_clipboard_top_level_paths;
            session->remote_clipboard_top_level_paths.clear();
            session->remote_clipboard_files.clear();
        }
    }

    if (hasWork)
        return cliprdr->ClientFileContentsRequest(cliprdr, &req);

    if (!completedPaths.empty())
    {
        notify_status(session, "RDP clipboard remote files downloaded.");
        notify_clipboard_files(session, completedPaths);
    }

    return CHANNEL_RC_OK;
}

std::string normalize_clipboard_newlines(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());

    for (size_t index = 0; index < value.size(); ++index)
    {
        const char ch = value[index];
        if (ch == '\r')
        {
            normalized.push_back('\r');
            normalized.push_back('\n');
            if (index + 1 < value.size() && value[index + 1] == '\n')
                ++index;
            continue;
        }

        if (ch == '\n')
        {
            normalized.push_back('\r');
            normalized.push_back('\n');
            continue;
        }

        normalized.push_back(ch);
    }

    return normalized;
}

std::vector<BYTE> encode_clipboard_unicode_text(const std::string& text)
{
    std::u16string utf16;
    try
    {
        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
        utf16 = converter.from_bytes(normalize_clipboard_newlines(text));
    }
    catch (const std::range_error&)
    {
        utf16.clear();
        for (const unsigned char ch : text)
            utf16.push_back(ch < 0x80 ? static_cast<char16_t>(ch) : u'?');
    }

    std::vector<BYTE> bytes;
    bytes.reserve((utf16.size() + 1) * 2);
    for (const char16_t ch : utf16)
    {
        const auto value = static_cast<uint16_t>(ch);
        bytes.push_back(static_cast<BYTE>(value & 0xff));
        bytes.push_back(static_cast<BYTE>((value >> 8) & 0xff));
    }

    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

std::vector<BYTE> encode_clipboard_narrow_text(const std::string& text, uint32_t formatId)
{
    const auto normalized = normalize_clipboard_newlines(text);

#if defined(_WIN32)
    const UINT codePage = formatId == CF_OEMTEXT ? CP_OEMCP : CP_ACP;
    const int wideLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        normalized.data(),
        static_cast<int>(normalized.size()),
        nullptr,
        0);

    if (wideLength > 0)
    {
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            normalized.data(),
            static_cast<int>(normalized.size()),
            wide.data(),
            wideLength);

        const int narrowLength = WideCharToMultiByte(
            codePage,
            0,
            wide.data(),
            wideLength,
            nullptr,
            0,
            nullptr,
            nullptr);

        if (narrowLength > 0)
        {
            std::vector<BYTE> bytes(static_cast<size_t>(narrowLength) + 1);
            WideCharToMultiByte(
                codePage,
                0,
                wide.data(),
                wideLength,
                reinterpret_cast<char*>(bytes.data()),
                narrowLength,
                nullptr,
                nullptr);
            bytes[static_cast<size_t>(narrowLength)] = 0;
            return bytes;
        }
    }
#else
    (void)formatId;
#endif

    std::vector<BYTE> bytes;
    bytes.reserve(normalized.size() + 1);
    for (const unsigned char ch : normalized)
        bytes.push_back(ch < 0x80 ? static_cast<BYTE>(ch) : static_cast<BYTE>('?'));
    bytes.push_back(0);
    return bytes;
}

std::vector<BYTE> encode_clipboard_text_format(const std::string& text, uint32_t formatId)
{
    if (formatId == CF_UNICODETEXT)
        return encode_clipboard_unicode_text(text);

    if (formatId == CF_TEXT || formatId == CF_OEMTEXT)
        return encode_clipboard_narrow_text(text, formatId);

    return {};
}

std::string decode_clipboard_unicode_text(const BYTE* data, uint32_t length)
{
    if (!data || length < 2)
        return {};

    std::u16string utf16;
    utf16.reserve(length / 2);
    for (uint32_t index = 0; index + 1 < length; index += 2)
    {
        const auto value = static_cast<char16_t>(data[index] | (static_cast<uint16_t>(data[index + 1]) << 8));
        if (value == 0)
            break;
        if (value == 0xfeff && utf16.empty())
            continue;
        utf16.push_back(value);
    }

    if (utf16.empty())
        return {};

    try
    {
        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
        return converter.to_bytes(utf16);
    }
    catch (const std::range_error&)
    {
        std::string fallback;
        fallback.reserve(utf16.size());
        for (const char16_t ch : utf16)
            fallback.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
        return fallback;
    }
}

std::string decode_clipboard_narrow_text(const BYTE* data, uint32_t length, uint32_t formatId)
{
    if (!data || length == 0)
        return {};

    uint32_t textLength = 0;
    while (textLength < length && data[textLength] != 0)
        ++textLength;

    if (textLength == 0)
        return {};

#if defined(_WIN32)
    const UINT codePage = formatId == CF_OEMTEXT ? CP_OEMCP : CP_ACP;
    const int wideLength = MultiByteToWideChar(
        codePage,
        0,
        reinterpret_cast<const char*>(data),
        static_cast<int>(textLength),
        nullptr,
        0);

    if (wideLength > 0)
    {
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        MultiByteToWideChar(
            codePage,
            0,
            reinterpret_cast<const char*>(data),
            static_cast<int>(textLength),
            wide.data(),
            wideLength);

        const int utf8Length = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            wideLength,
            nullptr,
            0,
            nullptr,
            nullptr);

        if (utf8Length > 0)
        {
            std::string utf8(static_cast<size_t>(utf8Length), '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                wide.data(),
                wideLength,
                utf8.data(),
                utf8Length,
                nullptr,
                nullptr);
            return utf8;
        }
    }
#else
    (void)formatId;
#endif

    std::string fallback;
    fallback.reserve(textLength);
    for (uint32_t index = 0; index < textLength; ++index)
        fallback.push_back(data[index] < 0x80 ? static_cast<char>(data[index]) : '?');
    return fallback;
}

std::string decode_clipboard_text_format(const BYTE* data, uint32_t length, uint32_t formatId)
{
    if (formatId == CF_UNICODETEXT)
        return decode_clipboard_unicode_text(data, length);

    if (formatId == CF_TEXT || formatId == CF_OEMTEXT)
        return decode_clipboard_narrow_text(data, length, formatId);

    return {};
}

const char* clipboard_format_name(uint32_t formatId)
{
    switch (formatId)
    {
    case CF_TEXT:
        return "CF_TEXT";
    case CF_OEMTEXT:
        return "CF_OEMTEXT";
    case CF_UNICODETEXT:
        return "CF_UNICODETEXT";
    case CF_DIB:
        return "CF_DIB";
    case CF_DIBV5:
        return "CF_DIBV5";
    case CX_CLIPRDR_FORMAT_PNG:
        return "PNG";
    case CX_CLIPRDR_FORMAT_FILE_GROUP_DESCRIPTOR_W:
        return "FileGroupDescriptorW";
    default:
        return "UNKNOWN";
    }
}

bool is_supported_text_format(uint32_t formatId)
{
    return formatId == CF_UNICODETEXT || formatId == CF_TEXT || formatId == CF_OEMTEXT;
}

void register_static_channel_provider()
{
    std::call_once(addin_provider_once, [] {
        freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
    });
}

bool equals_ignore_case(const char* left, const char* right)
{
    if (!left || !right)
        return left == right;

    while (*left != '\0' && *right != '\0')
    {
        const auto leftChar = static_cast<unsigned char>(*left);
        const auto rightChar = static_cast<unsigned char>(*right);
        if (std::tolower(leftChar) != std::tolower(rightChar))
            return false;

        ++left;
        ++right;
    }

    return *left == '\0' && *right == '\0';
}

bool is_clipboard_channel_allowed()
{
    const char* value = std::getenv("CXSHELL_RDP_CLIPBOARD_CHANNEL");
    if (!value || value[0] == '\0')
        return true;

    return std::strcmp(value, "0") != 0 &&
           !equals_ignore_case(value, "false") &&
           !equals_ignore_case(value, "off") &&
           !equals_ignore_case(value, "no");
}

bool is_channel_load_success(int result)
{
    return result == CHANNEL_RC_OK ||
           result == CHANNEL_RC_ALREADY_INITIALIZED ||
           result == CHANNEL_RC_ALREADY_CONNECTED;
}

bool process_rdp_events(CxRdpSession* session)
{
    if (!session || !session->instance || !session->instance->context)
        return false;

    HANDLE handles[64]{};
    const DWORD count = freerdp_get_event_handles(session->instance->context, handles, 64);
    if (count == 0)
    {
        set_error(session, "FreeRDP event handles unavailable.");
        return false;
    }

    const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
    if (status == WAIT_FAILED)
    {
        set_error(session, "FreeRDP event wait failed.");
        return false;
    }

    if (!freerdp_check_event_handles(session->instance->context))
    {
        set_error(session, "FreeRDP event processing failed.");
        return false;
    }

    return true;
}

UINT send_client_capabilities(CliprdrClientContext* cliprdr)
{
    if (!cliprdr || !cliprdr->ClientCapabilities)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    if (session)
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        if (session->client_capabilities_sent)
            return CHANNEL_RC_OK;
    }

    CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet{};
    generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
    generalCapabilitySet.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    generalCapabilitySet.version = CB_CAPS_VERSION_2;
    generalCapabilitySet.generalFlags =
        CB_USE_LONG_FORMAT_NAMES |
        CB_STREAM_FILECLIP_ENABLED |
        CB_FILECLIP_NO_FILE_PATHS |
        CB_HUGE_FILE_SUPPORT_ENABLED;

    CLIPRDR_CAPABILITIES capabilities{};
    capabilities.common.msgType = CB_CLIP_CAPS;
    capabilities.common.msgFlags = 0;
    capabilities.common.dataLen = sizeof(generalCapabilitySet);
    capabilities.cCapabilitiesSets = 1;
    capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&generalCapabilitySet);

    const UINT rc = cliprdr->ClientCapabilities(cliprdr, &capabilities);
    if (session && rc == CHANNEL_RC_OK)
    {
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            session->client_capabilities_sent = true;
        }
        notify_status(session, "RDP clipboard client capabilities sent.");
    }

    return rc;
}

UINT send_client_format_list(CliprdrClientContext* cliprdr)
{
    if (!cliprdr || !cliprdr->ClientFormatList)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    int kind = 0;
    bool hasPng = false;
    bool hasDib = false;
    bool hasFiles = false;
    if (session)
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        kind = session->local_clipboard_kind;
        hasPng = !session->local_clipboard_png.empty();
        hasDib = !session->local_clipboard_dib.empty();
        hasFiles = !session->local_clipboard_files.empty();
    }

    std::vector<CLIPRDR_FORMAT> formats;
    static char pngFormatName[] = "PNG";
    static char fileGroupDescriptorName[] = "FileGroupDescriptorW";
    const char* kindLabel = "text";

    if (kind == 2 && hasFiles)
    {
        kindLabel = "files";
        CLIPRDR_FORMAT fileFormat{};
        fileFormat.formatId = CX_CLIPRDR_FORMAT_FILE_GROUP_DESCRIPTOR_W;
        fileFormat.formatName = fileGroupDescriptorName;
        formats.push_back(fileFormat);
    }
    else if (kind == 1 && (hasPng || hasDib))
    {
        kindLabel = "image";
        if (hasDib)
        {
            CLIPRDR_FORMAT dibFormat{};
            dibFormat.formatId = CF_DIB;
            formats.push_back(dibFormat);
        }
        if (hasPng)
        {
            CLIPRDR_FORMAT pngFormat{};
            pngFormat.formatId = CX_CLIPRDR_FORMAT_PNG;
            pngFormat.formatName = pngFormatName;
            formats.push_back(pngFormat);
        }
    }
    else
    {
        CLIPRDR_FORMAT unicodeFormat{};
        unicodeFormat.formatId = CF_UNICODETEXT;
        formats.push_back(unicodeFormat);

        CLIPRDR_FORMAT textFormat{};
        textFormat.formatId = CF_TEXT;
        formats.push_back(textFormat);

        CLIPRDR_FORMAT oemFormat{};
        oemFormat.formatId = CF_OEMTEXT;
        formats.push_back(oemFormat);
    }

    CLIPRDR_FORMAT_LIST formatList{};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.common.dataLen = static_cast<UINT32>(formats.size() * sizeof(CLIPRDR_FORMAT));
    formatList.numFormats = static_cast<UINT32>(formats.size());
    formatList.formats = formats.data();

    const UINT rc = cliprdr->ClientFormatList(cliprdr, &formatList);
    if (session)
    {
        std::ostringstream message;
        message << "RDP clipboard offered local " << kindLabel << " formats. rc=" << rc;
        notify_status(session, message.str().c_str());
    }

    return rc;
}

UINT flush_local_clipboard_format_list(CxRdpSession* session)
{
    if (!session)
        return CHANNEL_RC_OK;

    CliprdrClientContext* cliprdr = nullptr;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        if (!session->cliprdr ||
            !session->cliprdr_wire_active ||
            session->offered_clipboard_revision == session->local_clipboard_revision)
            return CHANNEL_RC_OK;

        cliprdr = session->cliprdr;
        revision = session->local_clipboard_revision;
    }

    const UINT rc = send_client_format_list(cliprdr);
    if (rc == CHANNEL_RC_OK)
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        if (session->offered_clipboard_revision < revision)
            session->offered_clipboard_revision = revision;
    }
    else
    {
        std::ostringstream message;
        message << "RDP clipboard format list failed. code=" << rc;
        notify_status(session, message.str().c_str());
    }

    return rc;
}

UINT send_client_format_list_response(CliprdrClientContext* cliprdr, bool ok)
{
    if (!cliprdr || !cliprdr->ClientFormatListResponse)
        return CHANNEL_RC_OK;

    CLIPRDR_FORMAT_LIST_RESPONSE response{};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = 0;
    return cliprdr->ClientFormatListResponse(cliprdr, &response);
}

UINT send_client_format_data_request(CliprdrClientContext* cliprdr, uint32_t formatId)
{
    if (!cliprdr || !cliprdr->ClientFormatDataRequest || formatId == 0)
        return CHANNEL_RC_OK;

    CLIPRDR_FORMAT_DATA_REQUEST request{};
    request.common.msgType = CB_FORMAT_DATA_REQUEST;
    request.common.msgFlags = 0;
    request.common.dataLen = sizeof(formatId);
    request.requestedFormatId = formatId;

    if (auto* session = static_cast<CxRdpSession*>(cliprdr->custom))
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->clipboard_requested_format = formatId;
    }

    if (auto* session = static_cast<CxRdpSession*>(cliprdr->custom))
    {
        std::ostringstream message;
        message << "RDP clipboard requesting remote data. format=" << clipboard_format_name(formatId);
        notify_status(session, message.str().c_str());
    }

    return cliprdr->ClientFormatDataRequest(cliprdr, &request);
}

void log_cliprdr_diagnostics(CxRdpSession* session, const char* reason)
{
    if (!session || !session->instance || !session->instance->context)
        return;

    HANDLE handles[64]{};
    const DWORD handleCount = freerdp_get_event_handles(session->instance->context, handles, 64);
    const UINT16 channelId = freerdp_channels_get_id_by_name(session->instance, CLIPRDR_SVC_CHANNEL_NAME);

    std::ostringstream message;
    message << "RDP clipboard diagnostics";
    if (reason && reason[0] != '\0')
        message << " (" << reason << ")";
    message << ": channelId=" << channelId
            << " eventHandles=" << handleCount;
    notify_status(session, message.str().c_str());
}

void mark_cliprdr_confirmed(CxRdpSession* session, const char* reason)
{
    if (!session)
        return;

    bool notifyReady = false;
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        if (!session->cliprdr_ready)
        {
            session->cliprdr_ready = true;
            notifyReady = true;
        }
    }

    if (!notifyReady)
        return;

    std::ostringstream message;
    message << "RDP clipboard channel ready";
    if (reason && reason[0] != '\0')
        message << " (" << reason << ")";
    message << ".";
    notify_status(session, message.str().c_str());
}

UINT activate_cliprdr(CliprdrClientContext* cliprdr, const char* reason)
{
    if (!cliprdr)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    if (!session)
        return CHANNEL_RC_OK;

    uint64_t activatedRevision = 0;
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        activatedRevision = session->local_clipboard_revision;
    }

    UINT rc = send_client_capabilities(cliprdr);
    if (rc != CHANNEL_RC_OK)
    {
        std::ostringstream message;
        message << "RDP clipboard capabilities failed. code=" << rc;
        notify_status(session, message.str().c_str());
        return rc;
    }

    rc = send_client_format_list(cliprdr);
    if (rc != CHANNEL_RC_OK)
    {
        std::ostringstream message;
        message << "RDP clipboard format list failed. code=" << rc;
        notify_status(session, message.str().c_str());
        return rc;
    }

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->cliprdr_wire_active = true;
        session->cliprdr_activation_pending = false;
        if (session->offered_clipboard_revision < activatedRevision)
            session->offered_clipboard_revision = activatedRevision;
    }

    std::ostringstream message;
    message << "RDP clipboard channel activated";
    if (reason && reason[0] != '\0')
        message << " (" << reason << ")";
    message << ".";
    notify_status(session, message.str().c_str());
    log_cliprdr_diagnostics(session, reason);
    return CHANNEL_RC_OK;
}

UINT activate_pending_cliprdr(CxRdpSession* session)
{
    if (!session)
        return CHANNEL_RC_OK;

    std::chrono::steady_clock::time_point connectedAt{};
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        if (!session->cliprdr_activation_pending ||
            session->cliprdr_wire_active ||
            session->cliprdr_wait_logged ||
            !session->cliprdr)
            return CHANNEL_RC_OK;

        connectedAt = session->cliprdr_connected_at;
    }

    const auto elapsed = std::chrono::steady_clock::now() - connectedAt;
    if (elapsed < std::chrono::seconds(2))
        return CHANNEL_RC_OK;

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->cliprdr_wait_logged = true;
    }

    notify_status(session, "RDP clipboard waiting for server MonitorReady.");
    log_cliprdr_diagnostics(session, "waiting monitor ready");
    return CHANNEL_RC_OK;
}

std::string get_bridge_directory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&get_bridge_directory),
            &module))
        return {};

    const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    if (length == 0 || length >= sizeof(path))
        return {};

    std::string value(path, length);
    const auto index = value.find_last_of("\\/");
    return index == std::string::npos ? std::string{} : value.substr(0, index);
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&get_bridge_directory), &info) == 0 || !info.dli_fname)
        return {};

    std::string value(info.dli_fname);
    const auto index = value.find_last_of("\\/");
    return index == std::string::npos ? std::string{} : value.substr(0, index);
#endif
}

void ensure_openssl_providers(CxRdpSession* session)
{
    if (!session || session->openssl_providers_loaded)
        return;

    const auto bridgeDirectory = get_bridge_directory();
    std::string providerDirectory = bridgeDirectory;
#if defined(_WIN32)
    if (!bridgeDirectory.empty())
    {
        SetDllDirectoryA(bridgeDirectory.c_str());
        const auto moduleDirectory = bridgeDirectory + "\\ossl-modules";
        const DWORD attributes = GetFileAttributesA(moduleDirectory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            providerDirectory = moduleDirectory;
    }
#endif

    if (!providerDirectory.empty())
        OSSL_PROVIDER_set_default_search_path(nullptr, providerDirectory.c_str());

#if defined(_WIN32)
    std::string nativeLoadStatus;
    if (!providerDirectory.empty())
    {
        const auto providerPath = providerDirectory + "\\legacy.dll";
        SetLastError(0);
        HMODULE providerModule = LoadLibraryA(providerPath.c_str());
        if (providerModule)
        {
            nativeLoadStatus = " nativeLoad=ok";
        }
        else
        {
            std::ostringstream nativeError;
            nativeError << " nativeLoad=failed(" << GetLastError() << ")";
            nativeLoadStatus = nativeError.str();
        }
    }
#endif

    ERR_clear_error();
    auto* defaultProvider = OSSL_PROVIDER_load(nullptr, "default");
    std::vector<std::string> opensslErrors;
    auto* legacyProvider = OSSL_PROVIDER_load(nullptr, "legacy");
    unsigned long errorCode = 0;
    while ((errorCode = ERR_get_error()) != 0)
    {
        char buffer[256]{};
        ERR_error_string_n(errorCode, buffer, sizeof(buffer));
        opensslErrors.emplace_back(buffer);
    }

    std::ostringstream message;
    message << "OpenSSL providers default="
            << (defaultProvider ? "loaded" : "failed")
            << " legacy="
            << (legacyProvider ? "loaded" : "failed")
            << " path="
            << (providerDirectory.empty() ? "<empty>" : providerDirectory);
#if defined(_WIN32)
    message << nativeLoadStatus;
#endif
    if (!opensslErrors.empty())
    {
        message << " errors=";
        for (size_t index = 0; index < opensslErrors.size(); ++index)
        {
            if (index > 0)
                message << " | ";
            message << opensslErrors[index];
        }
    }

    notify_status(session, message.str().c_str());
    session->openssl_providers_loaded = true;
}

char* duplicate_string(const std::string& value)
{
    const auto length = value.size() + 1;
    auto* copy = static_cast<char*>(std::malloc(length));
    if (!copy)
        return nullptr;

    std::memcpy(copy, value.c_str(), length);
    return copy;
}

const char* auth_reason_name(rdp_auth_reason reason)
{
    switch (reason)
    {
    case AUTH_NLA:
        return "NLA";
    case AUTH_TLS:
        return "TLS";
    case AUTH_RDP:
        return "RDP";
    case GW_AUTH_HTTP:
        return "GatewayHttp";
    case GW_AUTH_RDG:
        return "GatewayRdg";
    case GW_AUTH_RPC:
        return "GatewayRpc";
    case AUTH_SMARTCARD_PIN:
        return "SmartcardPin";
#if FREERDP_VERSION_MAJOR > 3 || (FREERDP_VERSION_MAJOR == 3 && FREERDP_VERSION_MINOR >= 26)
    case AUTH_RDSTLS:
        return "Rdstls";
    case AUTH_FIDO_PIN:
        return "FidoPin";
#endif
    default:
        return "Unknown";
    }
}

std::string describe_last_error(CxRdpSession* session)
{
    if (!session || !session->instance || !session->instance->context)
        return "FreeRDP connection failed.";

    const UINT32 code = freerdp_get_last_error(session->instance->context);
    const char* name = freerdp_get_last_error_name(code);
    const char* text = freerdp_get_last_error_string(code);
    const char* category = freerdp_get_last_error_category(code);

    std::ostringstream stream;
    stream << "FreeRDP connection failed.";
    stream << " code=0x" << std::hex << std::setw(8) << std::setfill('0') << code;
    if (name && name[0] != '\0')
        stream << " name=" << name;
    if (category && category[0] != '\0')
        stream << " category=" << category;
    if (text && text[0] != '\0')
        stream << " message=" << text;

    return stream.str();
}

BOOL cx_authenticate(
    freerdp* instance,
    char** username,
    char** password,
    char** domain,
    rdp_auth_reason reason)
{
    if (!instance || !instance->context)
        return FALSE;

    auto* session = get_session(instance->context);
    if (!session)
        return FALSE;

    std::ostringstream message;
    message << "RDP authenticate callback reason=" << auth_reason_name(reason)
             << " usernameLen=" << session->username.size()
             << " domainLen=" << session->domain.size()
             << " passwordLen=" << session->password.size();
    notify_status(session, message.str().c_str());

    if (username)
    {
        std::free(*username);
        *username = duplicate_string(session->username);
    }

    if (password)
    {
        std::free(*password);
        *password = duplicate_string(session->password);
    }

    if (domain)
    {
        std::free(*domain);
        *domain = duplicate_string(session->domain);
    }

    return TRUE;
}

DWORD cx_verify_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* subject,
    const char* issuer,
    const char* fingerprint,
    DWORD flags)
{
    if (instance && instance->context)
    {
        auto* session = get_session(instance->context);
        std::ostringstream message;
        message << "RDP certificate accepted host=" << (host ? host : "<null>")
                << " port=" << port
                << " commonName=" << (common_name ? common_name : "<null>")
                << " flags=0x" << std::hex << flags;
        notify_status(session, message.str().c_str());
    }

    return 2;
}

DWORD cx_verify_changed_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* subject,
    const char* issuer,
    const char* new_fingerprint,
    const char* old_subject,
    const char* old_issuer,
    const char* old_fingerprint,
    DWORD flags)
{
    return cx_verify_certificate(instance, host, port, common_name, subject, issuer, new_fingerprint, flags);
}

int cx_verify_x509_certificate(
    freerdp* instance,
    const BYTE* data,
    size_t length,
    const char* hostname,
    UINT16 port,
    DWORD flags)
{
    if (instance && instance->context)
    {
        auto* session = get_session(instance->context);
        std::ostringstream message;
        message << "RDP X509 certificate accepted host=" << (hostname ? hostname : "<null>")
                << " port=" << port
                << " length=" << length
                << " flags=0x" << std::hex << flags;
        notify_status(session, message.str().c_str());
    }

    return 2;
}

int cx_logon_error(freerdp* instance, UINT32 data, UINT32 type)
{
    if (instance && instance->context)
    {
        auto* session = get_session(instance->context);
        std::ostringstream message;
        message << "RDP logon error data=0x" << std::hex << data << " type=0x" << type;
        notify_status(session, message.str().c_str());
    }

    return 1;
}

UINT cx_cliprdr_monitor_ready(CliprdrClientContext* cliprdr, const CLIPRDR_MONITOR_READY* monitorReady)
{
    (void)monitorReady;

    if (!cliprdr)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    if (!session)
        return CHANNEL_RC_OK;

    mark_cliprdr_confirmed(session, "monitor ready");
    return activate_cliprdr(cliprdr, "monitor ready");
}

UINT cx_cliprdr_server_capabilities(CliprdrClientContext* cliprdr, const CLIPRDR_CAPABILITIES* capabilities)
{
    if (!cliprdr)
        return CHANNEL_RC_OK;

    if (auto* session = static_cast<CxRdpSession*>(cliprdr->custom))
    {
        std::ostringstream message;
        message << "RDP clipboard server capabilities received.";
        if (capabilities && capabilities->cCapabilitiesSets > 0 && capabilities->capabilitySets)
        {
            const auto* general = reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(capabilities->capabilitySets);
            message << " flags=0x" << std::hex << general->generalFlags;
        }
        notify_status(session, message.str().c_str());
        mark_cliprdr_confirmed(session, "server capabilities");

        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            session->client_capabilities_sent = false;
        }
    }

    return send_client_capabilities(cliprdr);
}

UINT cx_cliprdr_server_format_list(CliprdrClientContext* cliprdr, const CLIPRDR_FORMAT_LIST* formatList)
{
    if (!cliprdr || !formatList)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    uint32_t selectedTextFormat = 0;
    uint32_t selectedImageFormat = 0;
    uint32_t selectedImageKind = 0;
    uint32_t selectedFilesFormat = 0;

    for (UINT32 index = 0; index < formatList->numFormats; ++index)
    {
        const auto& fmt = formatList->formats[index];
        const uint32_t formatId = fmt.formatId;
        if (formatId == CF_UNICODETEXT)
        {
            selectedTextFormat = CF_UNICODETEXT;
        }
        else if (formatId == CF_TEXT && selectedTextFormat == 0)
        {
            selectedTextFormat = CF_TEXT;
        }
        else if (formatId == CF_OEMTEXT && selectedTextFormat == 0)
        {
            selectedTextFormat = CF_OEMTEXT;
        }

        if (fmt.formatName && equals_ignore_case(fmt.formatName, "FileGroupDescriptorW"))
        {
            selectedFilesFormat = formatId;
        }
        else if (fmt.formatName &&
                 (equals_ignore_case(fmt.formatName, "PNG") || equals_ignore_case(fmt.formatName, "image/png")))
        {
            selectedImageFormat = formatId;
            selectedImageKind = CX_RDP_CLIPBOARD_IMAGE_PNG;
        }
        else if (formatId == CF_DIB && selectedImageKind != CX_RDP_CLIPBOARD_IMAGE_PNG)
        {
            selectedImageFormat = CF_DIB;
            selectedImageKind = CX_RDP_CLIPBOARD_IMAGE_DIB;
        }
        else if (formatId == CF_DIBV5 && selectedImageFormat == 0)
        {
            selectedImageFormat = CF_DIBV5;
            selectedImageKind = CX_RDP_CLIPBOARD_IMAGE_DIB;
        }
    }

    uint32_t selectedFormat = 0;
    uint32_t requestedImageKind = 0;
    bool requestedFiles = false;
    if (selectedFilesFormat != 0)
    {
        selectedFormat = selectedFilesFormat;
        requestedFiles = true;
    }
    else if (selectedTextFormat == CF_UNICODETEXT)
    {
        selectedFormat = selectedTextFormat;
    }
    else if (selectedImageFormat != 0)
    {
        selectedFormat = selectedImageFormat;
        requestedImageKind = selectedImageKind;
    }
    else
    {
        selectedFormat = selectedTextFormat;
    }

    if (session)
    {
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            session->clipboard_requested_image_format = requestedImageKind;
            session->clipboard_requested_files = requestedFiles;
        }
        mark_cliprdr_confirmed(session, "remote formats");
        std::ostringstream message;
        message << "RDP clipboard remote formats received. count=" << formatList->numFormats
                << " selected=" << (requestedFiles ? "FileGroupDescriptorW" : (requestedImageKind == CX_RDP_CLIPBOARD_IMAGE_PNG ? "PNG" : clipboard_format_name(selectedFormat)));
        notify_status(session, message.str().c_str());
    }

    send_client_format_list_response(cliprdr, true);

    if (selectedFormat == 0)
    {
        if (session && formatList->numFormats == 0)
            notify_clipboard_text(session, "");
        return CHANNEL_RC_OK;
    }

    return send_client_format_data_request(cliprdr, selectedFormat);
}

UINT cx_cliprdr_server_format_list_response(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
    if (cliprdr)
    {
        if (auto* session = static_cast<CxRdpSession*>(cliprdr->custom))
        {
            mark_cliprdr_confirmed(session, "format list response");
            std::ostringstream message;
            message << "RDP clipboard local format list response. flags=0x"
                    << std::hex
                    << (formatListResponse ? formatListResponse->common.msgFlags : 0);
            notify_status(session, message.str().c_str());
        }
    }

    return CHANNEL_RC_OK;
}

UINT cx_cliprdr_server_lock_clipboard_data(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_LOCK_CLIPBOARD_DATA* lockClipboardData)
{
    (void)cliprdr;
    (void)lockClipboardData;
    return CHANNEL_RC_OK;
}

UINT cx_cliprdr_server_unlock_clipboard_data(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlockClipboardData)
{
    (void)cliprdr;
    (void)unlockClipboardData;
    return CHANNEL_RC_OK;
}

UINT cx_cliprdr_server_format_data_request(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
    if (!cliprdr || !formatDataRequest || !cliprdr->ClientFormatDataResponse)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    if (session)
        mark_cliprdr_confirmed(session, "data request");

    std::vector<BYTE> data;
    CLIPRDR_FORMAT_DATA_RESPONSE response{};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_FAIL;
    response.common.dataLen = 0;
    response.requestedFormatData = nullptr;

    if (session && formatDataRequest->requestedFormatId == CX_CLIPRDR_FORMAT_FILE_GROUP_DESCRIPTOR_W)
    {
        std::vector<LocalClipboardFileEntry> files;
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            if (session->local_clipboard_kind == 2 && !session->local_clipboard_files.empty())
                files = session->local_clipboard_files;
        }

        if (!files.empty())
        {
            data = serialize_file_group_descriptor_w(files);
            response.common.msgFlags = CB_RESPONSE_OK;
            response.common.dataLen = static_cast<UINT32>(data.size());
            response.requestedFormatData = data.data();

            std::ostringstream message;
            message << "RDP clipboard remote requested local file list. count=" << files.size()
                    << " bytes=" << data.size();
            notify_status(session, message.str().c_str());
        }
    }
    else if (session && formatDataRequest->requestedFormatId == CF_DIB)
    {
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            if (session->local_clipboard_kind == 1 && !session->local_clipboard_dib.empty())
                data.assign(session->local_clipboard_dib.begin(), session->local_clipboard_dib.end());
        }

        if (!data.empty())
        {
            response.common.msgFlags = CB_RESPONSE_OK;
            response.common.dataLen = static_cast<UINT32>(data.size());
            response.requestedFormatData = data.data();

            std::ostringstream message;
            message << "RDP clipboard remote requested local image. format=CF_DIB bytes=" << data.size();
            notify_status(session, message.str().c_str());
        }
    }
    else if (session && formatDataRequest->requestedFormatId == CX_CLIPRDR_FORMAT_PNG)
    {
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            if (session->local_clipboard_kind == 1 && !session->local_clipboard_png.empty())
                data.assign(session->local_clipboard_png.begin(), session->local_clipboard_png.end());
        }

        if (!data.empty())
        {
            response.common.msgFlags = CB_RESPONSE_OK;
            response.common.dataLen = static_cast<UINT32>(data.size());
            response.requestedFormatData = data.data();

            std::ostringstream message;
            message << "RDP clipboard remote requested local image. format=PNG bytes=" << data.size();
            notify_status(session, message.str().c_str());
        }
    }
    else if (session && is_supported_text_format(formatDataRequest->requestedFormatId))
    {
        std::string text;
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            text = session->local_clipboard_text;
        }

        data = encode_clipboard_text_format(text, formatDataRequest->requestedFormatId);
        response.common.msgFlags = CB_RESPONSE_OK;
        response.common.dataLen = static_cast<UINT32>(data.size());
        response.requestedFormatData = data.data();

        std::ostringstream message;
        message << "RDP clipboard remote requested local data. format="
                << clipboard_format_name(formatDataRequest->requestedFormatId)
                << " textLength=" << text.size()
                << " bytes=" << data.size();
        notify_status(session, message.str().c_str());
    }
    else if (session)
    {
        std::ostringstream message;
        message << "RDP clipboard remote requested unsupported format. format=0x"
                << std::hex << (formatDataRequest ? formatDataRequest->requestedFormatId : 0);
        notify_status(session, message.str().c_str());
    }

    return cliprdr->ClientFormatDataResponse(cliprdr, &response);
}

UINT cx_cliprdr_server_format_data_response(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
    if (!cliprdr || !formatDataResponse)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    if (!session)
        return CHANNEL_RC_OK;

    mark_cliprdr_confirmed(session, "data response");

    if ((formatDataResponse->common.msgFlags & CB_RESPONSE_FAIL) != 0)
        return CHANNEL_RC_OK;

    uint32_t requestedFormat = 0;
    uint32_t requestedImageKind = 0;
    bool requestedFiles = false;
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        requestedFormat = session->clipboard_requested_format;
        requestedImageKind = session->clipboard_requested_image_format;
        requestedFiles = session->clipboard_requested_files;
        session->clipboard_requested_format = 0;
        session->clipboard_requested_image_format = 0;
        session->clipboard_requested_files = false;
    }

    const auto* data = formatDataResponse->requestedFormatData;
    const auto length = formatDataResponse->common.dataLen;

    if (requestedFiles)
    {
        if (data && length > 0)
        {
            const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            std::error_code ec;
            const auto baseDir = std::filesystem::temp_directory_path(ec) / "CxShell-RdpClip" / std::to_string(stamp);
            std::filesystem::create_directories(baseDir, ec);

            std::vector<RemoteClipboardFileEntry> parsedFiles;
            std::vector<std::string> topLevelPaths;
            if (parse_file_group_descriptor_w(data, length, baseDir, parsedFiles, topLevelPaths))
            {
                {
                    std::lock_guard<std::mutex> lock(session->clipboard_mutex);
                    session->remote_clipboard_files = std::move(parsedFiles);
                    session->remote_clipboard_top_level_paths = std::move(topLevelPaths);
                    session->remote_file_download_index = 0;
                    session->remote_file_requesting_size = false;
                }
                return request_next_remote_file_chunk(cliprdr, session);
            }
        }
        return CHANNEL_RC_OK;
    }

    if (requestedImageKind != 0)
    {
        if (data && length > 0)
        {
            std::ostringstream message;
            message << "RDP clipboard remote image received. kind=" << requestedImageKind
                    << " bytes=" << length;
            notify_status(session, message.str().c_str());
            notify_clipboard_image(session, requestedImageKind, data, length);
        }
        return CHANNEL_RC_OK;
    }

    if (!is_supported_text_format(requestedFormat))
        return CHANNEL_RC_OK;

    {
        std::ostringstream message;
        message << "RDP clipboard remote data received. format="
                << clipboard_format_name(requestedFormat)
                << " bytes=" << length;
        notify_status(session, message.str().c_str());
    }
    notify_clipboard_text(session, decode_clipboard_text_format(data, length, requestedFormat));
    return CHANNEL_RC_OK;
}

UINT cx_cliprdr_server_file_contents_request(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
    if (!cliprdr || !fileContentsRequest || !cliprdr->ClientFileContentsResponse)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    CLIPRDR_FILE_CONTENTS_RESPONSE response{};
    response.common.msgType = CB_FILECONTENTS_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_FAIL;
    response.streamId = fileContentsRequest->streamId;

    LocalClipboardFileEntry entry{};
    bool found = false;
    if (session)
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        if (session->local_clipboard_kind == 2 &&
            fileContentsRequest->listIndex < session->local_clipboard_files.size())
        {
            entry = session->local_clipboard_files[fileContentsRequest->listIndex];
            found = true;
        }
    }

    std::vector<BYTE> payload;
    if (found && (fileContentsRequest->dwFlags & FILECONTENTS_SIZE) != 0)
    {
        payload.resize(8);
        const uint64_t size = entry.file_size;
        std::memcpy(payload.data(), &size, 8);
        response.common.msgFlags = CB_RESPONSE_OK;
        response.cbRequested = 8;
        response.requestedData = payload.data();
    }
    else if (found && !entry.is_directory && (fileContentsRequest->dwFlags & FILECONTENTS_RANGE) != 0)
    {
        const uint64_t offset =
            (static_cast<uint64_t>(fileContentsRequest->nPositionHigh) << 32) |
            fileContentsRequest->nPositionLow;
        const uint32_t toRead = fileContentsRequest->cbRequested;
        std::ifstream file(entry.full_path, std::ios::binary);
        if (file)
        {
            file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            payload.resize(toRead);
            file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(toRead));
            const auto bytesRead = static_cast<uint32_t>(file.gcount());
            payload.resize(bytesRead);
            response.common.msgFlags = CB_RESPONSE_OK;
            response.cbRequested = bytesRead;
            response.requestedData = payload.empty() ? nullptr : payload.data();
        }
    }

    return cliprdr->ClientFileContentsResponse(cliprdr, &response);
}

UINT cx_cliprdr_server_file_contents_response(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse)
{
    if (!cliprdr || !fileContentsResponse)
        return CHANNEL_RC_OK;

    auto* session = static_cast<CxRdpSession*>(cliprdr->custom);
    if (!session)
        return CHANNEL_RC_OK;

    if ((fileContentsResponse->common.msgFlags & CB_RESPONSE_FAIL) != 0)
    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->remote_clipboard_files.clear();
        session->remote_clipboard_top_level_paths.clear();
        return CHANNEL_RC_OK;
    }

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        if (session->remote_file_download_index < session->remote_clipboard_files.size())
        {
            auto& entry = session->remote_clipboard_files[session->remote_file_download_index];
            if (session->remote_file_requesting_size)
            {
                if (fileContentsResponse->cbRequested >= 8 && fileContentsResponse->requestedData)
                {
                    uint64_t size = 0;
                    std::memcpy(&size, fileContentsResponse->requestedData, 8);
                    entry.expected_size = size;
                }
                entry.size_known = true;
                session->remote_file_requesting_size = false;
            }
            else
            {
                std::error_code ec;
                std::filesystem::create_directories(entry.local_path.parent_path(), ec);
                const auto mode = entry.received_bytes == 0
                                      ? (std::ios::binary | std::ios::trunc)
                                      : (std::ios::binary | std::ios::app);
                std::ofstream out(entry.local_path, mode);
                if (out && fileContentsResponse->requestedData && fileContentsResponse->cbRequested > 0)
                {
                    out.write(
                        reinterpret_cast<const char*>(fileContentsResponse->requestedData),
                        static_cast<std::streamsize>(fileContentsResponse->cbRequested));
                    entry.received_bytes += fileContentsResponse->cbRequested;
                }
                else
                {
                    entry.received_bytes = entry.expected_size;
                }
            }
        }
    }

    return request_next_remote_file_chunk(cliprdr, session);
}

void initialize_cliprdr(CxRdpSession* session, CliprdrClientContext* cliprdr)
{
    if (!session || !cliprdr)
        return;

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->cliprdr = cliprdr;
        session->cliprdr_wire_active = false;
        session->cliprdr_ready = false;
        session->cliprdr_activation_pending = true;
        session->cliprdr_wait_logged = false;
        session->client_capabilities_sent = false;
        session->cliprdr_connected_at = std::chrono::steady_clock::now();
        session->clipboard_requested_format = 0;
    }

    cliprdr->custom = session;
    cliprdr->MonitorReady = cx_cliprdr_monitor_ready;
    cliprdr->ServerCapabilities = cx_cliprdr_server_capabilities;
    cliprdr->ServerFormatList = cx_cliprdr_server_format_list;
    cliprdr->ServerFormatListResponse = cx_cliprdr_server_format_list_response;
    cliprdr->ServerLockClipboardData = cx_cliprdr_server_lock_clipboard_data;
    cliprdr->ServerUnlockClipboardData = cx_cliprdr_server_unlock_clipboard_data;
    cliprdr->ServerFormatDataRequest = cx_cliprdr_server_format_data_request;
    cliprdr->ServerFormatDataResponse = cx_cliprdr_server_format_data_response;
    cliprdr->ServerFileContentsRequest = cx_cliprdr_server_file_contents_request;
    cliprdr->ServerFileContentsResponse = cx_cliprdr_server_file_contents_response;

    notify_status(session, "RDP clipboard channel connected.");
    log_cliprdr_diagnostics(session, "connected");
}

void uninitialize_cliprdr(CxRdpSession* session, CliprdrClientContext* cliprdr)
{
    if (cliprdr)
        cliprdr->custom = nullptr;

    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->clipboard_mutex);
    if (session->cliprdr == cliprdr)
    {
        session->cliprdr = nullptr;
        session->cliprdr_wire_active = false;
        session->cliprdr_ready = false;
        session->cliprdr_activation_pending = false;
        session->cliprdr_wait_logged = false;
        session->client_capabilities_sent = false;
        session->clipboard_requested_format = 0;
    }
}

void cx_channel_connected(void* context, const ChannelConnectedEventArgs* e)
{
    if (!context || !e || !e->name || !e->pInterface)
        return;

    auto* rdp_context = static_cast<struct rdp_context*>(context);
    auto* session = get_session(rdp_context);
    if (!session)
        return;

    if (std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
    {
        initialize_cliprdr(session, static_cast<CliprdrClientContext*>(e->pInterface));
    }
    else if (std::strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
    {
        {
            std::lock_guard<std::mutex> lock(session->disp_mutex);
            session->disp = static_cast<DispClientContext*>(e->pInterface);
        }
        notify_status(session, "RDP dynamic display channel connected.");
    }
    else if (std::strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
    {
        if (rdp_context->gdi)
        {
            gdi_graphics_pipeline_init(rdp_context->gdi, static_cast<RdpgfxClientContext*>(e->pInterface));
            notify_status(session, "RDP graphics pipeline (RDPGFX) connected.");
        }
    }
}

void cx_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e)
{
    if (!context || !e || !e->name)
        return;

    auto* rdp_context = static_cast<struct rdp_context*>(context);
    auto* session = get_session(rdp_context);
    if (!session)
        return;

    if (std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
    {
        uninitialize_cliprdr(session, static_cast<CliprdrClientContext*>(e->pInterface));
    }
    else if (std::strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
    {
        std::lock_guard<std::mutex> lock(session->disp_mutex);
        if (session->disp == static_cast<DispClientContext*>(e->pInterface))
            session->disp = nullptr;
    }
    else if (std::strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
    {
        if (rdp_context->gdi)
            gdi_graphics_pipeline_uninit(rdp_context->gdi, static_cast<RdpgfxClientContext*>(e->pInterface));
    }
}

BOOL cx_desktop_resize(rdpContext* context)
{
    if (!context || !context->gdi || !context->settings)
        return FALSE;

    const UINT32 width = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
    const UINT32 height = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
    if (width == 0 || height == 0)
        return TRUE;

    return gdi_resize(context->gdi, width, height);
}

BOOL cx_begin_paint(rdpContext* context)
{
    if (!context || !context->gdi)
        return FALSE;

    auto* gdi = context->gdi;
    gdi->primary->hdc->hwnd->invalid->null = TRUE;
    return TRUE;
}

BOOL cx_end_paint(rdpContext* context)
{
    if (!context || !context->gdi || !context->instance)
        return FALSE;

    auto* session = get_session(context);
    if (!session)
        return TRUE;

    auto* gdi = context->gdi;
    if (!gdi->primary_buffer || gdi->width <= 0 || gdi->height <= 0)
        return TRUE;

    const int width = static_cast<int>(gdi->width);
    const int height = static_cast<int>(gdi->height);
    const int stride = width * 4;
    const auto* source = static_cast<const uint8_t*>(gdi->primary_buffer);
    if (!source)
        return TRUE;

    int dx = 0;
    int dy = 0;
    int dw = width;
    int dh = height;
    if (gdi->primary && gdi->primary->hdc && gdi->primary->hdc->hwnd && gdi->primary->hdc->hwnd->invalid)
    {
        const auto* inv = gdi->primary->hdc->hwnd->invalid;
        if (!inv->null && inv->w > 0 && inv->h > 0)
        {
            const int x0 = inv->x < 0 ? 0 : (inv->x > width ? width : inv->x);
            const int y0 = inv->y < 0 ? 0 : (inv->y > height ? height : inv->y);
            const int x1 = (inv->x + inv->w) > width ? width : (inv->x + inv->w);
            const int y1 = (inv->y + inv->h) > height ? height : (inv->y + inv->h);
            if (x1 > x0 && y1 > y0)
            {
                dx = x0;
                dy = y0;
                dw = x1 - x0;
                dh = y1 - y0;
            }
        }
    }

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->frame_rect_callback)
        session->frame_rect_callback(session->user_data, width, height, stride, source, dx, dy, dw, dh);
    else if (session->frame_callback)
        session->frame_callback(session->user_data, width, height, stride, source);

    return TRUE;
}

BOOL cx_pre_connect(freerdp* instance)
{
    if (!instance || !instance->context || !instance->context->settings)
        return FALSE;

    auto* session = get_session(instance->context);
    const bool clipboardEnabled = session && session->clipboard_channel_enabled;
    freerdp_settings_set_uint32(instance->context->settings, FreeRDP_OsMajorType, OSMAJORTYPE_WINDOWS);
    freerdp_settings_set_uint32(instance->context->settings, FreeRDP_OsMinorType, OSMINORTYPE_WINDOWS_NT);
    freerdp_settings_set_bool(instance->context->settings, FreeRDP_RedirectClipboard, clipboardEnabled ? TRUE : FALSE);
    freerdp_settings_set_bool(instance->context->settings, FreeRDP_SupportDynamicChannels, TRUE);
    freerdp_settings_set_bool(instance->context->settings, FreeRDP_SupportDisplayControl, TRUE);

    if (clipboardEnabled)
    {
        freerdp_settings_set_uint32(
            instance->context->settings,
            FreeRDP_ClipboardFeatureMask,
            CLIPRDR_FLAG_LOCAL_TO_REMOTE | CLIPRDR_FLAG_REMOTE_TO_LOCAL);
    }

    PubSub_SubscribeChannelConnected(instance->context->pubSub, cx_channel_connected);
    PubSub_SubscribeChannelDisconnected(instance->context->pubSub, cx_channel_disconnected);
    return TRUE;
}

BOOL cx_load_channels(freerdp* instance)
{
    if (!instance || !instance->context || !instance->context->channels || !instance->context->settings)
        return FALSE;

    auto* session = get_session(instance->context);
    const bool localAudioEnabled = session && session->audio_playback_mode == CX_RDP_AUDIO_PLAY_LOCAL;
    if (!session)
        return TRUE;

    register_static_channel_provider();

    if (freerdp_client_load_addins(instance->context->channels, instance->context->settings))
        return TRUE;

    if (session->drive_redirection_enabled)
    {
        const int drivePluginResult = freerdp_channels_load_plugin(
            instance->context->channels,
            instance->context->settings,
            RDPDR_CHANNEL_NAME,
            nullptr);

        if (!is_channel_load_success(drivePluginResult))
        {
            std::ostringstream message;
            message << "RDP drive channel load failed. code=" << drivePluginResult << ".";
            set_error(session, message.str());
            notify_status(session, message.str().c_str());
            return FALSE;
        }
    }

    if (session->clipboard_channel_enabled &&
        freerdp_settings_get_bool(instance->context->settings, FreeRDP_RedirectClipboard))
    {
        const int clipboardPluginResult = freerdp_channels_load_plugin(
            instance->context->channels,
            instance->context->settings,
            CLIPRDR_CHANNEL_NAME,
            nullptr);

        if (!is_channel_load_success(clipboardPluginResult))
        {
            std::ostringstream message;
            message << "RDP clipboard channel load failed. code=" << clipboardPluginResult
                    << " continuing without clipboard.";
            notify_status(session, message.str().c_str());

            freerdp_settings_set_bool(instance->context->settings, FreeRDP_RedirectClipboard, FALSE);
        }
    }

    if (localAudioEnabled)
    {
        const int soundPluginResult = freerdp_channels_load_plugin(
            instance->context->channels,
            instance->context->settings,
            RDPSND_CHANNEL_NAME,
            nullptr);

        if (!is_channel_load_success(soundPluginResult))
        {
            std::ostringstream message;
            message << "RDP sound channel load failed. code=" << soundPluginResult << ".";
            set_error(session, message.str());
            notify_status(session, message.str().c_str());
            return FALSE;
        }
    }

    if (session->microphone_enabled)
    {
        const int dynamicChannelResult = freerdp_channels_load_plugin(
            instance->context->channels,
            instance->context->settings,
            DRDYNVC_CHANNEL_NAME,
            nullptr);

        if (!is_channel_load_success(dynamicChannelResult))
        {
            std::ostringstream message;
            message << "RDP microphone channel load failed. code=" << dynamicChannelResult << ".";
            set_error(session, message.str());
            notify_status(session, message.str().c_str());
            return FALSE;
        }
    }

    return TRUE;
}

struct CxRdpPointer
{
    rdpPointer pointer;
    uint8_t* bgra;
    int width;
    int height;
    int hot_x;
    int hot_y;
};

BOOL cx_pointer_new(rdpContext* context, rdpPointer* pointer)
{
    if (!context || !pointer || pointer->width == 0 || pointer->height == 0)
        return FALSE;

    auto* cxPtr = reinterpret_cast<CxRdpPointer*>(pointer);
    const size_t byteCount = static_cast<size_t>(pointer->width) * static_cast<size_t>(pointer->height) * 4u;
    cxPtr->bgra = static_cast<uint8_t*>(std::calloc(1, byteCount));
    if (!cxPtr->bgra)
        return FALSE;

    cxPtr->width = static_cast<int>(pointer->width);
    cxPtr->height = static_cast<int>(pointer->height);
    cxPtr->hot_x = static_cast<int>(pointer->xPos);
    cxPtr->hot_y = static_cast<int>(pointer->yPos);

    const BOOL ok = freerdp_image_copy_from_pointer_data(
        cxPtr->bgra,
        PIXEL_FORMAT_BGRA32,
        0,
        0,
        0,
        pointer->width,
        pointer->height,
        pointer->xorMaskData,
        pointer->lengthXorMask,
        pointer->andMaskData,
        pointer->lengthAndMask,
        pointer->xorBpp,
        &context->gdi->palette);
    if (!ok)
    {
        std::free(cxPtr->bgra);
        cxPtr->bgra = nullptr;
        return FALSE;
    }

    return TRUE;
}

void cx_pointer_free(rdpContext* context, rdpPointer* pointer)
{
    (void)context;
    if (!pointer)
        return;

    auto* cxPtr = reinterpret_cast<CxRdpPointer*>(pointer);
    if (cxPtr->bgra)
    {
        std::free(cxPtr->bgra);
        cxPtr->bgra = nullptr;
    }
}

BOOL cx_pointer_set(rdpContext* context, rdpPointer* pointer)
{
    auto* session = get_session(context);
    if (!session || !pointer)
        return TRUE;

    auto* cxPtr = reinterpret_cast<CxRdpPointer*>(pointer);
    if (!cxPtr->bgra || cxPtr->width <= 0 || cxPtr->height <= 0)
        return TRUE;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->pointer_callback)
    {
        session->pointer_callback(
            session->user_data,
            CX_RDP_POINTER_CUSTOM,
            cxPtr->width,
            cxPtr->height,
            cxPtr->hot_x,
            cxPtr->hot_y,
            cxPtr->bgra);
    }
    return TRUE;
}

BOOL cx_pointer_set_null(rdpContext* context)
{
    auto* session = get_session(context);
    if (!session)
        return TRUE;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->pointer_callback)
        session->pointer_callback(session->user_data, CX_RDP_POINTER_NULL, 0, 0, 0, 0, nullptr);
    return TRUE;
}

BOOL cx_pointer_set_default(rdpContext* context)
{
    auto* session = get_session(context);
    if (!session)
        return TRUE;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->pointer_callback)
        session->pointer_callback(session->user_data, CX_RDP_POINTER_DEFAULT, 0, 0, 0, 0, nullptr);
    return TRUE;
}

BOOL cx_pointer_set_position(rdpContext* context, UINT32 x, UINT32 y)
{
    (void)context;
    (void)x;
    (void)y;
    return TRUE;
}

BOOL cx_post_connect(freerdp* instance)
{
    if (!instance || !instance->context)
        return FALSE;

    auto* session = get_session(instance->context);
    if (!session)
        return FALSE;

    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32))
    {
        set_error(session, "FreeRDP GDI initialization failed.");
        return FALSE;
    }

    instance->context->update->BeginPaint = cx_begin_paint;
    instance->context->update->EndPaint = cx_end_paint;
    instance->context->update->DesktopResize = cx_desktop_resize;

    if (instance->context->graphics)
    {
        rdpPointer pointer{};
        pointer.size = sizeof(CxRdpPointer);
        pointer.New = cx_pointer_new;
        pointer.Free = cx_pointer_free;
        pointer.Set = cx_pointer_set;
        pointer.SetNull = cx_pointer_set_null;
        pointer.SetDefault = cx_pointer_set_default;
        pointer.SetPosition = cx_pointer_set_position;
        graphics_register_pointer(instance->context->graphics, &pointer);
    }

    notify_status(session, "RDP connected.");
    return TRUE;
}

void cx_post_disconnect(freerdp* instance)
{
    if (!instance || !instance->context)
        return;

    auto* session = get_session(instance->context);
    gdi_free(instance);

    if (session)
    {
        const bool wasConnected = session->connected.exchange(false);
        session->connected = false;
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            session->cliprdr = nullptr;
            session->cliprdr_wire_active = false;
            session->cliprdr_ready = false;
            session->cliprdr_activation_pending = false;
            session->cliprdr_wait_logged = false;
            session->client_capabilities_sent = false;
            session->clipboard_requested_format = 0;
            session->offered_clipboard_revision = 0;
        }
        notify_status(session, "RDP disconnected.");
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        if (wasConnected && session->disconnect_callback)
            session->disconnect_callback(session->user_data);
    }
}

void connection_thread(CxRdpSession* session)
{
    notify_status(session, "Connecting RDP...");

    std::ostringstream target;
    target << "RDP target host=" << session->host << " port=" << session->port
           << " domain=" << session->domain
           << " user=" << session->username
           << " size=" << session->width << "x" << session->height
           << " colorDepth=" << session->color_depth;
    notify_status(session, target.str().c_str());

    const SecurityProfile defaultProfiles[] = {
        { "negotiate", true, true, true, true, false },
        { "nla", true, false, false, false, false },
        { "tls", false, true, false, false, false },
        { "rdp", false, false, true, false, true }
    };

    std::vector<SecurityProfile> profiles;
    std::string preferredMode = session->preferred_security_mode;
    if (!preferredMode.empty())
    {
        for (const auto& candidate : defaultProfiles)
        {
            if (preferredMode == candidate.name)
            {
                profiles.push_back(candidate);
                break;
            }
        }
    }
    if (profiles.empty() || session->allow_security_fallback)
    {
        for (const auto& candidate : defaultProfiles)
        {
            if (!profiles.empty() && std::string(profiles[0].name) == candidate.name)
                continue;
            profiles.push_back(candidate);
        }
    }

    const bool allowClipboardChannel = is_clipboard_channel_allowed();
    bool retriedWithoutClipboard = false;

    while (session->running)
    {
        session->clipboard_channel_enabled = allowClipboardChannel && !retriedWithoutClipboard;
        session->connected = false;
        {
            std::lock_guard<std::mutex> lock(session->clipboard_mutex);
            session->cliprdr = nullptr;
            session->cliprdr_wire_active = false;
            session->cliprdr_ready = false;
            session->cliprdr_activation_pending = false;
            session->cliprdr_wait_logged = false;
            session->client_capabilities_sent = false;
            session->clipboard_requested_format = 0;
            session->offered_clipboard_revision = 0;
        }

        notify_status(
            session,
            session->clipboard_channel_enabled
                ? "RDP clipboard channel enabled."
                : "RDP clipboard channel disabled.");

        std::string lastError;
        for (const auto& profile : profiles)
        {
            if (!session->running)
                break;

            if (!initialize_instance(session))
            {
                lastError = "FreeRDP instance initialization failed.";
                set_error(session, lastError);
                notify_status(session, lastError.c_str());
                break;
            }

            auto* settings = get_settings(session);
            if (!settings)
            {
                lastError = "FreeRDP settings unavailable.";
                set_error(session, lastError);
                notify_status(session, lastError.c_str());
                free_instance(session);
                break;
            }

            freerdp_settings_set_string(settings, FreeRDP_ServerHostname, session->host.c_str());
            freerdp_settings_set_string(settings, FreeRDP_UserSpecifiedServerName, session->host.c_str());
            freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, static_cast<uint32_t>(session->port > 0 ? session->port : 3389));
            freerdp_settings_set_string(settings, FreeRDP_Username, session->username.c_str());
            freerdp_settings_set_string(settings, FreeRDP_Password, session->password.c_str());
            freerdp_settings_set_string(settings, FreeRDP_Domain, session->domain.c_str());
            freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, static_cast<uint32_t>(session->width > 0 ? session->width : 1024));
            freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, static_cast<uint32_t>(session->height > 0 ? session->height : 768));
            if (session->desktop_scale_factor >= 100 && session->desktop_scale_factor <= 500)
            {
                freerdp_settings_set_uint32(settings, FreeRDP_DesktopScaleFactor, session->desktop_scale_factor);
                uint32_t deviceScale = session->device_scale_factor;
                if (deviceScale != 100u && deviceScale != 140u && deviceScale != 180u)
                {
                    deviceScale = session->desktop_scale_factor >= 180u
                        ? 180u
                        : (session->desktop_scale_factor >= 140u ? 140u : 100u);
                }
                freerdp_settings_set_uint32(settings, FreeRDP_DeviceScaleFactor, deviceScale);
            }
            freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, static_cast<uint32_t>(session->color_depth));
            freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_AutoAcceptCertificate, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_Authentication, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_LogonNotify, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_LogonErrors, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_LongCredentialsSupported, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, FALSE);
            freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, FALSE);
            freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, profile.negotiate ? TRUE : FALSE);
            freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, profile.nla ? TRUE : FALSE);
            freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, profile.tls ? TRUE : FALSE);
            freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, profile.rdp ? TRUE : FALSE);
            freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, profile.useRdpSecurityLayer ? TRUE : FALSE);
            freerdp_settings_set_uint32(
                settings,
                FreeRDP_KeyboardHook,
                session->apply_key_combinations_remotely ? 1u : 0u);

            if (!configure_drive_redirection(session, settings))
            {
                lastError = session->last_error;
                notify_status(session, lastError.c_str());
                free_instance(session);
                break;
            }

            if (!configure_audio_redirection(session, settings))
            {
                lastError = session->last_error;
                notify_status(session, lastError.c_str());
                free_instance(session);
                break;
            }

            if (session->drive_redirection_enabled)
            {
                std::ostringstream driveStatus;
                driveStatus << "RDP redirected drive name=" << session->drive_name
                            << " path=" << session->drive_path;
                notify_status(session, driveStatus.str().c_str());
            }

            std::ostringstream audioStatus;
            audioStatus << "RDP audio playbackMode=" << session->audio_playback_mode
                        << " microphone=" << (session->microphone_enabled ? "enabled" : "disabled");
            notify_status(session, audioStatus.str().c_str());

            notify_status(
                session,
                session->apply_key_combinations_remotely
                    ? "RDP key combinations are applied to the remote computer."
                    : "RDP key combinations are applied to the local computer.");

            const char* parsedServerName = freerdp_settings_get_server_name(settings);
            const char* parsedHostName = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
            const char* parsedUserSpecifiedName = freerdp_settings_get_string(settings, FreeRDP_UserSpecifiedServerName);
            const UINT32 parsedPort = freerdp_settings_get_uint32(settings, FreeRDP_ServerPort);
            std::ostringstream parsedTarget;
            parsedTarget << "RDP parsed target server="
                         << (parsedServerName ? parsedServerName : "<null>")
                         << " host="
                         << (parsedHostName ? parsedHostName : "<null>")
                         << " userSpecified="
                         << (parsedUserSpecifiedName ? parsedUserSpecifiedName : "<null>")
                         << " port="
                         << parsedPort;
            notify_status(session, parsedTarget.str().c_str());

            std::ostringstream security;
            security << "RDP security attempt mode=" << profile.name
                     << " nla=" << (freerdp_settings_get_bool(settings, FreeRDP_NlaSecurity) ? "true" : "false")
                     << " tls=" << (freerdp_settings_get_bool(settings, FreeRDP_TlsSecurity) ? "true" : "false")
                     << " rdp=" << (freerdp_settings_get_bool(settings, FreeRDP_RdpSecurity) ? "true" : "false")
                     << " negotiate=" << (freerdp_settings_get_bool(settings, FreeRDP_NegotiateSecurityLayer) ? "true" : "false");
            notify_status(session, security.str().c_str());

            if (!freerdp_connect(session->instance))
            {
                lastError = describe_last_error(session);
                std::ostringstream attemptError;
                attemptError << lastError << " mode=" << profile.name;
                set_error(session, attemptError.str());
                notify_status(session, attemptError.str().c_str());
                free_instance(session);
                continue;
            }

            session->connected = true;
            break;
        }

        if (!session->connected)
        {
            if (session->running && session->clipboard_channel_enabled && !retriedWithoutClipboard)
            {
                retriedWithoutClipboard = true;
                notify_status(session, "RDP clipboard channel connect failed; retrying without clipboard.");
                continue;
            }

            session->running = false;
            if (!lastError.empty())
                set_error(session, lastError);
            return;
        }

        const auto connectedAt = std::chrono::steady_clock::now();
        while (session->running && !freerdp_shall_disconnect_context(session->instance->context))
        {
            if (session->clipboard_channel_enabled)
            {
                activate_pending_cliprdr(session);
                flush_local_clipboard_format_list(session);
                if (!process_rdp_events(session))
                    break;
                activate_pending_cliprdr(session);
                flush_local_clipboard_format_list(session);
            }
            else if (freerdp_check_fds(session->instance) != TRUE)
            {
                set_error(session, "FreeRDP transport closed.");
                break;
            }
        }

        const auto connectedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - connectedAt).count();
        const bool shouldRetryWithoutClipboard =
            session->running &&
            session->clipboard_channel_enabled &&
            !retriedWithoutClipboard &&
            connectedMilliseconds < 2000;

        freerdp_disconnect(session->instance);
        free_instance(session);

        if (shouldRetryWithoutClipboard)
        {
            retriedWithoutClipboard = true;
            notify_status(session, "RDP clipboard channel closed the session early; retrying without clipboard.");
            continue;
        }

        session->running = false;
        return;
    }

    session->running = false;
}
}

extern "C"
{
CX_RDP_API uint32_t cxrdp_get_api_version(void)
{
    return CX_RDP_BRIDGE_API_VERSION;
}

CX_RDP_API uint32_t cxrdp_get_capabilities(void)
{
    uint32_t capabilities = CX_RDP_CAPABILITY_CLIPBOARD |
                            CX_RDP_CAPABILITY_DRIVE_REDIRECTION |
                            CX_RDP_CAPABILITY_KEYBOARD_HOOK |
                            CX_RDP_CAPABILITY_DISPLAY_SCALE |
                            CX_RDP_CAPABILITY_SECURITY_MODE;
    if (has_native_audio_backend())
        capabilities |= CX_RDP_CAPABILITY_AUDIO_PLAYBACK | CX_RDP_CAPABILITY_MICROPHONE;
    return capabilities |
           CX_RDP_CAPABILITY_CLIPBOARD_IMAGE |
           CX_RDP_CAPABILITY_CLIPBOARD_FILES |
           CX_RDP_CAPABILITY_DYNAMIC_DISPLAY |
           CX_RDP_CAPABILITY_POINTER_SHAPE |
           CX_RDP_CAPABILITY_DIRTY_RECT;
}

CX_RDP_API void* cxrdp_create(void)
{
    auto* session = new CxRdpSession();
    return session;
}

CX_RDP_API void cxrdp_destroy(void* handle)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    cxrdp_disconnect(handle);
    free_instance(session);
    delete session;
}

CX_RDP_API void cxrdp_set_callbacks(
    void* handle,
    cxrdp_frame_callback frame_callback,
    cxrdp_status_callback status_callback,
    cxrdp_disconnect_callback disconnect_callback,
    void* user_data)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->frame_callback = frame_callback;
    session->status_callback = status_callback;
    session->disconnect_callback = disconnect_callback;
    session->user_data = user_data;
}

CX_RDP_API void cxrdp_set_clipboard_callback(
    void* handle,
    cxrdp_clipboard_text_callback clipboard_text_callback)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->clipboard_text_callback = clipboard_text_callback;
}

CX_RDP_API void cxrdp_set_clipboard_image_callback(
    void* handle,
    cxrdp_clipboard_image_callback clipboard_image_callback)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->clipboard_image_callback = clipboard_image_callback;
}

CX_RDP_API void cxrdp_set_clipboard_files_callback(
    void* handle,
    cxrdp_clipboard_files_callback clipboard_files_callback)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->clipboard_files_callback = clipboard_files_callback;
}

CX_RDP_API void cxrdp_set_pointer_callback(
    void* handle,
    cxrdp_pointer_callback pointer_callback)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->pointer_callback = pointer_callback;
}

CX_RDP_API void cxrdp_set_frame_rect_callback(
    void* handle,
    cxrdp_frame_rect_callback frame_rect_callback)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->frame_rect_callback = frame_rect_callback;
}

CX_RDP_API int cxrdp_connect(
    void* handle,
    const char* host,
    int port,
    const char* username,
    const char* password,
    int width,
    int height,
    int color_depth)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return -1;

    if (session->running)
        return 0;

#if defined(_WIN32)
    if (!session->wsa_started)
    {
        WSADATA wsaData{};
        const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaResult != 0)
        {
            std::ostringstream error;
            error << "Winsock initialization failed. code=" << wsaResult;
            set_error(session, error.str());
            notify_status(session, error.str().c_str());
            return -2;
        }

        session->wsa_started = true;
    }
#endif

    session->host = host ? host : "";
    session->port = port > 0 ? port : 3389;
    const auto credentials = parse_rdp_credentials(username);
    session->username = credentials.username;
    session->domain = credentials.domain;
    session->password = password ? password : "";
    session->width = width > 0 ? width : 1024;
    session->height = height > 0 ? height : 768;
    session->color_depth = color_depth == 15 || color_depth == 16 || color_depth == 24 || color_depth == 32
        ? color_depth
        : 32;
    ensure_openssl_providers(session);

    session->running = true;
    session->worker = std::thread(connection_thread, session);
    return 0;
}

CX_RDP_API void cxrdp_disconnect(void* handle)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    session->running = false;
    if (session->instance)
        freerdp_abort_connect(session->instance);

    if (session->worker.joinable())
        session->worker.join();

#if defined(_WIN32)
    if (session->wsa_started)
    {
        WSACleanup();
        session->wsa_started = false;
    }
#endif
}

CX_RDP_API void cxrdp_send_pointer(void* handle, uint16_t flags, uint16_t x, uint16_t y)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    auto* input = session && session->instance && session->instance->context ? session->instance->context->input : nullptr;
    if (!session || !input || !session->connected)
        return;

    freerdp_input_send_mouse_event(input, flags, x, y);
}

CX_RDP_API void cxrdp_send_key(void* handle, uint32_t key, int down)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    auto* input = session && session->instance && session->instance->context ? session->instance->context->input : nullptr;
    if (!session || !input || !session->connected)
        return;

    freerdp_input_send_keyboard_event_ex(input, down ? TRUE : FALSE, FALSE, key);
}

CX_RDP_API void cxrdp_send_unicode_key(void* handle, uint16_t code, int down)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    auto* input = session && session->instance && session->instance->context ? session->instance->context->input : nullptr;
    if (!session || !input || !session->connected || code == 0)
        return;

    freerdp_input_send_unicode_keyboard_event(input, down ? 0 : KBD_FLAGS_RELEASE, code);
}

CX_RDP_API void cxrdp_set_clipboard_text(void* handle, const char* text)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->local_clipboard_kind = 0;
        session->local_clipboard_text = text ? text : "";
        session->local_clipboard_png.clear();
        session->local_clipboard_dib.clear();
        session->local_clipboard_files.clear();
        ++session->local_clipboard_revision;
    }
}

CX_RDP_API void cxrdp_set_clipboard_image(
    void* handle,
    const uint8_t* png_data,
    uint32_t png_length,
    const uint8_t* dib_data,
    uint32_t dib_length)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return;

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->local_clipboard_kind = 1;
        session->local_clipboard_text.clear();
        session->local_clipboard_files.clear();
        if (png_data && png_length > 0)
            session->local_clipboard_png.assign(png_data, png_data + png_length);
        else
            session->local_clipboard_png.clear();

        if (dib_data && dib_length > 0)
            session->local_clipboard_dib.assign(dib_data, dib_data + dib_length);
        else
            session->local_clipboard_dib.clear();

        ++session->local_clipboard_revision;
    }
}

CX_RDP_API void cxrdp_set_clipboard_files(void* handle, const char* file_paths_newline_separated)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session || !file_paths_newline_separated)
        return;

    std::vector<LocalClipboardFileEntry> entries;
    std::istringstream stream(file_paths_newline_separated);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        std::error_code ec;
        const auto rootPath = std::filesystem::u8path(line);
        if (!std::filesystem::exists(rootPath, ec))
            continue;

        const auto rootName = rootPath.filename().u8string();
        if (rootName.empty())
            continue;

        if (std::filesystem::is_directory(rootPath, ec))
        {
            entries.push_back({rootName, rootPath, true, 0});
            for (const auto& item : std::filesystem::recursive_directory_iterator(rootPath, ec))
            {
                if (ec)
                    break;
                const auto rel = std::filesystem::relative(item.path(), rootPath, ec);
                if (ec)
                    continue;
                std::string relName = rootName + "\\" + rel.u8string();
                for (char& ch : relName)
                {
                    if (ch == '/')
                        ch = '\\';
                }
                const bool isDir = item.is_directory(ec);
                const uint64_t size = isDir ? 0 : item.file_size(ec);
                entries.push_back({relName, item.path(), isDir, size});
            }
        }
        else if (std::filesystem::is_regular_file(rootPath, ec))
        {
            const uint64_t size = std::filesystem::file_size(rootPath, ec);
            entries.push_back({rootName, rootPath, false, size});
        }
    }

    if (entries.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(session->clipboard_mutex);
        session->local_clipboard_kind = 2;
        session->local_clipboard_text.clear();
        session->local_clipboard_png.clear();
        session->local_clipboard_dib.clear();
        session->local_clipboard_files = std::move(entries);
        ++session->local_clipboard_revision;
    }
}

CX_RDP_API int cxrdp_set_drive_redirection(
    void* handle,
    int enabled,
    const char* drive_name,
    const char* local_path)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return -1;

    if (session->running)
    {
        set_error(session, "RDP drive redirection must be configured before connecting.");
        return -2;
    }

    session->drive_redirection_enabled = enabled != 0;
    session->drive_name = trim_copy(drive_name ? drive_name : "");
    session->drive_path = trim_copy(local_path ? local_path : "");

    if (session->drive_redirection_enabled &&
        (session->drive_name.empty() || session->drive_path.empty()))
    {
        set_error(session, "RDP drive name and local path are required.");
        session->drive_redirection_enabled = false;
        return -3;
    }

    return 0;
}

CX_RDP_API int cxrdp_set_audio_redirection(
    void* handle,
    int playback_mode,
    int microphone_enabled)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return -1;

    if (session->running)
    {
        set_error(session, "RDP audio redirection must be configured before connecting.");
        return -2;
    }

    if (playback_mode < CX_RDP_AUDIO_PLAY_LOCAL || playback_mode > CX_RDP_AUDIO_DO_NOT_PLAY)
    {
        set_error(session, "Invalid RDP audio playback mode.");
        return -3;
    }

    if (playback_mode == CX_RDP_AUDIO_PLAY_LOCAL && !has_native_audio_backend())
    {
        set_error(session, "This CxRdpBridge build does not include a local audio playback backend.");
        return -4;
    }

    if (microphone_enabled != 0 && !has_native_audio_backend())
    {
        set_error(session, "This CxRdpBridge build does not include a microphone backend.");
        return -5;
    }

    session->audio_playback_mode = playback_mode;
    session->microphone_enabled = microphone_enabled != 0;
    return 0;
}

CX_RDP_API int cxrdp_set_keyboard_options(
    void* handle,
    int apply_key_combinations_remotely)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return -1;

    if (session->running)
    {
        set_error(session, "RDP keyboard options must be configured before connecting.");
        return -2;
    }

    session->apply_key_combinations_remotely = apply_key_combinations_remotely != 0;
    return 0;
}

CX_RDP_API int cxrdp_set_display_scale(
    void* handle,
    uint32_t desktop_scale_factor,
    uint32_t device_scale_factor)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return -1;

    if (session->running)
    {
        session->last_error = "RDP display scale must be configured before connecting.";
        return -2;
    }

    if (desktop_scale_factor < 100u)
        desktop_scale_factor = 100u;
    else if (desktop_scale_factor > 500u)
        desktop_scale_factor = 500u;

    if (device_scale_factor != 100u && device_scale_factor != 140u && device_scale_factor != 180u)
    {
        device_scale_factor = desktop_scale_factor >= 180u
            ? 180u
            : (desktop_scale_factor >= 140u ? 140u : 100u);
    }

    session->desktop_scale_factor = desktop_scale_factor;
    session->device_scale_factor = device_scale_factor;
    return 0;
}

CX_RDP_API int cxrdp_resize_display(
    void* handle,
    int width,
    int height,
    uint32_t desktop_scale_factor,
    uint32_t device_scale_factor)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session || !session->connected)
        return -1;

    DispClientContext* disp = nullptr;
    {
        std::lock_guard<std::mutex> lock(session->disp_mutex);
        disp = session->disp;
    }

    if (!disp || !disp->SendMonitorLayout)
        return -2;

    int clampedWidth = width < 200 ? 200 : (width > 8192 ? 8192 : width);
    int clampedHeight = height < 200 ? 200 : (height > 8192 ? 8192 : height);
    clampedWidth &= ~1;
    clampedHeight &= ~1;

    DISPLAY_CONTROL_MONITOR_LAYOUT layout{};
    layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
    layout.Left = 0;
    layout.Top = 0;
    layout.Width = static_cast<UINT32>(clampedWidth);
    layout.Height = static_cast<UINT32>(clampedHeight);
    layout.PhysicalWidth = static_cast<UINT32>(clampedWidth);
    layout.PhysicalHeight = static_cast<UINT32>(clampedHeight);
    layout.Orientation = ORIENTATION_LANDSCAPE;
    layout.DesktopScaleFactor = desktop_scale_factor >= 100 && desktop_scale_factor <= 500 ? desktop_scale_factor : 100;
    layout.DeviceScaleFactor = device_scale_factor == 180 ? 180 : (device_scale_factor == 140 ? 140 : 100);

    const UINT rc = disp->SendMonitorLayout(disp, 1, &layout);
    return rc == CHANNEL_RC_OK ? 0 : -3;
}

CX_RDP_API int cxrdp_set_security_mode(
    void* handle,
    const char* preferred_mode,
    int allow_fallback)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return -1;

    if (session->running)
    {
        session->last_error = "RDP security mode must be configured before connecting.";
        return -2;
    }

    std::string normalized;
    if (preferred_mode)
    {
        for (const char* p = preferred_mode; *p; ++p)
        {
            unsigned char ch = static_cast<unsigned char>(*p);
            if (ch >= 'A' && ch <= 'Z')
                normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
            else if (ch > ' ')
                normalized.push_back(static_cast<char>(ch));
        }
    }

    if (normalized == "negotiate" || normalized == "nla" || normalized == "tls" || normalized == "rdp")
        session->preferred_security_mode = normalized;
    else
        session->preferred_security_mode.clear();

    session->allow_security_fallback = allow_fallback != 0;
    return 0;
}

CX_RDP_API const char* cxrdp_get_last_error(void* handle)
{
    auto* session = static_cast<CxRdpSession*>(handle);
    if (!session)
        return "";

    return session->last_error.c_str();
}
}
