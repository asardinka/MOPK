#include <iostream>
#include <fstream>
#include <string>
#include <random>
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
            const std::string monthStr =
                (month < 10 ? "0" : "") + std::to_string(month);
            const std::string dayStr =
                (day < 10 ? "0" : "") + std::to_string(day);

            dates.push_back("2026-" + monthStr + "-" + dayStr);
        }
    }

    return dates;
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

    outFile << "id,string,data,float64\n";

    const std::vector<std::string> names = {
        "Alexander", "Dmitry", "Elena", "Maria", "Ivan",
        "Anna", "Sergey", "Olga", "Maxim", "Natalia",
        "Alexey", "Andrey", "Anton", "Artem", "Boris",
        "Victor", "Vladimir", "Denis", "Evgeny", "Kirill",
        "Mikhail", "Nikolay", "Oleg", "Pavel", "Roman",
        "Stanislav", "Yuri", "Alina", "Anastasia", "Daria",
        "Ekaterina", "Irina", "Ksenia", "Marina", "Nadezhda",
        "Polina", "Sofia", "Tatiana", "Yulia", "Victoria",
        "Grigory", "Ilya", "Konstantin", "Leonid", "Matvey",
        "Nikita", "Petr", "Ruslan", "Svetlana", "Vera"
    };

    const std::vector<std::string> dates = generateDates();

    std::uniform_int_distribution<size_t> nameDist(0, names.size() - 1);
    std::uniform_int_distribution<size_t> dateDist(0, dates.size() - 1);
    std::uniform_real_distribution<double> doubleDist(0.0, 1.0);

    for (int i = 1; i <= rowCount; ++i) {
        const std::string& randomName = names[nameDist(rng)];
        const std::string& randomDate = dates[dateDist(rng)];
        const double randomFloat = doubleDist(rng);

        outFile << i << "," << randomName << "," << randomDate << "," << randomFloat << "\n";
    }

    outFile.close();
    return true;
}

int main() {
    std::mt19937 rng(std::random_device{}());
    const int rowCount = 1000000;

    if (!generateCSV("data/data1.csv", rowCount, rng)) {
        return 1;
    }

    if (!generateCSV("data/data2.csv", rowCount, rng)) {
        return 1;
    }

    return 0;
}

// $Env:PATH += ";C:\msys64\ucrt64\bin"
// g++ -std=c++17 make_csv.cpp -o build/make_csv.exe && build/make_csv.exe