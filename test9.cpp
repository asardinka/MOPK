// Предположение по улучшению производительности относительно test8.cpp:
// В test8.cpp каждая строка читается через std::getline(), после чего три раза вызывается
// std::string::find() для поиска разделителей CSV.
// Эти операции выполняются для всех строк входных файлов, включая строки,
// которые затем сразу отбрасываются фильтром.
//
// В test9.cpp файл читается целиком одним блоком в std::vector<char>.
// После этого CSV разбирается напрямую через const char*:
// указатель последовательно перемещается до запятых и символа новой строки.
// Для полей не создаются отдельные std::string и не вызываются getline() и find().
// string_view используется только как невладеющее представление имени и даты
// внутри уже загруженного буфера.
// Остальная логика test8.cpp сохраняется: имя переводится в nameId через unordered_map,
// dateId вычисляется из цифр дня, а сумма записывается прямо в фиксированную таблицу.
// Тест показывает эффект именно от блочного чтения файла и разбора через указатели.

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
    std::array<std::array<double, 10>, 50> sums{};
    std::array<std::array<bool, 10>, 50> used{};
};

std::vector<std::ifstream> openDataFiles() {
    std::vector<std::ifstream> files;

    for (const auto& entry : std::filesystem::directory_iterator("data")) {
        const std::string filename = entry.path().filename().string();

        if (entry.is_regular_file() && filename.rfind("data", 0) == 0 && entry.path().extension() == ".csv") {
            files.emplace_back(entry.path(), std::ios::binary);
        }
    }

    return files;
}

Result parseFilterMergeFiles(std::vector<std::ifstream>& files, std::string_view startDate, std::string_view endDate) {
    Result result;

    for (auto& file : files) {
        file.seekg(0, std::ios::end);
        const std::size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(fileSize);
        file.read(buffer.data(), fileSize);

        const char* current = buffer.data();
        const char* end = buffer.data() + buffer.size();

        while (current < end && *current != '\n') {
            ++current;
        }

        if (current < end) {
            ++current;
        }

        while (current < end) {
            while (current < end && *current != ',') {
                ++current;
            }

            if (current >= end) {
                break;
            }

            ++current;
            const char* nameStart = current;

            while (current < end && *current != ',') {
                ++current;
            }

            const char* nameEnd = current;
            ++current;
            const char* dateStart = current;

            while (current < end && *current != ',') {
                ++current;
            }

            const char* dateEnd = current;
            ++current;
            const char* valueStart = current;

            while (current < end && *current != '\n') {
                ++current;
            }

            const char* valueEnd = current;

            if (valueEnd > valueStart && *(valueEnd - 1) == '\r') {
                --valueEnd;
            }

            if (current < end) {
                ++current;
            }

            std::string_view date(dateStart, dateEnd - dateStart);

            if (date < startDate || date > endDate) {
                continue;
            }

            std::string_view name(nameStart, nameEnd - nameStart);
            const std::size_t nameId = nameIds.find(name)->second;

            const int day = (dateStart[8] - '0') * 10 + (dateStart[9] - '0');
            const std::size_t dateId = day - 1;

            double value;
            std::from_chars(valueStart, valueEnd, value);

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

    const std::string_view startDate = "2026-11-01";
    const std::string_view endDate = "2026-11-10";

    const auto programStart = Clock::now();

    // 1. Open all data*.csv files.
    const auto openStart = Clock::now();
    std::vector<std::ifstream> files = openDataFiles();
    const auto openEnd = Clock::now();

    // 2. Read files by blocks, parse, filter and merge directly into fixed table.
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
// g++ -std=c++17 -O2 test9.cpp -o build/test9.exe && build/test9.exe
