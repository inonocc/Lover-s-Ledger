#include "SkyrimNetDecorators.h"

#include "LoversLedgerService.h"
#include "PCH.h"
#include "UniqueOverrides.h"

#include <functional>
#include <sstream>
#include <string>

// Forward-declare only the pointer we use from SkyrimNetPublicAPI.h.
// Must use extern "C" to match the C linkage given in the header.
extern "C" {
    extern bool (*PublicRegisterDecorator)(const char* name, const char* description,
                                           std::function<std::string(RE::Actor*)> callback);
}

namespace {

    // Returns the NPC base FormID if the actor is non-null and unique; 0 otherwise.
    static std::uint32_t GetUniqueBaseID(RE::Actor* actor) {
        auto* base = LL::GetStableBase(actor);
        if (!base || !LL::IsEffectivelyUnique(base)) return 0;
        return base->GetFormID();
    }

    // Get the display name for a base-form FormID.
    static std::string GetNameForFormID(std::uint32_t formID) {
        auto* form = RE::TESForm::LookupByID(formID);
        if (!form) return "Unknown";
        auto* npc = form->As<RE::TESNPC>();
        if (npc) {
            const char* name = npc->GetName();
            if (name && name[0]) return name;
        }
        return "Unknown";
    }

    // Derive a human-readable bond level from the lover score.
    // Thresholds chosen to align with the template's "weak" / stronger classifications.
    // NOTE: returns stable English keys because callers compare against them ("weak").
    //       Display text is produced separately by BondText() so logic never depends on translation.
    static std::string BondFromScore(float score) {
        if (score < 5.0f)  return "weak";
        if (score < 15.0f) return "casual";
        if (score < 30.0f) return "regular";
        if (score < 60.0f) return "close";
        return "intimate";
    }

    // 中文显示文案：把内部等级键映射成中文，比较逻辑仍使用上面的英文键。
    static std::string BondText(const std::string& bond) {
        if (bond == "weak")     return "淡薄";
        if (bond == "casual")   return "泛泛";
        if (bond == "regular")  return "稳定";
        if (bond == "close")    return "亲密";
        return "至深";
    }

    // Convert a game-time difference (in days) to a natural-language recency string.
    static std::string RecencyFromDiff(float diffDays) {
        if (diffDays < 0.0f) diffDays = 0.0f;

        if (diffDays < 1.0f) return "今天";

        int days = static_cast<int>(diffDays);
        if (days < 7) {
            return std::to_string(days) + " 天前";
        }

        int weeks = days / 7;
        if (weeks < 5) {
            return std::to_string(weeks) + " 周前";
        }

        int months = days / 30;
        if (months < 13) {
            return std::to_string(months) + " 个月前";
        }

        return "一年多以前";
    }

    // Decorator: ttll_get_npc_sexual_behavior
    // Returns a short prose description of an NPC's sexual behavior patterns,
    // mirroring the logic in 0301_personality_lovers_ledger_insights.prompt.
    static std::string GetNpcSexualBehaviorDecorator(RE::Actor* actor) {
        std::uint32_t baseID = GetUniqueBaseID(actor);
        if (baseID == 0) return "";

        auto& service = LL::LoversLedgerService::GetSingleton();

        int soloSex           = service.GetNpcInt(baseID, "solosex");
        int exclusiveSex      = service.GetNpcInt(baseID, "exclusivesex");
        int groupSex          = service.GetNpcInt(baseID, "groupsex");
        int sameSexEncounter  = service.GetNpcInt(baseID, "samesexencounter");
        int totalEncounters   = soloSex + exclusiveSex + groupSex;

        if (totalEncounters == 0) return "";

        float sameSexRatio  = static_cast<float>(sameSexEncounter) / totalEncounters;
        float soloRatio     = static_cast<float>(soloSex)          / totalEncounters;
        float exclusiveRatio = static_cast<float>(exclusiveSex)    / totalEncounters;
        float groupRatio    = static_cast<float>(groupSex)         / totalEncounters;

        std::string orientation;
        if (sameSexRatio > 0.8f)
            orientation = "以同性为主";
        else if (sameSexRatio > 0.4f)
            orientation = "双性恋";
        else if (sameSexRatio > 0.1f)
            orientation = "以异性为主，偶有同性经历";
        else
            orientation = "仅限异性";

        std::string preference;
        if (soloRatio > 0.5f) preference += "经常自慰";
        if (exclusiveRatio > 0.7f)
            preference += (preference.empty() ? "" : "，") + std::string("偏好单对单");
        else if (groupRatio > 0.5f)
            preference += (preference.empty() ? "" : "，") + std::string("乐于多人");
        else
            preference += (preference.empty() ? "" : "，") + std::string("私下与多人皆有");

        std::ostringstream out;
        out << "### 角色性行为特征\n"
            << "**性取向：**" << orientation << "\n"
            << "**性偏好：**" << preference << "\n";
        return out.str();
    }

    // Decorator: ttll_get_npc_lovers
    // Returns a Markdown-formatted section listing all non-weak sexual partners of the NPC,
    // mirroring the logic in 0601_relations_lovers_ledger_insights.prompt.
    static std::string GetNpcLoversDecorator(RE::Actor* actor) {
        std::uint32_t baseID = GetUniqueBaseID(actor);
        if (baseID == 0) return "";

        auto& service = LL::LoversLedgerService::GetSingleton();

        // Retrieve all lover FormIDs sorted by score (highest first)
        std::vector<std::uint32_t> loverIDs = service.GetAllLovers(baseID, -1);
        int othersCount = service.GetNpcInt(baseID, "otherscount");
        std::vector<LL::PastLover> pastLovers = service.GetPastLovers(baseID);

        if (loverIDs.empty() && othersCount == 0 && pastLovers.empty()) return "";

        // Get current game time for recency computation
        float currentTime = 0.0f;
        if (auto* calendar = RE::Calendar::GetSingleton()) {
            currentTime = calendar->GetDaysPassed();
        }

        std::ostringstream out;
        bool headerWritten = false;
        int weakCount = 0;

        for (std::uint32_t loverID : loverIDs) {
            float score      = service.GetLoverScore(baseID, loverID);
            std::string bond = BondFromScore(score);

            if (bond == "weak") {
                // Tally weak-bond unique lovers into the insignificant count
                weakCount += 1;
                continue;
            }

            int exclusiveSex = service.GetLoverInt(baseID, loverID, "exclusivesex");
            int groupSex     = service.GetLoverInt(baseID, loverID, "partofsamegroupsex");
            int total        = exclusiveSex + groupSex;
            float lastTime   = service.GetLoverFloat(baseID, loverID, "lasttime");

            std::string recency = (lastTime > 0.0f) ? RecencyFromDiff(currentTime - lastTime) : "unknown";
            std::string name    = GetNameForFormID(loverID);

            if (!headerWritten) {
                out << "### 性伴侣\n";
                headerWritten = true;
            }

            out << "- **" << name << "**（" << BondText(bond) << "）："
                << total << " 次遭遇"
                << "（单独 " << exclusiveSex << " 次，多人 " << groupSex << " 次）"
                << "，最近一次：" << recency << "\n";
        }

        // Backfilled SexLab history with no known partner
        for (const auto& past : pastLovers) {
            auto* race = RE::TESForm::LookupByID<RE::TESRace>(past.raceFormID);
            const char* raceName = (race && race->GetName() && *race->GetName()) ? race->GetName() : "unknown";
            if (!headerWritten) {
                out << "### 性伴侣\n";
                headerWritten = true;
            }
            out << "- **一位不知名的" << raceName << (past.gender == 1 ? "女子" : "男子")
                << "（旧情人）**：" << past.encounters << " 次遭遇"
                << "，很久以前\n";
        }

        int insignificantTotal = othersCount + weakCount;
        if (insignificantTotal > 0) {
            if (!headerWritten) {
                out << "### 性伴侣\n";
            }
            out << "- **无关紧要的一夜情对象**（短暂邂逅，无感情分量也无持久联系）："
                << insignificantTotal << " 人\n";
        }

        return out.str();
    }

}  // namespace

void RegisterSkyrimNetDecorators() {
    if (!PublicRegisterDecorator) {
        SKSE::log::warn("RegisterSkyrimNetDecorators: PublicRegisterDecorator is null — SkyrimNet v5+ required");
        return;
    }

    bool ok = PublicRegisterDecorator(
        "ttll_get_npc_lovers",
        "Returns a formatted list of the NPC's sexual partners with bond strength and encounter stats.",
        GetNpcLoversDecorator);

    if (ok) {
        SKSE::log::info("RegisterSkyrimNetDecorators: 'ttll_get_npc_lovers' registered successfully");
    } else {
        SKSE::log::warn("RegisterSkyrimNetDecorators: failed to register 'ttll_get_npc_lovers' (name conflict?)");
    }

    ok = PublicRegisterDecorator(
        "ttll_get_npc_sexual_behavior",
        "Returns a prose summary of the NPC's sexual orientation and encounter preferences.",
        GetNpcSexualBehaviorDecorator);

    if (ok) {
        SKSE::log::info("RegisterSkyrimNetDecorators: 'ttll_get_npc_sexual_behavior' registered successfully");
    } else {
        SKSE::log::warn("RegisterSkyrimNetDecorators: failed to register 'ttll_get_npc_sexual_behavior' (name conflict?)");
    }
}
