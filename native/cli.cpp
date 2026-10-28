#include "document.h"
#include "scene_buffers.h"
#include <iostream>

int main(int argc, char** argv) {
  editor::Document document;
  try {
    if (argc == 3 && std::string(argv[1]) == "inspect") {
      document.open(argv[2]);
      std::cout << document.describe().dump(2) << '\n';
      return 0;
    }
    if (argc != 1) {
      std::cerr << "Usage: remix-editor-cli [inspect PROJECT]\n"
                << "With no arguments, execute newline-delimited JSON document commands from stdin.\n";
      return 2;
    }
    std::string line;
    bool failed = false;
    while (std::getline(std::cin, line)) {
      try { std::cout << editor::Json{{"ok", true}, {"result", editor::text_buffers(document.dispatch(editor::Json::parse(line)))}}.dump() << std::endl; }
      catch (const std::exception& error) { failed = true; std::cout << editor::Json{{"ok", false}, {"error", error.what()}}.dump() << std::endl; }
    }
    return failed ? 1 : 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
