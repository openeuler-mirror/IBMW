// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "topic_command.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "cli/command.h"
#include "find_command.h"
#include "info_command.h"
#include "list_command.h"
#include "type_command.h"

int RunTopicCommand(int argc, const char* const* argv) {
  try {
    ibmw::cli::Command topic("ibmw topic", "Inspect discovered topics");
    topic.AddHelpOption("--include-hidden-topics",
                        "Include hidden topics in list, type and find queries");
    ibmw::cli::topic::AddListCommand(topic);
    ibmw::cli::topic::AddTypeCommand(topic);
    ibmw::cli::topic::AddFindCommand(topic);
    ibmw::cli::topic::AddInfoCommand(topic);

    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    // Accept ROS 2's parent-level hidden-topic switch as well as the
    // existing subcommand form. Forward it before positional arguments.
    bool include_hidden_topics = false;
    while (arguments.size() > 1 && arguments[1] == "--include-hidden-topics") {
      include_hidden_topics = true;
      arguments.erase(arguments.begin() + 1);
    }
    if (include_hidden_topics && arguments.size() > 1 &&
        topic.HasSubcommand(arguments[1])) {
      arguments.insert(arguments.begin() + 2, "--include-hidden-topics");
    }
    std::vector<char*> mutable_argv;
    for (auto& argument : arguments) mutable_argv.push_back(argument.data());
    return topic.Run(static_cast<int>(mutable_argv.size()), mutable_argv.data());
  } catch (const std::exception& error) {
    std::cerr << "ibmw topic: error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "ibmw topic: error: Unknown failure.\n";
  }
  return 1;
}
