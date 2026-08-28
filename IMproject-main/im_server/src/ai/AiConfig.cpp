#include "ai/AiConfig.h"

#include <algorithm>
#include <cstdlib>

namespace imsrv::ai {
namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

int envInt(const char* name, int fallback, int minimum, int maximum) {
    const std::string value = env(name);
    if (value.empty()) return fallback;
    try {
        const long parsed = std::stol(value);
        if (parsed < minimum || parsed > maximum) return fallback;
        return static_cast<int>(parsed);
    } catch (...) {
        return fallback;
    }
}

bool envBool(const char* name, bool fallback) {
    const std::string value = env(name);
    if (value.empty()) return fallback;
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
}

void applyBaseUrl(const std::string& baseUrl, AiConfig& config) {
    if (baseUrl.empty()) return;
    std::string value = baseUrl;
    if (value.rfind("https://", 0) == 0) {
        value.erase(0, 8);
        config.port = 443;
    } else if (value.rfind("http://", 0) == 0) {
        value.erase(0, 7);
        config.port = 80;
    }
    const auto slash = value.find('/');
    config.host = value.substr(0, slash);
    if (slash != std::string::npos && slash + 1 < value.size()) {
        config.path = value.substr(slash);
        while (!config.path.empty() && config.path.back() == '/') config.path.pop_back();
        config.path += "/v1/messages";
    }
}

} // namespace

AiConfig AiConfig::fromEnvironment() {
    AiConfig config;
    config.featureEnabled = envBool("AI_ENABLED", true);
    config.rolloutPercent = envInt("AI_ROLLOUT_PERCENT", 100, 0, 100);
    const auto anthropicBaseUrl = env("ANTHROPIC_BASE_URL");
    const auto anthropicToken = env("ANTHROPIC_AUTH_TOKEN");
    const auto anthropicKey = env("ANTHROPIC_API_KEY");
    const auto provider = env("AI_PROVIDER");
    const bool useAnthropic = provider == "anthropic" || !anthropicBaseUrl.empty() ||
                              !anthropicToken.empty() || !anthropicKey.empty();
    if (useAnthropic) {
        config.provider = AiProvider::Anthropic;
        config.host = "api.anthropic.com";
        config.path = "/v1/messages";
        config.apiKey = !anthropicToken.empty() ? anthropicToken : anthropicKey;
        config.model = env("ANTHROPIC_MODEL");
        applyBaseUrl(anthropicBaseUrl, config);
        const auto version = env("ANTHROPIC_VERSION");
        if (!version.empty()) config.anthropicVersion = version;
    } else {
        config.apiKey = env("AI_API_KEY");
        if (config.apiKey.empty()) config.apiKey = env("OPENAI_API_KEY");
        config.model = env("AI_MODEL");
    }

    const auto host = env("AI_API_HOST");
    const auto path = env("AI_API_PATH");
    const auto caCertPath = env("AI_CA_CERT_PATH");
    if (!host.empty()) applyBaseUrl(host, config);
    if (!path.empty()) config.path = path;
    if (!caCertPath.empty()) config.caCertPath = caCertPath;

    config.port = envInt("AI_API_PORT", 443, 1, 65535);
    config.requestTimeoutMs = envInt("AI_REQUEST_TIMEOUT_MS", 15000, 1000, 120000);
    config.maxOutputTokens = envInt("AI_MAX_OUTPUT_TOKENS", 180, 64, 2000);
    config.disableThinking = envBool("AI_DISABLE_THINKING", true);
    config.maxContextMessages = envInt("AI_MAX_CONTEXT_MESSAGES", 20, 1, 100);
    config.maxContextBytes = envInt("AI_MAX_CONTEXT_BYTES", 12000, 1000, 100000);
    config.maxMessageBytes = envInt("AI_MAX_MESSAGE_BYTES", 2000, 100, 10000);
    config.rateLimitPerMinute = envInt("AI_RATE_LIMIT_PER_MINUTE", 5, 1, 60);
    config.maxPendingRequests = envInt("AI_MAX_PENDING_REQUESTS", 8, 1, 100);
    config.replayWindowSeconds = envInt("AI_REPLAY_WINDOW_SECONDS", 300, 30, 3600);
    return config;
}

bool AiConfig::enabled() const noexcept {
    return featureEnabled && !apiKey.empty() && !model.empty();
}

std::string AiConfig::validationError() const {
    if (!featureEnabled) return "AI 功能已关闭";
    if (apiKey.empty()) return provider == AiProvider::Anthropic
        ? "缺少 ANTHROPIC_AUTH_TOKEN（也兼容 ANTHROPIC_API_KEY）"
        : "缺少 AI_API_KEY（也兼容 OPENAI_API_KEY）";
    if (model.empty()) return provider == AiProvider::Anthropic
        ? "缺少 ANTHROPIC_MODEL" : "缺少 AI_MODEL";
    if (host.empty()) return "AI_API_HOST 不能为空";
    if (path.empty() || path.front() != '/') return "AI_API_PATH 必须以 / 开头";
    if (port < 1 || port > 65535) return "AI_API_PORT 无效";
    return {};
}

} // namespace imsrv::ai
