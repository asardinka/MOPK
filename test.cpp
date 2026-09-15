#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Row {
    int id;
    std::string name;
    std::string date;
    double value;
};

void parseAndFilter(const std::string& filename, std::vector<Row>& outputContainer) {
    std::ifstream inFile(filename);
    if (!inFile.is_open()) {
        std::cout << "ERROR";
        return;
    }

    std::string line;
    if (!std::getline(inFile, line)) return;

    while (std::getline(inFile, line)) {
        std::stringstream ss(line);
        std::string idStr, name, dateStr, doubleStr;

        if (std::getline(ss, idStr, ',') && std::getline(ss, name, ',') && std::getline(ss, dateStr, ',') && std::getline(ss, doubleStr, ',')) {

            if (name == "Dmitry") {
                if (dateStr >= "2026-11-01" && dateStr <= "2026-11-10") {
                    Row row;
                    row.id = std::stoi(idStr);
                    row.name = name;
                    row.date = dateStr;
                    row.value = std::stod(doubleStr);

                    outputContainer.push_back(row);
                }
            }
        }
    }
    inFile.close();
}

int main() {
    std::vector<Row> mergedResults;

    parseAndFilter("data1.csv", mergedResults);
    parseAndFilter("data2.csv", mergedResults);

    std::ofstream outFile("merged.csv");
    if (!outFile.is_open()) {
        std::cout << "ERROR\n";
        return 1;
    }

    outFile << "id,string,data,float64\n";

    for (const auto& row : mergedResults) {
        outFile << row.id << "," << row.name << "," << row.date << "," << row.value << "\n";
    }

    outFile.close();

    return 0;
}
