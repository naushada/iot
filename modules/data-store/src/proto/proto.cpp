#include "data_store/proto.hpp"

namespace data_store::proto {

Op parse_op(const std::string& s) {
    if (s == "set")      return Op::Set;
    if (s == "get")      return Op::Get;
    if (s == "register") return Op::Register;
    if (s == "remove")   return Op::Remove;
    return Op::Unknown;
}

std::string op_name(Op op) {
    switch (op) {
        case Op::Set:      return "set";
        case Op::Get:      return "get";
        case Op::Register: return "register";
        case Op::Remove:   return "remove";
        case Op::Unknown:  break;
    }
    return "unknown";
}

} // namespace data_store::proto
