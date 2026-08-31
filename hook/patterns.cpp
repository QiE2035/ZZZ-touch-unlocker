// patterns.cpp — runtime section scanning and x86-64 relocation helpers.
#include "patterns.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

namespace zzz::patterns {
namespace {

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

const IMAGE_NT_HEADERS64* NtHeaders(HMODULE module) {
    if (!module) return nullptr;
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    return nt;
}

struct ScanPattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
    size_t firstConcrete = 0;
    bool valid = false;
};

ScanPattern CompileScanPattern(const char* pattern_hex) {
    ScanPattern out;
    if (!pattern_hex) return out;

    size_t i = 0;
    while (pattern_hex[i]) {
        while (pattern_hex[i] &&
               (std::isspace(static_cast<unsigned char>(pattern_hex[i])) ||
                pattern_hex[i] == ',')) ++i;
        if (!pattern_hex[i]) break;

        if (pattern_hex[i] == '?') {
            ++i;
            if (pattern_hex[i] == '?') ++i;
            out.bytes.push_back(0);
            out.mask.push_back(0);
            continue;
        }

        const int hi = HexNibble(pattern_hex[i++]);
        if (hi < 0 || !pattern_hex[i]) return {};
        const int lo = HexNibble(pattern_hex[i++]);
        if (lo < 0) return {};
        out.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        out.mask.push_back(1);
    }

    if (out.bytes.empty()) return {};
    for (size_t j = 0; j < out.mask.size(); ++j) {
        if (out.mask[j]) {
            out.firstConcrete = j;
            out.valid = true;
            break;
        }
    }
    return out;
}

}  // namespace

bool GetSectionView(HMODULE module, const char* section_name, SectionView& out) {
    out = {};
    if (!module || !section_name) return false;
    const auto* nt = NtHeaders(module);
    if (!nt) return false;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1]{};
        std::memcpy(name, section[i].Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::strncmp(name, section_name, IMAGE_SIZEOF_SHORT_NAME) != 0)
            continue;

        const size_t size = section[i].Misc.VirtualSize
            ? static_cast<size_t>(section[i].Misc.VirtualSize)
            : static_cast<size_t>(section[i].SizeOfRawData);
        if (!size) return false;
        out.begin = base + section[i].VirtualAddress;
        out.size = size;
        return true;
    }
    return false;
}

uintptr_t FindUniquePattern(HMODULE module,
                            const char* section_name,
                            const char* pattern_hex,
                            size_t* match_count) {
    if (match_count) *match_count = 0;

    SectionView section{};
    if (!GetSectionView(module, section_name, section)) return 0;
    const ScanPattern pattern = CompileScanPattern(pattern_hex);
    if (!pattern.valid || pattern.bytes.size() > section.size) return 0;

    const auto* begin = reinterpret_cast<const uint8_t*>(section.begin);
    const size_t length = pattern.bytes.size();
    const size_t first = pattern.firstConcrete;
    const uint8_t needle = pattern.bytes[first];
    const uint8_t* cursor = begin + first;
    const uint8_t* limit = begin + section.size - (length - first) + 1;

    uintptr_t only = 0;
    size_t count = 0;
    while (cursor < limit) {
        const void* found = std::memchr(
            cursor, needle, static_cast<size_t>(limit - cursor));
        if (!found) break;

        const auto* hitByte = reinterpret_cast<const uint8_t*>(found);
        const auto* candidate = hitByte - first;
        bool ok = true;
        for (size_t j = 0; j < length; ++j) {
            if (pattern.mask[j] && candidate[j] != pattern.bytes[j]) {
                ok = false;
                break;
            }
        }

        if (ok) {
            ++count;
            if (count == 1) only = reinterpret_cast<uintptr_t>(candidate);
            if (count >= 2) {
                if (match_count) *match_count = count;
                return 0;
            }
        }
        cursor = hitByte + 1;
    }

    if (match_count) *match_count = count;
    return count == 1 ? only : 0;
}

uintptr_t DecodeRel32Target(uintptr_t instruction,
                            size_t displacement_offset,
                            size_t instruction_length) {
    if (!instruction) return 0;
    const int32_t displacement = *reinterpret_cast<const int32_t*>(
        instruction + displacement_offset);
    return instruction + instruction_length + static_cast<intptr_t>(displacement);
}

uintptr_t DecodeRipRelativeAddress(uintptr_t instruction,
                                   size_t displacement_offset,
                                   size_t instruction_length) {
    return DecodeRel32Target(instruction, displacement_offset, instruction_length);
}

bool AddressInModule(HMODULE module, uintptr_t address) {
    const auto* nt = NtHeaders(module);
    if (!nt || !address) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    return address >= base && address < base + nt->OptionalHeader.SizeOfImage;
}

bool AddressInSection(HMODULE module, const char* section_name, uintptr_t address) {
    SectionView section{};
    if (!GetSectionView(module, section_name, section)) return false;
    return address >= section.begin && address < section.begin + section.size;
}

}  // namespace zzz::patterns
