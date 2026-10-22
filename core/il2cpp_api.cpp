#include "il2cpp_api.h"

namespace sr2::il2cpp {

Api& api() {
    static Api a;
    return a;
}

bool Load(HMODULE ga, std::vector<std::string>& missingRequired, std::vector<std::string>& missingOptional) {
    Api& a = api();
    a.module = ga;
    a.moduleBase = reinterpret_cast<uintptr_t>(ga);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ga);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(a.moduleBase + static_cast<uintptr_t>(dos->e_lfanew));
    a.moduleSize = nt->OptionalHeader.SizeOfImage;

#define SR2_RESOLVE_FN(req, ret, name, params)                                          \
    a.name = reinterpret_cast<ret(*) params>(GetProcAddress(ga, #name));                \
    if (!a.name) {                                                                      \
        if constexpr (req) missingRequired.emplace_back(#name);                         \
        else missingOptional.emplace_back(#name);                                       \
    }
    SR2_IL2CPP_API(SR2_RESOLVE_FN)
#undef SR2_RESOLVE_FN

    return missingRequired.empty();
}

bool InGameAssembly(const void* p) {
    const Api& a = api();
    auto v = reinterpret_cast<uintptr_t>(p);
    return a.moduleBase && v >= a.moduleBase && v < a.moduleBase + a.moduleSize;
}

}  // namespace sr2::il2cpp
