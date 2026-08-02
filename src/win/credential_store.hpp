#pragma once

#include <string>

namespace coax::win {

// User-bound credential storage backed by DPAPI. The blob is decryptable only
// by the same Windows user on the same machine, so the saved portal never sits
// on disk in plaintext even in the POC.
class CredentialStore {
public:
    // Encrypts and writes to %LOCALAPPDATA%\Coax\portal.dat.
    static bool save(const std::string& plaintext);

    // Returns false when nothing is stored or the blob cannot be decrypted,
    // which is the expected outcome on a different machine or user account.
    static bool load(std::string& plaintext);

    static void clear();

private:
    static std::wstring storage_path();
};

}  // namespace coax::win
