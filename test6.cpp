// Предположение по изменению структуры данных относительно test5.cpp:
// В test5.cpp итоговые группы хранятся одновременно в двух структурах:
// std::unordered_map используется для поиска группы по ключу name + date,
// а std::vector<Row> хранит сами результаты и затем отдельно сортируется.
//
// В test6.cpp используется один std::map.
// Ключом является std::pair<std::string, std::string> = name + date,
// а значением сразу хранится накопленная сумма.
// std::map является упорядоченным деревом, поэтому отдельный std::sort() больше не нужен.
// При этом каждый поиск и вставка имеют логарифмическую сложность и требуют сравнений строк,
// а узлы map обычно размещаются отдельно в памяти.
// Тест проверяет, компенсирует ли отсутствие unordered_map + vector + отдельной сортировки
// дополнительные расходы древовидной структуры std::map.

#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

std::map<std::pair<std::string, std::string>, std::array<double, 7>> parseFilterMergeFiles(std::vector<std::ifstream>& files, const std::string& startDate, const std::string& endDate) {
    std::map<std::pair<std::string, std::string>, std::array<double, 7>> mergedRows;

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

            std::array<double, 7> values{};
            std::size_t valueStart = third + 1;

            for (int i = 0; i < 7; ++i) {
                const std::size_t valueEnd = i == 6 ? line.size() : line.find(',', valueStart);
                std::from_chars(line.data() + valueStart, line.data() + valueEnd, values[i]);
                valueStart = valueEnd + 1;
            }

            auto& sums = mergedRows[{name, date}];
            for (int i = 0; i < 7; ++i) sums[i] += values[i];
        }
    }

    return mergedRows;
}

void printRows(const std::map<std::pair<std::string, std::string>, std::array<double, 7>>& rows) {
    std::cout << "string,date,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7\n";

    for (const auto& row : rows) {
        std::cout << row.first.first << "," << row.first.second; for (double value : row.second) std::cout << "," << value; std::cout << "\n";
    }
}

void saveRows(const std::map<std::pair<std::string, std::string>, std::array<double, 7>>& rows, const std::string& outputFilename) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string,date,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7\n";

    for (const auto& row : rows) {
        outputFile << row.first.first << "," << row.first.second; for (double value : row.second) outputFile << "," << value; outputFile << "\n";
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

    // 2. Parse, filter, merge and keep result sorted inside std::map.
    const auto processStart = Clock::now();
    std::map<std::pair<std::string, std::string>, std::array<double, 7>> mergedRows = parseFilterMergeFiles(files, startDate, endDate);
    const auto processEnd = Clock::now();

    const auto processingEnd = Clock::now();

    const double openMs = std::chrono::duration<double, std::milli>(openEnd - openStart).count();
    const double processMs = std::chrono::duration<double, std::milli>(processEnd - processStart).count();
    const double processingMs = std::chrono::duration<double, std::milli>(processingEnd - programStart).count();

    // 3. Print result to console.
    printRows(mergedRows);

    // 4. Save the same result to file.
    saveRows(mergedRows, "data/merged.csv");

    std::cout << "\n--- TIME ---\n";
    std::cout << "Open:                    " << openMs << " ms\n";
    std::cout << "Parse+Filter+Merge+Sort: " << processMs << " ms\n";
    std::cout << "Processing:              " << processingMs << " ms\n";

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 -O2 test6.cpp -o build/test6.exe && build/test6.exe

