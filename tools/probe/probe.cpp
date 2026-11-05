// Build-bound passive packet capture. No work is performed from DllMain.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr char expected_sha[] = "cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d";
constexpr std::size_t capture_limit = 64u * 1024u * 1024u;
using PacketCompress = int(__fastcall*)(const unsigned char*, std::uint32_t, unsigned char*, std::uint32_t);
using PacketDecompress = int(__fastcall*)(const unsigned char*, std::uintptr_t, unsigned char*, std::uint32_t);
using RawDecompress = int(__fastcall*)(const unsigned char*, unsigned char*, int, int);
using PhysicsStep = int(__fastcall*)(void*, float, int, float);
using SplitSend = char(__fastcall*)(void*, void*, unsigned int, int,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t,
    unsigned int, const void*, std::uintptr_t, int);
PacketCompress original_compress{};
PacketDecompress original_decompress{};
RawDecompress original_raw{};
PhysicsStep original_step{};
SplitSend original_split{};
using ConnectParser = bool(__fastcall*)(const char*, std::uint64_t*, std::uint64_t*, std::uint64_t*);
ConnectParser original_connect_parser{};
std::atomic<std::uint64_t> forced_connect_id{0};
bool connect_hook_created{};
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> capture_generation{0};
std::mutex lifecycle_mutex, file_mutex;
HANDLE log_file = INVALID_HANDLE_VALUE;
std::uint64_t file_size{}, file_limit{}, sequence{};
std::uintptr_t image_base{};
bool hooks_created{};
std::array<void*, 5> targets{};
thread_local bool inside_capture{};

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    std::uint32_t version, header_size;
    std::uint64_t qpc_frequency, process_id;
    char source_sha256[64];
};
struct RecordHeader {
    std::uint32_t magic{0x31434552}, header_size{sizeof(RecordHeader)};
    std::uint64_t qpc{}, sequence{}, caller_rva{};
    std::uint32_t kind{}, thread_id{};
    std::uint64_t source_size{}, destination_capacity{};
    std::int32_t result{};
    std::uint32_t input_size{}, output_size{}, flags{};
};
struct StepParameters { std::uint64_t world; float time_step; std::int32_t max_substeps; float fixed_time_step; };
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 96);
static_assert(sizeof(RecordHeader) == 72);
static_assert(sizeof(StepParameters) == 20);

bool safe_copy(void* destination, const void* source, std::size_t length) {
    __try { if (length) std::memcpy(destination, source, length); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool write_handle(HANDLE file, const void* data, std::size_t size) {
    auto bytes = static_cast<const unsigned char*>(data);
    while (size) {
        DWORD written{};
        const auto amount = static_cast<DWORD>((size > MAXDWORD) ? MAXDWORD : size);
        if (!WriteFile(file, bytes, amount, &written, nullptr) || !written) return false;
        size -= written;
        bytes += written;
    }
    return true;
}
bool write_all(const void* data, std::size_t size) { return write_handle(log_file, data, size); }

std::vector<unsigned char> snapshot(const unsigned char* data, std::uint64_t size, std::uint32_t& flags, std::uint32_t flag) noexcept {
    try {
        if (size > capture_limit || (size && !data)) { flags |= flag; return {}; }
        std::vector<unsigned char> copy(static_cast<std::size_t>(size));
        if (!safe_copy(copy.data(), data, copy.size())) { flags |= flag; return {}; }
        return copy;
    } catch (...) { flags |= flag; return {}; }
}

struct CaptureGuard {
    CaptureGuard() { inside_capture = true; }
    ~CaptureGuard() { inside_capture = false; }
};

void append(std::uint64_t generation, RecordHeader header, const std::vector<unsigned char>& input, const std::vector<unsigned char>& output) noexcept {
    try {
        std::lock_guard lock(file_mutex);
        if (!enabled.load(std::memory_order_relaxed) || generation != capture_generation.load() || log_file == INVALID_HANDLE_VALUE) return;
        const auto bytes = sizeof(header) + input.size() + output.size();
        if (bytes > file_limit || file_size > file_limit - bytes) { enabled.store(false); return; }
        header.sequence = sequence++;
        header.input_size = static_cast<std::uint32_t>(input.size());
        header.output_size = static_cast<std::uint32_t>(output.size());
        if (!write_all(&header, sizeof(header)) || !write_all(input.data(), input.size()) || !write_all(output.data(), output.size())) {
            enabled.store(false);
            LARGE_INTEGER previous{};
            previous.QuadPart = file_size;
            if (SetFilePointerEx(log_file, previous, nullptr, FILE_BEGIN)) SetEndOfFile(log_file);
            return;
        }
        file_size += bytes;
    } catch (...) { enabled.store(false); }
}

template<class Invoke>
int capture(std::uint32_t kind, std::uintptr_t caller, const unsigned char* source, std::uint64_t size,
            unsigned char* destination, std::uint64_t capacity, Invoke invoke) {
    if (!enabled.load(std::memory_order_relaxed) || inside_capture) return invoke();
    const auto generation = capture_generation.load();
    CaptureGuard guard;
    RecordHeader header;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    header.qpc = now.QuadPart;
    header.caller_rva = caller >= image_base ? caller - image_base : caller;
    header.kind = kind;
    header.thread_id = GetCurrentThreadId();
    header.source_size = size;
    header.destination_capacity = capacity;
    auto input = snapshot(source, size, header.flags, 1);
    const int result = invoke(); // Exactly one call, preserving original arguments and result.
    header.result = result;
    std::vector<unsigned char> output;
    if (result > 0) {
        if (static_cast<std::uint64_t>(result) <= capacity)
            output = snapshot(destination, result, header.flags, 2);
        else header.flags |= 2;
    }
    append(generation, header, input, output);
    return result;
}

int __fastcall hook_compress(const unsigned char* source, std::uint32_t size, unsigned char* destination, std::uint32_t capacity) {
    return capture(1, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), source, size, destination, capacity,
                   [&] { return original_compress(source, size, destination, capacity); });
}
int __fastcall hook_decompress(const unsigned char* source, std::uintptr_t size, unsigned char* destination, std::uint32_t capacity) {
    return capture(2, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), source, size, destination, capacity,
                   [&] { return original_decompress(source, size, destination, capacity); });
}
int __fastcall hook_raw(const unsigned char* source, unsigned char* destination, int size, int capacity) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    // Only the statically verified server receive callsite; generic codec users
    // include persistence and must not be mislabeled as network packets.
    if (caller != image_base + 0xB4DDE5 || size < 0 || capacity < 0)
        return original_raw(source, destination, size, capacity);
    return capture(3, caller, source, static_cast<std::uint32_t>(size), destination, static_cast<std::uint32_t>(capacity),
                   [&] { return original_raw(source, destination, size, capacity); });
}

int __fastcall hook_step(void* world, float time_step, int max_substeps, float fixed_time_step) {
    if (!enabled.load(std::memory_order_relaxed)) return original_step(world, time_step, max_substeps, fixed_time_step);
    const auto generation = capture_generation.load();
    RecordHeader header;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    header.qpc = now.QuadPart;
    header.caller_rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - image_base;
    header.kind = 4;
    header.thread_id = GetCurrentThreadId();
    const StepParameters parameters{reinterpret_cast<std::uint64_t>(world), time_step, max_substeps, fixed_time_step};
    header.source_size = sizeof(parameters);
    auto input = snapshot(reinterpret_cast<const unsigned char*>(&parameters), sizeof(parameters), header.flags, 1);
    // Do not set inside_capture here: codec traffic emitted during a physics
    // update must still be recorded independently.
    const int result = original_step(world, time_step, max_substeps, fixed_time_step);
    header.result = result;
    append(generation, header, input, {});
    return result;
}

char __fastcall hook_split(void* a1, void* a2, unsigned int size, int a4,
    std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8, std::uintptr_t a9,
    std::uintptr_t a10, std::uintptr_t a11, std::uintptr_t a12, std::uintptr_t a13, std::uintptr_t a14,
    std::uintptr_t a15, std::uintptr_t a16, std::uintptr_t a17, std::uintptr_t a18, std::uintptr_t a19,
    std::uintptr_t a20, std::uintptr_t a21, std::uintptr_t a22, std::uintptr_t a23, std::uintptr_t a24,
    std::uintptr_t a25, std::uintptr_t a26, std::uintptr_t a27, std::uintptr_t a28, std::uintptr_t a29,
    std::uintptr_t a30, std::uintptr_t a31,
    unsigned int a32, const void* data, std::uintptr_t a34, int a35) {
    if (!enabled.load(std::memory_order_relaxed) || inside_capture || !data || !size || size > capture_limit)
        return original_split(a1, a2, size, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17,
                              a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30, a31, a32, data, a34, a35);
    const auto generation = capture_generation.load();
    CaptureGuard guard;
    RecordHeader header;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    header.qpc = now.QuadPart;
    header.caller_rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - image_base;
    header.kind = 5;
    header.thread_id = GetCurrentThreadId();
    header.source_size = size;
    header.destination_capacity = 0;
    auto input = snapshot(static_cast<const unsigned char*>(data), size, header.flags, 1);
    const char result = original_split(a1, a2, size, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15,
                                      a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29,
                                      a30, a31, a32, data, a34, a35);
    header.result = result;
    append(generation, header, input, {});
    return result;
}

bool __fastcall hook_connect_parser(const char* text, std::uint64_t* connect, std::uint64_t* friend_id, std::uint64_t* secret) {
    const auto forced = forced_connect_id.load(std::memory_order_relaxed);
    if (forced && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == image_base + 0x2E3683) {
        const std::string substitute = "-connect_steam_id " + std::to_string(forced);
        original_connect_parser(substitute.c_str(), connect, friend_id, secret);
        if (connect) *connect = forced;
        if (secret) *secret = 0;
        if (friend_id) *friend_id = 0;
        return true;
    }
    return original_connect_parser(text, connect, friend_id, secret);
}

bool check_executable_hash() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<unsigned char, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> buffer{};
    while (ok) {
        DWORD read{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) { ok = false; break; }
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; ok && i < digest.size(); ++i)
        ok = expected_sha[i * 2] == hex[digest[i] >> 4] && expected_sha[i * 2 + 1] == hex[digest[i] & 15];
    return ok;
}

bool check_prologue(std::uintptr_t rva, const std::array<unsigned char, 16>& expected) {
    const auto address = reinterpret_cast<void*>(image_base + rva);
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        memory.AllocationBase != reinterpret_cast<void*>(image_base) || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto protection = memory.Protect & 0xff;
    if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    std::array<unsigned char, 16> actual{};
    return safe_copy(actual.data(), address, actual.size()) && actual == expected;
}

DWORD open_log(const wchar_t* explicit_path) {
    wchar_t path[32768]{};
    DWORD length{};
    if (explicit_path) {
        for (; length < 32768; ++length) {
            if (!safe_copy(path + length, explicit_path + length, sizeof(wchar_t))) return ERROR_INVALID_PARAMETER;
            if (!path[length]) break;
        }
    } else length = GetEnvironmentVariableW(L"SCRAPDS_PROBE_LOG", path, 32768);
    if (length >= 32768) return ERROR_FILENAME_EXCED_RANGE;
    if (!length) {
        const auto temp = GetTempPathW(32768, path);
        if (!temp || temp >= 32768) return ERROR_PATH_NOT_FOUND;
        const std::wstring full = std::wstring(path) + L"scrapds-probe-" + std::to_wstring(GetCurrentProcessId()) + L".cap";
        if (full.size() >= 32768) return ERROR_FILENAME_EXCED_RANGE;
        std::memcpy(path, full.c_str(), (full.size() + 1) * sizeof(wchar_t));
    }
    log_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_file == INVALID_HANDLE_VALUE) return GetLastError();
    LARGE_INTEGER length_on_disk{};
    if (!GetFileSizeEx(log_file, &length_on_disk)) return GetLastError();
    FileHeader expected{};
    std::memcpy(expected.magic, "SDSCAP01", 8);
    expected.version = 1;
    expected.header_size = sizeof(FileHeader);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    expected.qpc_frequency = frequency.QuadPart;
    expected.process_id = GetCurrentProcessId();
    std::memcpy(expected.source_sha256, expected_sha, 64);
    if (length_on_disk.QuadPart) {
        FileHeader existing{};
        DWORD read{};
        if (!ReadFile(log_file, &existing, sizeof(existing), &read, nullptr) || read != sizeof(existing) ||
            std::memcmp(&existing, &expected, sizeof(expected)) != 0) return ERROR_INVALID_DATA;
        // Validate complete append boundaries and recover the next sequence.
        // This also rejects a partially written tail from an interrupted run.
        std::uint64_t offset = sizeof(FileHeader), next_sequence{};
        while (offset < static_cast<std::uint64_t>(length_on_disk.QuadPart)) {
            RecordHeader record{};
            if (!ReadFile(log_file, &record, sizeof(record), &read, nullptr) || read != sizeof(record) ||
                record.magic != 0x31434552 || record.header_size != sizeof(record) ||
                record.kind < 1 || record.kind > 5 || (record.flags & ~3u) ||
                record.input_size > capture_limit || record.output_size > capture_limit ||
                record.input_size > record.source_size || record.output_size > record.destination_capacity ||
                record.sequence < next_sequence || record.sequence == UINT64_MAX) return ERROR_INVALID_DATA;
            const std::uint64_t size = sizeof(record) + static_cast<std::uint64_t>(record.input_size) + record.output_size;
            if (size > static_cast<std::uint64_t>(length_on_disk.QuadPart) - offset) return ERROR_INVALID_DATA;
            offset += size;
            next_sequence = record.sequence + 1;
            LARGE_INTEGER position{};
            position.QuadPart = offset;
            if (!SetFilePointerEx(log_file, position, nullptr, FILE_BEGIN)) return GetLastError();
        }
        if (sequence < next_sequence) sequence = next_sequence;
    } else if (!write_all(&expected, sizeof(expected))) return ERROR_WRITE_FAULT;
    file_size = length_on_disk.QuadPart ? length_on_disk.QuadPart : sizeof(FileHeader);
    file_limit = 256ull * 1024 * 1024;
    wchar_t limit[32]{};
    const auto n = GetEnvironmentVariableW(L"SCRAPDS_PROBE_MAX_MB", limit, 32);
    if (n && n < 32) {
        wchar_t* end{};
        const auto mb = std::wcstoull(limit, &end, 10);
        if (end == limit || *end || mb < 1 || mb > 65536) return ERROR_INVALID_PARAMETER;
        file_limit = mb * 1024 * 1024;
    }
    return ERROR_SUCCESS;
}
} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI ScrapdsProbeStart(void* argument) {
    std::lock_guard lock(lifecycle_mutex);
    if (enabled.load()) return ERROR_ALREADY_INITIALIZED;
    image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!check_executable_hash()) return ERROR_BAD_EXE_FORMAT;
    if (!hooks_created) {
        if (!check_prologue(0xB59510, {0x89,0x54,0x24,0x10,0x53,0x55,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x50,0x48}) ||
            !check_prologue(0x4658D0, {0x40,0x53,0x56,0x57,0x48,0x83,0xec,0x50,0x49,0x8b,0xf8,0x48,0x8b,0xf1,0x48,0x85}) ||
            !check_prologue(0x4EEA30, {0x48,0x8b,0xc4,0x48,0x89,0x48,0x08,0x41,0x55,0x48,0x83,0xec,0x70,0x4c,0x8b,0xea}) ||
            !check_prologue(0x1118410, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x30,0x33}) ||
            !check_prologue(0xB5A480, {0x4c,0x8b,0xdc,0x45,0x89,0x4b,0x20,0x45,0x89,0x43,0x18,0x49,0x89,0x53,0x10,0x89})) return ERROR_REVISION_MISMATCH;
        std::uintptr_t vtable_step{};
        if (!safe_copy(&vtable_step, reinterpret_cast<void*>(image_base + 0x1605900 + 13 * 8), sizeof(vtable_step)) ||
            vtable_step != image_base + 0x1118410) return ERROR_REVISION_MISMATCH;
    }
    { std::lock_guard file_lock(file_mutex);
      if (log_file != INVALID_HANDLE_VALUE) { CloseHandle(log_file); log_file = INVALID_HANDLE_VALUE; }
      const auto result = open_log(static_cast<const wchar_t*>(argument));
      if (result) { if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file); log_file = INVALID_HANDLE_VALUE; return result; }
    }
    auto fail = [](DWORD error) {
        std::lock_guard file_lock(file_mutex);
        if (log_file != INVALID_HANDLE_VALUE) { CloseHandle(log_file); log_file = INVALID_HANDLE_VALUE; }
        return error;
    };
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return fail(ERROR_DLL_INIT_FAILED);
    if (!hooks_created) {
        // Pin our own module: disabled hooks and trampolines may still be on a
        // thread's stack. Stop never unloads or destroys their executable memory.
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&ScrapdsProbeStart), &pinned)) return fail(GetLastError());
        targets = {reinterpret_cast<void*>(image_base + 0xB59510), reinterpret_cast<void*>(image_base + 0x4658D0), reinterpret_cast<void*>(image_base + 0x4EEA30), reinterpret_cast<void*>(image_base + 0x1118410), reinterpret_cast<void*>(image_base + 0xB5A480)};
        const std::array<void*, 5> detours = {reinterpret_cast<void*>(&hook_compress), reinterpret_cast<void*>(&hook_decompress), reinterpret_cast<void*>(&hook_raw), reinterpret_cast<void*>(&hook_step), reinterpret_cast<void*>(&hook_split)};
        const std::array<void**, 5> originals = {reinterpret_cast<void**>(&original_compress), reinterpret_cast<void**>(&original_decompress), reinterpret_cast<void**>(&original_raw), reinterpret_cast<void**>(&original_step), reinterpret_cast<void**>(&original_split)};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (MH_CreateHook(targets[i], detours[i], originals[i]) != MH_OK) {
                for (std::size_t j = 0; j < i; ++j) MH_RemoveHook(targets[j]);
                return fail(ERROR_INVALID_FUNCTION);
            }
        }
        hooks_created = true;
    }
    for (auto target : targets) {
        const auto enable_status = MH_EnableHook(target);
        if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED) {
            for (auto address : targets) MH_DisableHook(address);
            return fail(ERROR_INVALID_FUNCTION);
        }
    }
    capture_generation.fetch_add(1);
    enabled.store(true);
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI ScrapdsForceConnectId(void* argument) {
    if (!argument) return ERROR_INVALID_PARAMETER;
    if (!enabled.load()) return ERROR_INVALID_STATE;
    const auto* text = static_cast<const wchar_t*>(argument);
    wchar_t* end{};
    const unsigned long long id = std::wcstoull(text, &end, 10);
    if (end == text || *end || !id) return ERROR_INVALID_PARAMETER;
    forced_connect_id.store(id, std::memory_order_release);
    const auto target = reinterpret_cast<void*>(image_base + 0x2F1C90);
    if (!connect_hook_created) {
        if (!check_prologue(0x2F1C90, {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac})) return ERROR_REVISION_MISMATCH;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&hook_connect_parser), reinterpret_cast<void**>(&original_connect_parser)) != MH_OK) return ERROR_INVALID_FUNCTION;
        connect_hook_created = true;
    }
    const auto status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) return ERROR_INVALID_FUNCTION;
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI ScrapdsProbeStop(void*) {
    std::lock_guard lock(lifecycle_mutex);
    enabled.store(false);
    capture_generation.fetch_add(1);
    DWORD result = ERROR_SUCCESS;
    if (hooks_created) for (auto target : targets) {
        const auto status = MH_DisableHook(target);
        if (status != MH_OK && status != MH_ERROR_DISABLED) result = ERROR_INVALID_FUNCTION;
    }
    std::lock_guard file_lock(file_mutex);
    if (log_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(log_file);
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
    return result;
}

extern "C" __declspec(dllexport) DWORD WINAPI ScrapdsProbeCodecTest(void* argument) {
    std::lock_guard lock(lifecycle_mutex);
    if (!argument) return ERROR_INVALID_PARAMETER;
    wchar_t path[32768]{};
    std::size_t length{};
    const auto supplied = static_cast<const wchar_t*>(argument);
    for (; length < 32768; ++length) {
        if (!safe_copy(path + length, supplied + length, sizeof(wchar_t))) return ERROR_INVALID_PARAMETER;
        if (!path[length]) break;
    }
    if (!length || length == 32768) return ERROR_INVALID_PARAMETER;
    image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!check_executable_hash()) return ERROR_BAD_EXE_FORMAT;
    if (!check_prologue(0x4EE990, {0xb8,0x68,0x40,0x00,0x00,0xe8,0xd6,0xcf,0xd2,0x00,0x48,0x2b,0xe0,0x48,0x8b,0x05}))
        return ERROR_REVISION_MISMATCH;
    if (!hooks_created &&
        (!check_prologue(0xB59510, {0x89,0x54,0x24,0x10,0x53,0x55,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x50,0x48}) ||
         !check_prologue(0x4658D0, {0x40,0x53,0x56,0x57,0x48,0x83,0xec,0x50,0x49,0x8b,0xf8,0x48,0x8b,0xf1,0x48,0x85})))
        return ERROR_REVISION_MISMATCH;
    std::array<unsigned char, 4097> source{}, decoded{};
    std::array<unsigned char, 8192> compressed{};
    source[0] = 0x7f;
    for (std::size_t i = 1; i < source.size(); ++i) source[i] = static_cast<unsigned char>((i * 17) ^ (i >> 3));
    const auto compress = hooks_created ? original_compress : reinterpret_cast<PacketCompress>(image_base + 0xB59510);
    const auto decompress = hooks_created ? original_decompress : reinterpret_cast<PacketDecompress>(image_base + 0x4658D0);
    // Invoke trampolines directly so synthetic bytes never enter the packet
    // capture. The raw server hook rejects this unrelated codec callsite.
    const int compressed_size = compress(source.data(), static_cast<std::uint32_t>(source.size()), compressed.data(), static_cast<std::uint32_t>(compressed.size()));
    if (compressed_size <= 0 || compressed_size > static_cast<int>(compressed.size()) || compressed[0] != source[0]) return ERROR_INVALID_DATA;
    const int decoded_size = decompress(compressed.data(), static_cast<std::uintptr_t>(compressed_size), decoded.data(), static_cast<std::uint32_t>(decoded.size()));
    if (decoded_size != static_cast<int>(source.size()) || decoded != source) return ERROR_CRC;
    HANDLE fixture = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fixture == INVALID_HANDLE_VALUE) return GetLastError();
    const std::uint32_t sizes[2] = {static_cast<std::uint32_t>(source.size()), static_cast<std::uint32_t>(compressed_size)};
    const bool ok = write_handle(fixture, "SDSCOD01", 8) && write_handle(fixture, sizes, sizeof(sizes)) &&
        write_handle(fixture, source.data(), source.size()) && write_handle(fixture, compressed.data(), compressed_size) && FlushFileBuffers(fixture);
    CloseHandle(fixture);
    if (!ok) { DeleteFileW(path); return ERROR_WRITE_FAULT; }
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
