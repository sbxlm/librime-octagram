#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"
#include <rime/config.h>
#include <rime/resource.h>
#include <rime/service.h>
#include <utf8.h>

namespace rime {

struct GrammarConfig {
  int collocation_max_length = 4;
  int collocation_min_length = 3;
  double collocation_penalty = -12;
  double non_collocation_penalty = -12;
  double weak_collocation_penalty = -24;
  double rear_penalty = -18;
};

const ResourceType kGramDbType = {"gram_db", "", ".gram"};
const string kGrammarDefaultLanguage = "zh-hant";

Octagram::Octagram(Config* config) {
  string language;
  config_.reset(new GrammarConfig);
  if (config) {
    if (config->GetString("grammar/language", &language)) {
      LOG(INFO) << "use grammar: " << language;
    } else {
      return;
    }
    config->GetInt("grammar/collocation_max_length",
                   &config_->collocation_max_length);
    config->GetInt("grammar/collocation_min_length",
                   &config_->collocation_min_length);
    config->GetDouble("grammar/collocation_penalty",
                      &config_->collocation_penalty);
    config->GetDouble("grammar/non_collocation_penalty",
                      &config_->non_collocation_penalty);
    config->GetDouble("grammar/weak_collocation_penalty",
                      &config_->weak_collocation_penalty);
    config->GetDouble("grammar/rear_penalty",
                      &config_->rear_penalty);
  }
  the<ResourceResolver> resolver(
      Service::instance().CreateResourceResolver(kGramDbType));
  the<GramDb> db(new GramDb(resolver->ResolvePath(language).string()));
  if (!db->Load()) {
    LOG(ERROR) << "failed to load grammar database: " << language;
    return;
  }
  db_ = std::move(db);
}

Octagram::~Octagram() {}

inline static double scale_value(int value) {
  return value >= 0 ? double(value) / GramDb::kValueScale : -1;
}

inline static bool update_result(double& result, double new_value) {
  if (new_value > result) {
    result = new_value;
    return true;
  }
  return false;
}

inline static const char* str_begin(const string& str) {
  return str.c_str();
}

inline static const char* str_end(const string& str) {
  return str.c_str() + str.length();
}

inline static const char* last_n_unicode(const string& str,
                                         int max,
                                         int& out_count) {
  const char* begin = str_begin(str);
  const char* p = str_end(str);
  out_count = 0;
  while (p != begin && out_count < max) {
    utf8::unchecked::prior(p);
    ++out_count;
  }
  return p;
}

inline static bool matches_whole_query(const char* context_ptr,
                                       const string& context_query,
                                       size_t match_length,
                                       const string& word_query) {
  return context_ptr == str_begin(context_query) &&
      match_length == word_query.length();
}

double Octagram::Query(const string& context,
                       const string& word,
                       bool is_rear) {
  if (!db_ || context.empty()) {
    return config_->non_collocation_penalty;
  }
  double result = config_->non_collocation_penalty;
  GramDb::Match matches[GramDb::kMaxResults];
  int context_len = 0;
  string context_query = grammar::encode(
      last_n_unicode(context,
                     config_->collocation_max_length - 1,
                     context_len));
  string word_query = grammar::encode(word);
  for (const char* context_ptr = str_begin(context_query);
       context_len > 0;
       --context_len, context_ptr = grammar::next_unicode(context_ptr)) {
    int num_results = db_->Lookup(context_ptr, word_query, matches);
    DLOG(INFO) << "Lookup(" << context_ptr << " + " << word_query << ") returns "
               << num_results << " results";
    for (auto i = 0; i < num_results; ++i) {
      const auto& match(matches[i]);
      const int match_len = grammar::unicode_length(word_query, match.length);
      DLOG(INFO) << "match[" << match.length << "] = "
                 << scale_value(match.value);
      const int collocation_len = context_len + match_len;
      if (update_result(result,
                        scale_value(match.value) +
                        (collocation_len >= config_->collocation_min_length ||
                         matches_whole_query(context_ptr, context_query,
                                             match.length, word_query)
                         ? config_->collocation_penalty
                         : config_->weak_collocation_penalty))) {
        DLOG(INFO) << "update: " << context << "[" << context_len << "] + "
                   << word << "[" << match_len << "] = " << result;
      }
    }
  }
  if (is_rear) {
    if (db_->Lookup(word_query, "$", matches) > 0 &&
        update_result(result,
                      scale_value(matches[0].value) + config_->rear_penalty)) {
      DLOG(INFO) << "update: " << word << "$ / " << result;
    }
  }
  DLOG(INFO) << "context = " << context << ", word = " << word
             << " / " << result;
  return result;
}

}  // namespace rime
