#include "preview.hpp"

#include <windows.h>
#include <winhttp.h>
#include <memory>
#include <string_view>

struct pollfd;
extern "C" int poll(pollfd*, unsigned, int);
#include <vlc_common.h>
#include <vlc_block.h>
#include <vlc_picture.h>
#include <vlc_vout.h>

namespace addon {
namespace {
using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

std::string post_frame(const std::wstring& endpoint, block_t& jpeg)
{
    URL_COMPONENTS url{};
    url.dwStructSize = sizeof(url);
    url.dwHostNameLength = url.dwUrlPathLength = url.dwExtraInfoLength = DWORD(-1);
    url.dwUserNameLength = url.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(endpoint.c_str(), 0, 0, &url) || url.nScheme != INTERNET_SCHEME_HTTPS
        || !url.dwHostNameLength || url.dwUserNameLength || url.dwPasswordLength) return {};

    const std::wstring host(url.lpszHostName, url.dwHostNameLength);
    std::wstring path = url.dwUrlPathLength ? std::wstring(url.lpszUrlPath, url.dwUrlPathLength) : L"/";
    if (url.dwExtraInfoLength) path.append(url.lpszExtraInfo, url.dwExtraInfoLength);
    HttpHandle session(WinHttpOpen(L"WatchKit", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    if (!session || !WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, 3000)) return {};
    HttpHandle connection(WinHttpConnect(session.get(), host.c_str(), url.nPort, 0), WinHttpCloseHandle);
    if (!connection) return {};
    HttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                       WinHttpCloseHandle);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    const auto length = static_cast<DWORD>(jpeg.i_buffer);
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))
        || !WinHttpSendRequest(request.get(), L"Content-Type: image/jpeg\r\n", DWORD(-1), jpeg.p_buffer, length, length, 0)
        || !WinHttpReceiveResponse(request.get(), nullptr)) return {};
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)
        || status < 200 || status >= 300) return {};

    std::string response;
    char buffer[513];
    while (response.size() <= 512) {
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &received)) return {};
        if (!received) break;
        response.append(buffer, received);
    }
    const auto first = response.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || response.size() > 512) return {};
    response = response.substr(first, response.find_last_not_of(" \t\r\n") - first + 1);
    if (!response.starts_with("https://") || response.find_first_of(" \t\r\n") != std::string::npos) return {};
    return response;
}
}

std::string upload_video_frame(vout_thread_t* output, const std::wstring& endpoint)
{
    const auto release = [](vout_thread_t* value) { vlc_object_release(value); };
    std::unique_ptr<vout_thread_t, decltype(release)> vout(output, release);
    picture_t* raw = nullptr;
    if (vout_GetSnapshot(output, nullptr, &raw, nullptr, nullptr, VLC_TICK_FROM_MS(400)) || !raw)
        return {};
    std::unique_ptr<picture_t, decltype(&picture_Release)> picture(raw, picture_Release);
    const auto& format = picture->format;
    const bool landscape = format.i_visible_width * double(format.i_sar_num ? format.i_sar_num : 1)
        >= format.i_visible_height * double(format.i_sar_den ? format.i_sar_den : 1);
    block_t* encoded = nullptr;
    if (picture_Export(VLC_OBJECT(output), &encoded, nullptr, raw, VLC_CODEC_JPEG,
                       landscape ? 512 : 0, landscape ? 0 : 512) || !encoded) return {};
    std::unique_ptr<block_t, decltype(&block_Release)> jpeg(encoded, block_Release);
    picture.reset();
    vout.reset();
    return jpeg->i_buffer <= 1024 * 1024 ? post_frame(endpoint, *jpeg) : std::string{};
}
}
