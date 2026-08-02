#include "win/credential_store.hpp"

#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>

#include <cstdio>
#include <vector>

#include "util/log.hpp"

namespace coax::win {
namespace {

// Ties the ciphertext to this application so another program cannot decrypt it
// merely by running as the same user.
constexpr const wchar_t* kEntropyText = L"coax-native/portal/v1";

DATA_BLOB entropy_blob() {
    DATA_BLOB blob{};
    blob.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kEntropyText));
    blob.cbData = static_cast<DWORD>(wcslen(kEntropyText) * sizeof(wchar_t));
    return blob;
}

}  // namespace

std::wstring CredentialStore::storage_path() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data))) {
        return {};
    }

    std::wstring directory = local_app_data;
    CoTaskMemFree(local_app_data);

    directory += L"\\Coax";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\portal.dat";
}

bool CredentialStore::save(const std::string& plaintext) {
    const std::wstring path = storage_path();
    if (path.empty()) {
        return false;
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB entropy = entropy_blob();
    DATA_BLOB output{};

    if (!CryptProtectData(&input, L"Coax portal", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        log::warn("Encrypting the saved portal failed ({})", GetLastError());
        return false;
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(file, output.pbData, output.cbData, &written, nullptr) &&
             written == output.cbData;
        CloseHandle(file);
    }

    LocalFree(output.pbData);
    return ok;
}

bool CredentialStore::load(std::string& plaintext) {
    const std::wstring path = storage_path();
    if (path.empty()) {
        return false;
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
        CloseHandle(file);
        return false;
    }

    std::vector<BYTE> ciphertext(static_cast<std::size_t>(size.QuadPart));
    DWORD             read = 0;
    const bool        read_ok =
        ReadFile(file, ciphertext.data(), static_cast<DWORD>(ciphertext.size()), &read, nullptr);
    CloseHandle(file);

    if (!read_ok || read != ciphertext.size()) {
        return false;
    }

    DATA_BLOB input{};
    input.pbData = ciphertext.data();
    input.cbData = static_cast<DWORD>(ciphertext.size());

    DATA_BLOB entropy = entropy_blob();
    DATA_BLOB output{};

    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        log::warn("Stored portal could not be decrypted; ignoring it");
        return false;
    }

    plaintext.assign(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

void CredentialStore::clear() {
    const std::wstring path = storage_path();
    if (!path.empty()) {
        DeleteFileW(path.c_str());
    }
}

}  // namespace coax::win
