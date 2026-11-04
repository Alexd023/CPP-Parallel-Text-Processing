#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <new>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif


using Frequencies = std::unordered_map<std::string, std::uint64_t>;
using Clock = std::chrono::steady_clock;

struct Result {
    Frequencies frequencies;
    std::uint64_t word_count = 0;
};

std::uint32_t read_utf8(const std::string& text, std::size_t& position, std::size_t end) {
    const unsigned char first = static_cast<unsigned char>(text[position++]);
    if (first < 0x80) return first;

    int extra_bytes = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xC2 && first <= 0xDF) {
        extra_bytes = 1;
        codepoint = first & 0x1F;
    }
    else if (first >= 0xE0 && first <= 0xEF) {
        extra_bytes = 2;
        codepoint = first & 0x0F;
    }
    else if (first >= 0xF0 && first <= 0xF4) {
        extra_bytes = 3;
        codepoint = first & 0x07;
    }
    else {
        return 0;
    }

    const std::size_t previous_position = position;
    for (int i = 0; i < extra_bytes; ++i) {
        if (position >= end) {
            position = previous_position;
            return 0;
        }
        const unsigned char next = static_cast<unsigned char>(text[position]);
        if ((next & 0xC0) != 0x80) {
            position = previous_position;
            return 0;
        }
        codepoint = (codepoint << 6) | (next & 0x3F);
        ++position;
    }

    if ((extra_bytes == 2 && codepoint < 0x800) ||
        (extra_bytes == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
        codepoint > 0x10FFFF) {
        return 0;
    }
    return codepoint;
}

bool is_word_character(std::uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9')) return true;
    return (cp >= 0x00C0 && cp <= 0x00D6) ||
        (cp >= 0x00D8 && cp <= 0x00F6) ||
        (cp >= 0x00F8 && cp <= 0x024F) ||
        (cp >= 0x1E00 && cp <= 0x1EFF);
}

std::uint32_t lower_case(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    if ((cp >= 0x00C0 && cp <= 0x00D6) ||
        (cp >= 0x00D8 && cp <= 0x00DE)) return cp + 0x20;

    switch (cp) {
    case 0x0102: return 0x0103;
    case 0x0218: return 0x0219;
    case 0x021A: return 0x021B;
    case 0x015E: return 0x015F;
    case 0x0162: return 0x0163;
    default: break;
    }
    if ((cp >= 0x0100 && cp <= 0x012F && cp % 2 == 0) ||
        (cp >= 0x0132 && cp <= 0x0137 && cp % 2 == 0) ||
        (cp >= 0x014A && cp <= 0x0177 && cp % 2 == 0) ||
        (cp >= 0x0139 && cp <= 0x0148 && cp % 2 == 1) ||
        (cp >= 0x0179 && cp <= 0x017E && cp % 2 == 1) ||
        (cp >= 0x1E00 && cp <= 0x1EFF && cp % 2 == 0)) return cp + 1;
    if (cp == 0x0178) return 0x00FF;
    return cp;
}

void append_utf8(std::string& output, std::uint32_t cp) {
    if (cp < 0x80) {
        output.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800) {
        output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000) {
        output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else {
        output.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void process_range(const std::string& text, std::size_t start,
    std::size_t end, Result& result) {
    std::string word;
    word.reserve(32);

    for (std::size_t i = start; i < end;) {
        const std::uint32_t cp = read_utf8(text, i, end);
        if (is_word_character(cp)) {
            append_utf8(word, lower_case(cp));
        }
        else if (!word.empty()) {
            ++result.frequencies[word];
            ++result.word_count;
            word.clear();
        }
    }
    if (!word.empty()) {
        ++result.frequencies[word];
        ++result.word_count;
    }
}

std::vector<std::size_t> make_boundaries(const std::string& text, int threads) {
    std::vector<std::size_t> boundaries;
    boundaries.reserve(static_cast<std::size_t>(threads) + 1);
    boundaries.push_back(0);
    for (int i = 1; i < threads; ++i) {
        std::size_t pos = (text.size() / static_cast<std::size_t>(threads)) * i;
        while (pos < text.size() && pos > 0 &&
            !std::isspace(static_cast<unsigned char>(text[pos - 1]))) {
            ++pos;
        }
        boundaries.push_back(pos);
    }
    boundaries.push_back(text.size());
    return boundaries;
}

std::string load_file(const std::string& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Nu se poate deschide fisierul: " + path);
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("Nu se poate determina marimea fisierului.");
    std::string text(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        throw std::runtime_error("Eroare la citirea fisierului.");
    }
    return text;
}

void print_top_10(const Result& result) {
    std::vector<std::pair<std::string, std::uint64_t>> sorted(
        result.frequencies.begin(), result.frequencies.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
        });

    std::cout << "\nTop 10 cuvinte:\n";
    const std::size_t limit = std::min<std::size_t>(10, sorted.size());
    for (std::size_t i = 0; i < limit; ++i) {
        std::cout << "  " << (i + 1) << ". " << sorted[i].first
            << " -> " << sorted[i].second << '\n';
    }
    if (sorted.empty()) std::cout << "  Nu au fost gasite cuvinte.\n";
}

void map_and_combine_range(const std::string& text, std::size_t start,
    std::size_t end, Result& local) {
    process_range(text, start, end, local);
}

using GroupedValues = std::unordered_map<std::string, std::vector<std::uint64_t>>;

std::vector<GroupedValues> shuffle_by_key(const std::vector<Result>& mapper_outputs,
    int reducer_count) {
    std::vector<GroupedValues> groups(static_cast<std::size_t>(reducer_count));
    const std::hash<std::string> hash_word;
    for (const auto& output : mapper_outputs) {
        for (const auto& [word, frequency] : output.frequencies) {
            const std::size_t shard = hash_word(word) % groups.size();
            groups[shard][word].push_back(frequency);
        }
    }
    return groups;
}

void reduce_shard(const GroupedValues& grouped, Frequencies& result) {
    result.reserve(grouped.size());
    for (const auto& [word, partial_counts] : grouped) {
        std::uint64_t sum = 0;
        for (std::uint64_t count : partial_counts) sum += count;
        result.emplace(word, sum);
    }
}

using TimePoint = Clock::time_point;
double milliseconds(TimePoint a, TimePoint b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct MapReduceTimings {
    double map_combine_ms = 0.0;
    double shuffle_ms = 0.0;
    double reduce_ms = 0.0;
    double final_merge_ms = 0.0;
    double total_ms = 0.0;
};

Result count_words_mapreduce(const std::string& text, int mapper_count,
    int reducer_count, MapReduceTimings& timings) {
    if (mapper_count < 1 || reducer_count < 1)
        throw std::invalid_argument("Numarul de fire trebuie sa fie pozitiv.");

    const auto started = Clock::now();

    const auto boundaries = make_boundaries(text, mapper_count);
    std::vector<Result> mapped(static_cast<std::size_t>(mapper_count));
    {
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(mapper_count - 1));
        for (int i = 1; i < mapper_count; ++i) {
            workers.emplace_back(map_and_combine_range, std::cref(text),
                boundaries[i], boundaries[i + 1], std::ref(mapped[i]));
        }
        map_and_combine_range(text, boundaries[0], boundaries[1], mapped[0]);
        for (auto& worker : workers) worker.join();
    }
    const auto after_map = Clock::now();

    const auto grouped = shuffle_by_key(mapped, reducer_count);
    const auto after_shuffle = Clock::now();

    std::vector<Frequencies> reduced(static_cast<std::size_t>(reducer_count));
    {
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(reducer_count - 1));
        for (int i = 1; i < reducer_count; ++i) {
            workers.emplace_back(reduce_shard, std::cref(grouped[i]),
                std::ref(reduced[i]));
        }
        reduce_shard(grouped[0], reduced[0]);
        for (auto& worker : workers) worker.join();
    }
    const auto after_reduce = Clock::now();

    Result merged;
    for (const auto& part : mapped) merged.word_count += part.word_count;
    for (auto& dictionary : reduced) {
        merged.frequencies.merge(dictionary);
    }
    const auto finished = Clock::now();

    timings.map_combine_ms = milliseconds(started, after_map);
    timings.shuffle_ms = milliseconds(after_map, after_shuffle);
    timings.reduce_ms = milliseconds(after_shuffle, after_reduce);
    timings.final_merge_ms = milliseconds(after_reduce, finished);
    timings.total_ms = milliseconds(started, finished);
    return merged;
}

bool same_result(const Result& a, const Result& b) {
    return a.word_count == b.word_count && a.frequencies == b.frequencies;
}

std::vector<int> parse_thread_list(const std::string& list) {
    std::vector<int> values;
    std::stringstream stream(list);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.empty()) throw std::invalid_argument("Lista de fire este invalida.");
        std::size_t consumed = 0;
        const int value = std::stoi(token, &consumed);
        if (consumed != token.size() || value < 1 || value > 64)
            throw std::invalid_argument("Numarul de fire trebuie sa fie intre 1 si 64.");
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    }
    if (values.empty()) throw std::invalid_argument("Lista de fire este goala.");
    std::sort(values.begin(), values.end());
    return values;
}

void run_self_test() {
    struct Test {
        std::string text;
        Frequencies expected;
    };
    const std::vector<Test> samples = {
        { "",{} },
        { "ana ANA ana",{ { "ana", 3 } } },
        { "măr MĂR pere ȘCOALĂ școală",{ { "măr", 2 },{ "pere", 1 },{ "școală", 2 } } },
        { "ana,are.mere!pere?",{ { "ana", 1 },{ "are", 1 },{ "mere", 1 },{ "pere", 1 } } },
        { "   \n\t  ",{} },
        { "un_cuvant două_cuvinte C++ 2026",
         { { "un", 1 },{ "cuvant", 1 },{ "două", 1 },{ "cuvinte", 1 },{ "c", 1 },{ "2026", 1 } } },
        { "foartelungfaraspatiisialtelitere",{ { "foartelungfaraspatiisialtelitere", 1 } } }
    };

    for (std::size_t i = 0; i < samples.size(); ++i) {
        std::uint64_t expected_total = 0;
        for (const auto& [word, frequency] : samples[i].expected)
            expected_total += frequency;

        for (const int n : {1, 2, 4, 8}) {
            MapReduceTimings timings;
            const Result actual = count_words_mapreduce(samples[i].text, n, n, timings);
            if (actual.word_count != expected_total ||
                actual.frequencies != samples[i].expected) {
                throw std::runtime_error(
                    "Test MapReduce esuat (cazul " + std::to_string(i) + ", " + std::to_string(n) + " fire)."
                );
            }
        }
    }
    std::cout << "OK: " << samples.size() << " cazuri de test, pentru 1, 2, 4 si 8 fire MapReduce.\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        std::string path;
        std::vector<int> thread_counts{ 1, 2, 4, 8 };
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--self-test") { run_self_test(); return 0; }
            if (arg == "--help" || arg == "-h") {
                std::cout << "Utilizare: procesare_text_v2.exe [fisier.txt] [--threads 1,2,4,8]\n"
                    << "           procesare_text_v2.exe --self-test\n";
                return 0;
            }
            if (arg == "--threads") {
                if (i + 1 >= argc) throw std::invalid_argument("Lipseste lista dupa --threads.");
                thread_counts = parse_thread_list(argv[++i]);
            }
            else if (path.empty()) {
                path = arg;
            }
            else {
                throw std::invalid_argument("Argument necunoscut: " + arg);
            }
        }
        if (path.empty()) {
            std::cout << "Calea fisierului .txt (Enter = exemplu.txt): ";
            std::getline(std::cin, path);
            if (path.empty()) path = "exemplu.txt";
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                path = path.substr(1, path.size() - 2);
        }

        std::cout << "\n=== PROCESARE DE TEXT IN C++ CU FIRE - V2 MAPREDUCE ===\n";
        const auto start_loading = Clock::now();
        const std::string text = load_file(path);
        const double loading_ms = milliseconds(start_loading, Clock::now());
        std::cout << "Fisier: " << path << '\n';
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Dimensiune: " << (static_cast<double>(text.size()) / (1024 * 1024)) << " MiB\n";
        std::cout << "Citire RAM (separat): " << loading_ms << " ms\n\n";
        std::cout << "Fire  MapReduce(ms)   Accelerare    Corect\n";
        std::cout << "--------------------------------------------\n";

        bool has_reference = false;
        bool has_baseline = false;
        double baseline_ms = 0.0;
        Result reference;
        MapReduceTimings details;
        int details_threads = 0;

        for (const int n : thread_counts) {
            MapReduceTimings timing;
            const Result mr = count_words_mapreduce(text, n, n, timing);

            if (has_reference && !same_result(reference, mr))
                throw std::runtime_error("Rezultatele MapReduce difera intre numerele de fire!");
            if (!has_reference) {
                reference = mr;
                has_reference = true;
            }

            if (n == 1) {
                baseline_ms = timing.total_ms;
                has_baseline = true;
            }

            std::cout << std::left << std::setw(6) << n
                << std::setw(16) << timing.total_ms;
            if (has_baseline && timing.total_ms > 0) {
                std::cout << std::setw(14) << (baseline_ms / timing.total_ms);
            }
            else {
                std::cout << std::setw(14) << "-";
            }
            std::cout << "OK\n";

            if (n == 4 || details_threads == 0) {
                details = timing;
                details_threads = n;
            }
        }

        std::cout << "\nDetalii V2 (" << details_threads << " fire MAP si "
            << details_threads << " fire REDUCE, etape succesive):\n";
        std::cout << "  MAP + COMBINER:  " << details.map_combine_ms << " ms\n"
            << "  SHUFFLE:         " << details.shuffle_ms << " ms\n"
            << "  REDUCE:          " << details.reduce_ms << " ms\n"
            << "  REUNIRE FINALA:  " << details.final_merge_ms << " ms\n"
            << "  TOTAL V2:        " << details.total_ms << " ms\n";
        std::cout << "\nTotal cuvinte: " << reference.word_count << '\n'
            << "Cuvinte distincte: " << reference.frequencies.size() << '\n';
        print_top_10(reference);
        return 0;
    }
    catch (const std::bad_alloc&) {
        std::cerr << "Memorie insuficienta: fisierul si dictionarele sunt in RAM.\n";
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "Eroare: " << e.what() << '\n';
        return 1;
    }
}