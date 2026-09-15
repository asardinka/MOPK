#include <iostream>
#include <fstream>
#include <string>
#include <random>
#include <vector>

bool generateCSV(const std::string& filename, int rowCount, std::mt19937& rng) {
    std::ofstream outFile;

    std::vector<char> writeBuffer(64 * 1024);
    outFile.rdbuf()->pubsetbuf(writeBuffer.data(), writeBuffer.size());
    
    outFile.open(filename);
    if (!outFile.is_open()) {
        std::cerr << "ERROR: " << filename << "\n";
        return false;
    }

    outFile << "id,string,data,float64\n";

    const std::vector<std::string> names = {
        "Alexander", "Dmitry", "Elena", "Maria", "Ivan", 
        "Anna", "Sergey", "Olga", "Maxim", "Natalia"
    };

    const std::vector<std::string> dates = {
        "2026-11-01", "2026-11-02", "2026-11-03", "2026-11-04", "2026-11-05",
        "2026-11-06", "2026-11-07", "2026-11-08", "2026-11-09", "2026-11-10",
        "2026-11-11", "2026-11-12", "2026-11-13", "2026-11-14", "2026-11-15",
        "2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19", "2026-11-20",
        "2026-11-21", "2026-11-22", "2026-11-23", "2026-11-24", "2026-11-25",
        "2026-11-26", "2026-11-27", "2026-11-28", "2026-11-29", "2026-11-30"
    };

    std::uniform_int_distribution<size_t> nameDist(0, names.size() - 1);
    std::uniform_int_distribution<size_t> dateDist(0, dates.size() - 1);
    std::uniform_real_distribution<double> doubleDist(0.0, 1000.0);

    for (int i = 1; i <= rowCount; ++i) {
        const std::string& randomName = names[nameDist(rng)];
        const std::string& randomDate = dates[dateDist(rng)]; // ИСПРАВЛЕНО: теперь используется dateDist
        double randomFloat = doubleDist(rng);

        outFile << i << "," << randomName << "," << randomDate << "," << randomFloat << "\n";
    }

    outFile.close();
    return true;
}

int main() {
    std::mt19937 rng(std::random_device{}());
    const int rowCount = 1000000;

    generateCSV("data1.csv", rowCount, rng);
    generateCSV("data2.csv", rowCount, rng);

    return 0;
}
