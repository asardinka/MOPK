#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// Предположение относительно test13.cpp:
// data1.csv и data2.csv независимы друг от друга, поэтому их можно обрабатывать параллельно.
// В test13 каждый файл обрабатывается своим потоком тем же алгоритмом, что и в test12:
// чтение блоками по 4 MB, быстрый разбор даты, фильтрация и локальное суммирование.
// После завершения потоков два небольших отсортированных результата объединяются.
// Общих unordered_map между потоками нет, поэтому mutex в горячем цикле не нужен.

struct Row {
    std::string name;
    int date;
    std::array<double, 7> values{};
};

int parseDate(const char* date) {
    const int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
    const int month = (date[5] - '0') * 10 + (date[6] - '0');
    const int day = (date[8] - '0') * 10 + (date[9] - '0');

    return year * 10000 + month * 100 + day;
}

double parseFloat64(const char* begin, const char* end, char decimalSeparator) {
    if (decimalSeparator == '.') {
        double value;
        std::from_chars(begin, end, value);
        return value;
    }

    bool negative = false;

    if (begin < end && *begin == '-') {
        negative = true;
        ++begin;
    }

    double value = 0.0;

    while (begin < end && *begin >= '0' && *begin <= '9') {
        value = value * 10.0 + (*begin - '0');
        ++begin;
    }

    if (begin < end && *begin == decimalSeparator) {
        ++begin;
        double factor = 0.1;

        while (begin < end && *begin >= '0' && *begin <= '9') {
            value += (*begin - '0') * factor;
            factor *= 0.1;
            ++begin;
        }
    }

    if (begin < end && (*begin == 'e' || *begin == 'E')) {
        ++begin;
        bool exponentNegative = false;

        if (begin < end && (*begin == '+' || *begin == '-')) {
            exponentNegative = *begin == '-';
            ++begin;
        }

        int exponent = 0;

        while (begin < end && *begin >= '0' && *begin <= '9') {
            exponent = exponent * 10 + (*begin - '0');
            ++begin;
        }

        value *= std::pow(10.0, exponentNegative ? -exponent : exponent);
    }

    return negative ? -value : value;
}

std::string dateToString(int date, char dateSeparator, const std::string& dateFormat) {
    const int year = date / 10000;
    const int month = date / 100 % 100;
    const int day = date % 100;
    std::string result;

    for (int i = 0; i < 3; ++i) {
        if (i > 0) result += dateSeparator;

        if (dateFormat[i] == 'Y') result += std::to_string(year);
        if (dateFormat[i] == 'M') result += month < 10 ? "0" + std::to_string(month) : std::to_string(month);
        if (dateFormat[i] == 'D') result += day < 10 ? "0" + std::to_string(day) : std::to_string(day);
    }

    return result;
}

std::vector<Row> processFile(const std::string& filename, char csvSeparator, char decimalSeparator, int start, int finish) {
    std::deque<std::string> names;
    std::unordered_map<std::string_view, std::size_t> nameIds;
    std::unordered_map<std::uint64_t, std::array<double, 7>> sums;

    const std::size_t chunkSize = 4 * 1024 * 1024;
    std::ifstream file(filename, std::ios::binary);
    std::vector<char> buffer(chunkSize * 2);
    std::size_t carrySize = 0;
    bool firstChunk = true;

    while (file) {
        file.read(buffer.data() + carrySize, chunkSize);
        const std::size_t bytesRead = static_cast<std::size_t>(file.gcount());
        const std::size_t dataSize = carrySize + bytesRead;

        if (dataSize == 0) {
            break;
        }

        const char* end = buffer.data() + dataSize;
        const char* parseEnd = end;

        if (!file.eof()) {
            while (parseEnd > buffer.data() && *(parseEnd - 1) != '\n') {
                --parseEnd;
            }
        }

        const char* current = buffer.data();

        if (firstChunk) {
            while (current < parseEnd && *current != '\n') {
                ++current;
            }

            if (current < parseEnd) {
                ++current;
            }

            firstChunk = false;
        }

        while (current < parseEnd) {
            while (current < parseEnd && *current != csvSeparator) {
                ++current;
            }

            if (current >= parseEnd) {
                break;
            }

            ++current;
            const char* nameStart = current;

            while (current < parseEnd && *current != csvSeparator) {
                ++current;
            }

            const char* nameEnd = current;
            ++current;
            const char* dateStart = current;

            while (current < parseEnd && *current != csvSeparator) {
                ++current;
            }

            ++current;
            const char* valueStart = current;

            while (current < parseEnd && *current != '\n') {
                ++current;
            }

            const char* valueEnd = current;

            if (valueEnd > valueStart && *(valueEnd - 1) == '\r') {
                --valueEnd;
            }

            if (current < parseEnd) {
                ++current;
            }

            const int date = parseDate(dateStart);

            if (date < start || date > finish) {
                continue;
            }

            const std::string_view name(nameStart, nameEnd - nameStart);
            auto nameIt = nameIds.find(name);
            std::size_t nameId;

            if (nameIt == nameIds.end()) {
                nameId = names.size();
                names.emplace_back(name);
                nameIds.emplace(std::string_view(names.back()), nameId);
            } else {
                nameId = nameIt->second;
            }

            std::array<double, 7> values{};
            const char* valueCurrent = valueStart;

            for (int i = 0; i < 7; ++i) {
                const char* valuePartEnd = valueCurrent;

                while (valuePartEnd < valueEnd && *valuePartEnd != csvSeparator) {
                    ++valuePartEnd;
                }

                values[i] = parseFloat64(valueCurrent, valuePartEnd, decimalSeparator);
                valueCurrent = valuePartEnd + 1;
            }

            const std::uint64_t key = (static_cast<std::uint64_t>(nameId) << 32) | static_cast<std::uint32_t>(date);
            auto& total = sums[key];
            for (int i = 0; i < 7; ++i) total[i] += values[i];
        }

        carrySize = static_cast<std::size_t>(end - parseEnd);

        if (carrySize > 0) {
            std::memmove(buffer.data(), parseEnd, carrySize);
        }
    }

    std::vector<Row> rows;
    rows.reserve(sums.size());

    for (const auto& item : sums) {
        const std::size_t nameId = static_cast<std::size_t>(item.first >> 32);
        const int date = static_cast<int>(item.first & 0xffffffff);
        rows.push_back({names[nameId], date, item.second});
    }

    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }

        return left.date < right.date;
    });

    return rows;
}

std::vector<Row> mergeRows(const std::vector<Row>& left, const std::vector<Row>& right) {
    std::vector<Row> rows;
    rows.reserve(left.size() + right.size());

    std::size_t i = 0;
    std::size_t j = 0;

    while (i < left.size() && j < right.size()) {
        if (left[i].name < right[j].name || (left[i].name == right[j].name && left[i].date < right[j].date)) {
            rows.push_back(left[i]);
            ++i;
        } else if (right[j].name < left[i].name || (right[j].name == left[i].name && right[j].date < left[i].date)) {
            rows.push_back(right[j]);
            ++j;
        } else {
            Row row = left[i];
            for (int valueIndex = 0; valueIndex < 7; ++valueIndex) row.values[valueIndex] += right[j].values[valueIndex];
            rows.push_back(row);
            ++i;
            ++j;
        }
    }

    while (i < left.size()) {
        rows.push_back(left[i]);
        ++i;
    }

    while (j < right.size()) {
        rows.push_back(right[j]);
        ++j;
    }

    return rows;
}

void printRows(const std::vector<Row>& rows, char csvSeparator, char dateSeparator, const std::string& dateFormat) {
    std::cout << "string" << csvSeparator << "date" << csvSeparator << "float64_1" << csvSeparator << "float64_2" << csvSeparator << "float64_3" << csvSeparator << "float64_4" << csvSeparator << "float64_5" << csvSeparator << "float64_6" << csvSeparator << "float64_7\n";

    for (const auto& row : rows) {
        std::cout << row.name << csvSeparator << dateToString(row.date, dateSeparator, dateFormat); for (double value : row.values) std::cout << csvSeparator << value; std::cout << "\n";
    }
}

void saveRows(const std::vector<Row>& rows, const std::string& outputFilename, char csvSeparator, char dateSeparator, const std::string& dateFormat) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string" << csvSeparator << "date" << csvSeparator << "float64_1" << csvSeparator << "float64_2" << csvSeparator << "float64_3" << csvSeparator << "float64_4" << csvSeparator << "float64_5" << csvSeparator << "float64_6" << csvSeparator << "float64_7\n";

    for (const auto& row : rows) {
        outputFile << row.name << csvSeparator << dateToString(row.date, dateSeparator, dateFormat); for (double value : row.values) outputFile << csvSeparator << value; outputFile << "\n";
    }
}

int main() {
    using Clock = std::chrono::steady_clock;

    const std::string filename1 = "data/data1.csv";
    const std::string filename2 = "data/data2.csv";
    const char csvSeparator = ',';
    const char dateSeparator = '-';
    const char decimalSeparator = '.';
    const std::string startDate = "2026-11-01";
    const std::string endDate = "2026-11-10";
    const int start = parseDate(startDate.data());
    const int finish = parseDate(endDate.data());

    std::vector<Row> rows1;
    std::vector<Row> rows2;

    const auto processingStart = Clock::now();

    std::thread thread1([&]() {
        rows1 = processFile(filename1, csvSeparator, decimalSeparator, start, finish);
    });

    std::thread thread2([&]() {
        rows2 = processFile(filename2, csvSeparator, decimalSeparator, start, finish);
    });

    thread1.join();
    thread2.join();

    std::vector<Row> rows = mergeRows(rows1, rows2);
    const auto processingEnd = Clock::now();

    const double processingMs = std::chrono::duration<double, std::milli>(processingEnd - processingStart).count();

    printRows(rows, csvSeparator, dateSeparator, "YMD");
    saveRows(rows, "data/merged.csv", csvSeparator, dateSeparator, "YMD");

    std::cout << "\n--- TIME ---\n";
    std::cout << "Processing: " << processingMs << " ms\n";

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 -O2 test13.cpp -o build/test13.exe && build/test13.exe

