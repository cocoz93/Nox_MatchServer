// LaunchKey.h — 런처 자동 로그인 열쇠를 이 PC 에 두고 꺼내는 부품(윈도 사용자 암호화 DPAPI · 머리 파일만).
//   런처(client_mod/launcher)가 쓰고 · DLL(wsock32_proxy)이 읽는다 — 같은 코드를 둘이 본다(`LoginLine.h` 와 같은 꼴).
//   설계 Docs/LAUNCHER_PLAN.md · 11-01 사용자 결정 2(자동 로그인 = 열쇠를 PC 에 암호화)
//
// 자리 — %LOCALAPPDATA%\NoxOnline\keys\<계정 이름 16진>.key
//   · 이름을 16진으로 — 윈도 파일 이름은 대소문자를 안 가리는데 계정은 가린다(`wn` 과 `Wn` 이 다른 계정 — db_test ⑧)
//   · 계정마다 한 파일 — 한 PC 에서 클라 둘(mo_local 의 Game.exe · Game2.exe)이 다른 계정으로 떠도 안 섞인다
//   · 시험은 LOCALAPPDATA 환경 변수를 임시 폴더로 바꿔 쓴다(사용자 프로필을 안 건드림)
// 내용 — `NXK1<16진 64자>` 를 CryptProtectData(이 윈도 사용자만 풂 · 다른 PC 로 옮기면 못 풂) + 덧댄 값(아래 kEntropy)
//   열쇠는 환경 변수 · 명령 줄로 넘기지 않는다 — 같은 사용자의 다른 프로그램이 프로세스 환경을 읽을 수 있어서(LAUNCHER_RESEARCH 「런처 → DLL 넘김」)
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

#include <string>

#pragma comment(lib, "crypt32.lib")

namespace launchkey {

constexpr size_t kMaxName = 16;      // 로그인 토큰의 이름 칸(LoginToken.h kLoginNameSize) — 이름 규칙은 9자
constexpr size_t kMaxBlob = 4096;    // 이보다 큰 파일은 우리 열쇠 파일이 아니다
static const char kEntropy[] = "NoxOnline launch key v1";   // 덧댄 값 — 다른 프로그램이 같은 사용자로 DPAPI 를 불러도 이 값 없이는 못 풂(약한 막이 하나 더)

inline bool NameOk(const std::string& name)
{
    if (name.empty() || name.size() > kMaxName) return false;
    for (const char c : name)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) return false;
    return true;
}

// %LOCALAPPDATA%\NoxOnline\keys — 없으면 빈 글
inline std::wstring KeyDir()
{
    wchar_t base[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(base) + L"\\NoxOnline\\keys";
}

inline std::wstring KeyPath(const std::string& name)
{
    if (!NameOk(name)) return std::wstring();
    const std::wstring dir = KeyDir();
    if (dir.empty()) return std::wstring();
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring file;
    for (const char c : name) {
        file.push_back(hex[static_cast<unsigned char>(c) >> 4]);
        file.push_back(hex[static_cast<unsigned char>(c) & 15]);
    }
    return dir + L"\\" + file + L".key";
}

inline bool Save(const std::string& name, const std::string& key)
{
    const std::wstring path = KeyPath(name);
    if (path.empty() || key.empty()) return false;
    const std::wstring dir = KeyDir();
    const std::wstring parent = dir.substr(0, dir.rfind(L'\\'));
    CreateDirectoryW(parent.substr(0, parent.rfind(L'\\')).c_str(), NULL);   // LOCALAPPDATA 자체(시험이 임시 폴더로 바꿨을 때) · 이미 있으면 실패해도 됨
    CreateDirectoryW(parent.c_str(), NULL);
    CreateDirectoryW(dir.c_str(), NULL);
    DATA_BLOB in = { static_cast<DWORD>(key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(key.data())) };
    DATA_BLOB ent = { static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)) };
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, L"NoxOnline", &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    // 다른 이름으로 다 쓴 뒤 바꿔 끼운다(쓰다 끊겨도 반쪽 파일이 안 남게)
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = h != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD wrote = 0;
        ok = WriteFile(h, out.pbData, out.cbData, &wrote, NULL) && wrote == out.cbData;
        CloseHandle(h);
    }
    LocalFree(out.pbData);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// 없거나 못 풀면 false(key 는 빈 글)
inline bool Load(const std::string& name, std::string& key)
{
    key.clear();
    const std::wstring path = KeyPath(name);
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string blob;
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= static_cast<LONGLONG>(kMaxBlob);
    if (ok) {
        blob.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, &blob[0], static_cast<DWORD>(blob.size()), &got, NULL) && got == blob.size();
    }
    CloseHandle(h);
    if (!ok) return false;
    DATA_BLOB in = { static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(&blob[0]) };
    DATA_BLOB ent = { static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)) };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, NULL, &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    key.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return !key.empty();
}

inline bool Remove(const std::string& name)
{
    const std::wstring path = KeyPath(name);
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
}

}  // namespace launchkey
