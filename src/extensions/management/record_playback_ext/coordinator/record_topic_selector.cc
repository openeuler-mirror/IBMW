// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/record_topic_selector.h"

#include <algorithm>
#include <regex>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

RecordTopicSelector::RecordTopicSelector(Options options)
    : options_(std::move(options)) {
  MW_ASSERT(
      !options_.explicit_topics.empty() || options_.all ||
          !options_.regex.empty(),
      "Record selection requires explicit topics, --all, or --regex.");
  if (!options_.regex.empty()) {
    static_cast<void>(std::regex(options_.regex));
  }
  if (!options_.exclude_regex.empty()) {
    static_cast<void>(std::regex(options_.exclude_regex));
  }
}

std::set<std::string> RecordTopicSelector::Select(
    const TopicGraph& graph) const {
  std::set<std::string> result(
      options_.explicit_topics.begin(), options_.explicit_topics.end());
  const std::regex include_pattern(
      options_.regex.empty() ? "a^" : options_.regex);
  const std::regex exclude_pattern(
      options_.exclude_regex.empty() ? "a^" : options_.exclude_regex);
  const std::set<std::string> explicit_topics(
      options_.explicit_topics.begin(), options_.explicit_topics.end());

  for (const auto& topic : graph.Topics()) {
    const auto advertisements = graph.Find(topic);
    const bool published = std::any_of(
        advertisements.begin(), advertisements.end(),
        [](const auto& advertisement) { return advertisement.published; });
    const bool hidden = !advertisements.empty() && std::all_of(
        advertisements.begin(), advertisements.end(),
        [](const auto& advertisement) { return advertisement.hidden; });
    const bool selected_by_pattern =
        options_.all ||
        (!options_.regex.empty() && std::regex_search(topic, include_pattern));
    if (!selected_by_pattern) {
      continue;
    }
    if (hidden && !options_.include_hidden_topics) {
      continue;
    }
    if (!published &&
        (!options_.include_unpublished_topics || options_.ignore_leaf_topics)) {
      continue;
    }
    result.emplace(topic);
  }

  for (auto it = result.begin(); it != result.end();) {
    if (!options_.exclude_regex.empty() &&
        std::regex_search(*it, exclude_pattern)) {
      it = result.erase(it);
      continue;
    }
    if (options_.ignore_leaf_topics) {
      const auto advertisements = graph.Find(*it);
      const bool published = std::any_of(
          advertisements.begin(), advertisements.end(),
          [](const auto& advertisement) { return advertisement.published; });
      if (!published) {
        it = result.erase(it);
        continue;
      }
    }
    // Explicit topic names are authoritative even when hidden or not yet
    // advertised; discovery can attach them later unless leaf topics were
    // explicitly disabled.
    if (explicit_topics.contains(*it)) {
      ++it;
      continue;
    }
    ++it;
  }
  return result;
}

}  // namespace ibmw::extensions::record_playback_extension
