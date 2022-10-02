#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <winioctl.h>

#pragma comment(lib, "ws2_32.lib")

#include <cstdint>
#include <iostream>
#include <string>
#include <optional>
#include <unordered_set>
#include <sstream>
#include <algorithm>
#include <execution>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <mutex>

using namespace std;

constexpr wchar_t StartDrive = L'D';
constexpr wchar_t EndDrive = L'Z';
constexpr wchar_t const* TargetLabel = L"Realtek";
constexpr char const* TestTargetHost = "bspfp.pe.kr";
constexpr chrono::milliseconds LoopInterval = 10s;
constexpr wchar_t const* LogName = L"EjectRealtekCD.log";

constexpr int LockTryCount = 20;
constexpr chrono::milliseconds LockTryInterval = 500ms;

class LogClass {
	wstring ExecFolder;
	wstring LogPath;
	mutex Mtx;
	FILE* fp = nullptr;

public:
	bool Init() {
		try {
			wchar_t buf[4096] = {};
			::GetModuleFileNameW(GetModuleHandleW(nullptr), buf, (DWORD)size(buf));
			ExecFolder = filesystem::path(buf).parent_path();
			LogPath = filesystem::path(ExecFolder) / LogName;

			for (auto& e : filesystem::recursive_directory_iterator(filesystem::path(ExecFolder))) {
				if (e.is_regular_file() && e.exists()) {
					filesystem::path p = e;
					auto ext = p.extension().wstring();
					if (ext.length() <= 0)
						continue;
					auto ext2 = ext.substr(1);
					wchar_t* pEnd = nullptr;
					if (auto n = std::wcstol(ext2.c_str(), &pEnd, 10); pEnd != nullptr && *pEnd == 0 && n >= 9)
						filesystem::remove(e);
				}
			}

			for (auto i = 8; i >= 0; i--) {
				auto oldName = i > 0 ? wstring(LogName) + L"." + to_wstring(i) : wstring(LogName);
				auto oldPath = filesystem::path(ExecFolder) / oldName;
				auto newPath = filesystem::path(ExecFolder) / (wstring(LogName) + L"." + to_wstring(i + 1));
				if (filesystem::exists(oldPath))
					filesystem::rename(oldPath, newPath);
			}

			_wfopen_s(&fp, LogPath.c_str(), L"a, ccs=UTF-8");
			if (fp == nullptr) {
				cerr << "failed open log file" << endl;
				return false;
			}

			return true;
		} catch (...) {
			return false;
		}
	}

	~LogClass() {
		if (fp != nullptr) {
			fclose(fp);
			fp = nullptr;
		}
	}

	void operator()(const wstring& msg) {
		SYSTEMTIME lt = {};
		GetLocalTime(&lt);
		FILE* files[] = { fp,stdout };
		lock_guard lock(Mtx);
		for (auto& file : files) {
			fwprintf_s(file, L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\n", (int)lt.wYear, (int)lt.wMonth, (int)lt.wDay, (int)lt.wHour, (int)lt.wMinute, (int)lt.wSecond, (int)lt.wMilliseconds, msg.c_str());
			fflush(file);
#ifndef _DEBUG
			break;
#endif
		}
	}

	template<typename T1, typename... T2>
	void operator()(const wchar_t* fmt, T1&& arg, T2&&... args) {
		SYSTEMTIME lt = {};
		GetLocalTime(&lt);
		FILE* files[] = { fp,stdout };
		lock_guard lock(Mtx);
		for (auto& file : files) {
			fwprintf_s(file, L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] ", (int)lt.wYear, (int)lt.wMonth, (int)lt.wDay, (int)lt.wHour, (int)lt.wMinute, (int)lt.wSecond, (int)lt.wMilliseconds);
			fwprintf_s(file, fmt, forward<T1>(arg), forward<T2>(args)...);
			fwprintf_s(file, L"\n");
			fflush(file);
#ifndef _DEBUG
			break;
#endif
		}
	}
} Log;

class WinsockInit {
	bool init = false;

public:
	bool Init() {
		if (!init) {
			Log(L"[%s] Initialize Winsock", __FUNCTIONW__);
			WSADATA wsadata = {};
			if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
				auto lasterr = WSAGetLastError();
				Log(L"[%s] failed WSAStartup: error: %x(%d)", __FUNCTIONW__, lasterr, lasterr);
				return false;
			}
			init = true;
		}
		return true;
	}

	~WinsockInit() {
		if (init) {
			WSACleanup();
			init = false;
		}
	}
} _winsockInit;

optional<bool> TestNetwork() {
	Log(L"[%s] Test network", __FUNCTIONW__);

	static unordered_set<int> RetryErrorSet = { 11001, };
	addrinfo hints = {};
	addrinfo* results = nullptr;
	hints.ai_flags = AI_BYPASS_DNS_CACHE;
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (getaddrinfo(TestTargetHost, nullptr, &hints, &results) != 0) {
		auto lasterr = WSAGetLastError();
		if (RetryErrorSet.find(lasterr) != RetryErrorSet.end())
			return false;
		Log(L"[%s] failed getaddrinfo: error: %x(%d)", __FUNCTIONW__, lasterr, lasterr);
		return {};
	}
	return true;
}

class EjectVolume {
	HANDLE volume = INVALID_HANDLE_VALUE;
public:
	pair<bool/*removeSafely*/, bool/*autoEject*/> operator()(wchar_t ch) {
		if (volume != INVALID_HANDLE_VALUE) {
			CloseHandle(volume);
			volume = INVALID_HANDLE_VALUE;
		}

		auto rootName = (std::wstringstream() << ch << L":\\").str();
		DWORD dwDesiredAccess = GENERIC_READ;
		auto driveType = GetDriveTypeW(rootName.c_str());
		switch (driveType) {
		case DRIVE_REMOVABLE:
			dwDesiredAccess |= GENERIC_WRITE;
			break;
		case DRIVE_CDROM:
			break;
		default:
			return { false, false };
		}

		Log(L"[%s] Check volume: %s", __FUNCTIONW__, rootName.c_str());
		auto volumePath = (std::wstringstream() << L"\\\\.\\" << ch << L":").str();
		volume = CreateFileW(volumePath.c_str(), dwDesiredAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (volume == INVALID_HANDLE_VALUE) {
			Log(L"[%s] failed CreateFileW: Drive %c", __FUNCTIONW__, ch);
			return { false, false };
		}

		Log(L"[%s] Try Lock: %c", __FUNCTIONW__, ch);
		DWORD notUsed;
		bool locked = false;
		for (auto i = 0; i < LockTryCount; i++) {
			if (DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &notUsed, nullptr)) {
				Log(L"[%s] Locked: %c", __FUNCTIONW__, ch);
				locked = true;
				break;
			}
			this_thread::sleep_for(LockTryInterval);
		}
		if (locked) {
			if (DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &notUsed, nullptr)) {
				Log(L"[%s] Dismounted: %c", __FUNCTIONW__, ch);
				PREVENT_MEDIA_REMOVAL pmrbuf = {};
				pmrbuf.PreventMediaRemoval = false;

				if (DeviceIoControl(volume, IOCTL_STORAGE_MEDIA_REMOVAL, &pmrbuf, sizeof(pmrbuf), nullptr, 0, &notUsed, nullptr)) {
					Log(L"[%s] Removed storage media: %c", __FUNCTIONW__, ch);
					if (DeviceIoControl(volume, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &notUsed, nullptr)) {
						Log(L"[%s] Ejected media: %c", __FUNCTIONW__, ch);
						return { true, true };
					}
				}

				return { true, false };
			}
		}

		return { false, false };
	}

	~EjectVolume() {
		if (volume != INVALID_HANDLE_VALUE) {
			CloseHandle(volume);
			volume = INVALID_HANDLE_VALUE;
		}
	}
};

int main() {
	if (!Log.Init()) {
		cerr << "failed log init" << endl;
		return 1;
	}

	WinsockInit winsockInit;
	if (!winsockInit.Init())
		return 1;

	wstring driveLetters;
	driveLetters.reserve(26);
	for (auto i = StartDrive; i <= EndDrive; i++)
		driveLetters.push_back(i);

	bool first = true;
	while (true) {
		if (auto resTestNet = TestNetwork(); resTestNet) {
			if (*resTestNet) {
				Log(L"[%s] success test network. exit program", __FUNCTIONW__);
				return 0;
			}
		} else {
			return 1;
		}

		if (first) {
			Log(L"[%s] target drive: ", __FUNCTIONW__, driveLetters.c_str());
			Log(L"[%s] target label: ", __FUNCTIONW__, TargetLabel);

			first = false;
		}

		atomic_bool failed = false;
		atomic_bool ejected = false;
		for_each(
#ifdef _DEBUG
			execution::seq,
#else
			execution::par_unseq,
#endif
			driveLetters.begin(),
			driveLetters.end(),
			[&](wchar_t ch) {
				auto rootName = (std::wstringstream() << ch << L":\\").str();
				wchar_t nameBuffer[MAX_PATH + 1] = {};
				wchar_t sysNameBuffer[MAX_PATH + 1] = {};
				if (!GetVolumeInformationW(rootName.c_str(),
										   nameBuffer, (DWORD)size(nameBuffer),
										   nullptr, nullptr, nullptr,
										   sysNameBuffer, (DWORD)size(sysNameBuffer))) {
					auto lasterr = GetLastError();
					if (lasterr == 3) // ERROR_PATH_NOT_FOUND
						return;

					Log(L"[%s] failed GetVolumeInformationW: error: %x(%d): drive: %c", __FUNCTIONW__, lasterr, lasterr, ch);

					failed = true;
					return;
				}

				if (_wcsicmp(TargetLabel, nameBuffer) != 0)
					return;

				ejected = EjectVolume()(ch).first;
			});

		if (failed)
			return 1;

		if (ejected)
			return 0;

		this_thread::sleep_for(LoopInterval);
	}

	return 0;
}
