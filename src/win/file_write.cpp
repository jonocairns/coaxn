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
    // Read before CloseHandle. The last-error value belongs to the thread rather
    // than to the call, and a *successful* close is free to overwrite it — so
    // asking afterwards reports whatever happened last instead of what went
    // wrong. Zero when the write itself succeeded but came up short, which is
    // the honest answer: nothing failed, it just did not all arrive.
    const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();

    // Before the rename, not after. The rename orders the directory entry; it
    // says nothing about whether the bytes it now points at have reached the
    // disk. Without this the atomic replace is still atomic and can still leave
    // an empty file after a power cut.
    //
    // And it has to be answered for. A flush that fails has not committed
    // anything, so renaming over the destination anyway would trade a file that
    // is definitely good for one that might be — which is the outcome this
    // function exists to make impossible, arrived at from the other direction.
    // Same reason as the write error above for reading it here: CloseHandle is
    // free to overwrite the thread's last-error on its way out.
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
