#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct Row {
    std::string name;
    std::string date;
    double value;
};

struct MergedRow {
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

std::vector<Row> parseFiles(std::vector<std::ifstream>& files) {
    std::vector<Row> rows;

    for (auto& file : files) {
        std::string line;
        std::getline(file, line);

        while (std::getline(file, line)) {
            std::stringstream stream(line);

            std::string id;
            std::string name;
            std::string date;
            std::string value;

            std::getline(stream, id, ',');
            std::getline(stream, name, ',');
            std::getline(stream, date, ',');
            std::getline(stream, value);

            rows.push_back({name, date, std::stod(value)});
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

std::vector<MergedRow> mergeByNameAndDate(const std::vector<Row>& rows) {
    std::vector<MergedRow> mergedRows;
    std::unordered_map<std::string, std::size_t> positions;

    for (const auto& row : rows) {
        const std::string key = row.name + "|" + row.date;
        auto it = positions.find(key);

        if (it == positions.end()) {
            positions[key] = mergedRows.size();
            mergedRows.push_back({row.name, row.date, row.value});
        } else {
            mergedRows[it->second].value += row.value;
        }
    }

    return mergedRows;
}

void sortRows(std::vector<MergedRow>& rows) {
    std::sort(rows.begin(), rows.end(), [](const MergedRow& left, const MergedRow& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }

        return left.date < right.date;
    });
}

void printRows(const std::vector<MergedRow>& rows) {
    std::cout << "string,date,float64\n";

    for (const auto& row : rows) {
        std::cout << row.name << "," << row.date << "," << row.value << "\n";
    }
}

void saveRows(const std::vector<MergedRow>& rows, const std::string& outputFilename) {
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
    std::vector<MergedRow> mergedRows = mergeByNameAndDate(filteredRows);
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
// g++ -std=c++17 test.cpp -o build/test.exe && build/test.exe

// --- TIME ---
// Open:       0.6097 ms
// Parse:      1773.39 ms
// Filter:     106.744 ms
// Merge:      87.7525 ms
// Sort:       0.6537 ms
// Processing: 1969.15 ms