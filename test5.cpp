// Предположение по улучшению производительности относительно test4.cpp:
// В test4.cpp для каждой строки, прошедшей фильтр, создаются отдельные std::string
// для name и date, а затем ещё один std::string key = name + "|" + date.
// Это означает копирование символов из line и работу со строковыми объектами
// для каждой подходящей строки, хотя итоговых групп name + date значительно меньше.
//
// В test5.cpp name, date и key сначала представлены как std::string_view.
// std::string_view хранит только указатель на существующие символы и их длину,
// поэтому не копирует содержимое line и не владеет отдельным буфером.
// Поиск существующей группы выполняется прямо по string_view на текущую строку.
// Собственные std::string создаются только при появлении новой итоговой группы.
// Для ключей используется отдельное стабильное хранилище std::deque<std::string>,
// потому что string_view не может ссылаться на line после следующего std::getline().
// Остальная логика test4.cpp не изменяется.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Row {
    std::string name;
    std::string date;
    std::array<double, 7> values{};
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
    std::deque<std::string> storedKeys;
    std::unordered_map<std::string_view, std::size_t> positions;

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

            std::string_view name(line.data() + first + 1, second - first - 1);
            std::string_view date(line.data() + second + 1, third - second - 1);
            std::string_view key(line.data() + first + 1, third - first - 1);

            std::array<double, 7> values{};
            std::size_t valueStart = third + 1;

            for (int i = 0; i < 7; ++i) {
                const std::size_t valueEnd = i == 6 ? line.size() : line.find(',', valueStart);
                std::from_chars(line.data() + valueStart, line.data() + valueEnd, values[i]);
                valueStart = valueEnd + 1;
            }

            auto it = positions.find(key);

            if (it == positions.end()) {
                storedKeys.emplace_back(key);
                positions.emplace(std::string_view(storedKeys.back()), mergedRows.size());
                mergedRows.push_back({std::string(name), std::string(date), values});
            } else {
                for (int i = 0; i < 7; ++i) mergedRows[it->second].values[i] += values[i];
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
    std::cout << "string,date,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7\n";

    for (const auto& row : rows) {
        std::cout << row.name << "," << row.date; for (double value : row.values) std::cout << "," << value; std::cout << "\n";
    }
}

void saveRows(const std::vector<Row>& rows, const std::string& outputFilename) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string,date,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7\n";

    for (const auto& row : rows) {
        outputFile << row.name << "," << row.date; for (double value : row.values) outputFile << "," << value; outputFile << "\n";
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
// g++ -std=c++17 -O2 test5.cpp -o build/test5.exe && build/test5.exe

