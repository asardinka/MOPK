// Предположение по улучшению производительности относительно test7.cpp:
// В test7.cpp после фильтрации для каждой подходящей строки выполняются два бинарных поиска:
// std::lower_bound() ищет имя среди 50 строк и дату среди 10 строк.
// Для имени это требует нескольких сравнений строк, а для даты поиск вообще избыточен,
// потому что формат даты фиксирован и диапазон содержит дни 01..10 одного месяца.
//
// В test8.cpp имя ищется через заранее подготовленный std::unordered_map<string_view, id>.
// Для каждой строки вычисляется hash только имени и сразу получается nameId.
// dateId вычисляется напрямую из двух цифр дня в строке даты:
// 2026-11-07 -> day = 7 -> dateId = 6.
// После этого накопление по-прежнему выполняется напрямую:
// sums[nameId][dateId][valueIndex] += value.
// Таким образом, фиксированная таблица сохраняется, но оба std::lower_bound() удаляются.

#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

const std::array<std::string_view, 50> names = {
    "Alexander", "Alexey", "Alina", "Anastasia", "Andrey",
    "Anna", "Anton", "Artem", "Boris", "Daria",
    "Denis", "Dmitry", "Ekaterina", "Elena", "Evgeny",
    "Grigory", "Ilya", "Irina", "Ivan", "Kirill",
    "Konstantin", "Ksenia", "Leonid", "Maria", "Marina",
    "Matvey", "Maxim", "Mikhail", "Nadezhda", "Natalia",
    "Nikita", "Nikolay", "Oleg", "Olga", "Pavel",
    "Petr", "Polina", "Roman", "Ruslan", "Sergey",
    "Sofia", "Stanislav", "Svetlana", "Tatiana", "Vera",
    "Victor", "Victoria", "Vladimir", "Yulia", "Yuri"
};

const std::array<std::string_view, 10> dates = {
    "2026-11-01", "2026-11-02", "2026-11-03", "2026-11-04", "2026-11-05",
    "2026-11-06", "2026-11-07", "2026-11-08", "2026-11-09", "2026-11-10"
};

const std::unordered_map<std::string_view, std::size_t> nameIds = {
    {"Alexander", 0}, {"Alexey", 1}, {"Alina", 2}, {"Anastasia", 3}, {"Andrey", 4},
    {"Anna", 5}, {"Anton", 6}, {"Artem", 7}, {"Boris", 8}, {"Daria", 9},
    {"Denis", 10}, {"Dmitry", 11}, {"Ekaterina", 12}, {"Elena", 13}, {"Evgeny", 14},
    {"Grigory", 15}, {"Ilya", 16}, {"Irina", 17}, {"Ivan", 18}, {"Kirill", 19},
    {"Konstantin", 20}, {"Ksenia", 21}, {"Leonid", 22}, {"Maria", 23}, {"Marina", 24},
    {"Matvey", 25}, {"Maxim", 26}, {"Mikhail", 27}, {"Nadezhda", 28}, {"Natalia", 29},
    {"Nikita", 30}, {"Nikolay", 31}, {"Oleg", 32}, {"Olga", 33}, {"Pavel", 34},
    {"Petr", 35}, {"Polina", 36}, {"Roman", 37}, {"Ruslan", 38}, {"Sergey", 39},
    {"Sofia", 40}, {"Stanislav", 41}, {"Svetlana", 42}, {"Tatiana", 43}, {"Vera", 44},
    {"Victor", 45}, {"Victoria", 46}, {"Vladimir", 47}, {"Yulia", 48}, {"Yuri", 49}
};

struct Result {
    std::array<std::array<std::array<double, 7>, 10>, 50> sums{};
    std::array<std::array<bool, 10>, 50> used{};
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

Result parseFilterMergeFiles(std::vector<std::ifstream>& files, const std::string& startDate, const std::string& endDate) {
    Result result;

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
            const std::size_t nameId = nameIds.find(name)->second;

            const int day = (line[second + 9] - '0') * 10 + (line[second + 10] - '0');
            const std::size_t dateId = day - 1;

            std::size_t valueStart = third + 1;

            for (int i = 0; i < 7; ++i) {
                const std::size_t valueEnd = i == 6 ? line.size() : line.find(',', valueStart);
                double value;
                std::from_chars(line.data() + valueStart, line.data() + valueEnd, value);
                result.sums[nameId][dateId][i] += value;
                valueStart = valueEnd + 1;
            }

            result.used[nameId][dateId] = true;
        }
    }

    return result;
}

void printRows(const Result& result) {
    std::cout << "string,date,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7\n";

    for (std::size_t nameId = 0; nameId < names.size(); ++nameId) {
        for (std::size_t dateId = 0; dateId < dates.size(); ++dateId) {
            if (result.used[nameId][dateId]) {
                std::cout << names[nameId] << "," << dates[dateId]; for (double value : result.sums[nameId][dateId]) std::cout << "," << value; std::cout << "\n";
            }
        }
    }
}

void saveRows(const Result& result, const std::string& outputFilename) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string,date,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7\n";

    for (std::size_t nameId = 0; nameId < names.size(); ++nameId) {
        for (std::size_t dateId = 0; dateId < dates.size(); ++dateId) {
            if (result.used[nameId][dateId]) {
                outputFile << names[nameId] << "," << dates[dateId]; for (double value : result.sums[nameId][dateId]) outputFile << "," << value; outputFile << "\n";
            }
        }
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

    // 2. Parse, filter and merge directly into fixed table.
    const auto processStart = Clock::now();
    Result result = parseFilterMergeFiles(files, startDate, endDate);
    const auto processEnd = Clock::now();

    const auto processingEnd = Clock::now();

    const double openMs = std::chrono::duration<double, std::milli>(openEnd - openStart).count();
    const double processMs = std::chrono::duration<double, std::milli>(processEnd - processStart).count();
    const double processingMs = std::chrono::duration<double, std::milli>(processingEnd - programStart).count();

    // 3. Print result to console.
    printRows(result);

    // 4. Save the same result to file.
    saveRows(result, "data/merged.csv");

    std::cout << "\n--- TIME ---\n";
    std::cout << "Open:                    " << openMs << " ms\n";
    std::cout << "Parse+Filter+Merge+Sort: " << processMs << " ms\n";
    std::cout << "Processing:              " << processingMs << " ms\n";

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 -O2 test8.cpp -o build/test8.exe && build/test8.exe

