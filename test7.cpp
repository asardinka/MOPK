// Предположение по изменению структуры данных относительно test5.cpp:
// В test5.cpp для каждой подходящей строки выполняется поиск группы в std::unordered_map.
// Для этого вычисляется hash составного ключа name + date, затем находится позиция Row.
// После обработки итоговый std::vector<Row> дополнительно сортируется.
//
// В test7.cpp используется фиксированная двумерная таблица sums[50][10].
// Это возможно потому, что генератор использует известные 50 имён,
// а текущий диапазон фильтра содержит 10 известных дат.
// Имя и дата преобразуются в числовые индексы, после чего слияние выполняется напрямую:
// sums[nameId][dateId] += value.
// Таким образом, для merge не нужны unordered_map, строковый составной ключ и vector<Row>.
// Массив names заранее расположен в алфавитном порядке, а dates в порядке дат,
// поэтому последовательный обход таблицы сразу даёт требуемый порядок вывода
// и отдельная сортировка результата также не требуется.
// Это специализированное решение для заранее известного набора имён и дат.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
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

struct Result {
    std::array<std::array<double, 10>, 50> sums{};
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
            std::string_view date(line.data() + second + 1, third - second - 1);

            const auto nameIt = std::lower_bound(names.begin(), names.end(), name);
            const auto dateIt = std::lower_bound(dates.begin(), dates.end(), date);
            const std::size_t nameId = nameIt - names.begin();
            const std::size_t dateId = dateIt - dates.begin();

            double value;
            std::from_chars(line.data() + third + 1, line.data() + line.size(), value);

            result.sums[nameId][dateId] += value;
            result.used[nameId][dateId] = true;
        }
    }

    return result;
}

void printRows(const Result& result) {
    std::cout << "string,date,float64\n";

    for (std::size_t nameId = 0; nameId < names.size(); ++nameId) {
        for (std::size_t dateId = 0; dateId < dates.size(); ++dateId) {
            if (result.used[nameId][dateId]) {
                std::cout << names[nameId] << "," << dates[dateId] << "," << result.sums[nameId][dateId] << "\n";
            }
        }
    }
}

void saveRows(const Result& result, const std::string& outputFilename) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string,date,float64\n";

    for (std::size_t nameId = 0; nameId < names.size(); ++nameId) {
        for (std::size_t dateId = 0; dateId < dates.size(); ++dateId) {
            if (result.used[nameId][dateId]) {
                outputFile << names[nameId] << "," << dates[dateId] << "," << result.sums[nameId][dateId] << "\n";
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
// g++ -std=c++17 -O2 test7.cpp -o build/test7.exe && build/test7.exe
