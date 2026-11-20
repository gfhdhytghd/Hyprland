#include "ShaderLoader.hpp"
#include <format>
#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprutils/string/String.hpp>
#include <hyprutils/path/Path.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include "../debug/log/Logger.hpp"
#include "shaders/Shaders.hpp"
#include "../helpers/fs/FsUtils.hpp"
#include "Renderer.hpp"
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <filesystem>

using namespace Render;

const std::vector<std::string> Render::SHADER_INCLUDES = {
    "defines.h",       "constants.h",      "cm_helpers.glsl", "rounding.glsl", "CM.glsl",         "tonemap.glsl",     "gain.glsl",     "border.glsl",      "shadow.glsl",
    "inner_glow.glsl", "blurprepare.glsl", "blur1.glsl",      "blur2.glsl",    "blurFinish.glsl", "motion_blur.glsl", "gradient.glsl", "glassFinish.glsl", "fluidJar.glsl",
};

// order matters, see ePreparedFragmentShader
const std::array<std::string, SH_FRAG_LAST> Render::FRAG_SHADERS = {
    "quad.frag",
    "passthru.frag",
    "rgbamatte.frag",
    "blur1.frag",
    "blur2.frag",
    "blurprepare.frag",
    "blurfinish.frag",
    "shadow.frag",
    "inner_glow.frag",
    "surface.frag",
    "border.frag",
    "glitch.frag",
    "frostfinish.frag",
    "ripplefinish.frag",
    "dropsfinish.frag",
    "waterstep.frag",
    "waterfinish.frag",
    "fluidjarinit.frag",
    "fluidjarstep.frag",
    "fluidjargraph.frag",
    "fluidjartrack.frag",
    "fluidjarvisual.frag",
    "fluidjarresample.frag",
    "fluidjarhistoryresample.frag",
    "fluidjartrackingresample.frag",
    "fluidjarfinish.frag",
    "prismfinish.frag",
    "heatshimmerfinish.frag",
    "acrylicfinish.frag",
    "aurorafinish.frag",
    "hazefinish.frag",
};

static std::string stripShaderComments(std::string_view source) {
    std::string result;
    result.reserve(source.size());

    while (!source.empty()) {
        const auto slash = source.find('/');
        result += source.substr(0, slash);
        if (slash == std::string_view::npos)
            break;

        source.remove_prefix(slash);
        if (source.starts_with("//")) {
            source.remove_prefix(std::min(source.find('\n'), source.size()));
        } else if (source.starts_with("/*")) {
            const auto end = source.find("*/", 2);
            if (end == std::string_view::npos) {
                result += source; // Leave malformed comments for the driver to diagnose.
                break;
            }

            // A block comment is whitespace, including inside a preprocessor directive.
            result += ' ';
            source.remove_prefix(end + 2);
        } else {
            result += '/';
            source.remove_prefix(1);
        }
    }

    return result;
}

static std::expected<std::string, std::string> convertToGLES2(const std::string& source, bool fragment, bool deferOutputErrors) {
    static const std::regex OUTPUT_RE(R"(^\s*(?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)?out\s+(\w+)\s+(\w+)\s*;\s*$)");
    static const std::regex PRECISION_RE(R"(^precision\s+highp\s+float\s*;$)");
    static const std::regex IN_RE(R"(^(\s*)in(\s))");
    static const std::regex OUT_RE(R"(^(\s*)out(\s))");
    static const std::regex TEXTURE_RE(R"(\btexture\s*\()");

    std::stringstream       stream(stripShaderComments(source));
    std::string             line;
    std::string             result;

    while (std::getline(stream, line)) {
        const auto TRIMMED = Hyprutils::String::trim(std::string_view{line});

        if (TRIMMED.starts_with("#version")) {
            result += "#version 100\n";
            continue;
        }

        if (TRIMMED.starts_with("#extension GL_ARB_shading_language_include"))
            continue;

        if (TRIMMED.starts_with("#extension GL_OES_EGL_image_external_essl3")) {
            result += "#extension GL_OES_EGL_image_external : require\n";
            continue;
        }

        if (std::smatch match; fragment && (TRIMMED.starts_with("layout") || TRIMMED.starts_with("out")) && std::regex_match(line, match, OUTPUT_RE)) {
            if ((match[1].matched && match[1].str() != "0") || match[2].str() != "vec4") {
                const auto error = std::format("fragment output {} has no GLES2 equivalent", match.str());
                if (!deferOutputErrors)
                    return std::unexpected(error);

                // The driver still has to evaluate feature guards around declarations such as the mirror output.
                result += std::format("#error {}\n", error);
                continue;
            }

            result += std::format("#define {} gl_FragColor\n", match[3].str());
            continue;
        }

        // GLES2 hardware may lack highp in fragment shaders
        if (fragment && TRIMMED.starts_with("precision") && std::regex_match(TRIMMED.begin(), TRIMMED.end(), PRECISION_RE)) {
            result += "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n";
            continue;
        }

        if (TRIMMED.starts_with("in"))
            line = std::regex_replace(line, IN_RE, fragment ? "$1varying$2" : "$1attribute$2");
        else if (!fragment && TRIMMED.starts_with("out"))
            line = std::regex_replace(line, OUT_RE, "$1varying$2");
        if (line.contains("texture"))
            line = std::regex_replace(line, TEXTURE_RE, "texture2D(");

        result += line + "\n";
    }

    return result;
}

std::expected<std::string, std::string> Render::toGLES2(const std::string& source, bool fragment) {
    return convertToGLES2(source, fragment, false);
}

// guards against include cycles, which can't be detected without evaluating #ifndef guards
static constexpr size_t MAX_INCLUDE_DEPTH = 16;

// returns the file of an #include "file" directive
static std::optional<std::string> includedFile(std::string_view line) {
    line = Hyprutils::String::trim(line);
    if (!line.starts_with("#include"))
        return std::nullopt;

    const auto BEGIN = line.find('"');
    if (BEGIN == std::string_view::npos)
        return std::nullopt;

    const auto END = line.find('"', BEGIN + 1);
    if (END == std::string_view::npos)
        return std::nullopt;

    return std::string{line.substr(BEGIN + 1, END - BEGIN - 1)};
}

CShaderLoader::CShaderLoader(const std::vector<std::string> includes, const std::array<std::string, SH_FRAG_LAST>& frags, const std::string shaderPath, bool gles2) :
    m_shaderPath(shaderPath), m_gles2(gles2) {
    for (const auto& inc : includes) {
        include(inc);
    }

    std::ranges::transform(frags, m_fragFiles.begin(), [&](const auto& filename) { return loadShader(filename); });
}

void CShaderLoader::include(const std::string& filename) {
    m_includes.insert({filename, loadShader(filename)});
}

std::string CShaderLoader::getDefines(const SShaderVariant& variant) {
    static constexpr auto defines = std::to_array<std::pair<std::string_view, ePreparedFragmentShaderFeature>>({
        {"USE_RGBA", SH_FEAT_RGBA},
        {"USE_DISCARD", SH_FEAT_DISCARD},
        {"USE_TINT", SH_FEAT_TINT},
        {"USE_ROUNDING", SH_FEAT_ROUNDING},
        {"USE_CM", SH_FEAT_CM},
        {"USE_TONEMAP", SH_FEAT_TONEMAP},
        {"USE_SDR_MOD", SH_FEAT_SDR_MOD},
        {"USE_BLUR", SH_FEAT_BLUR},
        {"USE_ICC", SH_FEAT_ICC},
        {"USE_MIRROR", SH_FEAT_MIRROR},
        {"USE_MOTION_BLUR", SH_FEAT_MOTION_BLUR},
        {"USE_BLUR_ALPHA_MASK", SH_FEAT_BLUR_ALPHA_MASK},
        {"USE_BLUR_MATTE", SH_FEAT_BLUR_MATTE},
        {"USE_ALT_TONEMAP", SH_FEAT_ALT_TONEMAP},
        {"USE_BLUR_PROPORTIONAL", SH_FEAT_BLUR_PROPORTIONAL},
        {"USE_EXTERNAL", SH_FEAT_EXTERNAL},
    });

    std::string           res;
    res.reserve(351);
    for (const auto& [name, flag] : defines) {
        std::format_to(std::back_inserter(res), "#define {} {}\n", name, (variant.features & flag) != 0 ? '1' : '0');
    }

    // eTransferFunction values, the shaders compare them against the CM_TRANSFER_FUNCTION_* in CM.glsl
    std::format_to(std::back_inserter(res), "#define SOURCE_TF {}\n", sc<int>(variant.sourceTF));
    std::format_to(std::back_inserter(res), "#define TARGET_TF {}\n", sc<int>(variant.targetTF));
    return res;
}

// expands includes in place, everything else (#if etc.) is left to the driver's preprocessor
std::string CShaderLoader::processSource(const std::string& source, size_t depth) {
    std::stringstream stream(source);
    std::string       code;
    std::string       line;

    while (std::getline(stream, line)) {
        const auto INCLUDE = includedFile(line);

        if (!INCLUDE) {
            std::format_to(std::back_inserter(code), "{}\n", line);
            continue;
        }

        if (*INCLUDE == "defines.h" && !m_overrideDefines.empty()) {
            code += m_overrideDefines;
            continue;
        }

        if (depth >= MAX_INCLUDE_DEPTH || !m_includes.contains(*INCLUDE)) {
            // keep the directive, the driver will fail the compile on it
            LOG(Log::ERR, "Shader include \"{}\" is missing or nested too deep", *INCLUDE);
            std::format_to(std::back_inserter(code), "{}\n", line);
            continue;
        }

        code += processSource(m_includes.at(*INCLUDE), depth + 1);
    }

    return code;
}

std::string CShaderLoader::process(const std::string& filename) {
    auto source = processSource(loadShader(filename));
    if (!m_gles2)
        return source;

    auto converted = convertToGLES2(source, !filename.ends_with(".vert"), true);
    if (!converted)
        throw std::runtime_error(std::format("{}: {}", filename, converted.error()));
    return std::move(*converted);
}

std::string CShaderLoader::process(const std::string& filename, const std::map<std::string, std::string>& defines) {
    m_overrideDefines.clear();
    const Hyprutils::Utils::CScopeGuard resetDefines([this] { m_overrideDefines.clear(); });
    for (const auto& [name, value] : defines) {
        m_overrideDefines += std::format("#define {} {}\n", name, value);
    }
    return process(filename);
}

std::string CShaderLoader::getVariantSource(ePreparedFragmentShader frag, SShaderVariant variant) {
    static const auto PCM = CConfigValue<Config::INTEGER>("render:cm_enabled");
    if (m_gles2 || !*PCM)
        variant.features &= ~(SH_FEAT_CM | SH_FEAT_TONEMAP | SH_FEAT_ALT_TONEMAP | SH_FEAT_SDR_MOD);

    if (m_gles2)
        variant.features &= ~(SH_FEAT_ICC | SH_FEAT_MIRROR | SH_FEAT_MOTION_BLUR);

    // without CM the transfer functions are unused, keep them at the default so identical
    // shaders end up with identical source
    if (!(variant.features & SH_FEAT_CM)) {
        variant.sourceTF = SHADER_DEFAULT_TF;
        variant.targetTF = SHADER_DEFAULT_TF;
    }

    ASSERT(m_fragFiles[frag].length());

    m_overrideDefines = getDefines(variant);
    const Hyprutils::Utils::CScopeGuard resetDefines([this] { m_overrideDefines.clear(); });
    if (m_gles2)
        m_overrideDefines += "#define LEGACY_GLES2 1\n";

    auto source = processSource(m_fragFiles[frag]);
    if (!m_gles2)
        return source;

    auto converted = convertToGLES2(source, true, true);
    if (!converted) {
        LOG(Log::ERR, "Shader {} can't run on GLES2: {}", FRAG_SHADERS[frag], converted.error());
        return {};
    }

    return std::move(*converted);
}

const std::map<std::string, std::string>& CShaderLoader::includes() {
    return m_includes;
}

// TODO notify user if bundled shader is newer than ~/.config override
std::string CShaderLoader::loadShader(const std::string& filename) {
    if (m_shaderPath.length()) {
        std::filesystem::path path = m_shaderPath;
        const auto            src  = NFsUtils::readFileAsString(path / filename);
        if (src.has_value())
            return src.value();
    }
    const auto home = Hyprutils::Path::getHome();
    if (home.has_value()) {
        const auto src = NFsUtils::readFileAsString(std::format("{}/hypr/shaders/{}", home.value(), filename));
        if (src.has_value())
            return src.value();
    }
    for (auto& e : ASSET_PATHS) {
        const auto src = NFsUtils::readFileAsString(std::format("{}/hypr/shaders/{}", e, filename));
        if (src.has_value())
            return src.value();
    }

    const auto shader = std::ranges::lower_bound(SHADERS, filename, {}, [](const auto& filenameSource) { return filenameSource.first; });
    if (shader != SHADERS.end() && shader->first == filename)
        return std::string{shader->second};
    throw std::runtime_error(std::format("Couldn't load shader {}", filename));
}
