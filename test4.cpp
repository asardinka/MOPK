// Предположение по улучшению производительности относительно test3.cpp:
// В test3.cpp числовое значение разбирается через std::stod(line.substr(...)).
// std::string::substr() сначала создаёт временный std::string и копирует в него
// символы числового значения из исходной строки.
// Затем std::stod() разбирает эту временную строку и преобразует её в double.
//
// В test4.cpp используется std::from_chars().
// Функция получает указатели на начало и конец числового значения прямо внутри line
// и преобразует этот диапазон символов в double без создания временного std::string.
// Это уменьшает количество копирований и выделений памяти при разборе float64.
// Остальная логика test3.cpp не изменяется, поэтому разница во времени показывает
// эффект именно от замены std::stod(line.substr(...)) на std::from_chars().

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

struct Row {
    std::string name;
    std::string date;
    double value;
};

std::vector<std::ifstream> openDataFiles() {
    std::vector<std::ifstream> files;

    for (const auto& entry : std::filesystem::directory_iterator("data")) {
        const std::string filename = entry.path().filename().string();

        if (entry.is_regular_file() && filename.rfind("data", 0) == 0 && entry.path().extension() == ".csv") {
            files.emplace_back(entry.path());
        }
    }

    return files;
}

std::vector<Row> parseFilterMergeFiles(std::vector<std::ifstream>& files, const std::string& startDate, const std::string& endDate) {
    std::vector<Row> mergedRows;
    std::unordered_map<std::string, std::size_t> positions;

    for (auto& file : files) {
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(',');
            const std::size_t second = line.find(',', first + 1);
            const std::size_t third = line.find(',', second + 1);

            if (line.compare(second + 1, third - second - 1, startDate) < 0) {
                continue;
            }

            if (line.compare(second + 1, third - second - 1, endDate) > 0) {
                continue;
            }

            std::string name = line.substr(first + 1, second - first - 1);
            std::string date = line.substr(second + 1, third - second - 1);
            double value;
            std::from_chars(line.data() + third + 1, line.data() + line.size(), value);

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

    return mergedRows;
}

void sortRows(std::vector<Row>& rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }

        return left.date < right.date;
    });
}

void printRows(const std::vector<Row>& rows) {
    std::cout << "string,date,float64\n";

    for (const auto& row : rows) {
        std::cout << row.name << "," << row.date << "," << row.value << "\n";
    }
}

void saveRows(const std::vector<Row>& rows, const std::string& outputFilename) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string,date,float64\n";

    for (const auto& row : rows) {
        outputFile << row.name << "," << row.date << "," << row.value << "\n";
    }
}

int main() {
    using Clock = std::chrono::steady_clock;

    const std::string startDate = "2026-11-01";
    const std::string endDate = "2026-11-10";

    const auto programStart = Clock::now();

    // 1. Open all data*.csv files.
    const auto openStart = Clock::now();
    std::vector<std::ifstream> files = openDataFiles();
    const auto openEnd = Clock::now();

    // 2. Parse, filter and merge in one pass.
    const auto processStart = Clock::now();
    std::vector<Row> mergedRows = parseFilterMergeFiles(files, startDate, endDate);
    const auto processEnd = Clock::now();

    // 3. Sort by name, then by date.
    const auto sortStart = Clock::now();
    sortRows(mergedRows);
    const auto sortEnd = Clock::now();

    const auto processingEnd = Clock::now();

    const double openMs = std::chrono::duration<double, std::milli>(openEnd - openStart).count();
    const double processMs = std::chrono::duration<double, std::milli>(processEnd - processStart).count();
    const double sortMs = std::chrono::duration<double, std::milli>(sortEnd - sortStart).count();
    const double processingMs = std::chrono::duration<double, std::milli>(processingEnd - programStart).count();

    // 4. Print result to console.
    printRows(mergedRows);

    // 5. Save the same result to file.
    saveRows(mergedRows, "data/merged.csv");

    std::cout << "\n--- TIME ---\n";
    std::cout << "Open:               " << openMs << " ms\n";
    std::cout << "Parse+Filter+Merge: " << processMs << " ms\n";
    std::cout << "Sort:               " << sortMs << " ms\n";
    std::cout << "Processing:         " << processingMs << " ms\n";

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 -O2 test4.cpp -o build/test4.exe && build/test4.exe

// --- TIME ---
// Open:               0.6293 ms
// Parse+Filter+Merge: 412.667 ms
// Sort:               0.6648 ms
// Processing:         413.962 ms

// --- TIME ---
// Open:               0.6234 ms
// Parse+Filter+Merge: 173.111 ms
// Sort:               0.0896 ms
// Processing:         173.824 ms

// --- TIME ---
// Open:               0.5776 ms
// Parse+Filter+Merge: 178.377 ms
// Sort:               0.0984 ms
// Processing:         179.053 ms