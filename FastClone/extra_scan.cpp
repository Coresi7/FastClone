#include "extra_scan.h"

// Shared local-extra enumeration - single implementation for FastClone + FastCheck
// (task unify-probe-extra-shared, design §4.3). The traversal below is the line-by-line
// merge of the two former implementations: compare_phase.cpp's CollectExtraLocal (no
// exclude, files only) and sync_engine_client.cpp's RemoveLocalExtras enumeration phase
// (exclude + extraDirs/localDirs). The two entry templates in extra_scan.h differ only
// in whether directory info is recorded, so both sides get the same extraFiles answer
// for the same (tree, remote set, exclude) triple (FR-09). Deletion stays entirely in
// FastClone's action layer: results are returned by value, so every find handle is
// closed before the caller acts (I-7).

#include "parallel_dir_walk.h"  // fc::detail::ParallelDirWalk / BuildRelPath / ResolveDirWalkWorkerCount / OpenDirFind / kDeleteDirPopBatch
#include "path_utils.h"         // fc::IsPathUnderRoot
#include "sync_util.h"          // fc::ToExtendedLengthPath / fc::WideToUtf8 / fc::CurrentExePath

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <wchar.h>  // _wcsicmp
#endif

namespace fs = std::filesystem;

namespace fc {

namespace detail {

ExtraScanResult ScanExtrasImpl(const fs::path& root,
                               RemotePredicate fileInRemote,
                               RemotePredicate dirInRemote,
                               const ExtraScanOptions& options) {
    ExtraScanResult result;
    std::mutex resultMu;  // merges happen once per worker, not per file
    struct ScanCtx {
        std::vector<std::string> extraFiles;  // per-worker, hot path is lock-free
        std::vector<std::string> extraDirs;
        std::vector<std::string> localDirs;
    };
    const std::atomic<bool> noCancel{false};  // scan pass is not cancellable (as before)
    const unsigned numWorkers = ResolveDirWalkWorkerCount();
    const bool collectDirs = options.collectDirs;

    auto finishWorker = [&](ScanCtx& ctx) {
        std::lock_guard<std::mutex> lock(resultMu);
        result.extraFiles.insert(result.extraFiles.end(),
                                 std::make_move_iterator(ctx.extraFiles.begin()),
                                 std::make_move_iterator(ctx.extraFiles.end()));
        if (collectDirs) {
            result.extraDirs.insert(result.extraDirs.end(),
                                    std::make_move_iterator(ctx.extraDirs.begin()),
                                    std::make_move_iterator(ctx.extraDirs.end()));
            result.localDirs.insert(result.localDirs.end(),
                                    std::make_move_iterator(ctx.localDirs.begin()),
                                    std::make_move_iterator(ctx.localDirs.end()));
        }
        ctx.extraFiles.clear();
        ctx.extraDirs.clear();
        ctx.localDirs.clear();
    };

#ifdef _WIN32
    struct PendingDir {
        std::wstring absDir;
        std::string relDir;
    };
    const std::wstring excludeW =
        options.excludeAbsPath.has_value() ? ToExtendedLengthPath(*options.excludeAbsPath) : L"";
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs, ScanCtx& ctx) {
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = OpenDirFind(current.absDir, fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            return;   // no permission / not found -> skip, not fatal (B-02/B-10)
        }
        do {
            const wchar_t* name = fd.cFileName;
            if ((name[0] == L'.' && name[1] == L'\0') ||
                (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')) {
                continue;
            }
            std::wstring absPath = current.absDir;
            if (!absPath.empty() && absPath.back() != L'\\' && absPath.back() != L'/') {
                absPath.push_back(L'\\');
            }
            absPath.append(name);
            // Exclude test BEFORE the isDir test (D-09, migrated line-by-line from
            // sync_engine_client.cpp's RemoveLocalExtras: _wcsicmp against the
            // extended-length exclude path). Excluded entries are skipped entirely -
            // not emitted and not descended into.
            if (!excludeW.empty() && _wcsicmp(absPath.c_str(), excludeW.c_str()) == 0) {
                continue;
            }
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const std::string nameUtf8 = WideToUtf8(name);
            std::string relPath = BuildRelPath(current.relDir, nameUtf8);
            if (isDir) {
                if (collectDirs) {
                    if (dirInRemote.fn == nullptr || !dirInRemote.fn(dirInRemote.ctx, relPath)) {
                        ctx.extraDirs.push_back(relPath);   // local dir absent from remote dir set
                    }
                    ctx.localDirs.push_back(relPath);
                }
                subdirs.push_back(PendingDir{std::move(absPath), std::move(relPath)});  // dirs never become extras (FR-06)
            } else if (fileInRemote.fn != nullptr && !fileInRemote.fn(fileInRemote.ctx, relPath)) {
                ctx.extraFiles.push_back(std::move(relPath));  // local file absent from remote manifest
            }
        } while (FindNextFileW(hFind, &fd) != 0);
        FindClose(hFind);
    };
    ParallelDirWalk(PendingDir{ToExtendedLengthPath(root), std::string()},
                    numWorkers, kDeleteDirPopBatch, noCancel, "extra-scan-walk",
                    ScanCtx{}, processDir, finishWorker);
#else
    struct PendingDir {
        fs::path absDir;
        std::string relDir;
    };
    auto processDir = [&](const PendingDir& current, std::vector<PendingDir>& subdirs, ScanCtx& ctx) {
        std::error_code ec;
        fs::directory_iterator it(current.absDir, fs::directory_options::skip_permission_denied, ec);
        const fs::directory_iterator end;
        if (ec) {
            return;   // B-02/B-10
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;   // per-entry failure / delete race -> skip (B-02/B-10)
            }
            const fs::path& absPath = it->path();
            // Exclude test BEFORE the isDir test (D-09, migrated line-by-line from
            // sync_engine_client.cpp's RemoveLocalExtras: fs::equivalent, error -> not excluded).
            if (options.excludeAbsPath.has_value()) {
                std::error_code eqec;
                if (fs::equivalent(absPath, *options.excludeAbsPath, eqec) && !eqec) {
                    continue;
                }
            }
            const bool isDir = it->is_directory(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            const bool isRegular = it->is_regular_file(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (!isDir && !isRegular) {
                continue;   // only dirs + regular files, matching BuildIndex
            }
            const bool isSymlink = it->is_symlink(ec);
            if (ec) {
                ec.clear();
            }
            const std::string name = absPath.filename().string();
            std::string relPath = BuildRelPath(current.relDir, name);
            if (relPath.empty()) {
                continue;   // B-04
            }
            if (isDir) {
                if (collectDirs) {
                    if (dirInRemote.fn == nullptr || !dirInRemote.fn(dirInRemote.ctx, relPath)) {
                        ctx.extraDirs.push_back(relPath);   // local dir absent from remote dir set
                    }
                    ctx.localDirs.push_back(relPath);
                }
                if (!isSymlink) {
                    subdirs.push_back(PendingDir{absPath, std::move(relPath)});  // symlink dirs are not recursed (B-03)
                }
            } else if (fileInRemote.fn != nullptr && !fileInRemote.fn(fileInRemote.ctx, relPath)) {
                ctx.extraFiles.push_back(std::move(relPath));  // symlink->file follows regular-file semantics (B-03)
            }
        }
    };
    ParallelDirWalk(PendingDir{root, std::string()},
                    numWorkers, kDeleteDirPopBatch, noCancel, "extra-scan-walk",
                    ScanCtx{}, processDir, finishWorker);
#endif

    // Parallel merge order is non-deterministic; sort ascending so detail rows stay
    // byte-identical and deterministic (I-8 / NFR-11, replicating the former
    // CollectExtraLocal sort).
    std::sort(result.extraFiles.begin(), result.extraFiles.end());
    if (collectDirs) {
        std::sort(result.extraDirs.begin(), result.extraDirs.end());
        std::sort(result.localDirs.begin(), result.localDirs.end());
    }
    return result;
}

}  // namespace detail

// OQ-01=A shared exclude injector (design §4.4): step-by-step identical to the sync
// client's former inline selfPath computation (sync_engine_client.cpp) - CurrentExePath,
// weakly_canonical both sides, keep the self path only when it lies under the root.
std::optional<fs::path> SelfExcludeUnderRoot(const fs::path& root) {
    std::optional<fs::path> selfPath = CurrentExePath();
    if (selfPath.has_value()) {
        std::error_code sec;
        const fs::path canonicalRoot = fs::weakly_canonical(root, sec);
        if (sec) {
            selfPath = std::nullopt;  // root not canonicalizable -> no exclusion (B-05)
        } else {
            const fs::path canonicalSelf = fs::weakly_canonical(*selfPath, sec);
            if (sec || !IsPathUnderRoot(canonicalRoot, canonicalSelf)) {
                selfPath = std::nullopt;  // self outside root / not canonicalizable (B-05/B-06, E-3)
            } else {
                selfPath = canonicalSelf;
            }
        }
    }
    return selfPath;
}

}  // namespace fc
