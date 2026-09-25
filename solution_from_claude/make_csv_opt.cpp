// make_csv_opt.cpp — быстрый генератор тестовых CSV (развитие make_csv.cpp).
//
// Формат тот же, что у make_csv: заголовок
//   id,string,data,float64_1,...,float64_7
// строки: id по порядку, случайное имя из 50, случайная дата 2026-09-01..2026-12-12,
// 7 равномерных значений [0, 1) в формате std::cout << double (%g, 6 значащих
// цифр); перевод строки как у текстового ofstream (на Windows — "\r\n").
//
// Что ускоряет (подробно — в SOLUTION.md):
//  * xoshiro256** вместо mt19937 + uniform_real_distribution;
//  * число печатается не через ostream (локаль, форматирование, виртуальные
//    вызовы на каждый <<), а целочисленной арифметикой: x = r / 2^53, и
//    6 значащих цифр — это точное округление r * 10^k / 2^53; результат
//    совпадает с ostream/to_chars байт в байт;
//  * id увеличивается прямо в ASCII-буфере, имена и даты копируются memcpy;
//  * строки генерируются блоками параллельно, главный поток пишет готовые блоки
//    в файл по порядку большими кусками; буферов T + 2 — память ограничена;
//  * seed блока зависит только от (seed, файл, блок) — одинаковый --seed даёт
//    одинаковый файл при любом числе потоков.
//
// Сборка (MSYS2 ucrt64, из корня репозитория):
//   g++ -std=c++17 -O2 solution_from_claude/make_csv_opt.cpp -o build/make_csv_opt.exe
//   build/make_csv_opt.exe                 (2 файла по 1 000 000 строк в data/, как make_csv)
//   build/make_csv_opt.exe --help

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Генератор случайных чисел
// ---------------------------------------------------------------------------

std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// xoshiro256** (Blackman, Vigna): 4 слова состояния, несколько сдвигов и
// умножений на число — в разы быстрее mt19937 при хорошем качестве.
class Xoshiro256 {
public:
    explicit Xoshiro256(std::uint64_t seed) {
        for (std::uint64_t& word : state_) word = splitmix64(seed);
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(state_[1] * 5, 7) * 9;
        const std::uint64_t t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);
        return result;
    }

    // Равномерное целое [0, n) методом Лемира (умножение вместо деления).
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(((next() >> 32) * n) >> 32); }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::uint64_t state_[4];
};

// ---------------------------------------------------------------------------
// Форматирование значения
// ---------------------------------------------------------------------------

// Печатает x = r / 2^53 (r < 2^53, то есть x из [0, 1)) так же, как
// std::cout << x: printf "%.6g" — 6 значащих цифр, без хвостовых нулей.
//
// Для x >= 1e-4 это запись 0.[нули]dddddd. Шесть цифр — это q = round(x * 10^k),
// k = 5 - e, где 10^e <= x < 10^(e+1). Так как x = r / 2^53, то
// x * 10^k = r * 10^k / 2^53 — целое произведение (до 2^83, 128 бит) и сдвиг,
// а остаток сдвига даёт точное округление "к ближайшему, при равенстве — к
// чётному", как у printf. Для x < 1e-4 (экспоненциальная запись, ~0.01% чисел)
// используется std::to_chars с тем же форматом.
char* writeValue(char* out, std::uint64_t r) {
    const double x = static_cast<double>(r) * 0x1.0p-53;

    if (r == 0) {
        *out++ = '0';
        return out;
    }

#if defined(__SIZEOF_INT128__)
    if (x >= 1e-4) {
        static constexpr std::uint64_t kPow10[10] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
        int e = x >= 0.1 ? -1 : x >= 0.01 ? -2 : x >= 0.001 ? -3 : -4;
        __extension__ typedef unsigned __int128 Uint128;  // 128-битное целое GCC/Clang
        const Uint128 product = static_cast<Uint128>(r) * kPow10[5 - e];
        std::uint64_t q = static_cast<std::uint64_t>(product >> 53);
        const std::uint64_t remainder = static_cast<std::uint64_t>(product) & ((1ull << 53) - 1);
        const std::uint64_t half = 1ull << 52;
        if (remainder > half || (remainder == half && (q & 1) != 0)) ++q;

        if (q == 1000000) {  // округление перенесло в следующий порядок: 0.0999999... -> 0.1
            q = 100000;
            ++e;
            if (e == 0) {
                *out++ = '1';
                return out;
            }
        }

        *out++ = '0';
        *out++ = '.';
        for (int zeros = -e - 1; zeros > 0; --zeros) *out++ = '0';

        char digits[6];
        for (int i = 5; i >= 0; --i) {
            digits[i] = static_cast<char>('0' + q % 10);
            q /= 10;
        }

        int length = 6;
        while (digits[length - 1] == '0') --length;
        std::memcpy(out, digits, 6);
        return out + length;
    }
#endif

    return std::to_chars(out, out + 32, x, std::chars_format::general, 6).ptr;
}

// ---------------------------------------------------------------------------
// Данные
// ---------------------------------------------------------------------------

const char* const kNames[] = {
    "Alexander", "Dmitry", "Elena", "Maria", "Ivan",
    "Anna", "Sergey", "Olga", "Maxim", "Natalia",
    "Alexey", "Andrey", "Anton", "Artem", "Boris",
    "Victor", "Vladimir", "Denis", "Evgeny", "Kirill",
    "Mikhail", "Nikolay", "Oleg", "Pavel", "Roman",
    "Stanislav", "Yuri", "Alina", "Anastasia", "Daria",
    "Ekaterina", "Irina", "Ksenia", "Marina", "Nadezhda",
    "Polina", "Sofia", "Tatiana", "Yulia", "Victoria",
    "Grigory", "Ilya", "Konstantin", "Leonid", "Matvey",
    "Nikita", "Petr", "Ruslan", "Svetlana", "Vera",
};

constexpr int kValueCount = 7;
// Верхняя граница длины строки: id до 20 цифр + имя до 10 + дата 10 + 7 чисел
// до 11 символов + разделители + "\r\n" < 140. Запас для memcpy по 16 байт —
// в конце буфера.
constexpr std::size_t kMaxLine = 140;

// Имя вместе с запятой, дополненное до 16 байт: копируется одной
// 16-байтной memcpy без ветвлений, длина берётся из таблицы.
struct Token {
    char text[16];
    std::uint32_t length;
};

std::vector<Token> makeNameTokens() {
    std::vector<Token> tokens;
    for (const char* name : kNames) {
        Token token{};
        const std::size_t length = std::strlen(name);
        std::memcpy(token.text, name, length);
        token.text[length] = ',';
        token.length = static_cast<std::uint32_t>(length + 1);
        tokens.push_back(token);
    }
    return tokens;
}

// Те же даты, что у make_csv: 2026-09-01 .. 2026-12-12 (103 дня), с запятой.
std::vector<Token> makeDateTokens() {
    std::vector<Token> tokens;
    const int lastDay[4] = {30, 31, 30, 12};

    for (int month = 9; month <= 12; ++month) {
        for (int day = 1; day <= lastDay[month - 9]; ++day) {
            Token token{};
            std::snprintf(token.text, sizeof(token.text), "2026-%02d-%02d,", month, day);
            token.length = 11;
            tokens.push_back(token);
        }
    }

    return tokens;
}

// Десятичный счётчик id прямо в ASCII: увеличение на 1 почти всегда меняет
// только последнюю цифру, без деления и to_chars на каждой строке.
class AsciiCounter {
public:
    explicit AsciiCounter(std::uint64_t value) {
        char text[24];
        const auto result = std::to_chars(text, text + sizeof(text), value);
        length_ = static_cast<int>(result.ptr - text);
        std::memcpy(digits_ + sizeof(digits_) - length_, text, length_);
    }

    char* write(char* out) const {
        std::memcpy(out, digits_ + sizeof(digits_) - length_, length_);
        return out + length_;
    }

    void increment() {
        int i = static_cast<int>(sizeof(digits_)) - 1;
        while (digits_[i] == '9') digits_[i--] = '0';
        if (i < static_cast<int>(sizeof(digits_)) - length_) {
            digits_[i] = '1';
            ++length_;
        } else {
            ++digits_[i];
        }
    }

private:
    char digits_[24] = {};
    int length_ = 0;
};

// ---------------------------------------------------------------------------
// Генерация и запись
// ---------------------------------------------------------------------------

struct Job {
    std::vector<std::string> paths;
    std::uint64_t rows = 0;
    std::uint64_t seed = 0;
    std::uint64_t rowsPerBlock = 0;
    std::uint64_t blocksPerFile = 0;
    bool crlf = true;
};

// Конвейер "генераторы -> писатель" с ограниченным пулом буферов.
//
// Запись в один файл на Windows не распараллеливается (проверено: 2-8 потоков
// пишут не быстрее одного), поэтому писатель один — главный поток, и он пишет
// блоки строго по порядку. Генераторы берут свободный буфер из пула, затем
// номер блока, заполняют буфер и отдают писателю. Буферов T + 2: писателю
// всегда есть что писать, а память ограничена — (T + 2) блока по ~0.6 MB,
// сколько бы строк ни генерировалось. Буфер берётся до номера блока, поэтому
// самый ранний незаписанный блок всегда у потока, у которого уже есть буфер, —
// взаимная блокировка невозможна.
class Pipeline {
public:
    Pipeline(const Job& job, unsigned slots, std::uint64_t totalBlocks) : job_(job), ready_(totalBlocks), buffers_(slots) {
        for (std::vector<char>& buffer : buffers_) free_.push_back(&buffer);
    }

    bool open() {
        for (const std::string& path : job_.paths) {
            std::FILE* file = std::fopen(path.c_str(), "wb");
            if (file == nullptr) {
                std::fprintf(stderr, "ERROR: %s\n", path.c_str());
                return false;
            }
            std::setvbuf(file, nullptr, _IONBF, 0);  // пишем большими блоками — свой буфер не нужен
            files_.push_back(file);
        }

        const char* header = "id,string,data,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7";
        for (std::FILE* file : files_) {
            std::fputs(header, file);
            std::fputs(job_.crlf ? "\r\n" : "\n", file);
        }
        return true;
    }

    std::vector<char>* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        freed_.wait(lock, [&] { return !free_.empty(); });
        std::vector<char>* buffer = free_.back();
        free_.pop_back();
        return buffer;
    }

    void release(std::vector<char>* buffer) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_.push_back(buffer);
        }
        freed_.notify_one();
    }

    void submit(std::uint64_t block, std::vector<char>* buffer, std::size_t size) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_[block] = {buffer, size};
        }
        readyCv_.notify_one();  // ждёт только писатель
    }

    // Писатель: блоки по порядку, как только очередной готов.
    bool writeAll() {
        bool ok = true;

        for (std::uint64_t block = 0; block < ready_.size(); ++block) {
            Ready item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                readyCv_.wait(lock, [&] { return ready_[block].buffer != nullptr; });
                item = ready_[block];
            }

            std::FILE* file = files_[block / job_.blocksPerFile];
            if (std::fwrite(item.buffer->data(), 1, item.size, file) != item.size) ok = false;
            release(item.buffer);
        }

        for (std::FILE* file : files_) ok = std::fclose(file) == 0 && ok;
        return ok;
    }

private:
    struct Ready {
        std::vector<char>* buffer = nullptr;
        std::size_t size = 0;
    };

    const Job& job_;
    std::vector<std::FILE*> files_;
    std::mutex mutex_;
    std::condition_variable freed_;
    std::condition_variable readyCv_;
    std::vector<Ready> ready_;
    std::vector<std::vector<char>> buffers_;
    std::vector<std::vector<char>*> free_;
};

void generateBlock(const Job& job, std::uint64_t block, const std::vector<Token>& names, const std::vector<Token>& dates, std::vector<char>& buffer, std::size_t& size) {
    const std::uint64_t fileIndex = block / job.blocksPerFile;
    const std::uint64_t blockInFile = block % job.blocksPerFile;
    const std::uint64_t firstRow = blockInFile * job.rowsPerBlock;
    const std::uint64_t rows = std::min(job.rowsPerBlock, job.rows - firstRow);

    std::uint64_t seedState = job.seed ^ (fileIndex * 0xD1B54A32D192ED03ull) ^ (blockInFile * 0x9E3779B97F4A7C15ull);
    Xoshiro256 rng(splitmix64(seedState));
    AsciiCounter id(firstRow + 1);

    buffer.resize(rows * kMaxLine + 64);
    char* out = buffer.data();
    const auto nameCount = static_cast<std::uint32_t>(names.size());
    const auto dateCount = static_cast<std::uint32_t>(dates.size());

    for (std::uint64_t row = 0; row < rows; ++row) {
        out = id.write(out);
        *out++ = ',';
        id.increment();

        const Token& name = names[rng.below(nameCount)];
        std::memcpy(out, name.text, 16);
        out += name.length;

        const Token& date = dates[rng.below(dateCount)];
        std::memcpy(out, date.text, 16);
        out += date.length;

        for (int i = 0; i < kValueCount; ++i) {
            if (i > 0) *out++ = ',';
            out = writeValue(out, rng.next() >> 11);
        }

        if (job.crlf) *out++ = '\r';
        *out++ = '\n';
    }

    size = static_cast<std::size_t>(out - buffer.data());
}

void printUsage() {
    std::fprintf(stderr,
                 "usage: make_csv_opt [options]\n"
                 "  --rows N      rows in each file (default 1000000)\n"
                 "  --files N     number of files (default 2: data1.csv, data2.csv)\n"
                 "  --dir DIR     output directory (default data)\n"
                 "  --seed N      seed (default random); the same seed gives the same files\n"
                 "  --threads N   generator threads (default: half of physical cores)\n"
                 "  --lf / --crlf line ending (default: like a text-mode ofstream on this OS)\n");
}

}  // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;

    Job job;
    job.rows = 1000000;
    std::string directory = "data";
    int fileCount = 2;
    unsigned threads = 0;
    bool seedGiven = false;
#if defined(_WIN32)
    job.crlf = true;
#else
    job.crlf = false;
#endif

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool hasValue = i + 1 < argc;

        if (argument == "--help" || argument == "-h") {
            printUsage();
            return 0;
        } else if (argument == "--rows" && hasValue) {
            job.rows = std::strtoull(argv[++i], nullptr, 10);
        } else if (argument == "--files" && hasValue) {
            fileCount = std::atoi(argv[++i]);
        } else if (argument == "--dir" && hasValue) {
            directory = argv[++i];
        } else if (argument == "--seed" && hasValue) {
            job.seed = std::strtoull(argv[++i], nullptr, 10);
            seedGiven = true;
        } else if (argument == "--threads" && hasValue) {
            threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (argument == "--lf") {
            job.crlf = false;
        } else if (argument == "--crlf") {
            job.crlf = true;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            printUsage();
            return 1;
        }
    }

    if (fileCount < 1) {
        std::fprintf(stderr, "bad --files\n");
        return 1;
    }

    if (!seedGiven) {
        std::random_device device;
        job.seed = (static_cast<std::uint64_t>(device()) << 32) ^ device() ^ static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    }

    for (int i = 1; i <= fileCount; ++i) job.paths.push_back(directory + "/data" + std::to_string(i) + ".csv");

    const auto start = Clock::now();

    // Блок 4096 строк (~360 KB данных): на скорость размер блока от 2K до 16K
    // строк не влияет (узкое место — запись), а память растёт пропорционально,
    // поэтому берётся маленький.
    job.rowsPerBlock = 4096;
    job.blocksPerFile = std::max<std::uint64_t>(1, (job.rows + job.rowsPerBlock - 1) / job.rowsPerBlock);
    const std::uint64_t totalBlocks = job.rows == 0 ? 0 : job.blocksPerFile * fileCount;

    if (threads == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        // Писатель один, и запись — узкое место: генераторов нужно столько,
        // чтобы успевать за ним. Лишние просто ждали бы свободный буфер, занимая
        // память. По умолчанию — половина физических ядер (оценка: четверть
        // логических потоков); на 16-ядерной машине 8 потоков не медленнее 16.
        threads = std::max(1u, hardware / 4);
    }
    threads = static_cast<unsigned>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(threads, std::max<std::uint64_t>(1, totalBlocks))));

    const std::vector<Token> names = makeNameTokens();
    const std::vector<Token> dates = makeDateTokens();
    std::atomic<std::uint64_t> nextBlock{0};
    Pipeline pipeline(job, threads + 2, totalBlocks);
    if (!pipeline.open()) return 1;

    auto generate = [&]() {
        for (;;) {
            std::vector<char>* buffer = pipeline.acquire();  // сначала буфер, потом номер блока
            const std::uint64_t block = nextBlock.fetch_add(1, std::memory_order_relaxed);

            if (block >= totalBlocks) {
                pipeline.release(buffer);
                break;
            }

            std::size_t size = 0;
            generateBlock(job, block, names, dates, *buffer, size);
            pipeline.submit(block, buffer, size);
        }
    };

    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(generate);
    const bool written = pipeline.writeAll();  // главный поток — писатель
    for (std::thread& thread : pool) thread.join();

    if (!written) {
        std::fprintf(stderr, "ERROR: write failed\n");
        return 1;
    }

    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::fprintf(stderr, "Generated %d files x %llu rows with %u threads in %.1f ms\n", fileCount, static_cast<unsigned long long>(job.rows), threads, ms);
    return 0;
}
