/*
КЛЮЧЕВЫЕ ИЗМЕНЕНИЯ

test1:
Базовый вариант. Файлы читались построчно, каждая строка разбиралась через std::stringstream.
Парсинг, фильтрация, слияние и сортировка выполнялись отдельными этапами.

test2:
std::stringstream заменён на прямой поиск разделителей через std::string::find().
Это убрало создание потокового объекта и работу streambuf для каждой строки.

test3:
Парсинг, фильтрация и слияние объединены в один проход.
Строки вне диапазона дат отбрасываются до создания Row и до преобразования float64.
Исчезли промежуточные vectors rows и filteredRows.

test4:
std::stod(line.substr(...)) заменён на std::from_chars().
Число преобразуется прямо из исходного буфера без создания временного std::string.

test5:
Для временной работы с именем и датой использован std::string_view.
Это уменьшило число копирований строк в горячем цикле.

test6:
Проверен std::map вместо unordered_map + vector + sort.
Результат оказался медленнее из-за дерева, сравнений строк и разрозненного размещения узлов в памяти.

test7:
Проверена фиксированная таблица сумм.
Само накопление стало очень дешёвым, но выигрыш съели два std::lower_bound() для поиска nameId и dateId.

test8:
lower_bound для даты заменён прямым вычислением индекса дня.
Имя стало переводиться в nameId через unordered_map<string_view, id>.

test9:
Главное ускорение второго этапа оптимизации.
std::getline() и std::string::find() заменены на чтение файла целиком в std::vector<char>
и последовательный разбор CSV через const char*.

test10:
Финальный универсальный вариант.
Сохраняет блочное чтение и pointer-based parsing из test9, но больше не зависит
от заранее известных имён и фиксированного диапазона дат.
Имена обнаруживаются динамически, дата преобразуется в YYYYMMDD,
а группы name + date хранятся по компактному числовому ключу.
Весь основной pipeline собран в одной функции processFiles().

КАК РАБОТАЕТ ИТОГОВЫЙ ВАРИАНТ

processFiles() получает:
- список входных файлов;
- разделитель колонок CSV;
- разделитель даты;
- десятичный разделитель float64;
- начальную дату;
- конечную дату.

Для каждого файла:
1. Файл читается целиком одним блоком в std::vector<char>.
2. CSV разбирается последовательным движением указателя по памяти.
3. Дата преобразуется в целое число YYYYMMDD и сразу фильтруется.
4. Имя представляется через string_view без копирования.
5. Каждому новому имени один раз назначается числовой nameId.
6. float64 преобразуется непосредственно из char-диапазона.
7. Пара nameId + date кодируется в один uint64_t и сумма накапливается в unordered_map.
8. После обработки всех файлов создаётся итоговый vector<Row>.
9. Итоговый vector сортируется по имени, затем по дате.

Формат даты предполагается YYYY-MM-DD или YYYY.MM.DD в зависимости от dateSeparator.
CSV-разделитель не должен совпадать с разделителем даты или десятичным разделителем,
если файл не использует кавычки вокруг полей.

РЕЗУЛЬТАТЫ ТЕСТОВ, Processing ms

          Без оптимизации     -O2         -O3
test1        1969.150       976.788     963.354
test2        1810.340       760.974     760.448
test3         479.483       221.853     234.117
test4         413.962       173.824     179.053
test5         325.107       173.237     165.413
test6         460.775       180.118     186.168
test7         350.844       175.968     175.041
test8         317.703       164.438     164.847
test9         146.948        75.418      75.492

test1 -O2 -> test9 -O2: 976.788 / 75.418 = примерно 12.95 раза быстрее.
*/

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Row {
    std::string name;
    int date;
    double value;
};

int parseDate(const char* begin, const char* end, char dateSeparator) {
    int parts[3] = {};
    int part = 0;

    for (const char* current = begin; current < end; ++current) {
        if (*current == dateSeparator) {
            ++part;
        } else {
            parts[part] = parts[part] * 10 + (*current - '0');
        }
    }

    return parts[0] * 10000 + parts[1] * 100 + parts[2];
}

double parseFloat64(const char* begin, const char* end, char decimalSeparator) {
    if (decimalSeparator == '.') {
        double value;
        std::from_chars(begin, end, value);
        return value;
    }

    bool negative = false;

    if (begin < end && *begin == '-') {
        negative = true;
        ++begin;
    }

    double value = 0.0;

    while (begin < end && *begin >= '0' && *begin <= '9') {
        value = value * 10.0 + (*begin - '0');
        ++begin;
    }

    if (begin < end && *begin == decimalSeparator) {
        ++begin;
        double factor = 0.1;

        while (begin < end && *begin >= '0' && *begin <= '9') {
            value += (*begin - '0') * factor;
            factor *= 0.1;
            ++begin;
        }
    }

    if (begin < end && (*begin == 'e' || *begin == 'E')) {
        ++begin;
        bool exponentNegative = false;

        if (begin < end && (*begin == '+' || *begin == '-')) {
            exponentNegative = *begin == '-';
            ++begin;
        }

        int exponent = 0;

        while (begin < end && *begin >= '0' && *begin <= '9') {
            exponent = exponent * 10 + (*begin - '0');
            ++begin;
        }

        value *= std::pow(10.0, exponentNegative ? -exponent : exponent);
    }

    return negative ? -value : value;
}

std::string dateToString(int date, char dateSeparator) {
    const int year = date / 10000;
    const int month = date / 100 % 100;
    const int day = date % 100;

    std::string result = std::to_string(year);
    result += dateSeparator;
    result += month < 10 ? "0" + std::to_string(month) : std::to_string(month);
    result += dateSeparator;
    result += day < 10 ? "0" + std::to_string(day) : std::to_string(day);

    return result;
}

std::vector<Row> processFiles(const std::vector<std::string>& filenames, char csvSeparator, char dateSeparator, char decimalSeparator, const std::string& startDate, const std::string& endDate) {
    const int start = parseDate(startDate.data(), startDate.data() + startDate.size(), dateSeparator);
    const int finish = parseDate(endDate.data(), endDate.data() + endDate.size(), dateSeparator);

    std::deque<std::string> names;
    std::unordered_map<std::string_view, std::size_t> nameIds;
    std::unordered_map<std::uint64_t, double> sums;

    for (const std::string& filename : filenames) {
        std::ifstream file(filename, std::ios::binary);

        file.seekg(0, std::ios::end);
        const std::size_t fileSize = static_cast<std::size_t>(file.tellg());
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
            while (current < end && *current != csvSeparator) {
                ++current;
            }

            if (current >= end) {
                break;
            }

            ++current;
            const char* nameStart = current;

            while (current < end && *current != csvSeparator) {
                ++current;
            }

            const char* nameEnd = current;
            ++current;
            const char* dateStart = current;

            while (current < end && *current != csvSeparator) {
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

            const int date = parseDate(dateStart, dateEnd, dateSeparator);

            if (date < start || date > finish) {
                continue;
            }

            const std::string_view name(nameStart, nameEnd - nameStart);
            auto nameIt = nameIds.find(name);
            std::size_t nameId;

            if (nameIt == nameIds.end()) {
                nameId = names.size();
                names.emplace_back(name);
                nameIds.emplace(std::string_view(names.back()), nameId);
            } else {
                nameId = nameIt->second;
            }

            const double value = parseFloat64(valueStart, valueEnd, decimalSeparator);
            const std::uint64_t key = (static_cast<std::uint64_t>(nameId) << 32) | static_cast<std::uint32_t>(date);
            sums[key] += value;
        }
    }

    std::vector<Row> rows;
    rows.reserve(sums.size());

    for (const auto& item : sums) {
        const std::size_t nameId = static_cast<std::size_t>(item.first >> 32);
        const int date = static_cast<int>(item.first & 0xffffffff);
        rows.push_back({names[nameId], date, item.second});
    }

    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }

        return left.date < right.date;
    });

    return rows;
}

void printRows(const std::vector<Row>& rows, char csvSeparator, char dateSeparator) {
    std::cout << "string" << csvSeparator << "date" << csvSeparator << "float64\n";

    for (const auto& row : rows) {
        std::cout << row.name << csvSeparator << dateToString(row.date, dateSeparator) << csvSeparator << row.value << "\n";
    }
}

void saveRows(const std::vector<Row>& rows, const std::string& outputFilename, char csvSeparator, char dateSeparator) {
    std::ofstream outputFile(outputFilename);
    outputFile << "string" << csvSeparator << "date" << csvSeparator << "float64\n";

    for (const auto& row : rows) {
        outputFile << row.name << csvSeparator << dateToString(row.date, dateSeparator) << csvSeparator << row.value << "\n";
    }
}

int main() {
    using Clock = std::chrono::steady_clock;

    const std::vector<std::string> filenames = {"data/data1.csv", "data/data2.csv"};
    const char csvSeparator = ',';
    const char dateSeparator = '-';
    const char decimalSeparator = '.';
    const std::string startDate = "2026-11-01";
    const std::string endDate = "2026-11-10";

    const auto processingStart = Clock::now();
    std::vector<Row> rows = processFiles(filenames, csvSeparator, dateSeparator, decimalSeparator, startDate, endDate);
    const auto processingEnd = Clock::now();

    const double processingMs = std::chrono::duration<double, std::milli>(processingEnd - processingStart).count();

    printRows(rows, csvSeparator, dateSeparator);
    saveRows(rows, "data/merged.csv", csvSeparator, dateSeparator);

    std::cout << "\n--- TIME ---\n";
    std::cout << "Processing: " << processingMs << " ms\n";

    return 0;
}

// $Env:PATH += ";C:\\msys64\\ucrt64\\bin"
// g++ -std=c++17 -O2 test10.cpp -o build/test10.exe && build/test10.exe
