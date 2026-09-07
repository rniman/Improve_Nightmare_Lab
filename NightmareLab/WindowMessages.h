#pragma once

#include <Windows.h>

namespace ClientWindowMessage
{
	inline constexpr UINT WM_SOCKET = WM_USER + 1;
	inline constexpr UINT WM_CREATE_TCP = WM_USER + 2;
	inline constexpr UINT WM_END_GAME = WM_USER + 3;
	inline constexpr UINT WM_START_GAME = WM_USER + 4;
	inline constexpr UINT WM_CHANGE_SLOT = WM_USER + 5;
	inline constexpr UINT WM_REQUEST_SEND = WM_USER + 6;
}

namespace ServerWindowMessage
{
	inline constexpr UINT WM_SOCKET = WM_USER + 1;
	inline constexpr UINT WM_SOUND = WM_USER + 2;
	inline constexpr UINT WM_OPENABLE_OBJECT_STATE = WM_USER + 3;
}
