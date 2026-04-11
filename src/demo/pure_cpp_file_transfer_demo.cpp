#include <iostream>
#include <string>
#include "demo/demo_harness.h"

int main(int argc, char **argv)
{
    if (argc >= 2)
    {
        const std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help")
        {
            std::cout << "Usage: pure_cpp_file_transfer_demo.exe [input-file] [output-root]\n";
            std::cout << "  input-file  : optional source file path. If omitted, a sample file is generated.\n";
            std::cout << "  output-root : optional output root directory. Receiver files are written under <output-root>/receiver/<sessionId>/.\n";
            return 0;
        }
    }

    std::string inputPath;
    std::string outputRoot;
    if (argc >= 2)
    {
        inputPath = argv[1];
    }
    if (argc >= 3)
    {
        outputRoot = argv[2];
    }

    demo::DemoHarness harness;
    return harness.run(inputPath, outputRoot) ? 0 : 1;
}
