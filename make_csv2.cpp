#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

std::vector<std::string> generateDates() {
    std::vector<std::string> dates;
    dates.reserve(103);

    for (int month = 9; month <= 12; ++month) {
        int lastDay = 0;

        switch (month) {
            case 9:
            case 11:
                lastDay = 30;
                break;
            case 10:
                lastDay = 31;
                break;
            case 12:
                lastDay = 12;
                break;
        }

        for (int day = 1; day <= lastDay; ++day) {
            const std::string monthStr = (month < 10 ? "0" : "") + std::to_string(month);
            const std::string dayStr = (day < 10 ? "0" : "") + std::to_string(day);

            dates.push_back(dayStr + "." + monthStr + ".2026");
        }
    }

    return dates;
}

std::string generateRandomString(std::mt19937& rng) {
    static const std::string characters = "abcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<int> lengthDist(10, 50);
    std::uniform_int_distribution<std::size_t> charDist(0, characters.size() - 1);

    const int length = lengthDist(rng);
    std::string result;
    result.reserve(length);

    for (int i = 0; i < length; ++i) {
        result += characters[charDist(rng)];
    }

    return result;
}

bool generateCSV(const std::string& filename, int rowCount, std::mt19937& rng) {
    std::ofstream outFile;

    std::vector<char> writeBuffer(64 * 1024);
    outFile.rdbuf()->pubsetbuf(writeBuffer.data(), writeBuffer.size());

    outFile.open(filename);
    if (!outFile.is_open()) {
        std::cerr << "ERROR: " << filename << "\n";
        return false;
    }

    outFile << "id;string;data;float64_1;float64_2;float64_3;float64_4;float64_5;float64_6;float64_7\n";

    const std::vector<std::string> dates = generateDates();

    std::uniform_int_distribution<std::size_t> dateDist(0, dates.size() - 1);
    std::uniform_real_distribution<double> doubleDist(0.0, 100000.0);

    for (int i = 1; i <= rowCount; ++i) {
        const std::string randomString = generateRandomString(rng);
        const std::string& randomDate = dates[dateDist(rng)];

        outFile << i << ";" << randomString << ";" << randomDate;

        for (int valueIndex = 0; valueIndex < 7; ++valueIndex) {
            outFile << ";" << doubleDist(rng);
        }

        outFile << "\n";
    }

    outFile.close();
    return true;
}

int main() {
    std::mt19937 rng(std::random_device{}());
    const int rowCount = 1000000;

    if (!generateCSV("data/S1.csv", rowCount, rng)) {
        return 1;
    }

    if (!generateCSV("data/S2.cvs", rowCount, rng)) {
        return 1;
    }

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 make_csv2.cpp -o build/make_csv2.exe && build/make_csv2.exe
