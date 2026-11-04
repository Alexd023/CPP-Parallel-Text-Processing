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

std::uint32_t read_utf8(const std::string& text, 
                        std::size_t& position, 
                        std::size_t end) {
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

void process_range(const std::string& text,
                   std::size_t start,
                   std::size_t end,
                   Result& result) {
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
        while (pos < text.size() && pos > 0 && !std::isspace(static_cast<unsigned char>(text[pos - 1]))) {
            ++pos;
        }
        boundaries.push_back(pos);
    }
    boundaries.push_back(text.size());
    return boundaries;
}

Result count_words(const std::string& text, int threads) {
    const auto boundaries = make_boundaries(text, threads);
    std::vector<Result> partial(static_cast<std::size_t>(threads));
    std::vector<std::thread> workers;
    workers.reserve(threads > 1 ? static_cast<std::size_t>(threads - 1) : 0);

    for (int i = 1; i < threads; ++i) {
        workers.emplace_back(process_range, std::cref(text), boundaries[i], boundaries[i + 1], std::ref(partial[i]));
    }
    process_range(text, boundaries[0], boundaries[1], partial[0]);
    for (auto& worker : workers) worker.join();

    Result merged;
    for (auto& part : partial) {
        merged.word_count += part.word_count;
        for (const auto& [word, frequency] : part.frequencies) {
            merged.frequencies[word] += frequency;
        }
    }
    return merged;
}

std::string load_file(const std::string& path) {
    std::u8string utf8Path(path.begin(), path.end());
    std::ifstream input(std::filesystem::path(utf8Path),
                        std::ios::binary | std::ios::ate);
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
    std::vector<std::pair<std::string, std::uint64_t>> sorted(result.frequencies.begin(), result.frequencies.end());
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

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        std::string path;
        if (argc >= 2) {
            path = argv[1];
        }
        else {
            std::cout << "Calea fisierului .txt (Enter = exemplu.txt): ";
            std::getline(std::cin, path);
            if (path.empty()) path = "exemplu.txt";
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
                path = path.substr(1, path.size() - 2);
            }
        }

        std::cout << "\n=== PROCESARE DE TEXT IN C++ CU FIRE ===\n";
        const auto loading_start = Clock::now();
        const std::string text = load_file(path);
        const auto loading_end = Clock::now();
        const double loading_ms = std::chrono::duration<double, std::milli>(loading_end - loading_start).count();

        std::cout << "Fisier: " << path << '\n';
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Dimensiune: " << (static_cast<double>(text.size()) / (1024 * 1024)) << " MiB\n";
        std::cout << "Citire in RAM: " << loading_ms << " ms\n";

        double baseline_ms = 0.0;
        Result reference;
        std::cout << "\nFire    Timp (ms)       Accelerare    Verificare\n";
        std::cout << "-------------------------------------------------\n";

        for (const int threads : {1, 2, 4, 8}) {
            const auto start = Clock::now();
            Result current = count_words(text, threads);
            const auto stop = Clock::now();
            const double time_ms = std::chrono::duration<double, std::milli>(stop - start).count();

            if (threads == 1) {
                baseline_ms = time_ms;
                reference = std::move(current);
            }
            else if (current.word_count != reference.word_count ||
                current.frequencies != reference.frequencies) {
                throw std::runtime_error("Rezultate diferite intre fire - verificarea a esuat.");
            }

            std::cout << std::left << std::setw(8) << threads
                      << std::setw(16) << time_ms
                      << std::setw(14) << ((time_ms > 0) ? baseline_ms / time_ms : 0.0)
                      << "OK\n";
        }

        std::cout << "\nTotal cuvinte: " << reference.word_count << '\n';
        std::cout << "Cuvinte distincte: " << reference.frequencies.size() << '\n';
        print_top_10(reference);
        return 0;
    }
    catch (const std::bad_alloc&) {
        std::cerr << "Memorie insuficienta\n";
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "Eroare: " << e.what() << '\n';
        return 1;
    }
}