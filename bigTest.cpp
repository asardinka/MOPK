#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Row {
    std::string name;
    std::string date;
    double value;
};

struct Settings {
    std::vector<std::string> filenames;
    char csvSeparator;
    char dateSeparator;
    char decimalSeparator;
    std::string startDate;
    std::string endDate;
};

int parseDate(const char* begin, const char* end, char dateSeparator) {
    int parts[3] = {};
    int part = 0;

    for (const char* current = begin; current < end; ++current) {
        if (*current == dateSeparator) {
            ++part;
        } else {
            parts[part] = parts[part] * 10 + (*current - '0');
        }
    }

    return parts[0] * 10000 + parts[1] * 100 + parts[2];
}

double parseFloatStod(std::string value, char decimalSeparator) {
    if (decimalSeparator != '.') {
        for (char& symbol : value) {
            if (symbol == decimalSeparator) {
                symbol = '.';
            }
        }
    }

    return std::stod(value);
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

std::string dateToString(int date, char dateSeparator) {
    const int year = date / 10000;
    const int month = date / 100 % 100;
    const int day = date % 100;

    std::string result = std::to_string(year);
    result += dateSeparator;
    result += month < 10 ? "0" + std::to_string(month) : std::to_string(month);
    result += dateSeparator;
    result += day < 10 ? "0" + std::to_string(day) : std::to_string(day);

    return result;
}

void sortRows(std::vector<Row>& rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }

        return left.date < right.date;
    });
}

std::vector<Row> mergeRows(const std::vector<Row>& rows) {
    std::vector<Row> mergedRows;
    std::unordered_map<std::string, std::size_t> positions;

    for (const Row& row : rows) {
        const std::string key = row.name + "|" + row.date;
        auto it = positions.find(key);

        if (it == positions.end()) {
            positions[key] = mergedRows.size();
            mergedRows.push_back(row);
        } else {
            mergedRows[it->second].value += row.value;
        }
    }

    return mergedRows;
}

std::vector<Row> test1(const Settings& settings) {
    std::vector<Row> rows;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            std::stringstream stream(line);
            std::string id;
            std::string name;
            std::string date;
            std::string value;

            std::getline(stream, id, settings.csvSeparator);
            std::getline(stream, name, settings.csvSeparator);
            std::getline(stream, date, settings.csvSeparator);
            std::getline(stream, value);

            rows.push_back({name, date, parseFloatStod(value, settings.decimalSeparator)});
        }
    }

    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);
    std::vector<Row> filteredRows;

    for (const Row& row : rows) {
        const int date = parseDate(row.date.data(), row.date.data() + row.date.size(), settings.dateSeparator);

        if (date >= startDate && date <= endDate) {
            filteredRows.push_back(row);
        }
    }

    std::vector<Row> mergedRows = mergeRows(filteredRows);
    sortRows(mergedRows);
    return mergedRows;
}

std::vector<Row> test2(const Settings& settings) {
    std::vector<Row> rows;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(settings.csvSeparator);
            const std::size_t second = line.find(settings.csvSeparator, first + 1);
            const std::size_t third = line.find(settings.csvSeparator, second + 1);

            std::string name = line.substr(first + 1, second - first - 1);
            std::string date = line.substr(second + 1, third - second - 1);
            double value = parseFloatStod(line.substr(third + 1), settings.decimalSeparator);

            rows.push_back({name, date, value});
        }
    }

    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);
    std::vector<Row> filteredRows;

    for (const Row& row : rows) {
        const int date = parseDate(row.date.data(), row.date.data() + row.date.size(), settings.dateSeparator);

        if (date >= startDate && date <= endDate) {
            filteredRows.push_back(row);
        }
    }

    std::vector<Row> mergedRows = mergeRows(filteredRows);
    sortRows(mergedRows);
    return mergedRows;
}

std::vector<Row> test3(const Settings& settings) {
    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);
    std::vector<Row> mergedRows;
    std::unordered_map<std::string, std::size_t> positions;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(settings.csvSeparator);
            const std::size_t second = line.find(settings.csvSeparator, first + 1);
            const std::size_t third = line.find(settings.csvSeparator, second + 1);
            const int dateNumber = parseDate(line.data() + second + 1, line.data() + third, settings.dateSeparator);

            if (dateNumber < startDate || dateNumber > endDate) {
                continue;
            }

            std::string name = line.substr(first + 1, second - first - 1);
            std::string date = line.substr(second + 1, third - second - 1);
            double value = parseFloatStod(line.substr(third + 1), settings.decimalSeparator);

            const std::string key = name + "|" + date;
            auto it = positions.find(key);

            if (it == positions.end()) {
                positions[key] = mergedRows.size();
                mergedRows.push_back({name, date, value});
            } else {
                mergedRows[it->second].value += value;
            }
        }
    }

    sortRows(mergedRows);
    return mergedRows;
}

std::vector<Row> test4(const Settings& settings) {
    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);
    std::vector<Row> mergedRows;
    std::unordered_map<std::string, std::size_t> positions;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(settings.csvSeparator);
            const std::size_t second = line.find(settings.csvSeparator, first + 1);
            const std::size_t third = line.find(settings.csvSeparator, second + 1);
            const int dateNumber = parseDate(line.data() + second + 1, line.data() + third, settings.dateSeparator);

            if (dateNumber < startDate || dateNumber > endDate) {
                continue;
            }

            std::string name = line.substr(first + 1, second - first - 1);
            std::string date = line.substr(second + 1, third - second - 1);
            const char* valueStart = line.data() + third + 1;
            const char* valueEnd = line.data() + line.size();
            double value = parseFloat64(valueStart, valueEnd, settings.decimalSeparator);

            const std::string key = name + "|" + date;
            auto it = positions.find(key);

            if (it == positions.end()) {
                positions[key] = mergedRows.size();
                mergedRows.push_back({name, date, value});
            } else {
                mergedRows[it->second].value += value;
            }
        }
    }

    sortRows(mergedRows);
    return mergedRows;
}

std::vector<Row> test5(const Settings& settings) {
    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);
    std::vector<Row> mergedRows;
    std::deque<std::string> storedKeys;
    std::unordered_map<std::string_view, std::size_t> positions;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(settings.csvSeparator);
            const std::size_t second = line.find(settings.csvSeparator, first + 1);
            const std::size_t third = line.find(settings.csvSeparator, second + 1);
            const int dateNumber = parseDate(line.data() + second + 1, line.data() + third, settings.dateSeparator);

            if (dateNumber < startDate || dateNumber > endDate) {
                continue;
            }

            std::string_view name(line.data() + first + 1, second - first - 1);
            std::string_view date(line.data() + second + 1, third - second - 1);
            std::string_view key(line.data() + first + 1, third - first - 1);
            double value = parseFloat64(line.data() + third + 1, line.data() + line.size(), settings.decimalSeparator);

            auto it = positions.find(key);

            if (it == positions.end()) {
                storedKeys.emplace_back(key);
                positions.emplace(std::string_view(storedKeys.back()), mergedRows.size());
                mergedRows.push_back({std::string(name), std::string(date), value});
            } else {
                mergedRows[it->second].value += value;
            }
        }
    }

    sortRows(mergedRows);
    return mergedRows;
}

std::vector<Row> test6(const Settings& settings) {
    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);
    std::map<std::pair<std::string, std::string>, double> mergedRows;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(settings.csvSeparator);
            const std::size_t second = line.find(settings.csvSeparator, first + 1);
            const std::size_t third = line.find(settings.csvSeparator, second + 1);
            const int dateNumber = parseDate(line.data() + second + 1, line.data() + third, settings.dateSeparator);

            if (dateNumber < startDate || dateNumber > endDate) {
                continue;
            }

            std::string name = line.substr(first + 1, second - first - 1);
            std::string date = line.substr(second + 1, third - second - 1);
            double value = parseFloat64(line.data() + third + 1, line.data() + line.size(), settings.decimalSeparator);

            mergedRows[{name, date}] += value;
        }
    }

    std::vector<Row> rows;
    rows.reserve(mergedRows.size());

    for (const auto& item : mergedRows) {
        rows.push_back({item.first.first, item.first.second, item.second});
    }

    return rows;
}

std::vector<Row> test7(const Settings& settings) {
    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);

    std::deque<std::string> names;
    std::unordered_map<std::string_view, std::size_t> nameIds;
    std::vector<int> dates;
    std::unordered_map<int, std::size_t> dateIds;
    std::vector<std::vector<double>> sums;
    std::vector<std::vector<bool>> used;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(settings.csvSeparator);
            const std::size_t second = line.find(settings.csvSeparator, first + 1);
            const std::size_t third = line.find(settings.csvSeparator, second + 1);
            const int date = parseDate(line.data() + second + 1, line.data() + third, settings.dateSeparator);

            if (date < startDate || date > endDate) {
                continue;
            }

            std::string_view name(line.data() + first + 1, second - first - 1);
            auto nameIt = nameIds.find(name);
            std::size_t nameId;

            if (nameIt == nameIds.end()) {
                nameId = names.size();
                names.emplace_back(name);
                nameIds.emplace(std::string_view(names.back()), nameId);
                sums.emplace_back(dates.size(), 0.0);
                used.emplace_back(dates.size(), false);
            } else {
                nameId = nameIt->second;
            }

            auto dateIt = dateIds.find(date);
            std::size_t dateId;

            if (dateIt == dateIds.end()) {
                dateId = dates.size();
                dates.push_back(date);
                dateIds[date] = dateId;

                for (std::vector<double>& row : sums) {
                    row.push_back(0.0);
                }

                for (std::vector<bool>& row : used) {
                    row.push_back(false);
                }
            } else {
                dateId = dateIt->second;
            }

            double value = parseFloat64(line.data() + third + 1, line.data() + line.size(), settings.decimalSeparator);
            sums[nameId][dateId] += value;
            used[nameId][dateId] = true;
        }
    }

    std::vector<Row> rows;

    for (std::size_t nameId = 0; nameId < names.size(); ++nameId) {
        for (std::size_t dateId = 0; dateId < dates.size(); ++dateId) {
            if (used[nameId][dateId]) {
                rows.push_back({names[nameId], dateToString(dates[dateId], settings.dateSeparator), sums[nameId][dateId]});
            }
        }
    }

    sortRows(rows);
    return rows;
}

std::vector<Row> makeRowsFromNumericSums(const std::deque<std::string>& names, const std::unordered_map<std::uint64_t, double>& sums, char dateSeparator) {
    std::vector<Row> rows;
    rows.reserve(sums.size());

    for (const auto& item : sums) {
        const std::size_t nameId = static_cast<std::size_t>(item.first >> 32);
        const int date = static_cast<int>(item.first & 0xffffffff);
        rows.push_back({names[nameId], dateToString(date, dateSeparator), item.second});
    }

    sortRows(rows);
    return rows;
}

std::vector<Row> test8(const Settings& settings) {
    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);

    std::deque<std::string> names;
    std::unordered_map<std::string_view, std::size_t> nameIds;
    std::unordered_map<std::uint64_t, double> sums;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename);
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(settings.csvSeparator);
            const std::size_t second = line.find(settings.csvSeparator, first + 1);
            const std::size_t third = line.find(settings.csvSeparator, second + 1);
            const int date = parseDate(line.data() + second + 1, line.data() + third, settings.dateSeparator);

            if (date < startDate || date > endDate) {
                continue;
            }

            std::string_view name(line.data() + first + 1, second - first - 1);
            auto nameIt = nameIds.find(name);
            std::size_t nameId;

            if (nameIt == nameIds.end()) {
                nameId = names.size();
                names.emplace_back(name);
                nameIds.emplace(std::string_view(names.back()), nameId);
            } else {
                nameId = nameIt->second;
            }

            double value = parseFloat64(line.data() + third + 1, line.data() + line.size(), settings.decimalSeparator);
            const std::uint64_t key = (static_cast<std::uint64_t>(nameId) << 32) | static_cast<std::uint32_t>(date);
            sums[key] += value;
        }
    }

    return makeRowsFromNumericSums(names, sums, settings.dateSeparator);
}

std::vector<Row> test9(const Settings& settings) {
    const int startDate = parseDate(settings.startDate.data(), settings.startDate.data() + settings.startDate.size(), settings.dateSeparator);
    const int endDate = parseDate(settings.endDate.data(), settings.endDate.data() + settings.endDate.size(), settings.dateSeparator);

    std::deque<std::string> names;
    std::unordered_map<std::string_view, std::size_t> nameIds;
    std::unordered_map<std::uint64_t, double> sums;

    for (const std::string& filename : settings.filenames) {
        std::ifstream file(filename, std::ios::binary);
        file.seekg(0, std::ios::end);
        const std::size_t fileSize = static_cast<std::size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(fileSize);
        file.read(buffer.data(), fileSize);

        const char* current = buffer.data();
        const char* end = buffer.data() + buffer.size();

        while (current < end && *current != '\n') {
            ++current;
        }

        if (current < end) {
            ++current;
        }

        while (current < end) {
            while (current < end && *current != settings.csvSeparator) {
                ++current;
            }

            if (current >= end) {
                break;
            }

            ++current;
            const char* nameStart = current;

            while (current < end && *current != settings.csvSeparator) {
                ++current;
            }

            const char* nameEnd = current;
            ++current;
            const char* dateStart = current;

            while (current < end && *current != settings.csvSeparator) {
                ++current;
            }

            const char* dateEnd = current;
            ++current;
            const char* valueStart = current;

            while (current < end && *current != '\n') {
                ++current;
            }

            const char* valueEnd = current;

            if (valueEnd > valueStart && *(valueEnd - 1) == '\r') {
                --valueEnd;
            }

            if (current < end) {
                ++current;
            }

            const int date = parseDate(dateStart, dateEnd, settings.dateSeparator);

            if (date < startDate || date > endDate) {
                continue;
            }

            std::string_view name(nameStart, nameEnd - nameStart);
            auto nameIt = nameIds.find(name);
            std::size_t nameId;

            if (nameIt == nameIds.end()) {
                nameId = names.size();
                names.emplace_back(name);
                nameIds.emplace(std::string_view(names.back()), nameId);
            } else {
                nameId = nameIt->second;
            }

            double value = parseFloat64(valueStart, valueEnd, settings.decimalSeparator);
            const std::uint64_t key = (static_cast<std::uint64_t>(nameId) << 32) | static_cast<std::uint32_t>(date);
            sums[key] += value;
        }
    }

    return makeRowsFromNumericSums(names, sums, settings.dateSeparator);
}

bool sameRows(const std::vector<Row>& left, const std::vector<Row>& right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].name != right[i].name || left[i].date != right[i].date) {
            return false;
        }

        const double scale = std::max({1.0, std::abs(left[i].value), std::abs(right[i].value)});

        if (std::abs(left[i].value - right[i].value) > 1e-9 * scale) {
            return false;
        }
    }

    return true;
}

std::string valueToString(double value, char decimalSeparator) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string text(buffer, result.ptr);

    if (decimalSeparator != '.') {
        for (char& symbol : text) {
            if (symbol == '.') {
                symbol = decimalSeparator;
            }
        }
    }

    return text;
}

void printRows(const std::vector<Row>& rows, char csvSeparator, char decimalSeparator) {
    std::cout << "string" << csvSeparator << "date" << csvSeparator << "float64\n";

    for (const Row& row : rows) {
        std::cout << row.name << csvSeparator << row.date << csvSeparator << valueToString(row.value, decimalSeparator) << "\n";
    }
}

void saveRows(const std::vector<Row>& rows, const std::string& outputFilename, char csvSeparator, char decimalSeparator) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string" << csvSeparator << "date" << csvSeparator << "float64\n";

    for (const Row& row : rows) {
        outputFile << row.name << csvSeparator << row.date << csvSeparator << valueToString(row.value, decimalSeparator) << "\n";
    }
}

int main() {
    using Clock = std::chrono::steady_clock;

    const Settings settings = {
        {"data/data1.csv", "data/data2.csv"},
        ',',
        '-',
        '.',
        "2026-11-01",
        "2026-11-10"
    };

    using TestFunction = std::vector<Row> (*)(const Settings&);

    struct Test {
        const char* name;
        TestFunction function;
    };

    const Test tests[] = {
        {"test1", test1},
        {"test2", test2},
        {"test3", test3},
        {"test4", test4},
        {"test5", test5},
        {"test6", test6},
        {"test7", test7},
        {"test8", test8},
        {"test9", test9}
    };

    if (settings.csvSeparator == settings.dateSeparator) {
        std::cout << "CSV separator and date separator must be different.\n";
        return 1;
    }

    struct BenchmarkResult {
        const char* name;
        double processingMs;
        bool correct;
    };

    std::vector<BenchmarkResult> benchmarkResults;
    std::vector<Row> referenceRows;
    std::vector<Row> finalRows;
    bool referenceSet = false;
    double bestTime = 0.0;
    const char* bestTest = nullptr;

    for (const Test& test : tests) {
        const auto start = Clock::now();
        std::vector<Row> rows = test.function(settings);
        const auto end = Clock::now();

        const double processingMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (!referenceSet) {
            referenceRows = rows;
            referenceSet = true;
        }

        const bool correct = sameRows(referenceRows, rows);
        benchmarkResults.push_back({test.name, processingMs, correct});

        if (bestTest == nullptr || processingMs < bestTime) {
            bestTime = processingMs;
            bestTest = test.name;
        }

        finalRows = std::move(rows);
    }

    printRows(finalRows, settings.csvSeparator, settings.decimalSeparator);
    saveRows(finalRows, "data/merged.csv", settings.csvSeparator, settings.decimalSeparator);

    std::cout << "\n--- BIG TEST ---\n";

    for (const BenchmarkResult& result : benchmarkResults) {
        std::cout << result.name << ": " << result.processingMs << " ms | " << (result.correct ? "OK" : "DIFFERENT RESULT") << "\n";
    }

    std::cout << "Fastest: " << bestTest << " | " << bestTime << " ms\n";

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 -O2 bigTest.cpp -o build/bigTest.exe && build/bigTest.exe
