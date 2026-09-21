// Предположение по улучшению производительности:
// В test.cpp каждая строка CSV сначала передаётся в std::stringstream.
// При этом для каждой строки создаётся объект потока и его внутренний буфер,
// строка копируется в этот буфер, а std::getline читает поля через streambuf
// с дополнительным контролем состояния потока (eofbit, failbit и т.д.).
// В test2.cpp разделители ищутся напрямую в памяти std::string через std::string::find().
// Это убирает создание stringstream и работу потокового механизма для каждой строки,
// поэтому при обработке миллионов строк время парсинга должно уменьшиться.
// Остальные этапы пайплайна не изменяются, поэтому разница во времени показывает
// именно эффект от замены способа разбора CSV.

#include <algorithm>\n#include <array>
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

std::vector<Row> parseFiles(std::vector<std::ifstream>& files) {
    std::vector<Row> rows;

    for (auto& file : files) {
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            const std::size_t first = line.find(',');
            const std::size_t second = line.find(',', first + 1);
            const std::size_t third = line.find(',', second + 1);

            Row row;
            row.name = line.substr(first + 1, second - first - 1);
            row.date = line.substr(second + 1, third - second - 1);

            std::size_t valueStart = third + 1;

            for (int i = 0; i < 7; ++i) {
                const std::size_t valueEnd = i == 6 ? line.size() : line.find(',', valueStart);
                row.values[i] = std::stod(line.substr(valueStart, valueEnd - valueStart));
                valueStart = valueEnd + 1;
            }

            rows.push_back(row);
        }
    }

    return rows;
}

std::vector<Row> filterByDate(const std::vector<Row>& rows, const std::string& startDate, const std::string& endDate) {
    std::vector<Row> filteredRows;

    for (const auto& row : rows) {
        if (row.date >= startDate && row.date <= endDate) {
            filteredRows.push_back(row);
        }
    }

    return filteredRows;
}

std::vector<Row> mergeByNameAndDate(const std::vector<Row>& rows) {
    std::vector<Row> mergedRows;
    std::unordered_map<std::string, std::size_t> positions;

    for (const auto& row : rows) {
        const std::string key = row.name + "|" + row.date;
        auto it = positions.find(key);

        if (it == positions.end()) {
            positions[key] = mergedRows.size();
            mergedRows.push_back(row);
        } else {
            for (int i = 0; i < 7; ++i) mergedRows[it->second].values[i] += row.values[i];
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

    // 2. Parse all rows.
    const auto parseStart = Clock::now();
    std::vector<Row> rows = parseFiles(files);
    const auto parseEnd = Clock::now();

    // 3. Filter rows by date.
    const auto filterStart = Clock::now();
    std::vector<Row> filteredRows = filterByDate(rows, startDate, endDate);
    const auto filterEnd = Clock::now();

    // 4. Merge by name + date and sum values.
    const auto mergeStart = Clock::now();
    std::vector<Row> mergedRows = mergeByNameAndDate(filteredRows);
    const auto mergeEnd = Clock::now();

    // 5. Sort by name, then by date.
    const auto sortStart = Clock::now();
    sortRows(mergedRows);
    const auto sortEnd = Clock::now();

    const auto processingEnd = Clock::now();

    const double openMs = std::chrono::duration<double, std::milli>(openEnd - openStart).count();
    const double parseMs = std::chrono::duration<double, std::milli>(parseEnd - parseStart).count();
    const double filterMs = std::chrono::duration<double, std::milli>(filterEnd - filterStart).count();
    const double mergeMs = std::chrono::duration<double, std::milli>(mergeEnd - mergeStart).count();
    const double sortMs = std::chrono::duration<double, std::milli>(sortEnd - sortStart).count();
    const double processingMs = std::chrono::duration<double, std::milli>(processingEnd - programStart).count();

    // 6. Print result to console.
    printRows(mergedRows);

    // 7. Save the same result to file.
    saveRows(mergedRows, "data/merged.csv");

    std::cout << "\n--- TIME ---\n";
    std::cout << "Open:       " << openMs << " ms\n";
    std::cout << "Parse:      " << parseMs << " ms\n";
    std::cout << "Filter:     " << filterMs << " ms\n";
    std::cout << "Merge:      " << mergeMs << " ms\n";
    std::cout << "Sort:       " << sortMs << " ms\n";
    std::cout << "Processing: " << processingMs << " ms\n";

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 -O2 test2.cpp -o build/test2.exe && build/test2.exe

