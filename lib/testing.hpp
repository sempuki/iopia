// Copyright 2024 -- CONTRIBUTORS. See LICENSE.

#pragma once

#include <sstream>
#include <vector>

#include "catch2/catch_test_case_info.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/reporters/catch_reporter_event_listener.hpp"
#include "catch2/reporters/catch_reporter_registrars.hpp"

namespace volcano {

// See: external/catch2/examples/210-Evt-EventListeners.cpp
struct SummaryReporter : Catch::EventListenerBase {
  using EventListenerBase::EventListenerBase;

  static std::string getDescription();

  void testRunStarting(Catch::TestRunInfo const& info) override;
  void testRunEnded(Catch::TestRunStats const& stats) override;

  void sectionStarting(Catch::SectionInfo const& info) override;
  void sectionEnded(Catch::SectionStats const& stats) override;

  void assertionStarting(Catch::AssertionInfo const& info) override;
  void assertionEnded(Catch::AssertionStats const& stats) override;

 private:
  std::size_t depth_ = 0;
  std::vector<std::stringstream> report_;
};

}  // namespace volcano
