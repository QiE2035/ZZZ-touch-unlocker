// patterns.h — runtime semantic-pattern relocation helpers.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace zzz::patterns {

struct SectionView {
    uintptr_t begin = 0;
    size_t size = 0;
};

bool GetSectionView(HMODULE module, const char* section_name, SectionView& out);

// Scan one mapped PE section. Returns an address only when exactly one match
// exists; otherwise returns 0. `match_count` receives 0, 1, or >=2.
uintptr_t FindUniquePattern(HMODULE module,
                            const char* section_name,
                            const char* pattern_hex,
                            size_t* match_count = nullptr);

// Decode x86-64 signed rel32 / RIP-relative operands.
uintptr_t DecodeRel32Target(uintptr_t instruction,
                            size_t displacement_offset,
                            size_t instruction_length);
uintptr_t DecodeRipRelativeAddress(uintptr_t instruction,
                                   size_t displacement_offset,
                                   size_t instruction_length);

bool AddressInModule(HMODULE module, uintptr_t address);
bool AddressInSection(HMODULE module, const char* section_name, uintptr_t address);

}  // namespace zzz::patterns
