#include <algorithm>
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

    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        const std::string filename = entry.path().filename().string();

        if (entry.is_regular_file() &&
            filename.rfind("data", 0) == 0 &&
            entry.path().extension() == ".csv") {
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

            rows.push_back({
                name,
                date,
                std::stod(value)
            });
        }
    }

    return rows;
}

std::vector<Row> filterByDate(
    const std::vector<Row>& rows,
    const std::string& startDate,
    const std::string& endDate
) {
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

            mergedRows.push_back({
                row.name,
                row.date,
                row.value
            });
        } else {
            mergedRows[it->second].value += row.value;
        }
    }

    return mergedRows;
}

void sortRows(std::vector<MergedRow>& rows) {
    std::sort(
        rows.begin(),
        rows.end(),
        [](const MergedRow& left, const MergedRow& right) {
            if (left.name != right.name) {
                return left.name < right.name;
            }

            return left.date < right.date;
        }
    );
}

void outputRows(
    const std::vector<MergedRow>& rows,
    const std::string& outputFilename
) {
    std::ofstream outputFile(outputFilename);

    std::cout << "string,date,float64\n";
    outputFile << "string,date,float64\n";

    for (const auto& row : rows) {
        std::cout
            << row.name << ","
            << row.date << ","
            << row.value << "\n";

        outputFile
            << row.name << ","
            << row.date << ","
            << row.value << "\n";
    }
}

int main() {
    const std::string startDate = "2026-11-01";
    const std::string endDate = "2026-11-10";

    // 1. Open all data*.csv files.
    std::vector<std::ifstream> files = openDataFiles();

    // 2. Parse all rows.
    std::vector<Row> rows = parseFiles(files);

    // 3. Filter rows by date.
    std::vector<Row> filteredRows =
        filterByDate(rows, startDate, endDate);

    // 4. Merge by name + date and sum values.
    std::vector<MergedRow> mergedRows =
        mergeByNameAndDate(filteredRows);

    // 5. Sort by name, then by date.
    sortRows(mergedRows);

    // 6. Print to console and write to file.
    outputRows(mergedRows, "merged.csv");

    return 0;
}
