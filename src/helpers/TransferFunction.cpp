#include "TransferFunction.hpp"
#include "../config/ConfigValue.hpp"
#include "../config/lua/types/LuaConfigString.hpp"
#include <string>
#include <unordered_map>

using namespace NTransferFunction;

static std::unordered_map<std::string, eTF> const table = {{"default", TF_DEFAULT}, {"0", TF_DEFAULT},       {"auto", TF_AUTO}, {"srgb", TF_SRGB},
                                                           {"3", TF_SRGB},          {"gamma22", TF_GAMMA22}, {"1", TF_GAMMA22}, {"gamma22force", TF_FORCED_GAMMA22},
                                                           {"2", TF_FORCED_GAMMA22}};

std::optional<eTF>                                NTransferFunction::parse(const std::string& tfName) {
    auto it = table.find(tfName);
    if (it == table.end())
        return std::nullopt;
    return it->second;
}

eTF NTransferFunction::fromString(const std::string tfName) {
    return parse(tfName).value_or(TF_DEFAULT);
}

std::string NTransferFunction::toString(eTF tf) {
    for (const auto& [key, value] : table) {
        if (value == tf)
            return key;
    }
    return "";
}

eTF NTransferFunction::fromConfig(bool useICC) {
    if (useICC)
        return TF_SRGB;

    static auto PSDREOTF         = CConfigValue<Config::STRING>("render:cm_sdr_eotf");
    static auto parsedGeneration = Config::Lua::CLuaConfigString::generation();
    static auto sdrEOTF          = fromString(*PSDREOTF);
    const auto  generation       = Config::Lua::CLuaConfigString::generation();
    if (generation != parsedGeneration) {
        sdrEOTF          = fromString(*PSDREOTF);
        parsedGeneration = generation;
    }

    return sdrEOTF;
}
