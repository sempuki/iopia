// Copyright 2024 -- CONTRIBUTORS. See LICENSE.

#include "lib/testing.hpp"

#include <iomanip>
#include <iostream>

namespace volcano {

constexpr const std::string_view test_marker{"\033[1;34m=\033[0m"};
constexpr const std::string_view section_marker{"\033[1;33m-\033[0m"};
constexpr const std::string_view result_marker{"\033[1;33m*\033[0m"};
constexpr const std::string_view passed{"\033[1;32mPASSED\033[0m."};
constexpr const std::string_view failed{"\033[1;31mFAILED\033[0m."};

std::string SummaryReporter::getDescription() { return "Summary reporter"; }

void SummaryReporter::testRunStarting(Catch::TestRunInfo const& info) {
  report_.emplace_back();
  report_.back() << test_marker << ' ' << info.name;
}

void SummaryReporter::testRunEnded(Catch::TestRunStats const& info) {
  std::cout << "\n\n**** Test Run Results. ****\n\n";
  for (auto&& line : report_) {
    std::cout << line.str() << "\n";
  }
}

void SummaryReporter::sectionStarting(Catch::SectionInfo const& info) {
  depth_++;
  switch (depth_) {
    case 1:
      report_.emplace_back();
      report_.back() << section_marker;
      [[fallthrough]];
    case 2:  //
      report_.back() << ' ' << info.name;
      break;
    default:  //
      report_.emplace_back();
      report_.back() << std::setw(depth_ * 2) << info.name;
      break;
  }
}

void SummaryReporter::sectionEnded(Catch::SectionStats const& stats) {
  depth_--;

  auto& result = stats.assertions.allPassed() ? passed : failed;
  switch (depth_) {
    case 1:  //
      report_.back() << ' ' << result;
      break;
    case 2:
      report_.emplace_back();
      report_.back() << result_marker;
      break;
    default:  //
      break;
  }
}

void SummaryReporter::assertionStarting(Catch::AssertionInfo const& info) {}
void SummaryReporter::assertionEnded(Catch::AssertionStats const& stats) {}

CATCH_REGISTER_LISTENER(SummaryReporter)

}  // namespace volcano
