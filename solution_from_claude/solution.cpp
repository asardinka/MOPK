// solution.cpp — оптимизированная универсальная версия (развитие test14).
//
// Задача та же: разобрать все CSV, отфильтровать по дате, слить строки
// с одинаковыми name + date (суммы всех значений), отсортировать, вывести.
//
// Главные отличия от test14 (подробно — в SOLUTION.md):
//  * параллелизм не "поток на файл", а "чанки": файлы режутся на куски
//    размером ~ половины L2 на аппаратный поток, у каждого файла свой
//    атомарный счётчик чанков, потоки распределяются по файлам;
//  * число потоков не "все ядра", а подбирается измерением скорости
//    (регулятор Governor): лишние потоки только мешают друг другу в ядре ОС;
//  * чанк читается позиционным чтением в выровненный по странице буфер,
//    который переиспользуется и живёт в L2, и сразу разбирается;
//  * начала строк берутся из битовой маски '\n' (SSE2, 64 байта за раз), конец
//    строки не ищется; у строк вне диапазона дат (≈90% на тестовых данных)
//    разбираются только разделители и дата;
//  * разделители id и name находятся одной SIMD-маской, float64 разбирается
//    своим быстрым путём, результат совпадает с from_chars бит в бит;
//  * суммы хранятся в open addressing таблице, ячейка {key, 7 double} = 64 байта
//    = ровно одна кэш-линия;
//  * память ограничена бюджетом: буферы не зависят от размера файлов, а при
//    большом числе групп локальные таблицы сливаются в глобальную.
//
// Результат CSV печатается в stdout, сведения о железе и время — в stderr.
//
// Сборка (MSYS2 ucrt64, из корня репозитория):
//   g++ -std=c++17 -O2 solution_from_claude/solution.cpp -o build/solution.exe
//   build/solution.exe                       (параметры по умолчанию = test14)
//   build/solution.exe --help

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <fstream>
#include <sched.h>
#endif
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define OPT_SSE2 1
#else
#define OPT_SSE2 0
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define OPT_BIG_ENDIAN 1
#else
#define OPT_BIG_ENDIAN 0
#endif

namespace {

#if defined(_MSC_VER) && !defined(__clang__)
inline unsigned ctz32(std::uint32_t x) {
    unsigned long i;
    _BitScanForward(&i, x);
    return i;
}
inline unsigned ctz64(std::uint64_t x) {
    unsigned long i;
    _BitScanForward64(&i, x);
    return i;
}
#else
inline unsigned ctz32(std::uint32_t x) { return static_cast<unsigned>(__builtin_ctz(x)); }
inline unsigned ctz64(std::uint64_t x) { return static_cast<unsigned>(__builtin_ctzll(x)); }
#endif

#if defined(_WIN32)
unsigned popcount64(std::uint64_t x) {
    unsigned count = 0;
    for (; x != 0; x &= x - 1) ++count;
    return count;
}
#endif

// Сколько байт после данных буфера всегда доступно для чтения. Туда пишутся '\n',
// поэтому SIMD-маски по 64 байта и чтение 8 байт имени не выходят за буфер,
// а у последней строки файла всегда есть '\n'.
constexpr std::size_t kPad = 64;
constexpr std::size_t kCacheLine = 64;
constexpr std::uint64_t kEmptyKey = ~0ull;

void* alignedAlloc(std::size_t bytes, std::size_t alignment) {
#if defined(_WIN32)
    return _aligned_malloc(bytes, alignment);
#else
    void* memory = nullptr;
    return posix_memalign(&memory, alignment, bytes) == 0 ? memory : nullptr;
#endif
}

void alignedFree(void* memory) {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

inline std::uint64_t load64(const char* p) {
    std::uint64_t word;
    std::memcpy(&word, p, sizeof(word));
    return word;
}

// ---------------------------------------------------------------------------
// Железо
// ---------------------------------------------------------------------------

struct Hardware {
    unsigned logicalCpus = 1;        // сколько аппаратных потоков доступно процессу
    unsigned cores = 1;              // физических ядер
    unsigned threadsPerL2 = 1;       // сколько аппаратных потоков делят один L2
    std::size_t pageSize = 4096;
    std::size_t granularity = 4096;  // выравнивание смещений чтения
    std::size_t l1d = 32 * 1024;
    std::size_t l2 = 256 * 1024;     // размер одного экземпляра L2
    std::size_t l3 = 0;
    std::uint64_t availableRam = 0;
};

#if defined(__linux__)
std::string readSmallFile(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

unsigned countCpuList(const std::string& list) {
    // Формат "0-3,8,10-11".
    unsigned count = 0;
    std::size_t i = 0;

    while (i < list.size()) {
        unsigned first = 0;
        while (i < list.size() && list[i] >= '0' && list[i] <= '9') first = first * 10 + (list[i++] - '0');
        unsigned last = first;

        if (i < list.size() && list[i] == '-') {
            ++i;
            last = 0;
            while (i < list.size() && list[i] >= '0' && list[i] <= '9') last = last * 10 + (list[i++] - '0');
        }

        count += last >= first ? last - first + 1 : 1;
        while (i < list.size() && (list[i] < '0' || list[i] > '9')) ++i;
    }

    return count;
}
#endif

Hardware detectHardware() {
    Hardware hw;
    const unsigned reported = std::thread::hardware_concurrency();
    hw.logicalCpus = reported != 0 ? reported : 1;
    hw.cores = hw.logicalCpus;

#if defined(_WIN32)
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    hw.pageSize = systemInfo.dwPageSize;
    hw.granularity = systemInfo.dwAllocationGranularity;

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask != 0) {
        hw.logicalCpus = std::min(hw.logicalCpus, popcount64(processMask));
    }

    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
    std::vector<unsigned char> info(length);

    if (length != 0 && GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(info.data()), &length)) {
        unsigned cores = 0;
        std::size_t smallestL2PerThread = 0;

        for (DWORD offset = 0; offset < length;) {
            const auto* item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(info.data() + offset);

            if (item->Relationship == RelationProcessorCore) {
                ++cores;
            } else if (item->Relationship == RelationCache) {
                const CACHE_RELATIONSHIP& cache = item->Cache;
                const unsigned share = std::max(1u, popcount64(cache.GroupMask.Mask));

                if (cache.Level == 1 && (cache.Type == CacheData || cache.Type == CacheUnified)) {
                    hw.l1d = cache.CacheSize;
                } else if (cache.Level == 2 && (cache.Type == CacheData || cache.Type == CacheUnified)) {
                    // На гибридных CPU L2 у разных ядер разный: берём самый маленький на поток.
                    const std::size_t perThread = cache.CacheSize / share;
                    if (smallestL2PerThread == 0 || perThread < smallestL2PerThread) {
                        smallestL2PerThread = perThread;
                        hw.l2 = cache.CacheSize;
                        hw.threadsPerL2 = share;
                    }
                } else if (cache.Level == 3 && (cache.Type == CacheData || cache.Type == CacheUnified)) {
                    hw.l3 = cache.CacheSize;
                }
            }

            offset += item->Size;
        }

        if (cores != 0) hw.cores = cores;
    }

    MEMORYSTATUSEX memoryStatus;
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus)) hw.availableRam = memoryStatus.ullAvailPhys;
#else
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize > 0) hw.pageSize = hw.granularity = static_cast<std::size_t>(pageSize);

#if defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int count = CPU_COUNT(&set);
        if (count > 0) hw.logicalCpus = std::min(hw.logicalCpus, static_cast<unsigned>(count));
    }

    unsigned threadsPerCore = 1;

    for (int index = 0; index < 16; ++index) {
        const std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(index) + "/";
        const std::string level = readSmallFile(base + "level");
        if (level.empty()) break;

        const std::string type = readSmallFile(base + "type");
        const std::string sizeText = readSmallFile(base + "size");
        const unsigned share = std::max(1u, countCpuList(readSmallFile(base + "shared_cpu_list")));
        std::size_t size = std::strtoull(sizeText.c_str(), nullptr, 10);
        if (!sizeText.empty() && (sizeText.back() == 'K' || sizeText.back() == 'k')) size *= 1024;
        if (!sizeText.empty() && (sizeText.back() == 'M' || sizeText.back() == 'm')) size *= 1024 * 1024;
        if (size == 0 || type == "Instruction") continue;

        if (level == "1") {
            hw.l1d = size;
            threadsPerCore = share;
        } else if (level == "2") {
            hw.l2 = size;
            hw.threadsPerL2 = share;
        } else if (level == "3") {
            hw.l3 = size;
        }
    }

    hw.cores = std::max(1u, hw.logicalCpus / threadsPerCore);

    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::uint64_t valueKb = 0;
    std::string unit;
    while (meminfo >> key >> valueKb >> unit) {
        if (key == "MemAvailable:") {
            hw.availableRam = valueKb * 1024;
            break;
        }
    }
#else
#if defined(_SC_LEVEL2_CACHE_SIZE)
    const long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
    if (l2 > 0) hw.l2 = static_cast<std::size_t>(l2);
#endif
#if defined(_SC_AVPHYS_PAGES)
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    if (pages > 0) hw.availableRam = static_cast<std::uint64_t>(pages) * hw.pageSize;
#endif
#endif
#endif

    if (hw.logicalCpus == 0) hw.logicalCpus = 1;
    if (hw.cores > hw.logicalCpus) hw.cores = hw.logicalCpus;
    if (hw.granularity < hw.pageSize) hw.granularity = hw.pageSize;
    return hw;
}

// ---------------------------------------------------------------------------
// Параметры разбора и даты
// ---------------------------------------------------------------------------

struct Params {
    char csvSeparator = ',';
    char dateSeparator = '-';
    char decimalSeparator = '.';
    int yearPos = 0;
    int monthPos = 5;
    int dayPos = 8;
    int dateLength = 10;           // YYYY + MM + DD + 2 разделителя
    int separatorPos[2] = {4, 7};  // позиции разделителей внутри даты
    int dateOrder[3] = {0, 1, 2};  // 0 = год, 1 = месяц, 2 = день по порядку полей
    std::uint32_t startKey = 0;    // YYYYMMDD
    std::uint32_t span = 0;        // finishKey - startKey
    unsigned valueCount = 7;
};

bool setDateFormat(Params& params, const std::string& format) {
    if (format.size() != 3) return false;

    int position = 0;
    bool seen[3] = {false, false, false};

    for (int i = 0; i < 3; ++i) {
        const char c = format[i];
        const int part = c == 'Y' ? 0 : c == 'M' ? 1 : c == 'D' ? 2 : -1;
        if (part < 0 || seen[part]) return false;
        seen[part] = true;
        params.dateOrder[i] = part;

        if (part == 0) params.yearPos = position;
        if (part == 1) params.monthPos = position;
        if (part == 2) params.dayPos = position;
        position += part == 0 ? 4 : 2;
        if (i < 2) params.separatorPos[i] = position;
        ++position;
    }

    params.dateLength = position - 1;
    return true;
}

// Универсальный (медленный) разбор даты: поля любой ширины, "7.11.2026".
// Возвращает YYYYMMDD или 0, если строка не является датой.
std::uint32_t parseDateGeneric(const char* begin, const char* end, const Params& params) {
    std::uint32_t parts[3] = {0, 0, 0};
    int index = 0;
    int digits = 0;

    for (const char* c = begin; c < end; ++c) {
        if (*c == params.dateSeparator) {
            if (digits == 0 || ++index > 2) return 0;
            digits = 0;
            continue;
        }

        const unsigned digit = static_cast<unsigned char>(*c) - '0';
        if (digit > 9 || ++digits > 4) return 0;
        parts[index] = parts[index] * 10 + digit;
    }

    if (index != 2 || digits == 0) return 0;

    std::uint32_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;

    for (int i = 0; i < 3; ++i) {
        if (params.dateOrder[i] == 0) year = parts[i];
        if (params.dateOrder[i] == 1) month = parts[i];
        if (params.dateOrder[i] == 2) day = parts[i];
    }

    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
    return year * 10000 + month * 100 + day;
}

// Быстрый разбор даты фиксированной ширины по заранее вычисленным позициям.
inline std::uint32_t parseDateFixed(const char* date, const Params& params) {
    const unsigned char* d = reinterpret_cast<const unsigned char*>(date);
    const std::uint32_t year = (d[params.yearPos] - '0') * 1000u + (d[params.yearPos + 1] - '0') * 100u + (d[params.yearPos + 2] - '0') * 10u + (d[params.yearPos + 3] - '0');
    const std::uint32_t month = (d[params.monthPos] - '0') * 10u + (d[params.monthPos + 1] - '0');
    const std::uint32_t day = (d[params.dayPos] - '0') * 10u + (d[params.dayPos + 1] - '0');
    return year * 10000u + month * 100u + day;
}

// Проверка, что дата фиксированной ширины действительно состоит из цифр и
// разделителей, а месяц и день допустимы (как в parseDateGeneric).
// Выполняется только для строк, попавших в диапазон.
inline bool isValidFixedDate(const char* date, std::uint32_t key, const Params& params) {
    for (int i = 0; i < params.dateLength; ++i) {
        const char c = date[i];

        if (i == params.separatorPos[0] || i == params.separatorPos[1]) {
            if (c != params.dateSeparator) return false;
        } else if (static_cast<unsigned>(static_cast<unsigned char>(c) - '0') > 9) {
            return false;
        }
    }

    const std::uint32_t month = key / 100 % 100;
    const std::uint32_t day = key % 100;
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// ---------------------------------------------------------------------------
// Числа
// ---------------------------------------------------------------------------

constexpr double kPow10[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

double parseNumberSlow(const char*& cursor, const Params& params) {
    const char* begin = cursor;
    const char* end = begin;

    while (*end != params.csvSeparator && *end != '\n' && *end != '\r') ++end;
    cursor = end;

    if (begin < end && *begin == '+') ++begin;
    if (begin == end) return 0.0;

    double value = 0.0;

    if (params.decimalSeparator == '.') {
        std::from_chars(begin, end, value);
        return value;
    }

    std::string copy(begin, end);
    for (char& c : copy) {
        if (c == params.decimalSeparator) c = '.';
    }

    std::from_chars(copy.data(), copy.data() + copy.size(), value);
    return value;
}

// Быстрый путь: все цифры собираются в одно целое m, результат = m / 10^frac.
// При m <= 2^53 и frac <= 22 оба числа представимы в double точно, а деление
// IEEE округляется корректно, поэтому результат совпадает с from_chars бит в бит.
// Экспонента, слишком длинные числа и прочие редкие случаи — через from_chars.
inline double parseNumber(const char*& cursor, const Params& params) {
    const char* c = cursor;
    const bool negative = *c == '-';
    c += negative;

    std::uint64_t mantissa = 0;
    const char* digitsStart = c;
    unsigned digit;

    while ((digit = static_cast<unsigned char>(*c) - '0') < 10) {
        mantissa = mantissa * 10 + digit;
        ++c;
    }

    std::size_t digits = static_cast<std::size_t>(c - digitsStart);
    std::size_t fraction = 0;

    if (*c == params.decimalSeparator) {
        const char* fractionStart = ++c;

        while ((digit = static_cast<unsigned char>(*c) - '0') < 10) {
            mantissa = mantissa * 10 + digit;
            ++c;
        }

        fraction = static_cast<std::size_t>(c - fractionStart);
        digits += fraction;
    }

    const char terminator = *c;

    if ((terminator == params.csvSeparator || terminator == '\r' || terminator == '\n') && digits != 0 && digits <= 19 && mantissa <= (1ull << 53) && fraction <= 22) {
        cursor = c;
        const double value = static_cast<double>(mantissa) / kPow10[fraction];
        return negative ? -value : value;
    }

    return parseNumberSlow(cursor, params);
}

// ---------------------------------------------------------------------------
// Битовые маски символов
// ---------------------------------------------------------------------------

// Бит i результата = 1, если p[i] == c (i = 0..63). На SSE2 (есть у любого
// x86-64) это 4 сравнения по 16 байт; на других процессорах — простой цикл.
inline std::uint64_t charMask64(const char* p, char c) {
#if OPT_SSE2
    const __m128i needle = _mm_set1_epi8(c);
    const std::uint64_t m0 = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), needle)));
    const std::uint64_t m1 = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16)), needle)));
    const std::uint64_t m2 = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 32)), needle)));
    const std::uint64_t m3 = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 48)), needle)));
    return m0 | (m1 << 16) | (m2 << 32) | (m3 << 48);
#else
    std::uint64_t mask = 0;
    for (int i = 0; i < 64; ++i) mask |= static_cast<std::uint64_t>(p[i] == c) << i;
    return mask;
#endif
}

// То же для первых 32 байт.
inline std::uint32_t charMask32(const char* p, char c) {
#if OPT_SSE2
    const __m128i needle = _mm_set1_epi8(c);
    const std::uint32_t low = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), needle)));
    const std::uint32_t high = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16)), needle)));
    return low | (high << 16);
#else
    std::uint32_t mask = 0;
    for (int i = 0; i < 32; ++i) mask |= static_cast<std::uint32_t>(p[i] == c) << i;
    return mask;
#endif
}

// ---------------------------------------------------------------------------
// Таблица имён: имя -> nameId (open addressing, линейное пробирование)
// ---------------------------------------------------------------------------

inline std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 32;
    x *= 0xd6e8feb86659fd93ull;
    x ^= x >> 32;
    return x;
}

// Первые 8 байт имени (остальные обнулены). Для имён до 8 байт это всё имя,
// и сравнение имён сводится к сравнению двух чисел — без memcmp.
inline std::uint64_t namePrefixFast(const char* p, std::size_t length) {
    if (length >= 8) return load64(p);
#if OPT_BIG_ENDIAN
    return load64(p) & ~(~0ull >> (8 * length));
#else
    return load64(p) & ((1ull << (8 * length)) - 1);  // чтение за концом имени безопасно (kPad)
#endif
}

inline std::uint64_t hashName(const char* p, std::size_t length, std::uint64_t prefix) {
    std::uint64_t h = (0x9E3779B97F4A7C15ull * (length + 1)) ^ prefix;

    if (length > 8) {
        const char* last = p + length - 8;
        for (const char* c = p + 8; c < last; c += 8) {
            h = (h ^ load64(c)) * 0xbf58476d1ce4e5b9ull;
            h ^= h >> 29;
        }
        h ^= load64(last) * 0x94d049bb133111ebull;
    }

    return mix64(h * 0x94d049bb133111ebull);
}

class NameTable {
public:
    NameTable() { reset(); }

    std::uint32_t intern(const char* name, std::uint32_t length) {
        const std::uint64_t prefix = namePrefixFast(name, length);
        return intern(hashName(name, length, prefix), prefix, name, length);
    }

    std::uint32_t intern(std::uint64_t hash, std::uint64_t prefix, const char* name, std::uint32_t length) {
        std::size_t i = static_cast<std::size_t>(hash) & mask_;

        for (;;) {
            Slot& slot = slots_[i];

            if (slot.id == kNoId) {
                const std::uint32_t id = static_cast<std::uint32_t>(refs_.size());
                refs_.push_back({hash, prefix, static_cast<std::uint32_t>(arena_.size()), length});
                arena_.insert(arena_.end(), name, name + length);
                slot = {hash, prefix, id, length};
                if (refs_.size() * 2 > slots_.size()) grow();
                return id;
            }

            if (slot.prefix == prefix && slot.length == length &&
                (length <= 8 || (slot.hash == hash && std::memcmp(arena_.data() + refs_[slot.id].offset, name, length) == 0))) {
                return slot.id;
            }

            i = (i + 1) & mask_;
        }
    }

    std::size_t size() const { return refs_.size(); }
    std::uint64_t hash(std::uint32_t id) const { return refs_[id].hash; }
    std::uint64_t prefix(std::uint32_t id) const { return refs_[id].prefix; }
    std::string_view name(std::uint32_t id) const { return {arena_.data() + refs_[id].offset, refs_[id].length}; }
    std::size_t bytes() const { return slots_.capacity() * sizeof(Slot) + refs_.capacity() * sizeof(Ref) + arena_.capacity(); }

    void reset() {
        slots_.assign(64, Slot{0, 0, kNoId, 0});
        slots_.shrink_to_fit();
        mask_ = slots_.size() - 1;
        refs_.clear();
        refs_.shrink_to_fit();
        arena_.clear();
        arena_.shrink_to_fit();
    }

private:
    static constexpr std::uint32_t kNoId = ~0u;

    struct Slot {
        std::uint64_t hash;
        std::uint64_t prefix;
        std::uint32_t id;
        std::uint32_t length;
    };

    struct Ref {
        std::uint64_t hash;
        std::uint64_t prefix;
        std::uint32_t offset;
        std::uint32_t length;
    };

    void grow() {
        std::vector<Slot> bigger(slots_.size() * 2, Slot{0, 0, kNoId, 0});
        const std::size_t mask = bigger.size() - 1;

        for (const Slot& slot : slots_) {
            if (slot.id == kNoId) continue;
            std::size_t i = static_cast<std::size_t>(slot.hash) & mask;
            while (bigger[i].id != kNoId) i = (i + 1) & mask;
            bigger[i] = slot;
        }

        slots_.swap(bigger);
        mask_ = mask;
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::vector<Ref> refs_;
    std::vector<char> arena_;
};

// ---------------------------------------------------------------------------
// Таблица групп: (nameId, date) -> суммы. Ячейка = [key][v0..vK-1], шаг ячейки
// кратен 64 байтам, массив выровнен на 64: при K = 7 ячейка — ровно одна
// кэш-линия, и обновление группы касается ровно одной линии памяти.
// ---------------------------------------------------------------------------

class GroupTable {
public:
    GroupTable() = default;
    GroupTable(const GroupTable&) = delete;
    GroupTable& operator=(const GroupTable&) = delete;
    ~GroupTable() { alignedFree(cells_); }

    void init(unsigned valueCount, std::size_t capacity) {
        valueCount_ = valueCount;
        const std::size_t words = 1 + valueCount;
        const std::size_t wordsPerLine = kCacheLine / sizeof(std::uint64_t);
        stride_ = (words + wordsPerLine - 1) / wordsPerLine * wordsPerLine;
        allocate(capacity);
    }

    double* at(std::uint64_t key) {
        std::size_t i = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);

        for (;;) {
            std::uint64_t* cell = cells_ + i * stride_;
            if (*cell == key) return reinterpret_cast<double*>(cell + 1);

            if (*cell == kEmptyKey) {
                if ((count_ + 1) * 2 > capacity_) {
                    grow();
                    return at(key);
                }

                *cell = key;
                ++count_;
                return reinterpret_cast<double*>(cell + 1);
            }

            i = (i + 1) & (capacity_ - 1);
        }
    }

    template <class Function>
    void forEach(Function function) const {
        for (std::size_t i = 0; i < capacity_; ++i) {
            const std::uint64_t* cell = cells_ + i * stride_;
            if (*cell != kEmptyKey) function(*cell, reinterpret_cast<const double*>(cell + 1));
        }
    }

    std::size_t count() const { return count_; }
    std::size_t bytes() const { return capacity_ * stride_ * sizeof(std::uint64_t); }
    void reset(std::size_t capacity) { allocate(capacity); }

private:
    void allocate(std::size_t capacity) {
        alignedFree(cells_);
        capacity_ = 16;
        shift_ = 60;
        while (capacity_ < capacity) {
            capacity_ *= 2;
            --shift_;
        }

        const std::size_t bytes = capacity_ * stride_ * sizeof(std::uint64_t);
        cells_ = static_cast<std::uint64_t*>(alignedAlloc(bytes, kCacheLine));
        if (cells_ == nullptr) throw std::bad_alloc();
        std::memset(cells_, 0, bytes);
        for (std::size_t i = 0; i < capacity_; ++i) cells_[i * stride_] = kEmptyKey;
        count_ = 0;
    }

    void grow() {
        std::uint64_t* old = cells_;
        const std::size_t oldCapacity = capacity_;
        cells_ = nullptr;
        allocate(oldCapacity * 2);

        for (std::size_t i = 0; i < oldCapacity; ++i) {
            const std::uint64_t* cell = old + i * stride_;
            if (*cell == kEmptyKey) continue;
            double* target = at(*cell);
            std::memcpy(target, cell + 1, valueCount_ * sizeof(double));
        }

        alignedFree(old);
    }

    std::uint64_t* cells_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t stride_ = 8;
    std::size_t count_ = 0;
    unsigned shift_ = 60;
    unsigned valueCount_ = 7;
};

constexpr std::size_t kInitialGroups = 256;

// Сливает локальные таблицы потока в глобальные и очищает локальные.
void mergeInto(NameTable& globalNames, GroupTable& globalGroups, NameTable& names, GroupTable& groups, unsigned valueCount) {
    std::vector<std::uint32_t> remap(names.size());

    for (std::uint32_t id = 0; id < names.size(); ++id) {
        const std::string_view name = names.name(id);
        remap[id] = globalNames.intern(names.hash(id), names.prefix(id), name.data(), static_cast<std::uint32_t>(name.size()));
    }

    groups.forEach([&](std::uint64_t key, const double* values) {
        const std::uint64_t globalKey = (static_cast<std::uint64_t>(remap[key >> 32]) << 32) | (key & 0xffffffffull);
        double* target = globalGroups.at(globalKey);
        for (unsigned i = 0; i < valueCount; ++i) target[i] += values[i];
    });

    names.reset();
    groups.reset(kInitialGroups);
}

// ---------------------------------------------------------------------------
// Файлы
// ---------------------------------------------------------------------------

struct InputFile {
    std::string path;
    std::uint64_t size = 0;
    std::uint64_t chunks = 0;
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};

bool openInput(InputFile& file) {
#if defined(_WIN32)
    // FILE_FLAG_OVERLAPPED: один дескриптор на файл, и потоки читают его
    // параллельно по своим смещениям (у синхронного дескриптора Windows
    // сериализует операции). FILE_FLAG_SEQUENTIAL_SCAN — агрессивный read-ahead.
    file.handle = CreateFileA(file.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.handle, &size)) return false;
    file.size = static_cast<std::uint64_t>(size.QuadPart);
#else
    file.fd = open(file.path.c_str(), O_RDONLY);
    if (file.fd < 0) return false;

    struct stat info;
    if (fstat(file.fd, &info) != 0) return false;
    file.size = static_cast<std::uint64_t>(info.st_size);
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(file.fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#endif
    return true;
}

void closeInput(InputFile& file) {
#if defined(_WIN32)
    if (file.handle != INVALID_HANDLE_VALUE) CloseHandle(file.handle);
    file.handle = INVALID_HANDLE_VALUE;
#else
    if (file.fd >= 0) close(file.fd);
    file.fd = -1;
#endif
}

// Позиционное чтение (аналог pread): потоки не делят указатель позиции файла.
class Reader {
public:
    Reader() {
#if defined(_WIN32)
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
    }

    ~Reader() {
#if defined(_WIN32)
        if (event_ != nullptr) CloseHandle(event_);
#endif
    }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // total — число прочитанных байт; false — ошибка ввода-вывода.
    bool readAt(const InputFile& file, std::uint64_t offset, char* destination, std::size_t bytes, std::size_t& total) {
        total = 0;

        while (total < bytes) {
#if defined(_WIN32)
            OVERLAPPED overlapped{};
            const std::uint64_t position = offset + total;
            overlapped.Offset = static_cast<DWORD>(position);
            overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
            overlapped.hEvent = event_;
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes - total, 1u << 30));
            DWORD got = 0;

            if (!ReadFile(file.handle, destination + total, request, nullptr, &overlapped)) {
                const DWORD error = GetLastError();
                if (error == ERROR_HANDLE_EOF) break;
                if (error != ERROR_IO_PENDING) return false;
            }

            if (!GetOverlappedResult(file.handle, &overlapped, &got, TRUE)) {
                if (GetLastError() == ERROR_HANDLE_EOF) break;
                return false;
            }
#else
            const ssize_t got = pread(file.fd, destination + total, bytes - total, static_cast<off_t>(offset + total));
            if (got < 0) {
                if (errno == EINTR) continue;
                return false;
            }
#endif
            if (got == 0) break;
            total += static_cast<std::size_t>(got);
        }

        return true;
    }

private:
#if defined(_WIN32)
    HANDLE event_ = nullptr;
#endif
};

// ---------------------------------------------------------------------------
// Обработка
// ---------------------------------------------------------------------------

// Счётчик следующего чанка файла — на своей кэш-линии, чтобы потоки,
// работающие с разными файлами, не мешали друг другу (false sharing).
struct alignas(kCacheLine) ChunkCursor {
    std::atomic<std::uint64_t> next{0};
};

struct Shared {
    const Params* params = nullptr;
    std::vector<InputFile>* files = nullptr;
    std::unique_ptr<ChunkCursor[]> cursors;
    std::size_t chunkSize = 0;
    std::size_t overrun = 0;      // сколько читать за границей чанка (1 страница)
    std::size_t tableBudget = 0;  // лимит памяти таблиц одного потока
    std::uint64_t totalBytes = 0;

    alignas(kCacheLine) std::atomic<std::uint64_t> bytesDone{0};  // для регулятора потоков
    std::atomic<unsigned> allowedThreads{1};                        // потоки с номером >= этого выходят

    alignas(kCacheLine) std::mutex globalMutex;
    NameTable globalNames;
    GroupTable globalGroups;
    std::atomic<bool> failed{false};
    std::string error;
};

// Регулятор числа потоков (hill climbing, как в пуле потоков .NET).
//
// Сколько потоков нужно, по характеристикам железа заранее не узнать: разбор
// масштабируется почти линейно, а чтение из файлового кэша ОС упирается в
// пропускную способность памяти и блокировки ядра — дальше лишние потоки
// только мешают друг другу. Поэтому число потоков подбирается измерением:
// стартуем с половины ядер, каждые ~1.5 мс считаем скорость (байт/мс) и
// удваиваем число потоков, пока это даёт прирост >= 25%. Если прироста нет —
// возвращаемся к лучшему уровню, лишние потоки доделывают чанк и выходят.
// Проба делается, только если оставшейся работы хватит, чтобы она окупилась.
// Потоки создаются только по мере надобности: на маленьких данных лишние
// потоки не создаются вовсе.
class Governor {
public:
    using Clock = std::chrono::steady_clock;

    Governor(Shared& shared, unsigned maxThreads, std::function<void(unsigned)> spawn)
        : shared_(shared), maxThreads_(maxThreads), spawn_(std::move(spawn)) {}

    void start(unsigned threads) {
        shared_.allowedThreads.store(threads);
        for (unsigned i = 1; i < threads; ++i) spawn_(i);
        active_ = threads;
        settling_ = true;
        phaseStart_ = Clock::now();
    }

    // Вызывается главным потоком после каждого его чанка.
    void tick() {
        if (done_) return;
        const auto now = Clock::now();

        if (settling_) {
            // Новым потокам нужно время стартовать и прогреть буферы.
            if (now - phaseStart_ < std::chrono::microseconds(200)) return;
            settling_ = false;
            phaseStart_ = now;
            bytesAtStart_ = shared_.bytesDone.load(std::memory_order_relaxed);
            return;
        }

        const std::uint64_t bytes = shared_.bytesDone.load(std::memory_order_relaxed) - bytesAtStart_;
        const double elapsedMs = std::chrono::duration<double, std::milli>(now - phaseStart_).count();
        if (elapsedMs < kWindowMs || bytes < 3ull * active_ * shared_.chunkSize) return;

        const double rate = bytes / elapsedMs;
        history_.push_back({active_, rate});

        if (bestRate_ == 0 || rate >= bestRate_ * 1.25) {
            bestRate_ = rate;
            bestThreads_ = active_;

            // Проба стоит времени (создание потоков, переходный процесс), поэтому
            // она имеет смысл, только если работы осталось хотя бы на ~20 окон.
            const std::uint64_t processed = shared_.bytesDone.load(std::memory_order_relaxed);
            const double remainingMs = (shared_.totalBytes > processed ? shared_.totalBytes - processed : 0) / rate;

            if (active_ >= maxThreads_ || remainingMs < 20 * kWindowMs) {
                done_ = true;
                return;
            }

            const unsigned next = std::min(maxThreads_, active_ * 2);
            shared_.allowedThreads.store(next);
            for (unsigned i = active_; i < next; ++i) spawn_(i);
            active_ = next;
            settling_ = true;
            phaseStart_ = now;
        } else {
            shared_.allowedThreads.store(bestThreads_);
            done_ = true;
        }
    }

    std::string describe() const {
        std::string text;
        char item[64];

        for (const auto& step : history_) {
            std::snprintf(item, sizeof(item), "%u thr %.1f GB/s -> ", step.first, step.second / 1e6);
            text += item;
        }

        std::snprintf(item, sizeof(item), "%s %u threads", done_ ? "kept" : "ended with", done_ ? bestThreads_ : active_);
        return text + item;
    }

private:
    static constexpr double kWindowMs = 1.5;

    Shared& shared_;
    unsigned maxThreads_;
    std::function<void(unsigned)> spawn_;
    unsigned active_ = 1;
    unsigned bestThreads_ = 1;
    double bestRate_ = 0;
    bool settling_ = false;
    bool done_ = false;
    Clock::time_point phaseStart_;
    std::uint64_t bytesAtStart_ = 0;
    std::vector<std::pair<unsigned, double>> history_;
};

class alignas(kCacheLine) Worker {
public:
    Worker(Shared& shared, unsigned index, std::size_t firstFile) : shared_(shared), params_(*shared.params), index_(index), firstFile_(firstFile) {
        groups_.init(params_.valueCount, kInitialGroups);
    }

    ~Worker() { alignedFree(buffer_); }

    // Поток начинает со "своего" файла и берёт его чанки по порядку; когда
    // файл закончился — помогает с остальными. Так потоки распределены по
    // файлам (у ОС блокировки файлового кэша — на файл), а каждый файл
    // читается последовательно (read-ahead ОС, дружелюбно к HDD).
    void run(Governor* governor) {
        std::vector<InputFile>& files = *shared_.files;
        reserve(shared_.chunkSize + shared_.overrun);

        for (std::size_t step = 0; step < files.size(); ++step) {
            const std::size_t fileIndex = (firstFile_ + step) % files.size();
            const InputFile& file = files[fileIndex];
            std::atomic<std::uint64_t>& cursor = shared_.cursors[fileIndex].next;

            for (;;) {
                if (index_ >= shared_.allowedThreads.load(std::memory_order_relaxed)) return;
                const std::uint64_t chunk = cursor.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= file.chunks || shared_.failed.load(std::memory_order_relaxed)) break;

                const std::uint64_t begin = chunk * shared_.chunkSize;
                const std::uint64_t end = std::min<std::uint64_t>(begin + shared_.chunkSize, file.size);

                if (!processChunk(file, begin, end)) {
                    std::lock_guard<std::mutex> lock(shared_.globalMutex);
                    shared_.error = "read error: " + file.path;
                    shared_.failed.store(true);
                    return;
                }

                if (names_.bytes() + groups_.bytes() > shared_.tableBudget) {
                    // Слишком много групп: сбрасываем частичные суммы в общую
                    // таблицу, чтобы память потока не росла без ограничения.
                    std::lock_guard<std::mutex> lock(shared_.globalMutex);
                    mergeInto(shared_.globalNames, shared_.globalGroups, names_, groups_, params_.valueCount);
                    ++flushes_;
                }

                shared_.bytesDone.fetch_add(end - begin, std::memory_order_relaxed);
                if (governor != nullptr) governor->tick();
            }
        }
    }

    void mergeToGlobal() {
        std::lock_guard<std::mutex> lock(shared_.globalMutex);
        mergeInto(shared_.globalNames, shared_.globalGroups, names_, groups_, params_.valueCount);
    }

    std::size_t flushes() const { return flushes_; }

private:
    void reserve(std::size_t bytes) {
        if (bytes <= capacity_) return;
        // Буфер выровнен на страницу и его размер кратен странице.
        const std::size_t page = 4096;
        const std::size_t capacity = (bytes + page - 1) / page * page;
        char* bigger = static_cast<char*>(alignedAlloc(capacity + kPad, page));
        if (bigger == nullptr) throw std::bad_alloc();
        if (buffer_ != nullptr) {
            std::memcpy(bigger, buffer_, capacity_);
            alignedFree(buffer_);
        }
        buffer_ = bigger;
        capacity_ = capacity;
    }

    // Чанк [begin, end) обрабатывает строки, которые начинаются сразу после
    // '\n', стоящего в позиции p из [begin, end). Так каждая строка достаётся
    // ровно одному чанку, а заголовок (строка с позиции 0) пропускается сам.
    bool processChunk(const InputFile& file, std::uint64_t begin, std::uint64_t end) {
        const std::size_t limit = static_cast<std::size_t>(end - begin);
        const std::size_t request = static_cast<std::size_t>(std::min<std::uint64_t>(limit + shared_.overrun, file.size - begin));
        std::size_t size = 0;
        if (!reader_.readAt(file, begin, buffer_, request, size)) return false;

        // Последняя строка чанка должна целиком лежать в буфере: ищем '\n'
        // не раньше limit; если строка длиннее запаса — дочитываем.
        std::size_t scanned = std::min(limit, size);

        while (std::memchr(buffer_ + scanned, '\n', size - scanned) == nullptr && begin + size < file.size) {
            scanned = size;
            const std::size_t extra = std::max(size, shared_.overrun);
            reserve(size + extra);
            std::size_t got = 0;
            if (!reader_.readAt(file, begin + size, buffer_ + size, extra, got)) return false;
            if (got == 0) break;
            size += got;
        }

        std::memset(buffer_ + size, '\n', kPad);
        parse(buffer_, buffer_ + std::min(limit, size));
        return true;
    }

    // Строки чанка перечисляются по битовой маске '\n': каждые 64 байта
    // превращаются в 64-битное слово (бит i = 1, если байт i — '\n'), и начала
    // строк достаются из него командой tzcnt. Искать конец каждой строки не
    // нужно, а следующая строка не зависит от разбора предыдущей.
    void parse(const char* data, const char* limit) {
        const std::size_t size = static_cast<std::size_t>(limit - data);

        for (std::size_t base = 0; base < size; base += 64) {
            std::uint64_t newlines = charMask64(data + base, '\n');
            if (size - base < 64) newlines &= (1ull << (size - base)) - 1;

            while (newlines != 0) {
                parseLine(data + base + ctz64(newlines) + 1);
                newlines &= newlines - 1;
            }
        }
    }

    void parseLine(const char* line) {
        const Params& params = params_;
        const char separator = params.csvSeparator;
        const unsigned dateLength = static_cast<unsigned>(params.dateLength);
        const char* nameBegin;
        const char* nameEnd;
        bool fixedDate;

        // Одна маска на первые 32 байта строки: где разделители, где '\n'.
        std::uint32_t separators = charMask32(line, separator);
        const std::uint32_t newlines = charMask32(line, '\n');
        separators &= (newlines & (0u - newlines)) - 1u;  // только до первого '\n'

        if ((separators & (separators - 1)) != 0) {
            nameBegin = line + ctz32(separators) + 1;
            separators &= separators - 1;
            const unsigned nameEndIndex = ctz32(separators);
            nameEnd = line + nameEndIndex;
            separators &= separators - 1;
            // Дата фиксированной ширины: следующий разделитель ровно через dateLength.
            fixedDate = separators != 0 && ctz32(separators) == nameEndIndex + 1 + dateLength;
        } else {
            // Длинные id и имя (не уместились в 32 байта) или некорректная строка.
            const char* c = line;
            while (*c != separator && *c != '\n') ++c;
            if (*c == '\n') return;

            nameBegin = ++c;
            while (*c != separator && *c != '\n') ++c;
            if (*c == '\n') return;

            nameEnd = c;
            const char* dateEnd = c + 1;
            while (*dateEnd != separator && *dateEnd != '\n') ++dateEnd;
            fixedDate = *dateEnd == separator && static_cast<unsigned>(dateEnd - (c + 1)) == dateLength;
        }

        const char* date = nameEnd + 1;
        const char* values;
        std::uint32_t key;

        if (fixedDate) {
            key = parseDateFixed(date, params);
            if (key - params.startKey > params.span || !isValidFixedDate(date, key, params)) return;
            values = date + dateLength + 1;
        } else {
            const char* dateEnd = date;
            while (*dateEnd != separator && *dateEnd != '\n') ++dateEnd;
            if (*dateEnd == '\n') return;

            key = parseDateGeneric(date, dateEnd, params);
            if (key == 0 || key - params.startKey > params.span) return;
            values = dateEnd + 1;
        }

        const std::uint32_t nameId = names_.intern(nameBegin, static_cast<std::uint32_t>(nameEnd - nameBegin));
        double* sums = groups_.at((static_cast<std::uint64_t>(nameId) << 32) | key);
        parseValues(values, sums);
    }

    // Значения разбираются последовательно: быстрый путь parseNumber сам
    // останавливается на разделителе. (Проверенные альтернативы — SWAR по 8 цифр
    // и независимый разбор полей по маске разделителей — оказались не быстрее.)
    void parseValues(const char* values, double* sums) {
        const char separator = params_.csvSeparator;
        const unsigned valueCount = params_.valueCount;
        const char* c = values;

        for (unsigned i = 0; i < valueCount; ++i) {
            sums[i] += parseNumber(c, params_);
            if (*c != separator) break;
            ++c;
        }
    }

    Shared& shared_;
    const Params& params_;
    unsigned index_;
    std::size_t firstFile_;
    NameTable names_;
    GroupTable groups_;
    Reader reader_;
    char* buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t flushes_ = 0;
};

// ---------------------------------------------------------------------------
// Заголовок, вывод, аргументы
// ---------------------------------------------------------------------------

bool readHeader(InputFile& file, char separator, std::vector<std::string>& columns) {
    Reader reader;
    std::string header;
    std::uint64_t offset = 0;
    char block[4096];

    for (;;) {
        std::size_t got = 0;
        if (!reader.readAt(file, offset, block, sizeof(block), got)) return false;
        const char* newline = static_cast<const char*>(std::memchr(block, '\n', got));
        header.append(block, newline != nullptr ? static_cast<std::size_t>(newline - block) : got);
        if (newline != nullptr || got < sizeof(block)) break;
        offset += got;
    }

    if (!header.empty() && header.back() == '\r') header.pop_back();

    columns.clear();
    std::size_t start = 0;

    for (;;) {
        const std::size_t position = header.find(separator, start);
        columns.push_back(header.substr(start, position == std::string::npos ? std::string::npos : position - start));
        if (position == std::string::npos) break;
        start = position + 1;
    }

    return true;
}

void appendDate(std::string& out, std::uint32_t key, const Params& params) {
    const std::uint32_t parts[3] = {key / 10000, key / 100 % 100, key % 100};
    char text[16];

    for (int i = 0; i < 3; ++i) {
        if (i > 0) out += params.dateSeparator;
        const int part = params.dateOrder[i];
        const std::uint32_t value = parts[part];

        if (part == 0) {
            const auto result = std::to_chars(text, text + sizeof(text), value);
            out.append(text, result.ptr);
        } else {
            out += static_cast<char>('0' + value / 10);
            out += static_cast<char>('0' + value % 10);
        }
    }
}

char parseCharArgument(const std::string& value) {
    if (value == "tab" || value == "\\t") return '\t';
    if (value == "space") return ' ';
    return value.empty() ? '\0' : value[0];
}

void printUsage() {
    std::fprintf(stderr,
                 "usage: solution [options] [file.csv ...]\n"
                 "  --from DATE            first date of the range (inclusive), default 2026-11-01\n"
                 "  --to DATE              last date of the range (inclusive), default 2026-11-10\n"
                 "  --format YMD|DMY|MDY   order of date fields, default YMD\n"
                 "  --sep C                CSV separator (',' ';' tab), default ','\n"
                 "  --date-sep C           date separator ('-' '.' '/'), default '-'\n"
                 "  --dec C                decimal separator ('.' or ','), default '.'\n"
                 "  --out FILE             also save the result to FILE\n"
                 "  --threads N            fixed number of threads (default: chosen by measurement)\n"
                 "  --chunk-kb N           chunk size in KB (default: from L2 cache size)\n"
                 "  --mem-mb N             memory budget in MB (default: from free RAM)\n"
                 "  --quiet                do not print hardware info\n"
                 "Without files data/data1.csv and data/data2.csv are used.\n"
                 "The result goes to stdout, hardware info and timings to stderr.\n");
}

}  // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;

    // Значения по умолчанию совпадают с test14.
    std::vector<std::string> filenames = {"data/data1.csv", "data/data2.csv"};
    std::string startDate = "2026-11-01";
    std::string endDate = "2026-11-10";
    std::string dateFormat = "YMD";
    Params params;
    params.csvSeparator = ',';
    params.dateSeparator = '-';
    params.decimalSeparator = '.';

    std::string outputPath;
    unsigned requestedThreads = 0;
    std::size_t requestedChunkKb = 0;
    std::size_t requestedMemoryMb = 0;
    bool quiet = false;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool hasValue = i + 1 < argc;

        if (argument == "--help" || argument == "-h") {
            printUsage();
            return 0;
        } else if (argument == "--quiet") {
            quiet = true;
        } else if (argument == "--from" && hasValue) {
            startDate = argv[++i];
        } else if (argument == "--to" && hasValue) {
            endDate = argv[++i];
        } else if (argument == "--format" && hasValue) {
            dateFormat = argv[++i];
        } else if (argument == "--sep" && hasValue) {
            params.csvSeparator = parseCharArgument(argv[++i]);
        } else if (argument == "--date-sep" && hasValue) {
            params.dateSeparator = parseCharArgument(argv[++i]);
        } else if (argument == "--dec" && hasValue) {
            params.decimalSeparator = parseCharArgument(argv[++i]);
        } else if (argument == "--out" && hasValue) {
            outputPath = argv[++i];
        } else if (argument == "--threads" && hasValue) {
            requestedThreads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (argument == "--chunk-kb" && hasValue) {
            requestedChunkKb = std::strtoull(argv[++i], nullptr, 10);
        } else if (argument == "--mem-mb" && hasValue) {
            requestedMemoryMb = std::strtoull(argv[++i], nullptr, 10);
        } else if (argument.size() > 1 && argument[0] == '-' && argument[1] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            printUsage();
            return 1;
        } else {
            positional.push_back(argument);
        }
    }

    if (!positional.empty()) filenames = positional;

    if (!setDateFormat(params, dateFormat)) {
        std::fprintf(stderr, "bad --format: %s (expected YMD, DMY or MDY)\n", dateFormat.c_str());
        return 1;
    }

    if (params.csvSeparator == '\0' || params.csvSeparator == '\n' || params.csvSeparator == '\r' || params.csvSeparator == params.decimalSeparator ||
        params.csvSeparator == params.dateSeparator || params.dateSeparator == '\0' || params.decimalSeparator == '\0') {
        std::fprintf(stderr, "bad separators\n");
        return 1;
    }

    const std::uint32_t startKey = parseDateGeneric(startDate.data(), startDate.data() + startDate.size(), params);
    const std::uint32_t finishKey = parseDateGeneric(endDate.data(), endDate.data() + endDate.size(), params);

    if (startKey == 0 || finishKey == 0) {
        std::fprintf(stderr, "bad --from/--to for format %s\n", dateFormat.c_str());
        return 1;
    }

    const auto processingStart = Clock::now();

    // --- Железо и файлы ------------------------------------------------------
    const Hardware hw = detectHardware();

    std::vector<InputFile> files(filenames.size());
    std::uint64_t totalBytes = 0;

    for (std::size_t i = 0; i < files.size(); ++i) {
        files[i].path = filenames[i];
        if (!openInput(files[i])) {
            std::fprintf(stderr, "cannot open %s\n", filenames[i].c_str());
            return 1;
        }
        totalBytes += files[i].size;
    }

    // Число столбцов значений берётся из заголовка (у всех файлов одинаковое).
    std::vector<std::string> header;
    std::vector<std::string> valueNames;

    for (InputFile& file : files) {
        if (file.size == 0) continue;
        if (!readHeader(file, params.csvSeparator, header)) {
            std::fprintf(stderr, "cannot read %s\n", file.path.c_str());
            return 1;
        }

        if (header.size() < 4) {
            std::fprintf(stderr, "%s: expected id, name, date and at least one value column\n", file.path.c_str());
            return 1;
        }

        if (valueNames.empty()) {
            valueNames.assign(header.begin() + 3, header.end());
        } else if (header.size() - 3 != valueNames.size()) {
            std::fprintf(stderr, "%s: different number of columns\n", file.path.c_str());
            return 1;
        }
    }

    if (valueNames.empty()) {
        for (int i = 1; i <= 7; ++i) valueNames.push_back("float64_" + std::to_string(i));
    }

    params.valueCount = static_cast<unsigned>(valueNames.size());
    params.startKey = startKey;
    params.span = finishKey - startKey;
    const bool emptyRange = finishKey < startKey;

    // --- План обработки под железо -------------------------------------------
    // Бюджет памяти: четверть свободной RAM, но не больше 2 GB.
    std::uint64_t memoryBudget = requestedMemoryMb != 0 ? requestedMemoryMb * 1024ull * 1024 : (hw.availableRam != 0 ? hw.availableRam / 4 : 256ull * 1024 * 1024);
    if (requestedMemoryMb == 0) memoryBudget = std::min<std::uint64_t>(memoryBudget, 2048ull * 1024 * 1024);
    memoryBudget = std::max<std::uint64_t>(memoryBudget, 4ull * 1024 * 1024);

    // Размер чанка: половина L2, приходящегося на один аппаратный поток, —
    // буфер с только что прочитанными данными остаётся в L2 на время разбора,
    // вторая половина L2 — под таблицы и стек. Кратно гранулярности (64 KB на
    // Windows), чтобы смещения чтения были выровнены на страницы.
    const std::size_t alignment = requestedChunkKb != 0 ? hw.pageSize : hw.granularity;
    std::size_t chunkSize = requestedChunkKb != 0 ? requestedChunkKb * 1024 : hw.l2 / hw.threadsPerL2 / 2;
    chunkSize = std::min<std::size_t>(std::max<std::size_t>(chunkSize, requestedChunkKb != 0 ? hw.pageSize : 64 * 1024), 64ull * 1024 * 1024);
    chunkSize = std::max(alignment, chunkSize / alignment * alignment);

    // Если памяти совсем мало — уменьшаем чанк (буферы — не больше половины бюджета).
    const std::size_t overrun = hw.pageSize;
    while (chunkSize > hw.pageSize && chunkSize + overrun + 256 * 1024 > memoryBudget / 2) {
        chunkSize = std::max(hw.pageSize, chunkSize / 2 / hw.pageSize * hw.pageSize);
    }

    std::uint64_t totalChunks = 0;
    for (InputFile& file : files) {
        file.chunks = emptyRange ? 0 : (file.size + chunkSize - 1) / chunkSize;
        totalChunks += file.chunks;
    }

    // Верхняя граница числа потоков: не больше физических ядер (SMT-"близнецы"
    // делят L2 и шину памяти ядра, а задача упирается в память, а не в
    // арифметику) и хотя бы ~4 чанка работы на поток. Каждому потоку нужен буфер
    // чанка и минимум памяти под таблицы — это тоже ограничивает число потоков.
    // Сколько из них реально запустить, решает регулятор (Governor) по скорости.
    unsigned threads = requestedThreads;
    if (threads == 0) {
        const std::uint64_t byWork = std::max<std::uint64_t>(1, totalChunks / 4);
        const std::uint64_t byMemory = std::max<std::uint64_t>(1, memoryBudget / 2 / (chunkSize + overrun + kPad + 256 * 1024));
        threads = static_cast<unsigned>(std::min<std::uint64_t>({hw.cores, byWork, byMemory}));
    }
    threads = static_cast<unsigned>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(threads, std::max<std::uint64_t>(1, totalChunks))));
    // Старт регулятора — половина физических ядер: задача, упирающаяся в память,
    // на клиентских машинах редко масштабируется дальше; удвоение он проверит сам.
    const unsigned initialThreads = std::min(threads, std::max(1u, hw.cores / 2));
    const bool adaptive = requestedThreads == 0 && threads > initialThreads;

    Shared shared;
    shared.params = &params;
    shared.files = &files;
    shared.cursors.reset(new ChunkCursor[files.size()]);
    shared.chunkSize = chunkSize;
    shared.overrun = overrun;
    shared.totalBytes = totalBytes;
    shared.tableBudget = static_cast<std::size_t>(std::max<std::uint64_t>(256 * 1024, (memoryBudget - threads * (chunkSize + overrun)) / (threads + 1)));
    shared.globalGroups.init(params.valueCount, kInitialGroups);

    // --- Параллельная обработка ---------------------------------------------
    // Поток i начинает с файла i % F: потоки равномерно распределены по файлам.
    // Главный поток — тоже рабочий (номер 0), он же ведёт регулятор.
    std::vector<std::unique_ptr<Worker>> workers(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);
    const std::size_t fileCount = std::max<std::size_t>(1, files.size());

    auto spawn = [&](unsigned i) {
        workers[i] = std::make_unique<Worker>(shared, i, i % fileCount);
        Worker* worker = workers[i].get();
        pool.emplace_back([worker]() { worker->run(nullptr); });
    };

    workers[0] = std::make_unique<Worker>(shared, 0, 0);
    std::string threadPlan;

    if (adaptive) {
        Governor governor(shared, threads, spawn);
        governor.start(initialThreads);
        workers[0]->run(&governor);
        for (std::thread& thread : pool) thread.join();
        threadPlan = governor.describe();
    } else {
        shared.allowedThreads.store(threads);
        for (unsigned i = 1; i < threads; ++i) spawn(i);
        workers[0]->run(nullptr);
        for (std::thread& thread : pool) thread.join();
        threadPlan = "fixed " + std::to_string(threads) + " threads";
    }

    if (shared.failed.load()) {
        std::fprintf(stderr, "%s\n", shared.error.c_str());
        return 1;
    }

    std::size_t flushes = 0;
    unsigned threadsUsed = 0;
    for (auto& worker : workers) {
        if (!worker) continue;
        ++threadsUsed;
        flushes += worker->flushes();
        worker->mergeToGlobal();
    }

    for (InputFile& file : files) closeInput(file);

    // --- Сортировка: имена сортируются один раз, группы — по целому ключу ----
    const NameTable& names = shared.globalNames;
    std::vector<std::uint32_t> nameOrder(names.size());
    for (std::uint32_t i = 0; i < nameOrder.size(); ++i) nameOrder[i] = i;
    std::sort(nameOrder.begin(), nameOrder.end(), [&](std::uint32_t left, std::uint32_t right) { return names.name(left) < names.name(right); });

    std::vector<std::uint32_t> rank(names.size());
    for (std::uint32_t i = 0; i < nameOrder.size(); ++i) rank[nameOrder[i]] = i;

    struct OutputRow {
        std::uint64_t order;  // rank(name) << 32 | YYYYMMDD
        std::uint32_t nameId;
        const double* values;
    };

    std::vector<OutputRow> rows;
    rows.reserve(shared.globalGroups.count());
    shared.globalGroups.forEach([&](std::uint64_t key, const double* values) {
        const std::uint32_t nameId = static_cast<std::uint32_t>(key >> 32);
        rows.push_back({(static_cast<std::uint64_t>(rank[nameId]) << 32) | (key & 0xffffffffull), nameId, values});
    });
    std::sort(rows.begin(), rows.end(), [](const OutputRow& left, const OutputRow& right) { return left.order < right.order; });

    const auto processingEnd = Clock::now();

    // --- Вывод ---------------------------------------------------------------
    std::string out;
    out.reserve(64 + rows.size() * (32 + params.valueCount * 12));
    out += "string";
    out += params.csvSeparator;
    out += "date";
    for (const std::string& name : valueNames) {
        out += params.csvSeparator;
        out += name;
    }
    out += '\n';

    char number[64];

    for (const OutputRow& row : rows) {
        out += names.name(row.nameId);
        out += params.csvSeparator;
        appendDate(out, static_cast<std::uint32_t>(row.order & 0xffffffffull), params);

        for (unsigned i = 0; i < params.valueCount; ++i) {
            out += params.csvSeparator;
            // Тот же формат, что у std::cout << double: %g, 6 значащих цифр.
            const auto result = std::to_chars(number, number + sizeof(number), row.values[i], std::chars_format::general, 6);
            if (params.decimalSeparator != '.') std::replace(number, result.ptr, '.', params.decimalSeparator);
            out.append(number, result.ptr);
        }

        out += '\n';
    }

    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);

    if (!outputPath.empty()) {
        std::FILE* output = std::fopen(outputPath.c_str(), "w");
        if (output == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", outputPath.c_str());
            return 1;
        }
        std::fwrite(out.data(), 1, out.size(), output);
        std::fclose(output);
    }

    const auto totalEnd = Clock::now();
    const double processingMs = std::chrono::duration<double, std::milli>(processingEnd - processingStart).count();
    const double totalMs = std::chrono::duration<double, std::milli>(totalEnd - processingStart).count();

    if (!quiet) {
        std::fprintf(stderr, "\n--- HARDWARE ---\n");
        std::fprintf(stderr, "CPU: %u logical / %u cores, L1d %zu KB, L2 %zu KB (shared by %u), L3 %zu KB\n", hw.logicalCpus, hw.cores, hw.l1d / 1024,
                     hw.l2 / 1024, hw.threadsPerL2, hw.l3 / 1024);
        std::fprintf(stderr, "Memory: page %zu B, granularity %zu KB, available RAM %llu MB, budget %llu MB\n", hw.pageSize, hw.granularity / 1024,
                     static_cast<unsigned long long>(hw.availableRam >> 20), static_cast<unsigned long long>(memoryBudget >> 20));
        std::fprintf(stderr, "Plan: chunk %zu KB, %llu chunks, %zu files, %.1f MB input, max %u threads, %zu groups, flushes %zu\n", chunkSize / 1024,
                     static_cast<unsigned long long>(totalChunks), files.size(), totalBytes / 1048576.0, threads, rows.size(), flushes);
        std::fprintf(stderr, "Threads: %s (%u started)\n", threadPlan.c_str(), threadsUsed);
    }

    std::fprintf(stderr, "\n--- TIME ---\n");
    std::fprintf(stderr, "Processing: %.3f ms\n", processingMs);
    std::fprintf(stderr, "Total (with output): %.3f ms\n", totalMs);
    return 0;
}
