#include "win/file_write.hpp"

#include <windows.h>

#include "util/log.hpp"

namespace coax::win {

bool write_file_atomically(const std::wstring& path, std::string_view bytes,
                           std::string_view what) {
    const std::wstring temporary = path + L".tmp";

    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log::warn("{} could not be written ({})", what, GetLastError());
        return false;
    }

    DWORD      written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr) &&
                    written == bytes.size();
    // Read before CloseHandle: the last-error value belongs to the thread, and
    // a *successful* close is free to overwrite it. Zero for a short write,
    // which is honest — nothing failed, it just did not all arrive.
    const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();

    // Before the rename, which orders the directory entry and says nothing
    // about whether the bytes it points at have reached the disk. And answered
    // for: a flush that failed has committed nothing, so renaming anyway trades
    // a file that is definitely good for one that might be.
    const bool  flushed     = ok && FlushFileBuffers(file);
    const DWORD flush_error = (ok && !flushed) ? GetLastError() : ERROR_SUCCESS;

    CloseHandle(file);

    if (!ok) {
        DeleteFileW(temporary.c_str());
        log::warn("{} was written incompletely ({})", what, write_error);
        return false;
    }

    if (!flushed) {
        DeleteFileW(temporary.c_str());
        log::warn("{} could not be committed to disk ({})", what, flush_error);
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        // The destination still holds the previous contents, which is the whole
        // point. Clearing the temporary keeps a failure from leaving litter
        // next to it that looks like a settings file.
        DeleteFileW(temporary.c_str());
        log::warn("{} could not replace its previous copy ({})", what, error);
        return false;
    }

    return true;
}

}  // namespace coax::win
