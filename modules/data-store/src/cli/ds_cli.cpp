/// ds-cli — minimal debugging client. D1 scope: connect to the
/// running ds-server, print its welcome line, exit.
///
/// Usage:  ds-cli [path]
///   path defaults to data_store::proto::kDefaultSocketPath.

#include "data_store/client.hpp"
#include "data_store/proto.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string path = (argc > 1) ? argv[1]
                                  : data_store::proto::kDefaultSocketPath;

    data_store::Client cli;
    auto cs = cli.connect(path);
    if (!cs.ok) {
        std::cerr << "[ds-cli] connect failed: " << cs.err << "\n";
        return 1;
    }

    std::string welcome;
    auto rs = cli.recv_welcome(welcome);
    if (!rs.ok) {
        std::cerr << "[ds-cli] recv_welcome failed: " << rs.err << "\n";
        return 2;
    }

    std::cout << welcome;          // already ends with '\n'
    return 0;
}
